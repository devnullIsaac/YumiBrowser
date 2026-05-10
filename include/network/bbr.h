/*
    Yumi UDP Congestion Control — Probe-then-Cruise (BBR-derived)
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

#ifndef YUMI_NET_H
#define YUMI_NET_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 *  Yumi UDP Congestion Control — Probe-then-Cruise model
 *
 *  Two top-level modes:
 *
 *    PROBE  — BBR-style bandwidth/RTT discovery.  Runs briefly at
 *             connection start, then periodically to re-measure the path.
 *
 *    TDS    — Time Division Sending.  Once the bottleneck rate and RTT
 *             are known, packets are dispatched in fixed time-slots at
 *             the discovered rate.  Minimal per-packet overhead; no
 *             delivery-rate tracking, no round counting.  Just blast.
 *
 *  All time values are in microseconds (uint64_t).
 *  All bandwidth values are in bytes/second (uint64_t).
 * ────────────────────────────────────────────────────────────────────────── */

/* ── Top-level mode ────────────────────────────────────────────────────── */
typedef enum {
    YUMI_MODE_PROBE,       /* BBR-like discovery in progress         */
    YUMI_MODE_TDS,         /* Time Division Sending (high throughput) */
} yumi_mode_t;

/* ── BBR probe sub-states ──────────────────────────────────────────────── */
typedef enum {
    BBR_STARTUP,
    BBR_DRAIN,
    BBR_PROBE_BW,
    BBR_PROBE_RTT,
} bbr_state_t;

/* ── Per-packet metadata (caller stores, feeds back on ACK) ────────────── */
typedef struct {
    uint32_t seq;
    uint64_t send_time_us;
    uint64_t size;
    /* Only used during PROBE mode */
    uint64_t delivered;
    uint64_t delivered_time;
    uint64_t first_sent_time;    /* send_time of pkt that set delivered   */
    bool     is_app_limited;
    bool     is_probe;           /* true = sent during PROBE phase */
} yumi_packet_t;

/* ── Windowed-max filter (for BtlBw) ──────────────────────────────────── */
typedef struct {
    uint64_t val;
    uint64_t stamp;
} bbr_filter_sample_t;

#define BBR_FILTER_LEN 10

typedef struct {
    bbr_filter_sample_t samples[BBR_FILTER_LEN];
    int                 count;
} bbr_max_filter_t;

/* ── BBR probe state (internal) ────────────────────────────────────────── */
typedef struct {
    bbr_state_t      state;

    bbr_max_filter_t btl_bw_filter;
    uint64_t         btl_bw;

    uint64_t         rtprop_us;
    uint64_t         rtprop_stamp;

    uint64_t         next_round_delivered;
    uint64_t         round_count;
    bool             round_start;

    uint64_t         delivered;
    uint64_t         delivered_time;

    double           pacing_gain;
    uint64_t         pacing_rate;

    double           cwnd_gain;
    uint64_t         cwnd;
    uint64_t         inflight;

    uint64_t         full_bw;
    int              full_bw_count;

    int              cycle_index;
    uint64_t         cycle_stamp;

    bool             probe_rtt_done;
    uint64_t         probe_rtt_done_stamp;
    bool             rtprop_expired;

    bool             is_app_limited;

    /* Send-time reference for delivery rate (prevents ACK-batch inflation) */
    uint64_t         first_sent_time;

    /* Loss-aware STARTUP exit (BBRv2-style) */
    uint64_t         startup_bytes_sent;
    uint64_t         startup_bytes_lost;
    bool             startup_loss_exit;  /* true = exited via loss */
} bbr_probe_t;

/* ── TDS (Time Division Sending) state ─────────────────────────────────── */
typedef struct {
    uint64_t send_rate;          /* bytes/sec — the discovered rate        */
    uint64_t rtprop_us;          /* propagation delay snapshot             */

    uint64_t slot_interval_us;   /* time between send bursts               */
    uint64_t burst_bytes;        /* bytes per slot                         */

    uint64_t last_slot_time;     /* timestamp of last burst                */

    /* Lightweight loss watchdog */
    uint64_t total_sent;
    uint64_t total_acked;
    uint64_t total_lost;
    uint64_t window_sent;        /* sent in current watchdog window        */
    uint64_t window_lost;        /* lost in current watchdog window        */
    uint64_t window_start;       /* timestamp when window started          */
} tds_state_t;

/* ── Main controller ───────────────────────────────────────────────────── */
typedef struct {
    yumi_mode_t   mode;

    /* Shared config */
    uint64_t      mss;
    uint64_t      min_cwnd;
    uint64_t      initial_cwnd;

    /* Sub-states */
    bbr_probe_t   probe;
    tds_state_t   tds;

    /* Re-probe scheduling */
    uint64_t      last_probe_time;      /* when PROBE last completed      */
    uint64_t      reprobe_interval_us;  /* how often to re-probe          */

    /* TDS tuning */
    double        tds_rate_factor;      /* fraction of BtlBw to use (≤1)  */
    uint64_t      tds_slot_target_us;   /* desired slot granularity       */

    /* Loss threshold to trigger re-probe (0.0–1.0) */
    double        loss_reprobe_thresh;
} yumi_cc_t;

/* ── Public API ────────────────────────────────────────────────────────── */

void     yumi_cc_init(yumi_cc_t *cc, uint64_t mss);

uint64_t yumi_cc_on_send(yumi_cc_t *cc, yumi_packet_t *pkt, uint64_t now_us);

void     yumi_cc_on_ack(yumi_cc_t *cc, const yumi_packet_t *pkt,
                        uint64_t acked_bytes, uint64_t rtt_us, uint64_t now_us);

void     yumi_cc_on_loss(yumi_cc_t *cc, uint64_t lost_bytes);

yumi_mode_t yumi_cc_get_mode(const yumi_cc_t *cc);
uint64_t    yumi_cc_get_send_rate(const yumi_cc_t *cc);
uint64_t    yumi_cc_get_cwnd(const yumi_cc_t *cc);
uint64_t    yumi_cc_get_slot_interval(const yumi_cc_t *cc);

void yumi_cc_force_reprobe(yumi_cc_t *cc, uint64_t now_us);

#endif
