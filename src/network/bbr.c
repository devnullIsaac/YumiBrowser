/*
    Yumi UDP Congestion Control — Probe-then-Cruise (Implementation)
    Copyright (C) 2026  DevNullIsaac

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

/*
 * yumi_udp.c — Yumi UDP Congestion Control: Probe-then-Cruise
 *
 * Two-phase design for UDP:
 *
 *   PROBE  — BBR-style bandwidth/RTT discovery.  Runs at connection start
 *            and periodically thereafter to re-measure the path.
 *
 *   TDS    — Time Division Sending.  Once the bottleneck rate is known,
 *            packets are dispatched in fixed time-slots at the discovered
 *            rate.  Minimal per-packet overhead for maximum throughput.
 *            A lightweight loss watchdog triggers re-probe if conditions
 *            deteriorate.
 *
 * Pure logical layer — no sockets, no I/O.
 */

#include "network/bbr.h"
#include <string.h>

/* ════════════════════════════════════════════════════════════════════════
 *  Constants
 * ════════════════════════════════════════════════════════════════════════ */

/* BBR probe constants */
#define BBR_STARTUP_PACING_GAIN   2.885
#define BBR_STARTUP_CWND_GAIN     2.885
#define BBR_DRAIN_PACING_GAIN     (1.0 / BBR_STARTUP_PACING_GAIN)
#define BBR_DRAIN_CWND_GAIN       BBR_STARTUP_CWND_GAIN

static const double bbr_pacing_gain_cycle[8] = {
    1.25, 0.75, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0
};
#define BBR_PROBE_BW_CWND_GAIN   2.0
#define BBR_PROBE_RTT_DURATION_US 200000   /* 200 ms */

#define BBR_BTLBW_FILTER_ROUNDS  10
#define BBR_RTPROP_FILTER_US     10000000  /* 10 s */
#define BBR_FULL_BW_THRESHOLD    1.25
#define BBR_FULL_BW_COUNT        3
#define BBR_MIN_CWND_PACKETS     4

/* Loss-aware STARTUP exit (BBRv2-style).
 * If loss exceeds this fraction during STARTUP, exit immediately
 * to DRAIN instead of waiting for 3 rounds of no BtlBw growth.
 * Prevents congestion collapse on fast/short paths where STARTUP's
 * 2.885× pacing gain overflows receiver socket buffers. */
#define BBR_STARTUP_LOSS_THRESH  0.02   /* 2 % */
#define BBR_STARTUP_LOSS_MIN_SENT (64 * 1500) /* need ≥64 packets of data */

/*
 * How many PROBE_BW cycles to run before the probe is "done" and we
 * graduate to TDS.  2 full 8-phase rotations gives solid confidence.
 */
#define PROBE_BW_SETTLE_CYCLES   16

/* TDS defaults */
#define TDS_DEFAULT_RATE_FACTOR    0.90   /* use 90 % of BtlBw           */
#define TDS_DEFAULT_SLOT_US        1000   /* 1 ms slots                  */
#define TDS_LOSS_WINDOW_US         1000000 /* 1 s watchdog window        */
#define TDS_DEFAULT_LOSS_THRESH    0.05   /* 5 % loss → re-probe         */
#define TDS_DEFAULT_REPROBE_US     30000000 /* re-probe every 30 s       */

/* ════════════════════════════════════════════════════════════════════════
 *  Windowed-max filter (BtlBw)
 * ════════════════════════════════════════════════════════════════════════ */

static void max_filter_reset(bbr_max_filter_t *f)
{
    memset(f, 0, sizeof(*f));
}

static void max_filter_update(bbr_max_filter_t *f, uint64_t val, uint64_t round)
{
    if (f->count == 0 || val >= f->samples[0].val) {
        f->samples[0] = (bbr_filter_sample_t){ .val = val, .stamp = round };
        f->count = 1;
        return;
    }
    int write = 0;
    for (int i = 0; i < f->count; i++) {
        if (round - f->samples[i].stamp < BBR_BTLBW_FILTER_ROUNDS)
            f->samples[write++] = f->samples[i];
    }
    f->count = write;
    while (f->count > 0 && f->samples[f->count - 1].val <= val)
        f->count--;
    if (f->count < BBR_FILTER_LEN)
        f->samples[f->count++] = (bbr_filter_sample_t){ .val = val, .stamp = round };
}

static uint64_t max_filter_get(const bbr_max_filter_t *f)
{
    return f->count > 0 ? f->samples[0].val : 0;
}

