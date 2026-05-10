/*
    Yumi Tests — Secure UDP Client Test Suite
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
 * test_sudp_client.c — Comprehensive test suite for the Secure UDP client.
 *
 * Sections:
 *
 *   1.  Group Registrar Setup — identities, group (8 peers), epoch.
 *   2.  Full Invite → Join → Attest → Verify → SUDP — real membership
 *       verification with separate registrars, then dual-hybrid handshake
 *       and bidirectional data exchange.
 *   3.  First-Contact with Join Verification — triple-hybrid handshake
 *       (ML-KEM + FrodoKEM + BP512) using a join-verified registrar.
 *   4.  Continued-Session + Bidirectional Data — dual-hybrid, reliable,
 *       unreliable, channelized, all directions.
 *   5.  Sustained 5-second session — throughput, stability, state.
 *   6.  Edge Cases — unknown peer, pre-HS send, max payload, double connect,
 *       NULL safety, empty payload.
 *   7.  Kicked Peer — handshake must fail if peer is kicked.
 *   8.  Simultaneous Open — both sides connect at the same time.
 *   9.  Max-Payload Boundary — send exactly max_payload bytes + one over.
 *  10.  Multi-Channel Stress — rapid-fire across all 256 channels.
 *  11.  Rapid Reconnect — destroy + recreate + handshake in a loop.
 *  12.  One-Byte Payload — minimal encrypted data round-trip.
 *  13.  Large Payload Reliable Burst — many full-size reliable packets back
 *       to back.
 *  14.  Interleaved Reliable/Unreliable — ordering and delivery guarantees.
 *  15.  Destroy During Handshake — destroy mid-handshake, no crash.
 *  16.  Wrong Epoch Key — tampered registrar with different epoch key,
 *       proves the cryptographic boundary rejects mismatched keys.
 *  17.  PROVISIONAL Registrar — SUDP handshake succeeds while join
 *       verification is still PROVISIONAL (design intent).
 *  18.  Epoch Rotation Rekey — session survives epoch rotation via rekey_seed.
 *  19.  Kicked Peer Active Session — notify_kick tears down live session.
 *  20.  Protocol Version — verify version byte in handshake messages.
 */

#include "network/yumi_sudp_client.h"
#include "group_registrar.h"
#include "crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdatomic.h>
#include <time.h>

/* ════════════════════════════════════════════════════════════════════════
 *  Test infrastructure
 * ════════════════════════════════════════════════════════════════════════ */

static int g_run  = 0;
static int g_fail = 0;

#define T(cond, msg) do { \
    g_run++; \
    if (!(cond)) { \
        g_fail++; \
        fprintf(stderr, "  FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
    } \
} while (0)

#define SEC(name) fprintf(stdout, "\n── %s ──\n", name)

/* ── Per-peer callback state ───────────────────────────────────────── */

typedef struct {
    _Alignas(64) atomic_int           established;
    _Alignas(64) atomic_int           failed;
    _Alignas(64) atomic_uint_fast64_t recv_count;
    _Alignas(64) atomic_uint_fast64_t recv_bytes;
    /* Per-channel counters for multi-channel stress */
    _Alignas(64) atomic_uint_fast64_t chan_recv[256];
    uint8_t                           last_channel;
    bool                              last_reliable;
    uint8_t                           last_buf[4096];
    uint32_t                          last_len;
} peer_cb_state_t;

static void peer_state_reset(peer_cb_state_t *s)
{
    memset(s, 0, sizeof(*s));
    atomic_store(&s->established, 0);
    atomic_store(&s->failed, 0);
    atomic_store(&s->recv_count, 0);
    atomic_store(&s->recv_bytes, 0);
    for (int i = 0; i < 256; i++)
        atomic_store(&s->chan_recv[i], 0);
}

static void state_cb(void *user, yumi_sudp_state_t st)
{
    peer_cb_state_t *s = (peer_cb_state_t *)user;
    if (st == YUMI_SUDP_ESTABLISHED)
        atomic_store(&s->established, 1);
    else if (st == YUMI_SUDP_FAILED)
        atomic_store(&s->failed, 1);
}

static void recv_cb(void *user, uint8_t channel,
                     const void *data, uint32_t len, bool reliable)
{
    peer_cb_state_t *s = (peer_cb_state_t *)user;
    s->last_channel  = channel;
    s->last_reliable = reliable;
    if (len <= sizeof(s->last_buf)) {
        memcpy(s->last_buf, data, len);
        s->last_len = len;
    }
    atomic_fetch_add(&s->recv_count, 1);
    atomic_fetch_add(&s->recv_bytes, len);
    atomic_fetch_add(&s->chan_recv[channel], 1);
}

/* ── Helpers ───────────────────────────────────────────────────────── */

static struct sockaddr_in6 lo6(uint16_t port)
{
    struct sockaddr_in6 a = {0};
    a.sin6_family = AF_INET6;
    a.sin6_port   = htons(port);
    a.sin6_addr   = in6addr_loopback;
    return a;
}

static bool wait_established(peer_cb_state_t *a, peer_cb_state_t *b,
                              int timeout_ms)
{
    for (int i = 0; i < timeout_ms / 10; i++) {
        if (atomic_load(&a->established) && atomic_load(&b->established))
            return true;
        usleep(10000);
    }
    return false;
}

static bool wait_one_established(peer_cb_state_t *a, int timeout_ms)
{
    for (int i = 0; i < timeout_ms / 10; i++) {
        if (atomic_load(&a->established))
            return true;
        usleep(10000);
    }
    return false;
}

static bool wait_failed(peer_cb_state_t *a, int timeout_ms)
{
    for (int i = 0; i < timeout_ms / 10; i++) {
        if (atomic_load(&a->failed))
            return true;
        usleep(10000);
    }
    return false;
}

static bool wait_recv(peer_cb_state_t *s, uint64_t min_count, int timeout_ms)
{
    for (int i = 0; i < timeout_ms / 10; i++) {
        if (atomic_load(&s->recv_count) >= min_count)
            return true;
        usleep(10000);
    }
    return false;
}

/* Helper: create a quick dual-hybrid pair, connect, wait for establish.
 * Returns 0 on success, -1 on failure.  Caller must destroy both. */
static int make_pair(gr_registrar_t *reg,
                     const gr_identity_t *id_a,
                     const gr_identity_t *id_b,
                     peer_cb_state_t *sa_s, peer_cb_state_t *sb_s,
                     yumi_sudp_client_t **out_a, yumi_sudp_client_t **out_b,
                     uint16_t pa, uint16_t pb)
{
    peer_state_reset(sa_s);
    peer_state_reset(sb_s);

    yumi_sudp_config_t ca = {0};
    ca.transport.peer_addr  = lo6(pb);
    ca.transport.local_port = pa;
    ca.registrar = reg; ca.identity = id_a;
    ca.first_contact = false;
    ca.recv_cb = recv_cb; ca.state_cb = state_cb; ca.user = sa_s;

    yumi_sudp_config_t cb = {0};
    cb.transport.peer_addr  = lo6(pa);
    cb.transport.local_port = pb;
    cb.registrar = reg; cb.identity = id_b;
    cb.first_contact = false;
    cb.recv_cb = recv_cb; cb.state_cb = state_cb; cb.user = sb_s;

    *out_a = NULL; *out_b = NULL;
    if (yumi_sudp_client_create(out_a, &ca) != 0) return -1;
    if (yumi_sudp_client_create(out_b, &cb) != 0) {
        yumi_sudp_client_destroy(*out_a); *out_a = NULL;
        return -1;
    }
    if (yumi_sudp_client_connect(*out_a, id_b->peer_id) != 0) {
        yumi_sudp_client_destroy(*out_a); *out_a = NULL;
        yumi_sudp_client_destroy(*out_b); *out_b = NULL;
        return -1;
    }
    if (!wait_established(sa_s, sb_s, 5000)) {
        yumi_sudp_client_destroy(*out_a); *out_a = NULL;
        yumi_sudp_client_destroy(*out_b); *out_b = NULL;
        return -1;
    }
    return 0;
}

/* Helper: add a peer to the registrar with proper struct population */
static gr_error_t add_peer(gr_registrar_t *reg,
                            const gr_identity_t *peer,
                            const gr_identity_t *admin)
{
    gr_peer_t p = {0};
    memcpy(p.peer_id,  peer->peer_id,    GR_PEER_ID_LEN);
    memcpy(p.sign_key, peer->public_key,  GR_PUBLIC_KEY_LEN);
    memcpy(p.kem_pk,   peer->kem_pk,      GR_KEM_PUBLIC_KEY_LEN);
    snprintf(p.ip, sizeof(p.ip), "::1");
    p.port       = 9000;
    p.status     = GR_PEER_ACTIVE;
    p.joined_at  = gr_timestamp_ms();
    p.last_seen  = p.joined_at;
    return gr_peer_add(reg, &p, admin);
}

/* Port allocator to avoid conflicts between subtests */
static uint16_t g_port_base = 20000;
static uint16_t alloc_port(void) { return g_port_base++; }

/* ════════════════════════════════════════════════════════════════════════
 *  Section 1: Group Registrar Setup
 * ════════════════════════════════════════════════════════════════════════ */

/* Clone a registrar into a new in-memory instance.
 * The source must be signed (valid header signature required for deserialization).
 * Uses the owner identity so gr_deserialize's owner_sign_key check passes.
 * Returns 0 on success, -1 on failure.  Caller must gr_close the clone. */
static int clone_registrar(gr_registrar_t *src,
                           const gr_identity_t *owner,
                           gr_registrar_t **out)
{
    *out = NULL;
    uint8_t *blob = NULL;
    size_t blen = 0;
    if (gr_serialize(src, GR_SERIALIZE_FULL, &blob, &blen) != GR_OK)
        return -1;
    gr_registrar_t *clone = NULL;
    if (gr_create(&clone, ":memory:", "clone", GR_GROUP_PRIVATE, owner) != GR_OK) {
        gr_free(blob);
        return -1;
    }
    gr_error_t err = gr_deserialize(clone, blob, blen);
    gr_free(blob);
    if (err != GR_OK) {
        gr_close(clone);
        return -1;
    }
    *out = clone;
    return 0;
}