/* ════════════════════════════════════════════════════════════════════════
 *  BBR probe internals
 * ════════════════════════════════════════════════════════════════════════ */

static uint64_t probe_bdp(const bbr_probe_t *p, uint64_t initial_cwnd)
{
    if (p->rtprop_us == UINT64_MAX || p->btl_bw == 0)
        return initial_cwnd;
    return (uint64_t)((double)p->btl_bw * (double)p->rtprop_us / 1e6);
}

static uint64_t probe_delivery_rate(const bbr_probe_t *p,
                                    const yumi_packet_t *pkt)
{
    uint64_t d_bytes = p->delivered - pkt->delivered;
    uint64_t ack_elapsed  = p->delivered_time - pkt->delivered_time;
    uint64_t send_elapsed = pkt->send_time_us > pkt->first_sent_time
                          ? pkt->send_time_us - pkt->first_sent_time : 0;
    /* Standard BBR: use the longer of send and ack intervals to prevent
     * inflated rates from ACK batching / compression. */
    uint64_t interval = ack_elapsed > send_elapsed ? ack_elapsed : send_elapsed;
    if (interval == 0) return 0;
    return (uint64_t)((double)d_bytes * 1e6 / (double)interval);
}

static void probe_update_round(bbr_probe_t *p, const yumi_packet_t *pkt)
{
    if (pkt->delivered >= p->next_round_delivered) {
        p->next_round_delivered = p->delivered;
        p->round_count++;
        p->round_start = true;
    } else {
        p->round_start = false;
    }
}

static void probe_update_btl_bw(bbr_probe_t *p, const yumi_packet_t *pkt,
                                 uint64_t rate)
{
    if (rate == 0) return;
    if (pkt->is_app_limited && rate <= p->btl_bw) return;
    max_filter_update(&p->btl_bw_filter, rate, p->round_count);
    p->btl_bw = max_filter_get(&p->btl_bw_filter);
}

static void probe_update_rtprop(bbr_probe_t *p, uint64_t rtt_us, uint64_t now_us)
{
    p->rtprop_expired = (now_us - p->rtprop_stamp > BBR_RTPROP_FILTER_US);
    if (rtt_us <= p->rtprop_us || p->rtprop_expired) {
        p->rtprop_us    = rtt_us;
        p->rtprop_stamp = now_us;
    }
}

static bool probe_filled_pipe(const bbr_probe_t *p)
{
    return p->full_bw_count >= BBR_FULL_BW_COUNT;
}

static void probe_check_full_bw(bbr_probe_t *p)
{
    if (!p->round_start) return;
    if (p->btl_bw >= (uint64_t)((double)p->full_bw * BBR_FULL_BW_THRESHOLD)) {
        p->full_bw       = p->btl_bw;
        p->full_bw_count = 0;
        return;
    }
    p->full_bw_count++;
}

static void probe_set_pacing_rate(bbr_probe_t *p)
{
    uint64_t rate = (uint64_t)(p->pacing_gain * (double)p->btl_bw);
    if (probe_filled_pipe(p) || rate > p->pacing_rate)
        p->pacing_rate = rate;
}

static void probe_set_cwnd(bbr_probe_t *p, uint64_t initial_cwnd, uint64_t min_cwnd)
{
    uint64_t bdp    = probe_bdp(p, initial_cwnd);
    uint64_t target = (uint64_t)(p->cwnd_gain * (double)bdp);

    if (p->state == BBR_PROBE_RTT) {
        p->cwnd = min_cwnd;
        return;
    }
    if (p->state == BBR_STARTUP) {
        if (target > p->cwnd) p->cwnd = target;
    } else {
        p->cwnd = target;
    }
    if (p->cwnd < min_cwnd) p->cwnd = min_cwnd;
}

/* State transitions (same BBR logic, but contained in probe sub-state) */

static void probe_enter_drain(bbr_probe_t *p)
{
    p->state       = BBR_DRAIN;
    p->pacing_gain = BBR_DRAIN_PACING_GAIN;
    p->cwnd_gain   = BBR_DRAIN_CWND_GAIN;
}

static void probe_enter_probe_bw(bbr_probe_t *p, uint64_t now_us)
{
    p->state       = BBR_PROBE_BW;
    p->cwnd_gain   = BBR_PROBE_BW_CWND_GAIN;
    p->cycle_index = 1 + (int)(now_us % 7);
    p->pacing_gain = bbr_pacing_gain_cycle[p->cycle_index];
    p->cycle_stamp = now_us;
}

static void probe_enter_probe_rtt(bbr_probe_t *p)
{
    p->state          = BBR_PROBE_RTT;
    p->pacing_gain    = 1.0;
    p->cwnd_gain      = 1.0;
    p->probe_rtt_done = false;
}

static void probe_update_state(bbr_probe_t *p, uint64_t initial_cwnd,
                                uint64_t min_cwnd, uint64_t now_us)
{
    uint64_t bdp = probe_bdp(p, initial_cwnd);

    switch (p->state) {
    case BBR_STARTUP:
        probe_check_full_bw(p);
        if (p->full_bw_count >= BBR_FULL_BW_COUNT)
            probe_enter_drain(p);
        break;

    case BBR_DRAIN:
        if (p->inflight <= bdp)
            probe_enter_probe_bw(p, now_us);
        break;

    case BBR_PROBE_BW:
        if (now_us - p->cycle_stamp > p->rtprop_us) {
            p->cycle_index = (p->cycle_index + 1) % 8;
            p->pacing_gain = bbr_pacing_gain_cycle[p->cycle_index];
            p->cycle_stamp = now_us;
        }
        if (p->rtprop_expired)
            probe_enter_probe_rtt(p);
        break;

    case BBR_PROBE_RTT:
        if (!p->probe_rtt_done && p->inflight <= min_cwnd) {
            p->probe_rtt_done       = true;
            p->probe_rtt_done_stamp = now_us;
        }
        if (p->probe_rtt_done &&
            (now_us - p->probe_rtt_done_stamp > BBR_PROBE_RTT_DURATION_US)) {
            p->rtprop_stamp = now_us;
            if (p->full_bw_count >= BBR_FULL_BW_COUNT)
                probe_enter_probe_bw(p, now_us);
            else {
                p->state       = BBR_STARTUP;
                p->pacing_gain = BBR_STARTUP_PACING_GAIN;
                p->cwnd_gain   = BBR_STARTUP_CWND_GAIN;
            }
        }
        break;
    }
}

static void probe_init(bbr_probe_t *p, uint64_t initial_cwnd)
{
    memset(p, 0, sizeof(*p));
    p->state       = BBR_STARTUP;
    p->pacing_gain = BBR_STARTUP_PACING_GAIN;
    p->cwnd_gain   = BBR_STARTUP_CWND_GAIN;
    p->cwnd        = initial_cwnd;
    p->rtprop_us   = UINT64_MAX;
    p->pacing_rate = (uint64_t)((double)initial_cwnd * 1e6 / 1000.0);
    max_filter_reset(&p->btl_bw_filter);
}

/* ════════════════════════════════════════════════════════════════════════
 *  TDS internals
 * ════════════════════════════════════════════════════════════════════════ */

static void tds_init_from_probe(tds_state_t *t, const bbr_probe_t *p,
                                double rate_factor, uint64_t slot_us,
                                uint64_t now_us)
{
    memset(t, 0, sizeof(*t));

    t->send_rate = (uint64_t)((double)p->btl_bw * rate_factor);
    t->rtprop_us = p->rtprop_us;

    /* Compute slot parameters:
     *   burst_bytes = send_rate × slot_interval / 1e6 */
    t->slot_interval_us = slot_us;
    t->burst_bytes      = (uint64_t)((double)t->send_rate *
                                     (double)slot_us / 1e6);
    if (t->burst_bytes == 0)
        t->burst_bytes = 1;

    t->last_slot_time = now_us;
    t->window_start   = now_us;
}

/*
 * Check if the loss watchdog says we should re-probe.
 * Returns true if loss in the current window exceeds the threshold.
 */
static bool tds_loss_exceeded(const tds_state_t *t, double thresh,
                              uint64_t now_us)
{
    /* Don't judge until the window has run for a while */
    if (now_us - t->window_start < TDS_LOSS_WINDOW_US)
        return false;
    if (t->window_sent == 0)
        return false;
    double ratio = (double)t->window_lost / (double)t->window_sent;
    return ratio > thresh;
}