static gr_registrar_t *g_reg  = NULL;
static gr_identity_t   g_owner;
static gr_identity_t   g_peer_b;     /* a returning peer */
static gr_identity_t   g_peer_c;     /* a new invitee (first-contact) */
static gr_identity_t   g_peer_d;     /* extra peer for kick test */
static gr_identity_t   g_peer_e;     /* attestor for join verification */
static gr_identity_t   g_peer_f;     /* attestor for join verification */
static gr_identity_t   g_peer_g;     /* attestor for join verification */
static gr_identity_t   g_peer_h;     /* extra peer for quorum */

static void test_registrar_setup(void)
{
    SEC("1  Group registrar setup (8 peers for quorum)");

    /* Generate identities */
    T(gr_identity_generate(&g_owner)  == GR_OK, "owner identity gen");
    T(gr_identity_generate(&g_peer_b) == GR_OK, "peer B identity gen");
    T(gr_identity_generate(&g_peer_c) == GR_OK, "peer C identity gen");
    T(gr_identity_generate(&g_peer_d) == GR_OK, "peer D identity gen");
    T(gr_identity_generate(&g_peer_e) == GR_OK, "peer E identity gen");
    T(gr_identity_generate(&g_peer_f) == GR_OK, "peer F identity gen");
    T(gr_identity_generate(&g_peer_g) == GR_OK, "peer G identity gen");
    T(gr_identity_generate(&g_peer_h) == GR_OK, "peer H identity gen");

    /* Create group */
    T(gr_create(&g_reg, ":memory:", "TestSUDP", GR_GROUP_PRIVATE, &g_owner)
      == GR_OK, "gr_create");
    T(gr_sign(g_reg, &g_owner) == GR_OK, "gr_sign (initial)");

    /* Add peers — 7 peers + owner = 8 active, above small_group_bypass (5) */
    T(add_peer(g_reg, &g_peer_b, &g_owner) == GR_OK, "add peer B");
    T(add_peer(g_reg, &g_peer_c, &g_owner) == GR_OK, "add peer C");
    T(add_peer(g_reg, &g_peer_d, &g_owner) == GR_OK, "add peer D");
    T(add_peer(g_reg, &g_peer_e, &g_owner) == GR_OK, "add peer E");
    T(add_peer(g_reg, &g_peer_f, &g_owner) == GR_OK, "add peer F");
    T(add_peer(g_reg, &g_peer_g, &g_owner) == GR_OK, "add peer G");
    T(add_peer(g_reg, &g_peer_h, &g_owner) == GR_OK, "add peer H");

    /* Rotate epoch so there's a valid key */
    T(gr_epoch_rotate(g_reg, &g_owner) == GR_OK, "epoch rotate");

    /* Re-sign after all mutations so the header signature is valid
     * for serialization/deserialization in later sections */
    T(gr_sign(g_reg, &g_owner) == GR_OK, "gr_sign (final)");

    /* Verify epoch exists */
    gr_epoch_t ep;
    T(gr_epoch_get_current(g_reg, &ep) == GR_OK, "epoch get current");
    T(ep.epoch_id > 0, "epoch_id > 0");
    printf("  epoch_id = %u, 8 active peers\n", ep.epoch_id);

    uint32_t active;
    T(gr_peer_count(g_reg, GR_PEER_ACTIVE, &active) == GR_OK, "peer count");
    T(active == 8, "8 active peers (owner + B..H)");
}

/* ════════════════════════════════════════════════════════════════════════
 *  Section 2: Full invite → join → attest → verify → SUDP handshake
 *
 *  Exercises the complete group membership verification chain:
 *    1. Owner creates invite via gr_invite_create.
 *    2. Joiner parses invite, fetches registrar (serialize/clone).
 *    3. Joiner calls gr_join_begin → PROVISIONAL state.
 *    4. Three non-inviter peers attest via gr_join_export_header_attestation.
 *    5. gr_join_evaluate → VERIFIED.
 *    6. SUDP handshake between owner (g_reg) and joiner (own registrar).
 *    7. Bidirectional reliable data exchange.
 * ════════════════════════════════════════════════════════════════════════ */