static void tds_rotate_window(tds_state_t *t, uint64_t now_us)
{
    if (now_us - t->window_start >= TDS_LOSS_WINDOW_US) {
        t->window_sent  = 0;
        t->window_lost  = 0;
        t->window_start = now_us;
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Mode transitions
 * ════════════════════════════════════════════════════════════════════════ */

/*
 * Called during PROBE mode's on_ack to check whether the probe has
 * settled enough to graduate to TDS.
 *
 * Conditions:
 *   1. BBR sub-state reached PROBE_BW (startup + drain complete).
 *   2. At least PROBE_BW_SETTLE_CYCLES cycle advances have happened
 *      (i.e. a couple of full 8-phase rotations for stable estimate).
 */
static bool should_graduate_to_tds(const bbr_probe_t *p)
{
    if (p->state != BBR_PROBE_BW)
        return false;
    if (p->btl_bw == 0 || p->rtprop_us == UINT64_MAX)
        return false;
    if (p->round_count < PROBE_BW_SETTLE_CYCLES)
        return false;
    return true;
}

static void enter_tds(yumi_cc_t *cc, uint64_t now_us)
{
    tds_init_from_probe(&cc->tds, &cc->probe,
                        cc->tds_rate_factor, cc->tds_slot_target_us, now_us);
    cc->mode            = YUMI_MODE_TDS;
    cc->last_probe_time = now_us;
}

static void enter_probe(yumi_cc_t *cc, uint64_t now_us)
{
    cc->mode = YUMI_MODE_PROBE;
    probe_init(&cc->probe, cc->initial_cwnd);
    /* Carry over the last known RTprop as a starting hint if available */
    if (cc->tds.rtprop_us != 0 && cc->tds.rtprop_us != UINT64_MAX) {
        cc->probe.rtprop_us    = cc->tds.rtprop_us;
        cc->probe.rtprop_stamp = now_us;
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Public API
 * ════════════════════════════════════════════════════════════════════════ */

void yumi_cc_init(yumi_cc_t *cc, uint64_t mss)
{
    memset(cc, 0, sizeof(*cc));

    cc->mss          = mss;
    cc->min_cwnd     = BBR_MIN_CWND_PACKETS * mss;
    cc->initial_cwnd = 10 * mss;

    cc->tds_rate_factor     = TDS_DEFAULT_RATE_FACTOR;
    cc->tds_slot_target_us  = TDS_DEFAULT_SLOT_US;
    cc->reprobe_interval_us = TDS_DEFAULT_REPROBE_US;
    cc->loss_reprobe_thresh = TDS_DEFAULT_LOSS_THRESH;

    cc->mode = YUMI_MODE_PROBE;
    probe_init(&cc->probe, cc->initial_cwnd);
}

uint64_t yumi_cc_on_send(yumi_cc_t *cc, yumi_packet_t *pkt, uint64_t now_us)
{
    pkt->send_time_us = now_us;

    if (cc->mode == YUMI_MODE_PROBE) {
        /* ── PROBE: full BBR pacing ── */
        bbr_probe_t *p = &cc->probe;

        pkt->delivered      = p->delivered;
        pkt->delivered_time = p->delivered_time;
        pkt->first_sent_time = p->first_sent_time;
        pkt->is_app_limited = p->is_app_limited;
        pkt->is_probe       = true;

        p->inflight += pkt->size;
        p->startup_bytes_sent += pkt->size;

        if (p->pacing_rate == 0) return 0;
        return (uint64_t)((double)pkt->size * 1e6 / (double)p->pacing_rate);

    } else {
        /* ── TDS: fixed time-slot pacing ── */
        tds_state_t *t = &cc->tds;

        pkt->is_probe = false;
        t->total_sent += pkt->size;
        t->window_sent += pkt->size;

        /* Return the fixed slot interval — caller spaces sends by this. */
        return t->slot_interval_us;
    }
}

void yumi_cc_on_ack(yumi_cc_t *cc, const yumi_packet_t *pkt,
                    uint64_t acked_bytes, uint64_t rtt_us, uint64_t now_us)
{
    if (cc->mode == YUMI_MODE_PROBE) {
        /* ── PROBE: full BBR processing ── */
        bbr_probe_t *p = &cc->probe;

        p->delivered     += acked_bytes;
        p->delivered_time = now_us;
        p->first_sent_time = pkt->send_time_us;

        if (p->inflight >= acked_bytes)
            p->inflight -= acked_bytes;
        else
            p->inflight = 0;

        probe_update_round(p, pkt);

        uint64_t dr = probe_delivery_rate(p, pkt);
        probe_update_btl_bw(p, pkt, dr);
        probe_update_rtprop(p, rtt_us, now_us);
        probe_update_state(p, cc->initial_cwnd, cc->min_cwnd, now_us);
        probe_set_pacing_rate(p);
        probe_set_cwnd(p, cc->initial_cwnd, cc->min_cwnd);

        /* Check graduation to TDS */
        if (should_graduate_to_tds(p))
            enter_tds(cc, now_us);

    } else {
        /* ── TDS: lightweight bookkeeping ── */
        tds_state_t *t = &cc->tds;
        t->total_acked += acked_bytes;

        /* Rotate the loss-watchdog window if needed */
        tds_rotate_window(t, now_us);

        /* Periodic re-probe timer */
        if (now_us - cc->last_probe_time > cc->reprobe_interval_us) {
            enter_probe(cc, now_us);
            return;
        }

        /* Loss-triggered re-probe */
        if (tds_loss_exceeded(t, cc->loss_reprobe_thresh, now_us)) {
            enter_probe(cc, now_us);
            return;
        }
    }
}

void yumi_cc_on_loss(yumi_cc_t *cc, uint64_t lost_bytes)
{
    if (cc->mode == YUMI_MODE_PROBE) {
        bbr_probe_t *p = &cc->probe;
        if (p->inflight >= lost_bytes)
            p->inflight -= lost_bytes;
        else
            p->inflight = 0;
        p->startup_bytes_lost += lost_bytes;
    } else {
        tds_state_t *t = &cc->tds;
        t->total_lost  += lost_bytes;
        t->window_lost += lost_bytes;
    }
}

/* ── Queries ───────────────────────────────────────────────────────────── */

yumi_mode_t yumi_cc_get_mode(const yumi_cc_t *cc)
{
    return cc->mode;
}

uint64_t yumi_cc_get_send_rate(const yumi_cc_t *cc)
{
    if (cc->mode == YUMI_MODE_PROBE)
        return cc->probe.pacing_rate;
    return cc->tds.send_rate;
}

uint64_t yumi_cc_get_cwnd(const yumi_cc_t *cc)
{
    if (cc->mode == YUMI_MODE_PROBE)
        return cc->probe.cwnd;
    /* In TDS the "window" is the burst size × BDP-worth of slots */
    return cc->tds.burst_bytes *
           (cc->tds.rtprop_us / (cc->tds.slot_interval_us ? cc->tds.slot_interval_us : 1));
}

uint64_t yumi_cc_get_slot_interval(const yumi_cc_t *cc)
{
    if (cc->mode == YUMI_MODE_TDS)
        return cc->tds.slot_interval_us;
    return 0;  /* not meaningful in PROBE mode */
}

void yumi_cc_force_reprobe(yumi_cc_t *cc, uint64_t now_us)
{
    enter_probe(cc, now_us);
}

/*
 * ── Usage Example ─────────────────────────────────────────────────────────
 *
 *  yumi_cc_t cc;
 *  yumi_cc_init(&cc, 1200);              // starts in PROBE mode
 *
 *  // Optional tuning before first send:
 *  cc.tds_rate_factor     = 0.85;        // use 85 % of discovered BtlBw
 *  cc.tds_slot_target_us  = 500;         // 0.5 ms slots for tighter pacing
 *  cc.reprobe_interval_us = 15000000;    // re-probe every 15 s
 *  cc.loss_reprobe_thresh = 0.02;        // re-probe on 2 % loss
 *
 *  // Send loop
 *  while (has_data()) {
 *      yumi_packet_t pkt = { .seq = next_seq++, .size = payload_len };
 *      uint64_t now   = get_monotonic_us();
 *      uint64_t delay = yumi_cc_on_send(&cc, &pkt, now);
 *      store_metadata(pkt);
 *
 *      // In TDS mode, delay is the constant slot interval.
 *      // In PROBE mode, delay is BBR pacing.
 *      // Either way: wait `delay` µs, then send the next packet.
 *
 *      if (cc.mode == YUMI_MODE_TDS) {
 *          // High-throughput path — blast at slot rate, no cwnd check
 *          send_packet(pkt);
 *      } else {
 *          // During probe, respect cwnd
 *          if (cc.probe.inflight + pkt.size <= yumi_cc_get_cwnd(&cc))
 *              send_packet(pkt);
 *      }
 *
 *      sleep_us(delay);
 *  }
 *
 *  // On ACK
 *  yumi_packet_t *orig = lookup(acked_seq);
 *  yumi_cc_on_ack(&cc, orig, orig->size, now - orig->send_time_us, now);
 *  //  ^-- may switch mode automatically (PROBE→TDS or TDS→PROBE)
 *
 *  // On loss
 *  yumi_cc_on_loss(&cc, lost_pkt_size);
 *  //  ^-- in TDS, excessive loss triggers automatic re-probe
 *
 *  // Force re-probe (e.g. on network interface change)
 *  yumi_cc_force_reprobe(&cc, now);
 *
 * ────────────────────────────────────────────────────────────────────────── */