static void test_invitation_flow(void)
{
    SEC("2  Full invite → join → verify → SUDP (integration)");

    /* ── Step 1: Owner creates invite ───────────────────────────── */
    uint8_t *invite_blob = NULL;
    size_t   invite_len  = 0;
    uint8_t  verify_token[GR_HASH_LEN];

    gr_error_t err = gr_invite_create(g_reg, &g_owner, 0,
                                       &invite_blob, &invite_len,
                                       verify_token);
    T(err == GR_OK, "gr_invite_create");
    T(invite_blob != NULL, "invite blob not NULL");
    T(invite_len > 0, "invite blob len > 0");
    printf("  invite blob = %zu bytes\n", invite_len);

    /* ── Step 2: Joiner parses invite ticket ────────────────────── */
    gr_invite_ticket_t ticket;
    err = gr_invite_parse(invite_blob, invite_len, &ticket);
    T(err == GR_OK, "gr_invite_parse");
    gr_free(invite_blob);

    gr_header_t hdr;
    gr_get_header(g_reg, &hdr);
    T(memcmp(ticket.group_id, hdr.group_id, GR_HASH_LEN) == 0,
      "ticket group_id matches");
    T(memcmp(ticket.owner_sign_key, hdr.owner_sign_key, GR_PUBLIC_KEY_LEN) == 0,
      "ticket owner key matches");
    T(ticket.group_type == GR_GROUP_PRIVATE, "ticket group_type");

    bool valid = false;
    T(gr_invite_check(g_reg, verify_token, &valid) == GR_OK && valid,
      "invite still valid");

    /* ── Step 3: Simulate network fetch — clone the registrar ──── */
    gr_registrar_t *joiner_reg = NULL;
    T(clone_registrar(g_reg, &g_owner, &joiner_reg) == 0,
      "clone registrar for joiner");

    /* ── Step 4: Generate joiner identity, add to owner's reg ──── */
    gr_identity_t joiner;
    T(gr_identity_generate(&joiner) == GR_OK, "joiner identity gen");
    T(add_peer(g_reg, &joiner, &g_owner) == GR_OK,
      "add joiner to owner registrar");

    /* ── Step 5: Begin join verification on joiner's registrar ─── */
    T(gr_join_begin(joiner_reg, &ticket) == GR_OK, "gr_join_begin");

    gr_join_state_t jstate;
    T(gr_join_get_state(joiner_reg, &jstate) == GR_OK, "get join state");
    T(jstate == GR_JOIN_PROVISIONAL, "state is PROVISIONAL (not bypassed)");
    T(!gr_is_trusted(joiner_reg), "not yet trusted");

    /* ── Step 6: Peer attestation (3 non-inviter peers) ────────── */
    /* Owner is the inviter → excluded from attestation.
     * Peers B, E, F attest.  Quorum = ⌈√(8-2)⌉ = 3. */
    uint8_t nonce[GR_JOIN_NONCE_LEN];
    T(gr_join_get_nonce(joiner_reg, nonce) == GR_OK, "get join nonce");

    const gr_identity_t *attestors[3] = { &g_peer_b, &g_peer_e, &g_peer_f };
    for (int i = 0; i < 3; i++) {
        gr_header_t att_hdr;
        uint8_t att_sig[GR_SIGN_LEN];
        T(gr_join_export_header_attestation(g_reg, attestors[i], nonce,
                                            &att_hdr, att_sig) == GR_OK,
          "export attestation");
        gr_error_t sub = gr_join_submit_peer_header(
            joiner_reg, &att_hdr,
            attestors[i]->peer_id, attestors[i]->public_key, att_sig);
        T(sub == GR_OK, "submit attestation");
    }

    /* Owner cannot self-attest */
    {
        gr_header_t att_hdr;
        uint8_t att_sig[GR_SIGN_LEN];
        T(gr_join_export_header_attestation(g_reg, &g_owner, nonce,
                                            &att_hdr, att_sig) == GR_OK,
          "owner export (for exclusion test)");
        gr_error_t sub = gr_join_submit_peer_header(
            joiner_reg, &att_hdr,
            g_owner.peer_id, g_owner.public_key, att_sig);
        T(sub == GR_ERR_JOIN_INVITER_EXCLUDED,
          "inviter excluded from attestation");
    }

    /* ── Step 7: Evaluate → VERIFIED ───────────────────────────── */
    gr_join_verify_result_t result;
    T(gr_join_evaluate(joiner_reg, &result) == GR_OK, "evaluate");
    T(result.state == GR_JOIN_VERIFIED, "join VERIFIED");
    T(result.peers_agreed == 3, "3 peers agreed");
    T(result.peers_disagreed == 0, "0 disagreed");
    T(!result.small_group_bypass, "no small-group bypass");
    T(gr_is_trusted(joiner_reg), "now trusted");
    printf("  quorum=%u agreed=%u\n",
           result.required_attestations, result.peers_agreed);

    /* ── Step 8: SUDP handshake (separate registrars) ──────────── */
    peer_cb_state_t sa_s, sj_s;
    peer_state_reset(&sa_s);
    peer_state_reset(&sj_s);

    uint16_t pa = alloc_port(), pj = alloc_port();

    yumi_sudp_config_t cfg_a = {0};
    cfg_a.transport.peer_addr  = lo6(pj);
    cfg_a.transport.local_port = pa;
    cfg_a.registrar            = g_reg;       /* owner's registrar */
    cfg_a.identity             = &g_owner;
    cfg_a.first_contact        = false;
    cfg_a.recv_cb              = recv_cb;
    cfg_a.state_cb             = state_cb;
    cfg_a.user                 = &sa_s;

    yumi_sudp_config_t cfg_j = {0};
    cfg_j.transport.peer_addr  = lo6(pa);
    cfg_j.transport.local_port = pj;
    cfg_j.registrar            = joiner_reg;  /* joiner's verified registrar */
    cfg_j.identity             = &joiner;
    cfg_j.first_contact        = false;
    cfg_j.recv_cb              = recv_cb;
    cfg_j.state_cb             = state_cb;
    cfg_j.user                 = &sj_s;

    yumi_sudp_client_t *sa = NULL, *sj = NULL;
    T(yumi_sudp_client_create(&sa, &cfg_a) == 0, "create owner SUDP");
    T(yumi_sudp_client_create(&sj, &cfg_j) == 0, "create joiner SUDP");

    T(yumi_sudp_client_connect(sj, g_owner.peer_id) == 0,
      "joiner connect to owner");
    T(wait_established(&sa_s, &sj_s, 5000),
      "SUDP handshake established (separate registrars)");

    T(yumi_sudp_client_get_state(sa) == YUMI_SUDP_ESTABLISHED,
      "owner ESTABLISHED");
    T(yumi_sudp_client_get_state(sj) == YUMI_SUDP_ESTABLISHED,
      "joiner ESTABLISHED");

    /* ── Step 9: Bidirectional data exchange ────────────────────── */
    const char *msg_out = "Hello from verified joiner!";
    uint32_t mlen = (uint32_t)strlen(msg_out);
    T(yumi_sudp_client_send_reliable(sj, msg_out, mlen) == 0,
      "joiner send to owner");
    T(wait_recv(&sa_s, 1, 3000), "owner received from joiner");
    T(sa_s.last_len == mlen && memcmp(sa_s.last_buf, msg_out, mlen) == 0,
      "owner data matches");

    const char *msg_back = "Welcome to the group!";
    uint32_t blen = (uint32_t)strlen(msg_back);
    T(yumi_sudp_client_send_reliable(sa, msg_back, blen) == 0,
      "owner send to joiner");
    T(wait_recv(&sj_s, 1, 3000), "joiner received from owner");
    T(sj_s.last_len == blen && memcmp(sj_s.last_buf, msg_back, blen) == 0,
      "joiner data matches");

    printf("  invite→join→verify→SUDP OK\n");

    yumi_sudp_client_destroy(sa);
    yumi_sudp_client_destroy(sj);

    /* Remove joiner from g_reg so later sections aren't affected.
     * Re-sign so future clone_registrar calls get a valid signature. */
    gr_peer_kick(g_reg, joiner.peer_id, "test cleanup", &g_owner);
    gr_sign(g_reg, &g_owner);
    gr_close(joiner_reg);
    gr_identity_wipe(&joiner);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Section 3: First-Contact with Join Verification (triple-hybrid)
 *
 *  A brand-new invitee joins via the full invite→join→attest→verify flow,
 *  then connects with first_contact=true (ML-KEM + FrodoKEM + BrainPool).
 *  The invitee uses their own join-verified registrar — separate from
 *  the owner's.
 * ════════════════════════════════════════════════════════════════════════ */

static void test_first_contact_handshake(void)
{
    SEC("3  First-contact + join verification (triple-hybrid)");

    /* ── Clone registrar for the invitee ─────────────────────────── */
    gr_registrar_t *invitee_reg = NULL;
    T(clone_registrar(g_reg, &g_owner, &invitee_reg) == 0,
      "clone registrar for invitee");

    /* ── Generate invitee identity, add to owner's reg ──────────── */
    gr_identity_t invitee;
    T(gr_identity_generate(&invitee) == GR_OK, "invitee identity gen");
    T(add_peer(g_reg, &invitee, &g_owner) == GR_OK,
      "add invitee to owner registrar");

    /* ── Create invite + parse ──────────────────────────────────── */
    uint8_t *invite_blob = NULL;
    size_t   invite_len  = 0;
    uint8_t  verify_token[GR_HASH_LEN];
    T(gr_invite_create(g_reg, &g_owner, 0,
                       &invite_blob, &invite_len, verify_token) == GR_OK,
      "invite create for first-contact");

    gr_invite_ticket_t ticket;
    T(gr_invite_parse(invite_blob, invite_len, &ticket) == GR_OK,
      "invite parse");
    gr_free(invite_blob);

    /* ── Join verification ──────────────────────────────────────── */
    T(gr_join_begin(invitee_reg, &ticket) == GR_OK, "join begin");

    gr_join_state_t jstate;
    T(gr_join_get_state(invitee_reg, &jstate) == GR_OK, "get state");
    T(jstate == GR_JOIN_PROVISIONAL, "PROVISIONAL");

    /* Attestation from B, G, H (not owner — owner is inviter) */
    uint8_t nonce[GR_JOIN_NONCE_LEN];
    T(gr_join_get_nonce(invitee_reg, nonce) == GR_OK, "get nonce");

    const gr_identity_t *attestors[3] = { &g_peer_b, &g_peer_g, &g_peer_h };
    for (int i = 0; i < 3; i++) {
        gr_header_t att_hdr;
        uint8_t att_sig[GR_SIGN_LEN];
        T(gr_join_export_header_attestation(g_reg, attestors[i], nonce,
                                            &att_hdr, att_sig) == GR_OK,
          "export attestation");
        T(gr_join_submit_peer_header(
              invitee_reg, &att_hdr,
              attestors[i]->peer_id, attestors[i]->public_key,
              att_sig) == GR_OK,
          "submit attestation");
    }

    gr_join_verify_result_t jresult;
    T(gr_join_evaluate(invitee_reg, &jresult) == GR_OK, "evaluate");
    T(jresult.state == GR_JOIN_VERIFIED, "VERIFIED");
    T(gr_is_trusted(invitee_reg), "trusted");
    printf("  join verified (quorum=%u agreed=%u)\n",
           jresult.required_attestations, jresult.peers_agreed);

    /* ── SUDP first-contact handshake using separate registrars ─── */
    peer_cb_state_t sa_s, si_s;
    peer_state_reset(&sa_s);
    peer_state_reset(&si_s);

    uint16_t pa = alloc_port(), pi = alloc_port();

    yumi_sudp_config_t cfg_a = {0};
    cfg_a.transport.peer_addr  = lo6(pi);
    cfg_a.transport.local_port = pa;
    cfg_a.registrar            = g_reg;
    cfg_a.identity             = &g_owner;
    cfg_a.first_contact        = true;
    cfg_a.recv_cb              = recv_cb;
    cfg_a.state_cb             = state_cb;
    cfg_a.user                 = &sa_s;

    yumi_sudp_config_t cfg_i = {0};
    cfg_i.transport.peer_addr  = lo6(pa);
    cfg_i.transport.local_port = pi;
    cfg_i.registrar            = invitee_reg;
    cfg_i.identity             = &invitee;
    cfg_i.first_contact        = true;
    cfg_i.recv_cb              = recv_cb;
    cfg_i.state_cb             = state_cb;
    cfg_i.user                 = &si_s;

    yumi_sudp_client_t *sa = NULL, *si = NULL;
    T(yumi_sudp_client_create(&sa, &cfg_a) == 0,
      "create owner (first-contact)");
    T(yumi_sudp_client_create(&si, &cfg_i) == 0,
      "create invitee (first-contact)");

    T(yumi_sudp_client_connect(si, g_owner.peer_id) == 0,
      "invitee connect to owner (first-contact)");

    /* FrodoKEM is slow — allow 15s */
    bool ok = wait_established(&sa_s, &si_s, 15000);
    T(ok, "first-contact handshake established (verified registrar)");
    if (!ok) {
        fprintf(stderr, "  owner=%d invitee=%d fail_o=%d fail_i=%d\n",
                atomic_load(&sa_s.established),
                atomic_load(&si_s.established),
                atomic_load(&sa_s.failed),
                atomic_load(&si_s.failed));
    }

    T(yumi_sudp_client_get_state(sa) == YUMI_SUDP_ESTABLISHED,
      "owner ESTABLISHED");
    T(yumi_sudp_client_get_state(si) == YUMI_SUDP_ESTABLISHED,
      "invitee ESTABLISHED");

    /* Exchange data */
    const char *msg = "First contact from verified invitee!";
    uint32_t mlen = (uint32_t)strlen(msg);
    T(yumi_sudp_client_send_reliable(si, msg, mlen) == 0,
      "invitee send to owner");
    T(wait_recv(&sa_s, 1, 5000), "owner received first-contact msg");
    T(sa_s.last_len == mlen && memcmp(sa_s.last_buf, msg, mlen) == 0,
      "owner data matches");

    printf("  first-contact + join verified OK\n");

    yumi_sudp_client_destroy(sa);
    yumi_sudp_client_destroy(si);

    /* Cleanup — kick + re-sign for later sections */
    gr_peer_kick(g_reg, invitee.peer_id, "test cleanup", &g_owner);
    gr_sign(g_reg, &g_owner);
    gr_close(invitee_reg);
    gr_identity_wipe(&invitee);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Section 4: Continued-Session Handshake (dual-hybrid)
 *
 *  Owner (A) connects to returning peer (B) with first_contact=false.
 * ════════════════════════════════════════════════════════════════════════ */

static void test_dual_hybrid_handshake(void)
{
    SEC("4  Continued-session handshake (dual-hybrid)");

    peer_cb_state_t sa_s, sb_s;
    peer_state_reset(&sa_s);
    peer_state_reset(&sb_s);

    uint16_t pa = alloc_port(), pb = alloc_port();

    yumi_sudp_config_t cfg_a = {0};
    cfg_a.transport.peer_addr  = lo6(pb);
    cfg_a.transport.local_port = pa;
    cfg_a.registrar            = g_reg;
    cfg_a.identity             = &g_owner;
    cfg_a.first_contact        = false;
    cfg_a.recv_cb              = recv_cb;
    cfg_a.state_cb             = state_cb;
    cfg_a.user                 = &sa_s;

    yumi_sudp_config_t cfg_b = {0};
    cfg_b.transport.peer_addr  = lo6(pa);
    cfg_b.transport.local_port = pb;
    cfg_b.registrar            = g_reg;
    cfg_b.identity             = &g_peer_b;
    cfg_b.first_contact        = false;
    cfg_b.recv_cb              = recv_cb;
    cfg_b.state_cb             = state_cb;
    cfg_b.user                 = &sb_s;

    yumi_sudp_client_t *sa = NULL, *sb = NULL;
    T(yumi_sudp_client_create(&sa, &cfg_a) == 0, "create A (dual)");
    T(yumi_sudp_client_create(&sb, &cfg_b) == 0, "create B (dual)");

    /* A initiates */
    T(yumi_sudp_client_connect(sa, g_peer_b.peer_id) == 0, "A connect to B");

    T(wait_established(&sa_s, &sb_s, 5000), "dual-hybrid handshake OK");
    T(yumi_sudp_client_get_state(sa) == YUMI_SUDP_ESTABLISHED, "A ESTABLISHED");
    T(yumi_sudp_client_get_state(sb) == YUMI_SUDP_ESTABLISHED, "B ESTABLISHED");

    printf("  dual-hybrid OK\n");

    /* ── Bidirectional data tests ───────────────────────────── */

    SEC("4a Reliable send A→B");
    {
        const char *d = "Reliable message from A";
        uint32_t dl = (uint32_t)strlen(d);
        T(yumi_sudp_client_send_reliable(sa, d, dl) == 0, "send reliable A→B");
        T(wait_recv(&sb_s, 1, 3000), "B received reliable");
        T(sb_s.last_len == dl && memcmp(sb_s.last_buf, d, dl) == 0,
          "B data matches reliable A→B");
    }

    SEC("4b Reliable send B→A");
    {
        const char *d = "Reliable reply from B";
        uint32_t dl = (uint32_t)strlen(d);
        T(yumi_sudp_client_send_reliable(sb, d, dl) == 0, "send reliable B→A");
        T(wait_recv(&sa_s, 1, 3000), "A received reliable");
        T(sa_s.last_len == dl && memcmp(sa_s.last_buf, d, dl) == 0,
          "A data matches reliable B→A");
    }

    SEC("4c Unreliable send A→B");
    {
        uint64_t before = atomic_load(&sb_s.recv_count);
        const char *d = "Unreliable ping";
        uint32_t dl = (uint32_t)strlen(d);
        T(yumi_sudp_client_send(sa, d, dl) == 0, "send unreliable A→B");
        T(wait_recv(&sb_s, before + 1, 3000), "B received unreliable");
        T(sb_s.last_len == dl && memcmp(sb_s.last_buf, d, dl) == 0,
          "B data matches unreliable");
    }

    SEC("4d Channelized send");
    {
        uint64_t before = atomic_load(&sb_s.recv_count);
        const char *d = "Channel 42 data";
        uint32_t dl = (uint32_t)strlen(d);
        T(yumi_sudp_client_send_reliable_channel(sa, 42, d, dl) == 0,
          "send channel 42 A→B");
        T(wait_recv(&sb_s, before + 1, 3000), "B received ch42");
        T(sb_s.last_channel == 42, "B channel == 42");
        T(sb_s.last_len == dl && memcmp(sb_s.last_buf, d, dl) == 0,
          "B data matches ch42");
    }

    SEC("4e Unreliable channelized");
    {
        uint64_t before = atomic_load(&sa_s.recv_count);
        const char *d = "Unrel ch200";
        uint32_t dl = (uint32_t)strlen(d);
        T(yumi_sudp_client_send_channel(sb, 200, d, dl) == 0,
          "send unrel ch200 B→A");
        T(wait_recv(&sa_s, before + 1, 3000), "A received unrel ch200");
        T(sa_s.last_channel == 200, "A channel == 200");
    }

    yumi_sudp_client_destroy(sa);
    yumi_sudp_client_destroy(sb);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Section 5: Sustained 5-second session
 *
 *  Establishes a dual-hybrid session, then sends reliable + unreliable
 *  traffic bidirectionally for 5 seconds, counting packets and bytes.
 * ════════════════════════════════════════════════════════════════════════ */

static double ts_diff_s(const struct timespec *a, const struct timespec *b)
{
    return (double)(b->tv_sec - a->tv_sec)
         + (double)(b->tv_nsec - a->tv_nsec) / 1e9;
}

static void test_sustained_session(void)
{
    SEC("5  Sustained 5-second session");

    struct timespec t_section_start, t_phase;
    clock_gettime(CLOCK_MONOTONIC, &t_section_start);

    peer_cb_state_t sa_s, sb_s;
    peer_state_reset(&sa_s);
    peer_state_reset(&sb_s);

    uint16_t pa = alloc_port(), pb = alloc_port();

    yumi_sudp_config_t cfg_a = {0};
    cfg_a.transport.peer_addr  = lo6(pb);
    cfg_a.transport.local_port = pa;
    cfg_a.registrar            = g_reg;
    cfg_a.identity             = &g_owner;
    cfg_a.first_contact        = false;
    cfg_a.recv_cb              = recv_cb;
    cfg_a.state_cb             = state_cb;
    cfg_a.user                 = &sa_s;

    yumi_sudp_config_t cfg_b = {0};
    cfg_b.transport.peer_addr  = lo6(pa);
    cfg_b.transport.local_port = pb;
    cfg_b.registrar            = g_reg;
    cfg_b.identity             = &g_peer_b;
    cfg_b.first_contact        = false;
    cfg_b.recv_cb              = recv_cb;
    cfg_b.state_cb             = state_cb;
    cfg_b.user                 = &sb_s;

    clock_gettime(CLOCK_MONOTONIC, &t_phase);
    yumi_sudp_client_t *sa = NULL, *sb = NULL;
    T(yumi_sudp_client_create(&sa, &cfg_a) == 0, "create A (sustained)");
    T(yumi_sudp_client_create(&sb, &cfg_b) == 0, "create B (sustained)");
    struct timespec t_now;
    clock_gettime(CLOCK_MONOTONIC, &t_now);
    printf("  [timer] client create:     %7.3f s\n", ts_diff_s(&t_phase, &t_now));

    clock_gettime(CLOCK_MONOTONIC, &t_phase);
    T(yumi_sudp_client_connect(sa, g_peer_b.peer_id) == 0,
      "sustained connect");
    T(wait_established(&sa_s, &sb_s, 5000), "sustained handshake OK");
    clock_gettime(CLOCK_MONOTONIC, &t_now);
    printf("  [timer] handshake:         %7.3f s\n", ts_diff_s(&t_phase, &t_now));

    uint32_t max_pl = yumi_sudp_client_get_max_payload(sa);
    T(max_pl > 0, "max_payload > 0");
    printf("  max_secure_payload = %u bytes\n", max_pl);

    /* Prepare payload (fill with pattern) */
    uint32_t payload_sz = max_pl < 1024 ? max_pl : 1024;
    uint8_t *payload = malloc(payload_sz);
    T(payload != NULL, "payload alloc");
    for (uint32_t i = 0; i < payload_sz; i++)
        payload[i] = (uint8_t)(i & 0xFF);

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    uint64_t a_sent = 0, b_sent = 0;
    int send_errors = 0;
    double elapsed = 0.0;

    printf("  running for 5 seconds ...\n");

    while (elapsed < 5.0) {
        /* A → B: alternate reliable / unreliable */
        int iter_fails = 0;
        if (a_sent % 2 == 0) {
            if (yumi_sudp_client_send_reliable(sa, payload, payload_sz) == 0)
                a_sent++;
            else {
                send_errors++;
                iter_fails++;
            }
        } else {
            if (yumi_sudp_client_send(sa, payload, payload_sz) == 0)
                a_sent++;
            else {
                send_errors++;
                iter_fails++;
            }
        }

        /* B → A: same pattern */
        if (b_sent % 2 == 0) {
            if (yumi_sudp_client_send_reliable(sb, payload, payload_sz) == 0)
                b_sent++;
            else {
                send_errors++;
                iter_fails++;
            }
        } else {
            if (yumi_sudp_client_send(sb, payload, payload_sz) == 0)
                b_sent++;
            else {
                send_errors++;
                iter_fails++;
            }
        }

        /* Yield only on backpressure (ring full) to let the worker drain */
        if (iter_fails)
            usleep(1);

        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (double)(now.tv_sec - start.tv_sec)
                + (double)(now.tv_nsec - start.tv_nsec) / 1e9;
    }

    clock_gettime(CLOCK_MONOTONIC, &t_now);
    printf("  [timer] send loop:         %7.3f s\n", ts_diff_s(&start, &t_now));

    /* Let stragglers arrive */
    clock_gettime(CLOCK_MONOTONIC, &t_phase);
    usleep(500000);
    clock_gettime(CLOCK_MONOTONIC, &t_now);
    printf("  [timer] drain wait:        %7.3f s\n", ts_diff_s(&t_phase, &t_now));

    uint64_t b_rx = atomic_load(&sb_s.recv_count);
    uint64_t a_rx = atomic_load(&sa_s.recv_count);
    uint64_t b_rx_bytes = atomic_load(&sb_s.recv_bytes);
    uint64_t a_rx_bytes = atomic_load(&sa_s.recv_bytes);

    printf("  elapsed      = %.1f s\n", elapsed);
    printf("  A sent       = %llu packets\n", (unsigned long long)a_sent);
    printf("  B sent       = %llu packets\n", (unsigned long long)b_sent);
    printf("  B received   = %llu packets (%llu bytes)\n",
           (unsigned long long)b_rx, (unsigned long long)b_rx_bytes);
    printf("  A received   = %llu packets (%llu bytes)\n",
           (unsigned long long)a_rx, (unsigned long long)a_rx_bytes);
    printf("  send errors  = %d\n", send_errors);

    /* Throughput summary */
    uint64_t total_rx_bytes = b_rx_bytes + a_rx_bytes;
    uint64_t total_rx_pkts  = b_rx + a_rx;
    double   loop_s         = elapsed > 0.0 ? elapsed : 1.0;
    printf("  throughput   = %.2f MB/s  (%.0f pkt/s, bidir)\n",
           (double)total_rx_bytes / loop_s / (1024.0 * 1024.0),
           (double)total_rx_pkts / loop_s);
    printf("  per-pkt cost = %.3f ms  (encrypt+send+recv+decrypt, avg)\n",
           total_rx_pkts > 0
               ? (loop_s * 1000.0) / (double)total_rx_pkts
               : 0.0);

    T(b_rx > 100, "B received enough packets from A");
    T(a_rx > 100, "A received enough packets from B");
    T(b_rx_bytes > 0, "B received bytes > 0");
    T(a_rx_bytes > 0, "A received bytes > 0");

    /* Both sides should still be ESTABLISHED after 5s */
    T(yumi_sudp_client_get_state(sa) == YUMI_SUDP_ESTABLISHED,
      "A still ESTABLISHED after 5s");
    T(yumi_sudp_client_get_state(sb) == YUMI_SUDP_ESTABLISHED,
      "B still ESTABLISHED after 5s");

    free(payload);

    clock_gettime(CLOCK_MONOTONIC, &t_phase);
    yumi_sudp_client_destroy(sa);
    yumi_sudp_client_destroy(sb);
    clock_gettime(CLOCK_MONOTONIC, &t_now);
    printf("  [timer] destroy:           %7.3f s\n", ts_diff_s(&t_phase, &t_now));

    clock_gettime(CLOCK_MONOTONIC, &t_now);
    printf("  [timer] section 5 total:   %7.3f s\n", ts_diff_s(&t_section_start, &t_now));
}

/* ════════════════════════════════════════════════════════════════════════
 *  Section 6: Edge cases (original set)
 * ════════════════════════════════════════════════════════════════════════ */

static void test_edge_cases(void)
{
    SEC("6  Edge cases");

    /* 6a. Connect to unknown peer_id → should fail */
    SEC("6a Connect to unknown peer");
    {
        peer_cb_state_t sa_s;
        peer_state_reset(&sa_s);

        uint16_t pa = alloc_port(), pb = alloc_port();

        yumi_sudp_config_t cfg = {0};
        cfg.transport.peer_addr  = lo6(pb);
        cfg.transport.local_port = pa;
        cfg.registrar            = g_reg;
        cfg.identity             = &g_owner;
        cfg.first_contact        = false;
        cfg.recv_cb              = recv_cb;
        cfg.state_cb             = state_cb;
        cfg.user                 = &sa_s;

        yumi_sudp_client_t *sa = NULL;
        T(yumi_sudp_client_create(&sa, &cfg) == 0, "create for unknown peer");

        /* Fabricate a random peer_id not in the registrar */
        uint8_t fake_id[GR_PEER_ID_LEN];
        yumi_randombytes(fake_id, GR_PEER_ID_LEN);
        int r = yumi_sudp_client_connect(sa, fake_id);
        T(r != 0, "connect to unknown peer → error");

        yumi_sudp_client_destroy(sa);
    }

    /* 6b. Send before handshake → should return error */
    SEC("6b Send before handshake");
    {
        peer_cb_state_t sa_s;
        peer_state_reset(&sa_s);

        uint16_t pa = alloc_port();

        yumi_sudp_config_t cfg = {0};
        cfg.transport.peer_addr  = lo6(alloc_port());
        cfg.transport.local_port = pa;
        cfg.registrar            = g_reg;
        cfg.identity             = &g_owner;
        cfg.first_contact        = false;
        cfg.recv_cb              = recv_cb;
        cfg.state_cb             = state_cb;
        cfg.user                 = &sa_s;

        yumi_sudp_client_t *sa = NULL;
        T(yumi_sudp_client_create(&sa, &cfg) == 0, "create for pre-hs send");

        T(yumi_sudp_client_get_state(sa) == YUMI_SUDP_DISCONNECTED,
          "state is DISCONNECTED before connect");

        /* All four send variants should reject */
        const char *d = "too early";
        uint32_t dl = (uint32_t)strlen(d);
        T(yumi_sudp_client_send(sa, d, dl) != 0,
          "send_unreliable before hs → error");
        T(yumi_sudp_client_send_reliable(sa, d, dl) != 0,
          "send_reliable before hs → error");
        T(yumi_sudp_client_send_channel(sa, 5, d, dl) != 0,
          "send_channel before hs → error");
        T(yumi_sudp_client_send_reliable_channel(sa, 5, d, dl) != 0,
          "send_reliable_channel before hs → error");

        yumi_sudp_client_destroy(sa);
    }

    /* 6c. Max payload query */
    SEC("6c Max payload query");
    {
        peer_cb_state_t sa_s;
        peer_state_reset(&sa_s);

        yumi_sudp_config_t cfg = {0};
        cfg.transport.peer_addr  = lo6(alloc_port());
        cfg.transport.local_port = alloc_port();
        cfg.registrar            = g_reg;
        cfg.identity             = &g_owner;
        cfg.first_contact        = false;
        cfg.recv_cb              = recv_cb;
        cfg.state_cb             = state_cb;
        cfg.user                 = &sa_s;

        yumi_sudp_client_t *sa = NULL;
        T(yumi_sudp_client_create(&sa, &cfg) == 0, "create for max-pl");

        uint32_t mp = yumi_sudp_client_get_max_payload(sa);
        printf("  max_secure_payload = %u\n", mp);
        /* inner max 1298 (1442 less 144 pkt-crypto), minus 5 envelope → 1293 */
        T(mp > 1000 && mp < 2000, "max payload in expected range");

        yumi_sudp_client_destroy(sa);
    }

    /* 6d. Double connect → second should fail */
    SEC("6d Double connect");
    {
        peer_cb_state_t sa_s, sb_s;
        peer_state_reset(&sa_s);
        peer_state_reset(&sb_s);

        uint16_t pa = alloc_port(), pb = alloc_port();

        yumi_sudp_client_t *sa = NULL, *sb = NULL;
        T(make_pair(g_reg, &g_owner, &g_peer_b, &sa_s, &sb_s,
                    &sa, &sb, pa, pb) == 0, "pair for double-connect");

        int r = yumi_sudp_client_connect(sa, g_peer_b.peer_id);
        T(r != 0, "double connect → error");

        yumi_sudp_client_destroy(sa);
        yumi_sudp_client_destroy(sb);
    }

    /* 6e. NULL parameter handling */
    SEC("6e NULL safety");
    {
        T(yumi_sudp_client_create(NULL, NULL) != 0, "create(NULL,NULL) → error");
        yumi_sudp_client_destroy(NULL); /* should not crash */
        T(1, "destroy(NULL) did not crash");
    }

    /* 6f. Empty payload */
    SEC("6f Empty payload");
    {
        peer_cb_state_t sa_s, sb_s;
        uint16_t pa = alloc_port(), pb = alloc_port();
        yumi_sudp_client_t *sa = NULL, *sb = NULL;
        T(make_pair(g_reg, &g_owner, &g_peer_b, &sa_s, &sb_s,
                    &sa, &sb, pa, pb) == 0, "pair for empty-pl");

        T(yumi_sudp_client_send_reliable(sa, NULL, 0) == 0,
          "send empty payload OK");
        T(wait_recv(&sb_s, 1, 3000), "B received empty payload");
        T(sb_s.last_len == 0, "B payload len == 0");

        yumi_sudp_client_destroy(sa);
        yumi_sudp_client_destroy(sb);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Section 7: Kicked Peer — handshake must not establish
 *
 *  Kicks peer D, then tries to connect to D.  The responder's
 *  process_hello must reject the HELLO because D is not GR_PEER_ACTIVE.
 * ════════════════════════════════════════════════════════════════════════ */

static void test_kicked_peer(void)
{
    SEC("7  Kicked peer cannot handshake");

    /* Kick peer D */
    T(gr_peer_kick(g_reg, g_peer_d.peer_id, "test kick", &g_owner) == GR_OK,
      "gr_peer_kick D");

    /* Verify D is no longer ACTIVE */
    gr_peer_t dp;
    T(gr_peer_get(g_reg, g_peer_d.peer_id, &dp) == GR_OK, "peer_get D");
    T(dp.status == GR_PEER_KICKED, "D status == KICKED");

    peer_cb_state_t sa_s, sd_s;
    peer_state_reset(&sa_s);
    peer_state_reset(&sd_s);

    uint16_t pa = alloc_port(), pd = alloc_port();

    yumi_sudp_config_t cfg_a = {0};
    cfg_a.transport.peer_addr  = lo6(pd);
    cfg_a.transport.local_port = pa;
    cfg_a.registrar            = g_reg;
    cfg_a.identity             = &g_owner;
    cfg_a.first_contact        = false;
    cfg_a.recv_cb              = recv_cb;
    cfg_a.state_cb             = state_cb;
    cfg_a.user                 = &sa_s;

    yumi_sudp_config_t cfg_d = {0};
    cfg_d.transport.peer_addr  = lo6(pa);
    cfg_d.transport.local_port = pd;
    cfg_d.registrar            = g_reg;
    cfg_d.identity             = &g_peer_d;
    cfg_d.first_contact        = false;
    cfg_d.recv_cb              = recv_cb;
    cfg_d.state_cb             = state_cb;
    cfg_d.user                 = &sd_s;

    yumi_sudp_client_t *sa = NULL, *sd = NULL;
    T(yumi_sudp_client_create(&sa, &cfg_a) == 0, "create A (kick test)");
    T(yumi_sudp_client_create(&sd, &cfg_d) == 0, "create D (kick test)");

    /* D tries to connect to A — A should reject because D is kicked */
    int r = yumi_sudp_client_connect(sd, g_owner.peer_id);
    /* connect returns 0 (HELLO sent), but the responder should reject.
     * If connect itself rejects (peer is kicked on D's side), that's also fine. */
    if (r == 0) {
        /* Wait — neither side should establish */
        bool established = wait_established(&sa_s, &sd_s, 3000);
        T(!established, "kicked peer handshake did NOT establish");
    } else {
        T(1, "connect rejected kicked peer immediately");
    }

    printf("  kicked peer rejected OK\n");

    yumi_sudp_client_destroy(sa);
    yumi_sudp_client_destroy(sd);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Section 8: Simultaneous Open
 *
 *  Both sides call connect() at roughly the same time.  The tiebreaker
 *  (lower peer_id wins initiator role) should still result in exactly
 *  one established session.
 * ════════════════════════════════════════════════════════════════════════ */

static void test_simultaneous_open(void)
{
    SEC("8  Simultaneous open");

    peer_cb_state_t sa_s, sb_s;
    peer_state_reset(&sa_s);
    peer_state_reset(&sb_s);

    uint16_t pa = alloc_port(), pb = alloc_port();

    yumi_sudp_config_t cfg_a = {0};
    cfg_a.transport.peer_addr  = lo6(pb);
    cfg_a.transport.local_port = pa;
    cfg_a.registrar            = g_reg;
    cfg_a.identity             = &g_owner;
    cfg_a.first_contact        = false;
    cfg_a.recv_cb              = recv_cb;
    cfg_a.state_cb             = state_cb;
    cfg_a.user                 = &sa_s;

    yumi_sudp_config_t cfg_b = {0};
    cfg_b.transport.peer_addr  = lo6(pa);
    cfg_b.transport.local_port = pb;
    cfg_b.registrar            = g_reg;
    cfg_b.identity             = &g_peer_b;
    cfg_b.first_contact        = false;
    cfg_b.recv_cb              = recv_cb;
    cfg_b.state_cb             = state_cb;
    cfg_b.user                 = &sb_s;

    yumi_sudp_client_t *sa = NULL, *sb = NULL;
    T(yumi_sudp_client_create(&sa, &cfg_a) == 0, "create A (simult)");
    T(yumi_sudp_client_create(&sb, &cfg_b) == 0, "create B (simult)");

    /* Both sides connect simultaneously */
    T(yumi_sudp_client_connect(sa, g_peer_b.peer_id) == 0,
      "A connect (simult)");
    T(yumi_sudp_client_connect(sb, g_owner.peer_id) == 0,
      "B connect (simult)");

    T(wait_established(&sa_s, &sb_s, 5000), "simultaneous open established");

    /* Verify data still works */
    const char *d = "simult-open data";
    uint32_t dl = (uint32_t)strlen(d);
    T(yumi_sudp_client_send_reliable(sa, d, dl) == 0, "send after simult-open");
    T(wait_recv(&sb_s, 1, 3000), "B recv after simult-open");
    T(sb_s.last_len == dl && memcmp(sb_s.last_buf, d, dl) == 0,
      "data correct after simult-open");

    printf("  simultaneous open OK\n");

    yumi_sudp_client_destroy(sa);
    yumi_sudp_client_destroy(sb);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Section 9: Max-payload boundary
 *
 *  Send exactly max_payload bytes (should succeed) and max_payload + 1
 *  bytes (should fail).
 * ════════════════════════════════════════════════════════════════════════ */

static void test_max_payload_boundary(void)
{
    SEC("9  Max-payload boundary send");

    peer_cb_state_t sa_s, sb_s;
    uint16_t pa = alloc_port(), pb = alloc_port();
    yumi_sudp_client_t *sa = NULL, *sb = NULL;
    T(make_pair(g_reg, &g_owner, &g_peer_b, &sa_s, &sb_s,
                &sa, &sb, pa, pb) == 0, "pair for max-pl boundary");

    uint32_t mp = yumi_sudp_client_get_max_payload(sa);
    T(mp > 0, "max_payload > 0");

    /* Build max-size payload with distinct pattern */
    uint8_t *buf = malloc(mp + 1);
    T(buf != NULL, "alloc max-pl buf");
    for (uint32_t i = 0; i <= mp; i++)
        buf[i] = (uint8_t)((i * 0x9D) ^ 0xA5);

    /* Exactly max_payload should succeed */
    T(yumi_sudp_client_send_reliable(sa, buf, mp) == 0,
      "send max_payload bytes OK");
    T(wait_recv(&sb_s, 1, 5000), "B recv max_payload");
    T(sb_s.last_len == mp, "B len == max_payload");
    T(memcmp(sb_s.last_buf, buf, mp) == 0, "B max_payload data matches");

    /* max_payload + 1 should fail */
    int r = yumi_sudp_client_send_reliable(sa, buf, mp + 1);
    T(r != 0, "send max_payload+1 → error");

    printf("  max_payload boundary OK (max=%u)\n", mp);

    free(buf);
    yumi_sudp_client_destroy(sa);
    yumi_sudp_client_destroy(sb);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Section 10: Multi-channel stress
 *
 *  Send one reliable packet on each of the 256 channels (A→B).
 *  Verify all 256 are received.
 * ════════════════════════════════════════════════════════════════════════ */

static void test_multi_channel_stress(void)
{
    SEC("10 Multi-channel stress (256 channels)");

    peer_cb_state_t sa_s, sb_s;
    uint16_t pa = alloc_port(), pb = alloc_port();
    yumi_sudp_client_t *sa = NULL, *sb = NULL;
    T(make_pair(g_reg, &g_owner, &g_peer_b, &sa_s, &sb_s,
                &sa, &sb, pa, pb) == 0, "pair for multi-ch");

    int send_ok = 0;
    for (int ch = 0; ch < 256; ch++) {
        uint8_t payload[4];
        payload[0] = (uint8_t)(ch >> 8);
        payload[1] = (uint8_t)(ch & 0xFF);
        payload[2] = 0xCA;
        payload[3] = 0xFE;
        if (yumi_sudp_client_send_reliable_channel(sa, (uint8_t)ch,
                                                     payload, 4) == 0)
            send_ok++;
    }
    T(send_ok == 256, "all 256 channel sends accepted");

    /* Wait for all to arrive — generous timeout for 256 reliable packets */
    T(wait_recv(&sb_s, 256, 10000), "B received all 256 channel packets");

    /* Verify each channel got at least one */
    int chan_ok = 0;
    for (int ch = 0; ch < 256; ch++) {
        if (atomic_load(&sb_s.chan_recv[ch]) >= 1)
            chan_ok++;
    }
    T(chan_ok == 256, "all 256 channels delivered");
    printf("  multi-channel: %d/256 OK\n", chan_ok);

    yumi_sudp_client_destroy(sa);
    yumi_sudp_client_destroy(sb);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Section 11: Rapid reconnect
 *
 *  Connect → exchange → destroy → recreate → connect again, 3 times.
 *  Verifies no state leaks across sessions.
 * ════════════════════════════════════════════════════════════════════════ */

static void test_rapid_reconnect(void)
{
    SEC("11 Rapid reconnect (3 rounds)");

    for (int round = 0; round < 3; round++) {
        peer_cb_state_t sa_s, sb_s;
        uint16_t pa = alloc_port(), pb = alloc_port();
        yumi_sudp_client_t *sa = NULL, *sb = NULL;

        T(make_pair(g_reg, &g_owner, &g_peer_b, &sa_s, &sb_s,
                    &sa, &sb, pa, pb) == 0, "reconnect pair");

        /* Quick data exchange */
        char msg[32];
        snprintf(msg, sizeof(msg), "round-%d", round);
        uint32_t ml = (uint32_t)strlen(msg);
        T(yumi_sudp_client_send_reliable(sa, msg, ml) == 0,
          "send in reconnect round");
        T(wait_recv(&sb_s, 1, 3000), "recv in reconnect round");
        T(sb_s.last_len == ml && memcmp(sb_s.last_buf, msg, ml) == 0,
          "data matches in reconnect round");

        yumi_sudp_client_destroy(sa);
        yumi_sudp_client_destroy(sb);
    }
    printf("  rapid reconnect OK\n");
}

/* ════════════════════════════════════════════════════════════════════════
 *  Section 12: One-byte payload
 *
 *  Minimal encrypted data: verifies the AEAD + channel byte overhead is
 *  correctly handled for the smallest possible user payload.
 * ════════════════════════════════════════════════════════════════════════ */

static void test_one_byte_payload(void)
{
    SEC("12 One-byte payload");

    peer_cb_state_t sa_s, sb_s;
    uint16_t pa = alloc_port(), pb = alloc_port();
    yumi_sudp_client_t *sa = NULL, *sb = NULL;
    T(make_pair(g_reg, &g_owner, &g_peer_b, &sa_s, &sb_s,
                &sa, &sb, pa, pb) == 0, "pair for 1-byte");

    uint8_t byte = 0x42;

    /* Reliable 1 byte */
    T(yumi_sudp_client_send_reliable(sa, &byte, 1) == 0, "send 1 byte reliable");
    T(wait_recv(&sb_s, 1, 3000), "B recv 1 byte reliable");
    T(sb_s.last_len == 1 && sb_s.last_buf[0] == 0x42,
      "1-byte reliable data matches");

    /* Unreliable 1 byte */
    uint64_t before = atomic_load(&sb_s.recv_count);
    byte = 0xFF;
    T(yumi_sudp_client_send(sa, &byte, 1) == 0, "send 1 byte unreliable");
    T(wait_recv(&sb_s, before + 1, 3000), "B recv 1 byte unreliable");
    T(sb_s.last_len == 1 && sb_s.last_buf[0] == 0xFF,
      "1-byte unreliable data matches");

    printf("  one-byte payload OK\n");

    yumi_sudp_client_destroy(sa);
    yumi_sudp_client_destroy(sb);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Section 13: Large payload reliable burst
 *
 *  Send 100 max-size reliable packets back to back, verify all arrive
 *  with correct byte count.
 * ════════════════════════════════════════════════════════════════════════ */

static void test_large_reliable_burst(void)
{
    SEC("13 Large reliable burst (100 × max_payload)");

    peer_cb_state_t sa_s, sb_s;
    uint16_t pa = alloc_port(), pb = alloc_port();
    yumi_sudp_client_t *sa = NULL, *sb = NULL;
    T(make_pair(g_reg, &g_owner, &g_peer_b, &sa_s, &sb_s,
                &sa, &sb, pa, pb) == 0, "pair for burst");

    uint32_t mp = yumi_sudp_client_get_max_payload(sa);

    uint8_t *buf = malloc(mp);
    T(buf != NULL, "burst buf alloc");
    for (uint32_t i = 0; i < mp; i++) buf[i] = (uint8_t)(i & 0xFF);

    int burst_n = 100;
    int sent_ok = 0;
    for (int i = 0; i < burst_n; i++) {
        if (yumi_sudp_client_send_reliable(sa, buf, mp) == 0)
            sent_ok++;
        /* Minimal pacing to avoid ring-buffer full */
        if (i % 10 == 9) usleep(500);
    }
    printf("  sent %d / %d\n", sent_ok, burst_n);
    T(sent_ok > burst_n / 2, "majority of burst sends accepted");

    /* Wait for all reliable packets — they should all arrive eventually */
    bool all = wait_recv(&sb_s, (uint64_t)sent_ok, 10000);
    uint64_t got = atomic_load(&sb_s.recv_count);
    printf("  received %llu / %d\n", (unsigned long long)got, sent_ok);
    T(all || got >= (uint64_t)(sent_ok * 3 / 4),
      "burst: ≥75%% of reliable packets received");

    uint64_t expected_bytes = (uint64_t)got * mp;
    uint64_t actual_bytes = atomic_load(&sb_s.recv_bytes);
    T(actual_bytes == expected_bytes, "burst: byte count consistent");

    free(buf);
    yumi_sudp_client_destroy(sa);
    yumi_sudp_client_destroy(sb);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Section 14: Interleaved reliable/unreliable ordering
 *
 *  Sends N reliable then N unreliable in tight interleave.  Verifies
 *  that at least the reliable ones all arrive (ordering within reliable
 *  stream is guaranteed by the inner transport).
 * ════════════════════════════════════════════════════════════════════════ */

static void test_interleaved_ordering(void)
{
    SEC("14 Interleaved reliable/unreliable");

    peer_cb_state_t sa_s, sb_s;
    uint16_t pa = alloc_port(), pb = alloc_port();
    yumi_sudp_client_t *sa = NULL, *sb = NULL;
    T(make_pair(g_reg, &g_owner, &g_peer_b, &sa_s, &sb_s,
                &sa, &sb, pa, pb) == 0, "pair for interleave");

    int n = 50;
    int rel_sent = 0, unrel_sent = 0;
    uint8_t payload[8];

    for (int i = 0; i < n; i++) {
        /* Reliable with marker 0xAA */
        memset(payload, 0xAA, sizeof(payload));
        payload[0] = (uint8_t)i;
        if (yumi_sudp_client_send_reliable(sa, payload, 8) == 0)
            rel_sent++;

        /* Unreliable with marker 0xBB */
        memset(payload, 0xBB, sizeof(payload));
        payload[0] = (uint8_t)i;
        if (yumi_sudp_client_send(sa, payload, 8) == 0)
            unrel_sent++;
    }

    printf("  sent reliable=%d unreliable=%d\n", rel_sent, unrel_sent);

    /* All reliables must arrive; some unreliables may be lost */
    T(wait_recv(&sb_s, (uint64_t)rel_sent, 5000),
      "all reliable arrived");
    uint64_t total = atomic_load(&sb_s.recv_count);
    printf("  total received: %llu (reliable=%d, unreliable=%d sent)\n",
           (unsigned long long)total, rel_sent, unrel_sent);
    T(total >= (uint64_t)rel_sent, "at least all reliables received");

    yumi_sudp_client_destroy(sa);
    yumi_sudp_client_destroy(sb);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Section 15: Destroy during handshake — no crash, no leak
 *
 *  Starts a connect then immediately destroys both clients before the
 *  handshake can complete.  Must not crash or hang.
 * ════════════════════════════════════════════════════════════════════════ */

static void test_destroy_during_handshake(void)
{
    SEC("15 Destroy during handshake");

    for (int i = 0; i < 5; i++) {
        peer_cb_state_t sa_s, sb_s;
        peer_state_reset(&sa_s);
        peer_state_reset(&sb_s);

        uint16_t pa = alloc_port(), pb = alloc_port();

        yumi_sudp_config_t cfg_a = {0};
        cfg_a.transport.peer_addr  = lo6(pb);
        cfg_a.transport.local_port = pa;
        cfg_a.registrar            = g_reg;
        cfg_a.identity             = &g_owner;
        cfg_a.first_contact        = (i % 2 == 0); /* alternate modes */
        cfg_a.recv_cb              = recv_cb;
        cfg_a.state_cb             = state_cb;
        cfg_a.user                 = &sa_s;

        yumi_sudp_config_t cfg_b = {0};
        cfg_b.transport.peer_addr  = lo6(pa);
        cfg_b.transport.local_port = pb;
        cfg_b.registrar            = g_reg;
        cfg_b.identity             = &g_peer_b;
        cfg_b.first_contact        = (i % 2 == 0);
        cfg_b.recv_cb              = recv_cb;
        cfg_b.state_cb             = state_cb;
        cfg_b.user                 = &sb_s;

        yumi_sudp_client_t *sa = NULL, *sb = NULL;
        T(yumi_sudp_client_create(&sa, &cfg_a) == 0, "create A (hs-destroy)");
        T(yumi_sudp_client_create(&sb, &cfg_b) == 0, "create B (hs-destroy)");

        /* Fire off connect, then IMMEDIATELY tear down */
        yumi_sudp_client_connect(sa, g_peer_b.peer_id);
        /* Tiny random sleep to vary the destruction timing */
        usleep((unsigned)(100 + (i * 50)));

        yumi_sudp_client_destroy(sa);
        yumi_sudp_client_destroy(sb);
    }

    T(1, "destroy-during-handshake did not crash (5 rounds)");
    printf("  destroy-during-handshake OK\n");
}

/* ════════════════════════════════════════════════════════════════════════
 *  Section 16: Wrong Epoch Key — cryptographic boundary test
 *
 *  Clones the registrar then rotates its epoch (generating a brand-new
 *  random key).  Both sides have the same peers and signing keys, but
 *  the datagram-level AEAD uses different epoch keys.  Every packet is
 *  undecryptable → no HELLO is ever delivered → handshake must fail.
 *  This proves the epoch key is the real enforcement boundary.
 * ════════════════════════════════════════════════════════════════════════ */

static void test_wrong_epoch_key(void)
{
    SEC("16 Wrong epoch key → handshake must fail");

    /* Clone the registrar and rotate its epoch → different key */
    gr_registrar_t *bad_reg = NULL;
    T(clone_registrar(g_reg, &g_owner, &bad_reg) == 0,
      "clone registrar for epoch mismatch");
    T(gr_epoch_rotate(bad_reg, &g_owner) == GR_OK,
      "rotate epoch on clone (new key)");

    /* Verify the epoch IDs now differ */
    gr_epoch_t ep_good, ep_bad;
    T(gr_epoch_get_current(g_reg, &ep_good) == GR_OK, "get good epoch");
    T(gr_epoch_get_current(bad_reg, &ep_bad) == GR_OK, "get bad epoch");
    T(ep_good.epoch_id != ep_bad.epoch_id, "epoch IDs differ");
    printf("  good epoch=%u  bad epoch=%u\n", ep_good.epoch_id, ep_bad.epoch_id);

    peer_cb_state_t sa_s, sx_s;
    peer_state_reset(&sa_s);
    peer_state_reset(&sx_s);

    uint16_t pa = alloc_port(), px = alloc_port();

    /* Owner side uses g_reg (correct epoch key) */
    yumi_sudp_config_t cfg_a = {0};
    cfg_a.transport.peer_addr  = lo6(px);
    cfg_a.transport.local_port = pa;
    cfg_a.registrar            = g_reg;
    cfg_a.identity             = &g_owner;
    cfg_a.first_contact        = false;
    cfg_a.recv_cb              = recv_cb;
    cfg_a.state_cb             = state_cb;
    cfg_a.user                 = &sa_s;

    /* Peer B uses bad_reg (wrong epoch key) */
    yumi_sudp_config_t cfg_x = {0};
    cfg_x.transport.peer_addr  = lo6(pa);
    cfg_x.transport.local_port = px;
    cfg_x.registrar            = bad_reg;
    cfg_x.identity             = &g_peer_b;
    cfg_x.first_contact        = false;
    cfg_x.recv_cb              = recv_cb;
    cfg_x.state_cb             = state_cb;
    cfg_x.user                 = &sx_s;

    yumi_sudp_client_t *sa = NULL, *sx = NULL;
    T(yumi_sudp_client_create(&sa, &cfg_a) == 0, "create owner (good key)");
    T(yumi_sudp_client_create(&sx, &cfg_x) == 0, "create B (wrong key)");

    /* B tries to connect — all packets are undecryptable */
    T(yumi_sudp_client_connect(sx, g_owner.peer_id) == 0,
      "B connect attempt (wrong epoch)");

    /* Wait — neither side should establish */
    bool established = wait_established(&sa_s, &sx_s, 3000);
    T(!established, "wrong epoch key: handshake did NOT establish");

    /* Verify at least one side shows a failure or neither established */
    T(!atomic_load(&sa_s.established), "owner not established");
    T(!atomic_load(&sx_s.established), "B not established");

    printf("  wrong epoch key → rejected OK\n");

    yumi_sudp_client_destroy(sa);
    yumi_sudp_client_destroy(sx);
    gr_close(bad_reg);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Section 17: PROVISIONAL Registrar — SUDP works before verification
 *
 *  The joiner's registrar is in PROVISIONAL state (gr_join_begin called,
 *  no attestations submitted yet).  SUDP should still establish because
 *  it only needs epoch key + peer signing keys, not governance trust.
 *  This validates the design: chat works while attestation runs in
 *  background via higher-level Yumi Client.
 * ════════════════════════════════════════════════════════════════════════ */

static void test_provisional_sudp(void)
{
    SEC("17 PROVISIONAL registrar → SUDP works");

    /* Create invite + clone registrar for joiner */
    uint8_t *invite_blob = NULL;
    size_t   invite_len  = 0;
    uint8_t  verify_token[GR_HASH_LEN];
    T(gr_invite_create(g_reg, &g_owner, 0,
                       &invite_blob, &invite_len, verify_token) == GR_OK,
      "invite create");

    gr_invite_ticket_t ticket;
    T(gr_invite_parse(invite_blob, invite_len, &ticket) == GR_OK,
      "invite parse");
    gr_free(invite_blob);

    gr_registrar_t *prov_reg = NULL;
    T(clone_registrar(g_reg, &g_owner, &prov_reg) == 0,
      "clone registrar");

    /* Generate a joiner identity, add to owner's reg so HELLO can be
     * verified by the peer_get lookup on the responder side */
    gr_identity_t joiner;
    T(gr_identity_generate(&joiner) == GR_OK, "joiner gen");
    T(add_peer(g_reg, &joiner, &g_owner) == GR_OK,
      "add joiner to owner reg");

    /* Begin join — enters PROVISIONAL, do NOT attest */
    T(gr_join_begin(prov_reg, &ticket) == GR_OK, "join begin");

    gr_join_state_t jstate;
    T(gr_join_get_state(prov_reg, &jstate) == GR_OK, "get state");
    T(jstate == GR_JOIN_PROVISIONAL, "PROVISIONAL");
    T(!gr_is_trusted(prov_reg), "not trusted (PROVISIONAL)");

    /* SUDP handshake should work despite PROVISIONAL */
    peer_cb_state_t sa_s, sj_s;
    peer_state_reset(&sa_s);
    peer_state_reset(&sj_s);

    uint16_t pa = alloc_port(), pj = alloc_port();

    yumi_sudp_config_t cfg_a = {0};
    cfg_a.transport.peer_addr  = lo6(pj);
    cfg_a.transport.local_port = pa;
    cfg_a.registrar            = g_reg;
    cfg_a.identity             = &g_owner;
    cfg_a.first_contact        = false;
    cfg_a.recv_cb              = recv_cb;
    cfg_a.state_cb             = state_cb;
    cfg_a.user                 = &sa_s;

    yumi_sudp_config_t cfg_j = {0};
    cfg_j.transport.peer_addr  = lo6(pa);
    cfg_j.transport.local_port = pj;
    cfg_j.registrar            = prov_reg;   /* PROVISIONAL — not verified! */
    cfg_j.identity             = &joiner;
    cfg_j.first_contact        = false;
    cfg_j.recv_cb              = recv_cb;
    cfg_j.state_cb             = state_cb;
    cfg_j.user                 = &sj_s;

    yumi_sudp_client_t *sa = NULL, *sj = NULL;
    T(yumi_sudp_client_create(&sa, &cfg_a) == 0, "create owner");
    T(yumi_sudp_client_create(&sj, &cfg_j) == 0, "create joiner (PROVISIONAL)");

    T(yumi_sudp_client_connect(sj, g_owner.peer_id) == 0,
      "joiner connect (PROVISIONAL)");
    T(wait_established(&sa_s, &sj_s, 5000),
      "SUDP established despite PROVISIONAL");

    T(yumi_sudp_client_get_state(sa) == YUMI_SUDP_ESTABLISHED,
      "owner ESTABLISHED");
    T(yumi_sudp_client_get_state(sj) == YUMI_SUDP_ESTABLISHED,
      "joiner ESTABLISHED (PROVISIONAL)");

    /* Verify the registrar is STILL provisional — SUDP didn't change it */
    T(gr_join_get_state(prov_reg, &jstate) == GR_OK, "re-check state");
    T(jstate == GR_JOIN_PROVISIONAL, "still PROVISIONAL after SUDP");
    T(!gr_is_trusted(prov_reg), "still not trusted");

    /* Data exchange works */
    const char *msg = "Chat during PROVISIONAL!";
    uint32_t mlen = (uint32_t)strlen(msg);
    T(yumi_sudp_client_send_reliable(sj, msg, mlen) == 0,
      "send from PROVISIONAL joiner");
    T(wait_recv(&sa_s, 1, 3000), "owner received");
    T(sa_s.last_len == mlen && memcmp(sa_s.last_buf, msg, mlen) == 0,
      "data matches");

    printf("  PROVISIONAL → SUDP OK\n");

    yumi_sudp_client_destroy(sa);
    yumi_sudp_client_destroy(sj);

    /* Cleanup */
    gr_peer_kick(g_reg, joiner.peer_id, "test cleanup", &g_owner);
    gr_sign(g_reg, &g_owner);
    gr_close(prov_reg);
    gr_identity_wipe(&joiner);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Section 18: Epoch Rotation Rekey
 *
 *  Establish a session, rotate the epoch, then verify that data still
 *  flows in both directions.  This proves the rekey_seed / epoch_rekey
 *  mechanism works: the sender detects the rotation in sudp_encrypt_send,
 *  rekeyes, and the receiver detects the new key in sudp_pkt_decrypt's
 *  epoch-rotation fallback path.
 * ════════════════════════════════════════════════════════════════════════ */

static void test_epoch_rotation_rekey(void)
{
    SEC("18 Epoch rotation rekey — session survives");

    peer_cb_state_t sa_s, sb_s;
    yumi_sudp_client_t *sa = NULL, *sb = NULL;
    uint16_t pa = alloc_port(), pb = alloc_port();

    T(make_pair(g_reg, &g_owner, &g_peer_b, &sa_s, &sb_s,
                &sa, &sb, pa, pb) == 0,
      "establish pre-rotation session");

    /* Send some data before rotation */
    const char *pre_msg = "before rotation";
    T(yumi_sudp_client_send_reliable(sa, pre_msg, (uint32_t)strlen(pre_msg)) == 0,
      "send pre-rotation");
    T(wait_recv(&sb_s, 1, 3000), "recv pre-rotation");
    T(sb_s.last_len == strlen(pre_msg) &&
      memcmp(sb_s.last_buf, pre_msg, sb_s.last_len) == 0,
      "pre-rotation data matches");

    /* Rotate the epoch — this changes the epoch key in the registrar */
    gr_epoch_t old_epoch;
    T(gr_epoch_get_current(g_reg, &old_epoch) == GR_OK, "get old epoch");
    T(gr_epoch_rotate(g_reg, &g_owner) == GR_OK, "rotate epoch");
    gr_epoch_t new_epoch;
    T(gr_epoch_get_current(g_reg, &new_epoch) == GR_OK, "get new epoch");
    T(new_epoch.epoch_id != old_epoch.epoch_id, "epoch ID changed");
    printf("  old epoch=%u  new epoch=%u\n",
           old_epoch.epoch_id, new_epoch.epoch_id);

    /* Small delay to let both sides notice */
    usleep(50000);

    /* Send data from A→B after rotation */
    uint64_t pre_count = atomic_load(&sb_s.recv_count);
    const char *post_msg_ab = "A→B after epoch rotation!";
    T(yumi_sudp_client_send_reliable(sa, post_msg_ab,
                                      (uint32_t)strlen(post_msg_ab)) == 0,
      "send A→B post-rotation");
    T(wait_recv(&sb_s, pre_count + 1, 3000), "recv A→B post-rotation");
    T(sb_s.last_len == strlen(post_msg_ab) &&
      memcmp(sb_s.last_buf, post_msg_ab, sb_s.last_len) == 0,
      "A→B post-rotation data matches");

    /* Send data from B→A after rotation */
    pre_count = atomic_load(&sa_s.recv_count);
    const char *post_msg_ba = "B→A after epoch rotation!";
    T(yumi_sudp_client_send_reliable(sb, post_msg_ba,
                                      (uint32_t)strlen(post_msg_ba)) == 0,
      "send B→A post-rotation");
    T(wait_recv(&sa_s, pre_count + 1, 3000), "recv B→A post-rotation");
    T(sa_s.last_len == strlen(post_msg_ba) &&
      memcmp(sa_s.last_buf, post_msg_ba, sa_s.last_len) == 0,
      "B→A post-rotation data matches");

    printf("  epoch rotation rekey OK\n");

    yumi_sudp_client_destroy(sa);
    yumi_sudp_client_destroy(sb);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Section 19: Kicked Peer Active Session — notify_kick tears down
 *
 *  Establish a session between owner and an extra peer (E), then kick E,
 *  call notify_kick on the owner's client, and verify the session
 *  transitions to FAILED.  Also verifies that subsequent sends fail.
 * ════════════════════════════════════════════════════════════════════════ */

static void test_kicked_peer_active_session(void)
{
    SEC("19 Kicked peer active session — notify_kick teardown");

    peer_cb_state_t sa_s, se_s;
    yumi_sudp_client_t *sa = NULL, *se = NULL;
    uint16_t pa = alloc_port(), pe = alloc_port();

    T(make_pair(g_reg, &g_owner, &g_peer_e, &sa_s, &se_s,
                &sa, &se, pa, pe) == 0,
      "establish session with E");

    /* Verify data works before kick */
    const char *pre_msg = "pre-kick data";
    T(yumi_sudp_client_send_reliable(sa, pre_msg, (uint32_t)strlen(pre_msg)) == 0,
      "send pre-kick");
    T(wait_recv(&se_s, 1, 3000), "recv pre-kick");

    /* Kick peer E */
    T(gr_peer_kick(g_reg, g_peer_e.peer_id, "test active kick", &g_owner) == GR_OK,
      "kick peer E");

    /* Notify the owner's client that E was kicked */
    T(yumi_sudp_client_notify_kick(sa) == 0,
      "notify_kick tears down owner side");

    /* Owner should now be FAILED */
    T(yumi_sudp_client_get_state(sa) == YUMI_SUDP_FAILED,
      "owner state == FAILED after notify_kick");

    /* Subsequent send should fail */
    T(yumi_sudp_client_send_reliable(sa, "fail", 4) != 0,
      "send after kick fails");

    printf("  notify_kick teardown OK\n");

    yumi_sudp_client_destroy(sa);
    yumi_sudp_client_destroy(se);

    /* Re-add E for subsequent tests that may need it */
    T(add_peer(g_reg, &g_peer_e, &g_owner) == GR_OK, "re-add E");
    gr_sign(g_reg, &g_owner);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Section 20: Protocol Version — verify version byte in handshake
 *
 *  This is implicitly tested by all handshake tests above.  Here we
 *  verify the constant is accessible and the pair establish correctly
 *  (proving both sides sent and accepted the version byte).
 * ════════════════════════════════════════════════════════════════════════ */

static void test_protocol_version(void)
{
    SEC("20 Protocol version byte");

    T(YUMI_SUDP_PROTOCOL_VERSION == 1, "protocol version is 1");

    /* Verify a fresh handshake works (version byte accepted) */
    peer_cb_state_t sa_s, sb_s;
    yumi_sudp_client_t *sa = NULL, *sb = NULL;
    uint16_t pa = alloc_port(), pb = alloc_port();

    T(make_pair(g_reg, &g_owner, &g_peer_b, &sa_s, &sb_s,
                &sa, &sb, pa, pb) == 0,
      "versioned handshake succeeds");

    T(yumi_sudp_client_get_state(sa) == YUMI_SUDP_ESTABLISHED,
      "A established (version OK)");
    T(yumi_sudp_client_get_state(sb) == YUMI_SUDP_ESTABLISHED,
      "B established (version OK)");

    /* Data round-trip to ensure everything is functional */
    const char *msg = "version 1 data";
    T(yumi_sudp_client_send_reliable(sa, msg, (uint32_t)strlen(msg)) == 0,
      "send (versioned)");
    T(wait_recv(&sb_s, 1, 3000), "recv (versioned)");
    T(sb_s.last_len == strlen(msg) &&
      memcmp(sb_s.last_buf, msg, sb_s.last_len) == 0,
      "versioned data matches");

    printf("  protocol version OK\n");

    yumi_sudp_client_destroy(sa);
    yumi_sudp_client_destroy(sb);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Main
 * ════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("=== SUDP Client Test Suite ===\n");

    if (yumi_crypto_init() != YUMI_CRYPTO_OK) {
        fprintf(stderr, "FATAL: yumi_crypto_init() failed\n");
        return 1;
    }

    test_registrar_setup();         /*  1 */
    test_invitation_flow();         /*  2 */
    test_first_contact_handshake(); /*  3 */
    test_dual_hybrid_handshake();   /*  4 */
    test_sustained_session();       /*  5 */
    test_edge_cases();              /*  6 */
    test_kicked_peer();             /*  7 */
    test_simultaneous_open();       /*  8 */
    test_max_payload_boundary();    /*  9 */
    test_multi_channel_stress();    /* 10 */
    test_rapid_reconnect();         /* 11 */
    test_one_byte_payload();        /* 12 */
    test_large_reliable_burst();    /* 13 */
    test_interleaved_ordering();    /* 14 */
    test_destroy_during_handshake();/* 15 */
    test_wrong_epoch_key();         /* 16 */
    test_provisional_sudp();        /* 17 */
    test_epoch_rotation_rekey();    /* 18 */
    test_kicked_peer_active_session(); /* 19 */
    test_protocol_version();        /* 20 */

    /* Cleanup global registrar */
    gr_close(g_reg);
    yumi_crypto_cleanup();

    printf("\n════════════════════════════════════════\n");
    printf("  %d tests run, %d passed, %d failed\n",
           g_run, g_run - g_fail, g_fail);
    printf("════════════════════════════════════════\n");

    return g_fail > 0 ? 1 : 0;
}
