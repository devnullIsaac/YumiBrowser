/*
    Yumi Tests — Join Verification Test Suite
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

/**
 * @file test_join_verify.c
 * @brief Test suite for join verification — 46 tests across 11 sections.
 */

#include "internal.h"
#include "crypto.h"
#include "buf.h"
#include <stdio.h>
#include <string.h>

static int g_run = 0, g_fail = 0;
#define T(c, m) do { g_run++; if (!(c)) { g_fail++; \
    fprintf(stderr, "  FAIL [%s:%d] %s\n", __FILE__, __LINE__, m); } } while(0)
#define SEC(n) fprintf(stdout, "── %s\n", n)

/* ── Helpers ───────────────────────────────────────────────────── */

static void mkid(gr_identity_t *id) {
    gr_identity_generate(id);
}

static gr_registrar_t *mkg(const gr_identity_t *o, const char *n) {
    gr_registrar_t *r = NULL;
    if (gr_create(&r, ":memory:", n, GR_GROUP_PRIVATE, o) != GR_OK) return NULL;
    gr_sign(r, o); return r;
}

static gr_error_t addp(gr_registrar_t *r, const gr_identity_t *p,
                       const gr_identity_t *s) {
    gr_peer_t pp; memset(&pp, 0, sizeof(pp));
    memcpy(pp.peer_id, p->peer_id, GR_PEER_ID_LEN);
    memcpy(pp.kem_pk, p->kem_pk, GR_KEM_PUBLIC_KEY_LEN);
    memcpy(pp.sign_key, p->public_key, GR_PUBLIC_KEY_LEN);
    strncpy(pp.ip, "10.0.0.1", GR_MAX_IP_LEN);
    pp.port = 9000; pp.status = GR_PEER_ACTIVE;
    pp.joined_at = gr_timestamp_ms(); pp.last_seen = pp.joined_at;
    return gr_peer_add(r, &pp, s);
}

static gr_error_t setrole(gr_registrar_t *r, const gr_identity_t *p,
                          uint32_t perms, const gr_identity_t *o, const char *n) {
    uint32_t rid; gr_error_t e = gr_role_add(r, n, perms, o, &rid);
    return e != GR_OK ? e : gr_peer_set_role(r, p->peer_id, rid, o);
}

static void tkt(const gr_registrar_t *r, const gr_identity_t *inv,
                gr_invite_ticket_t *t) {
    memset(t, 0, sizeof(*t)); gr_header_t h; gr_get_header(r, &h);
    memcpy(t->group_id, h.group_id, GR_HASH_LEN);
    strncpy(t->group_name, h.group_name, GR_MAX_NAME_LEN);
    t->group_type = h.group_type;
    memcpy(t->owner_sign_key, h.owner_sign_key, GR_PUBLIC_KEY_LEN);
    memcpy(t->registrar_hash, h.hash, GR_HASH_LEN);
    memcpy(t->inviter_sign_pk, inv->public_key, GR_PUBLIC_KEY_LEN);
    memcpy(t->inviter_kem_pk, inv->kem_pk, GR_KEM_PUBLIC_KEY_LEN);
}

static gr_error_t goprov(gr_registrar_t *r, const gr_identity_t *o) {
    gr_invite_ticket_t t; tkt(r, o, &t); return gr_join_begin(r, &t);
}

static gr_error_t att_ok(gr_registrar_t *j, const gr_registrar_t *p,
                         const gr_identity_t *peer) {
    uint8_t nonce[GR_JOIN_NONCE_LEN];
    gr_error_t e = gr_join_get_nonce(j, nonce); if (e != GR_OK) return e;
    gr_header_t h; uint8_t sig[GR_SIGN_LEN];
    e = gr_join_export_header_attestation(p, peer, nonce, &h, sig);
    if (e != GR_OK) return e;
    return gr_join_submit_peer_header(j, &h, peer->peer_id, peer->public_key, sig);
}

static gr_error_t att_dis(gr_registrar_t *j, const gr_identity_t *peer,
    const uint8_t oid[GR_PEER_ID_LEN], const uint8_t okey[GR_PUBLIC_KEY_LEN],
    const uint8_t gid[GR_HASH_LEN], uint32_t ver) {
    uint8_t nonce[GR_JOIN_NONCE_LEN];
    gr_error_t e = gr_join_get_nonce(j, nonce); if (e != GR_OK) return e;
    gr_header_t h; memset(&h, 0, sizeof(h));
    memcpy(h.group_id, gid, GR_HASH_LEN);
    memcpy(h.owner_id, oid, GR_PEER_ID_LEN);
    memcpy(h.owner_sign_key, okey, GR_PUBLIC_KEY_LEN); h.version = ver;
    uint8_t ad[GR_JOIN_ATTESTATION_LEN]; size_t p2 = 0;
    memcpy(ad+p2, h.group_id, GR_HASH_LEN); p2 += GR_HASH_LEN;
    memcpy(ad+p2, h.owner_id, GR_PEER_ID_LEN); p2 += GR_PEER_ID_LEN;
    memcpy(ad+p2, h.owner_sign_key, GR_PUBLIC_KEY_LEN); p2 += GR_PUBLIC_KEY_LEN;
    uint32_t vle = gr_htole32(ver); memcpy(ad+p2, &vle, 4); p2 += 4;
    memcpy(ad+p2, nonce, GR_JOIN_NONCE_LEN); p2 += GR_JOIN_NONCE_LEN;
    uint8_t sig[GR_SIGN_LEN];
    yumi_mldsa_sign(sig, ad, p2, peer->secret_key);
    return gr_join_submit_peer_header(j, &h, peer->peer_id, peer->public_key, sig);
}

static void add_n(gr_registrar_t *r, gr_identity_t *ps, int n,
                  const gr_identity_t *o) {
    for (int i = 0; i < n; i++) { mkid(&ps[i]); addp(r, &ps[i], o); }
}

static bool cb_yes(const gr_join_verify_result_t *r, void *ud) {
    (void)r; *(bool*)ud = true; return true;
}
static bool cb_no(const gr_join_verify_result_t *r, void *ud) {
    (void)r; *(bool*)ud = true; return false;
}
static bool cb_cnt(const gr_join_verify_result_t *r, void *ud) {
    (void)r; (*(int*)ud)++; return true;
}

/* ═══════════════════════════════════════════════════════════════════
 *  1. Legitimate flow
 * ═══════════════════════════════════════════════════════════════════ */
static void test_legit_join(void) {
    SEC("legit: 5 peers, 3 attest -> VERIFIED");
    gr_identity_t o; mkid(&o); gr_registrar_t *r = mkg(&o, "L1");
    gr_identity_t ps[4]; add_n(r, ps, 4, &o); gr_sign(r, &o);
    T(goprov(r, &o) == GR_OK, "prov");
    att_ok(r, r, &ps[0]); att_ok(r, r, &ps[1]); att_ok(r, r, &ps[2]);
    gr_join_verify_result_t res; gr_join_evaluate(r, &res);
    T(res.state == GR_JOIN_VERIFIED, "VERIFIED");
    T(res.required_attestations == 3, "quorum=3");
    T(gr_is_trusted(r), "trusted"); gr_close(r);
}
static void test_local_trusted(void) {
    SEC("locally created: trusted");
    gr_identity_t o; mkid(&o); gr_registrar_t *r = mkg(&o, "L2");
    T(gr_is_trusted(r), "trusted"); gr_close(r);
}

/* ═══════════════════════════════════════════════════════════════════
 *  2. Doctored registrar
 * ═══════════════════════════════════════════════════════════════════ */
static void test_doc_ticket_owner(void) {
    SEC("doctored ticket: wrong owner -> FAILED");
    gr_identity_t o, a; mkid(&o); mkid(&a);
    gr_registrar_t *r = mkg(&o, "D1"); gr_sign(r, &o);
    gr_invite_ticket_t t; tkt(r, &o, &t);
    memcpy(t.owner_sign_key, a.public_key, GR_PUBLIC_KEY_LEN);
    T(gr_join_begin(r, &t) == GR_ERR_JOIN_FAILED, "FAILED"); gr_close(r);
}
static void test_doc_ticket_gid(void) {
    SEC("doctored ticket: wrong group_id -> FAILED");
    gr_identity_t o; mkid(&o); gr_registrar_t *r = mkg(&o, "D2"); gr_sign(r, &o);
    gr_invite_ticket_t t; tkt(r, &o, &t); t.group_id[0] ^= 0xFF;
    T(gr_join_begin(r, &t) == GR_ERR_JOIN_FAILED, "FAILED"); gr_close(r);
}
static void test_doc_peer_disagrees(void) {
    SEC("doctored reg: peer reports real owner -> FAILED");
    gr_identity_t ro, atk, inv; mkid(&ro); mkid(&atk); mkid(&inv);
    gr_identity_t ps[4];
    gr_registrar_t *canon = mkg(&ro, "D3");
    addp(canon, &atk, &ro); setrole(canon, &atk, GR_PERM_INVITE_MEMBER, &ro, "mod");
    for (int i = 0; i < 4; i++) { mkid(&ps[i]); addp(canon, &ps[i], &ro); }
    addp(canon, &inv, &atk); gr_sign(canon, &ro);
    gr_registrar_t *doc = mkg(&atk, "D3");
    addp(doc, &ro, &atk);
    for (int i = 0; i < 4; i++) addp(doc, &ps[i], &atk);
    addp(doc, &inv, &atk); gr_sign(doc, &atk);
    gr_invite_ticket_t t; tkt(doc, &atk, &t);
    T(gr_join_begin(doc, &t) == GR_OK, "begin");
    gr_header_t dh; gr_get_header(doc, &dh);
    att_dis(doc, &ps[0], ro.peer_id, ro.public_key, dh.group_id, dh.version);
    gr_join_verify_result_t res; gr_join_evaluate(doc, &res);
    T(res.state == GR_JOIN_FAILED, "FAILED");
    T(memcmp(res.dissent_owner_key, ro.public_key, GR_PUBLIC_KEY_LEN)==0, "real owner");
    gr_close(canon); gr_close(doc);
}
static void test_doc_all_disagree(void) {
    SEC("all peers disagree -> FAILED");
    gr_identity_t o; mkid(&o); gr_registrar_t *r = mkg(&o, "D4");
    gr_identity_t ps[4]; add_n(r, ps, 4, &o); gr_sign(r, &o); goprov(r, &o);
    gr_identity_t f; mkid(&f); gr_header_t h; gr_get_header(r, &h);
    att_dis(r, &ps[0], f.peer_id, f.public_key, h.group_id, h.version);
    att_dis(r, &ps[1], f.peer_id, f.public_key, h.group_id, h.version);
    att_dis(r, &ps[2], f.peer_id, f.public_key, h.group_id, h.version);
    gr_join_verify_result_t res; gr_join_evaluate(r, &res);
    T(res.state == GR_JOIN_FAILED, "FAILED"); T(res.peers_disagreed==3, "3 dis");
    gr_close(r);
}
static void test_doc_single_dissenter(void) {
    SEC("1 dissenter among 2 agreeing -> FAILED");
    gr_identity_t o; mkid(&o); gr_registrar_t *r = mkg(&o, "D5");
    gr_identity_t ps[4]; add_n(r, ps, 4, &o); gr_sign(r, &o); goprov(r, &o);
    att_ok(r, r, &ps[0]); att_ok(r, r, &ps[1]);
    gr_identity_t f; mkid(&f); gr_header_t h; gr_get_header(r, &h);
    att_dis(r, &ps[2], f.peer_id, f.public_key, h.group_id, h.version);
    gr_join_verify_result_t res; gr_join_evaluate(r, &res);
    T(res.state == GR_JOIN_FAILED, "FAILED"); gr_close(r);
}
static void test_doc_3_colluders(void) {
    SEC("3 colluders -> VERIFIED (fundamental limit)");
    gr_identity_t a; mkid(&a); gr_registrar_t *r = mkg(&a, "D6");
    gr_identity_t c[4]; add_n(r, c, 4, &a); gr_sign(r, &a);
    gr_invite_ticket_t t; tkt(r, &a, &t); gr_join_begin(r, &t);
    att_ok(r, r, &c[0]); att_ok(r, r, &c[1]); att_ok(r, r, &c[2]);
    gr_join_verify_result_t res; gr_join_evaluate(r, &res);
    T(res.state == GR_JOIN_VERIFIED, "VERIFIED"); gr_close(r);
}
static void test_doc_deser_sync_ok(void) {
    SEC("deserialize: same-owner sync allowed during PROVISIONAL");
    gr_identity_t o; mkid(&o); gr_registrar_t *r = mkg(&o, "D7");
    gr_identity_t ps[4]; add_n(r, ps, 4, &o); gr_sign(r, &o);
    uint8_t *b; size_t bl; gr_serialize(r, GR_SERIALIZE_FULL, &b, &bl);
    goprov(r, &o);
    T(gr_deserialize(r, b, bl) == GR_OK, "sync ok"); gr_free(b); gr_close(r);
}
static void test_doc_delta_sync_ok(void) {
    SEC("apply_delta: sync allowed during PROVISIONAL");
    gr_identity_t o; mkid(&o); gr_registrar_t *r = mkg(&o, "D8");
    gr_identity_t ps[4]; add_n(r, ps, 4, &o); gr_sign(r, &o);
    uint8_t *d; size_t dl; gr_serialize_delta(r, 0, &d, &dl);
    goprov(r, &o); gr_merge_result_t mr;
    T(gr_apply_delta(r, d, dl, &mr) == GR_OK, "delta ok"); gr_free(d); gr_close(r);
}
static void test_doc_foreign_deser_blocked(void) {
    SEC("deserialize: foreign owner blocked during PROVISIONAL");
    gr_identity_t ro, atk; mkid(&ro); mkid(&atk);
    gr_registrar_t *real = mkg(&ro, "D9");
    gr_identity_t ps[4]; add_n(real, ps, 4, &ro); gr_sign(real, &ro);
    gr_registrar_t *fake = mkg(&atk, "D9");
    fake->header.version = real->header.version + 100; gr_sign(fake, &atk);
    uint8_t *b; size_t bl; gr_serialize(fake, GR_SERIALIZE_FULL, &b, &bl);
    goprov(real, &ro);
    T(gr_deserialize(real, b, bl) == GR_ERR_UNAUTHORIZED, "foreign blocked");
    gr_free(b); gr_close(real); gr_close(fake);
}
static void test_doc_owner_swap_established(void) {
    SEC("deserialize: owner swap blocked on established member");
    gr_identity_t ro, atk; mkid(&ro); mkid(&atk);
    gr_registrar_t *real = mkg(&ro, "D10"); gr_sign(real, &ro);
    gr_registrar_t *fake = mkg(&atk, "D10");
    fake->header.version = real->header.version + 100; gr_sign(fake, &atk);
    uint8_t *b; size_t bl; gr_serialize(fake, GR_SERIALIZE_FULL, &b, &bl);
    T(gr_deserialize(real, b, bl) == GR_ERR_UNAUTHORIZED, "blocked");
    gr_header_t h; gr_get_header(real, &h);
    T(yumi_memcmp(h.owner_sign_key, ro.public_key, GR_PUBLIC_KEY_LEN)==0, "unchanged");
    gr_free(b); gr_close(real); gr_close(fake);
}

/* ═══════════════════════════════════════════════════════════════════
 *  3. Inviter authorization
 * ═══════════════════════════════════════════════════════════════════ */
static void test_inv_outsider(void) {
    SEC("inviter not in registrar -> FAILED");
    gr_identity_t o, x; mkid(&o); mkid(&x);
    gr_registrar_t *r = mkg(&o, "I1"); gr_sign(r, &o);
    gr_invite_ticket_t t; tkt(r, &x, &t);
    T(gr_join_begin(r, &t) == GR_ERR_JOIN_FAILED, "FAILED"); gr_close(r);
}
static void test_inv_no_perm(void) {
    SEC("inviter without INVITE perm -> FAILED");
    gr_identity_t o, m; mkid(&o); mkid(&m);
    gr_registrar_t *r = mkg(&o, "I2"); addp(r, &m, &o); gr_sign(r, &o);
    gr_invite_ticket_t t; tkt(r, &m, &t);
    T(gr_join_begin(r, &t) == GR_ERR_JOIN_FAILED, "FAILED"); gr_close(r);
}
static void test_inv_wrong_perm(void) {
    SEC("inviter with KICK not INVITE -> FAILED");
    gr_identity_t o, m; mkid(&o); mkid(&m);
    gr_registrar_t *r = mkg(&o, "I3"); addp(r, &m, &o);
    setrole(r, &m, GR_PERM_KICK_MEMBER, &o, "k"); gr_sign(r, &o);
    gr_invite_ticket_t t; tkt(r, &m, &t);
    T(gr_join_begin(r, &t) == GR_ERR_JOIN_FAILED, "FAILED"); gr_close(r);
}
static void test_inv_kicked(void) {
    SEC("kicked inviter -> FAILED");
    gr_identity_t o, m; mkid(&o); mkid(&m);
    gr_registrar_t *r = mkg(&o, "I4"); addp(r, &m, &o);
    setrole(r, &m, GR_PERM_INVITE_MEMBER, &o, "mod");
    gr_peer_kick(r, m.peer_id, "bad", &o); gr_sign(r, &o);
    gr_invite_ticket_t t; tkt(r, &m, &t);
    T(gr_join_begin(r, &t) == GR_ERR_JOIN_FAILED, "FAILED"); gr_close(r);
}

/* ═══════════════════════════════════════════════════════════════════
 *  4. Small group bypass
 * ═══════════════════════════════════════════════════════════════════ */
static void test_sg_2(void) {
    SEC("2 members: bypass"); gr_identity_t o, i; mkid(&o); mkid(&i);
    gr_registrar_t *r = mkg(&o, "S1"); addp(r, &i, &o); gr_sign(r, &o);
    goprov(r, &o); T(gr_is_trusted(r), "trusted");
    gr_join_verify_result_t res; gr_join_evaluate(r, &res);
    T(res.small_group_bypass, "bypass"); gr_close(r);
}
static void test_sg_4(void) {
    SEC("4 members: bypass (<5)"); gr_identity_t o; mkid(&o);
    gr_registrar_t *r = mkg(&o, "S2");
    gr_identity_t ps[3]; add_n(r, ps, 3, &o); gr_sign(r, &o);
    goprov(r, &o); T(gr_is_trusted(r), "trusted"); gr_close(r);
}
static void test_sg_5_no(void) {
    SEC("5 members: NO bypass"); gr_identity_t o; mkid(&o);
    gr_registrar_t *r = mkg(&o, "S3");
    gr_identity_t ps[4]; add_n(r, ps, 4, &o); gr_sign(r, &o);
    goprov(r, &o); T(!gr_is_trusted(r), "not trusted"); gr_close(r);
}
static void test_sg_ticket_check(void) {
    SEC("bypass: ticket mismatch still fails");
    gr_identity_t o, a; mkid(&o); mkid(&a);
    gr_registrar_t *r = mkg(&o, "S4"); gr_sign(r, &o);
    gr_invite_ticket_t t; tkt(r, &o, &t);
    memcpy(t.owner_sign_key, a.public_key, GR_PUBLIC_KEY_LEN);
    T(gr_join_begin(r, &t) == GR_ERR_JOIN_FAILED, "fails"); gr_close(r);
}
static void test_sg_inviter_check(void) {
    SEC("bypass: unauthorized inviter still fails");
    gr_identity_t o, n, i; mkid(&o); mkid(&n); mkid(&i);
    gr_registrar_t *r = mkg(&o, "S5"); addp(r, &n, &o); addp(r, &i, &o);
    gr_sign(r, &o); gr_invite_ticket_t t; tkt(r, &n, &t);
    T(gr_join_begin(r, &t) == GR_ERR_JOIN_FAILED, "fails"); gr_close(r);
}
static void test_sg_mutations_work(void) {
    SEC("bypass: mutations work after VERIFIED");
    gr_identity_t o, i; mkid(&o); mkid(&i);
    gr_registrar_t *r = mkg(&o, "S6"); addp(r, &i, &o); gr_sign(r, &o);
    goprov(r, &o);
    T(gr_sign(r, &o) == GR_OK, "sign"); T(gr_epoch_rotate(r, &o) == GR_OK, "rotate");
    gr_close(r);
}

/* ═══════════════════════════════════════════════════════════════════
 *  5. Proportional quorum
 * ═══════════════════════════════════════════════════════════════════ */
static void test_q5(void) {
    SEC("5 active -> quorum 3"); gr_identity_t o; mkid(&o);
    gr_registrar_t *r = mkg(&o, "Q1");
    gr_identity_t ps[4]; add_n(r, ps, 4, &o); gr_sign(r, &o); goprov(r, &o);
    gr_join_verify_result_t res; gr_join_evaluate(r, &res);
    T(res.required_attestations == 3, "q=3"); gr_close(r);
}
static void test_q26(void) {
    SEC("26 active -> quorum 5"); gr_identity_t o; mkid(&o);
    gr_registrar_t *r = mkg(&o, "Q2");
    for (int i = 0; i < 25; i++) { gr_identity_t p; mkid(&p); addp(r, &p, &o); }
    gr_sign(r, &o); goprov(r, &o);
    gr_join_verify_result_t res; gr_join_evaluate(r, &res);
    T(res.required_attestations == 5, "q=5"); gr_close(r);
}
static void test_q102(void) {
    SEC("102 active -> quorum 10 (capped)"); gr_identity_t o; mkid(&o);
    gr_registrar_t *r = mkg(&o, "Q3");
    for (int i = 0; i < 101; i++) { gr_identity_t p; mkid(&p); addp(r, &p, &o); }
    gr_sign(r, &o); goprov(r, &o);
    gr_join_verify_result_t res; gr_join_evaluate(r, &res);
    T(res.required_attestations == 10, "q=10"); gr_close(r);
}
static void test_q_partial(void) {
    SEC("2/3 -> PROVISIONAL, 3/3 -> VERIFIED"); gr_identity_t o; mkid(&o);
    gr_registrar_t *r = mkg(&o, "Q4");
    gr_identity_t ps[4]; add_n(r, ps, 4, &o); gr_sign(r, &o); goprov(r, &o);
    att_ok(r, r, &ps[0]); att_ok(r, r, &ps[1]);
    gr_join_verify_result_t res; gr_join_evaluate(r, &res);
    T(res.state == GR_JOIN_PROVISIONAL, "2/3 prov");
    att_ok(r, r, &ps[2]); gr_join_evaluate(r, &res);
    T(res.state == GR_JOIN_VERIFIED, "3/3 verified"); gr_close(r);
}

/* ═══════════════════════════════════════════════════════════════════
 *  6. Inviter exclusion
 * ═══════════════════════════════════════════════════════════════════ */
static void test_inv_excl_owner(void) {
    SEC("owner-as-inviter excluded"); gr_identity_t o; mkid(&o);
    gr_registrar_t *r = mkg(&o, "IE1");
    gr_identity_t ps[4]; add_n(r, ps, 4, &o); gr_sign(r, &o); goprov(r, &o);
    T(att_ok(r, r, &o) == GR_ERR_JOIN_INVITER_EXCLUDED, "excluded");
    T(att_ok(r, r, &ps[0]) == GR_OK, "p0 ok"); gr_close(r);
}
static void test_inv_excl_mod(void) {
    SEC("mod-as-inviter excluded"); gr_identity_t o, m; mkid(&o); mkid(&m);
    gr_registrar_t *r = mkg(&o, "IE2"); addp(r, &m, &o);
    setrole(r, &m, GR_PERM_INVITE_MEMBER, &o, "mod");
    gr_identity_t ps[4]; add_n(r, ps, 4, &o); gr_sign(r, &o);
    gr_invite_ticket_t t; tkt(r, &m, &t); gr_join_begin(r, &t);
    T(att_ok(r, r, &m) == GR_ERR_JOIN_INVITER_EXCLUDED, "mod excluded");
    att_ok(r, r, &ps[0]); att_ok(r, r, &ps[1]); att_ok(r, r, &ps[2]);
    gr_join_verify_result_t res; gr_join_evaluate(r, &res);
    T(res.state == GR_JOIN_VERIFIED, "VERIFIED without mod"); gr_close(r);
}

/* ═══════════════════════════════════════════════════════════════════
 *  7. Dissent callback
 * ═══════════════════════════════════════════════════════════════════ */
static void test_cb_accept(void) {
    SEC("callback true -> VERIFIED+override"); gr_identity_t o; mkid(&o);
    gr_registrar_t *r = mkg(&o, "CB1");
    gr_identity_t ps[4]; add_n(r, ps, 4, &o); gr_sign(r, &o); goprov(r, &o);
    bool fired = false; gr_join_set_dissent_callback(r, cb_yes, &fired);
    gr_identity_t f; mkid(&f); gr_header_t h; gr_get_header(r, &h);
    att_dis(r, &ps[0], f.peer_id, f.public_key, h.group_id, h.version);
    gr_join_verify_result_t res; gr_join_evaluate(r, &res);
    T(res.state == GR_JOIN_VERIFIED, "VERIFIED"); T(res.user_override, "override");
    T(fired, "fired"); T(gr_is_trusted(r), "trusted"); gr_close(r);
}
static void test_cb_reject(void) {
    SEC("callback false -> FAILED"); gr_identity_t o; mkid(&o);
    gr_registrar_t *r = mkg(&o, "CB2");
    gr_identity_t ps[4]; add_n(r, ps, 4, &o); gr_sign(r, &o); goprov(r, &o);
    bool fired = false; gr_join_set_dissent_callback(r, cb_no, &fired);
    gr_identity_t f; mkid(&f); gr_header_t h; gr_get_header(r, &h);
    att_dis(r, &ps[0], f.peer_id, f.public_key, h.group_id, h.version);
    gr_join_verify_result_t res; gr_join_evaluate(r, &res);
    T(res.state == GR_JOIN_FAILED, "FAILED"); T(fired, "fired"); gr_close(r);
}
static void test_cb_none_fails(void) {
    SEC("no callback -> FAILED"); gr_identity_t o; mkid(&o);
    gr_registrar_t *r = mkg(&o, "CB3");
    gr_identity_t ps[4]; add_n(r, ps, 4, &o); gr_sign(r, &o); goprov(r, &o);
    gr_identity_t f; mkid(&f); gr_header_t h; gr_get_header(r, &h);
    att_dis(r, &ps[0], f.peer_id, f.public_key, h.group_id, h.version);
    gr_join_verify_result_t res; gr_join_evaluate(r, &res);
    T(res.state == GR_JOIN_FAILED, "FAILED"); gr_close(r);
}
static void test_cb_fires_once(void) {
    SEC("callback fires once"); gr_identity_t o; mkid(&o);
    gr_registrar_t *r = mkg(&o, "CB4");
    gr_identity_t ps[4]; add_n(r, ps, 4, &o); gr_sign(r, &o); goprov(r, &o);
    int cnt = 0; gr_join_set_dissent_callback(r, cb_cnt, &cnt);
    gr_identity_t f; mkid(&f); gr_header_t h; gr_get_header(r, &h);
    att_dis(r, &ps[0], f.peer_id, f.public_key, h.group_id, h.version);
    att_dis(r, &ps[1], f.peer_id, f.public_key, h.group_id, h.version);
    gr_join_verify_result_t res;
    gr_join_evaluate(r, &res); gr_join_evaluate(r, &res);
    T(cnt == 1, "once"); gr_close(r);
}

/* ═══════════════════════════════════════════════════════════════════
 *  8. Mutation lockdown
 * ═══════════════════════════════════════════════════════════════════ */
static void test_mut_prov(void) {
    SEC("PROVISIONAL: mutations blocked"); gr_identity_t o; mkid(&o);
    gr_registrar_t *r = mkg(&o, "M1");
    gr_identity_t ps[4]; add_n(r, ps, 4, &o); gr_sign(r, &o); goprov(r, &o);
    T(gr_sign(r, &o) == GR_ERR_JOIN_UNVERIFIED, "sign");
    T(gr_epoch_rotate(r, &o) == GR_ERR_JOIN_UNVERIFIED, "rotate");
    gr_peer_t out; T(gr_peer_get(r, ps[0].peer_id, &out) == GR_OK, "read ok");
    gr_close(r);
}
static void test_mut_failed(void) {
    SEC("FAILED: mutations blocked"); gr_identity_t o; mkid(&o);
    gr_registrar_t *r = mkg(&o, "M2");
    gr_identity_t ps[4]; add_n(r, ps, 4, &o); gr_sign(r, &o); goprov(r, &o);
    gr_identity_t f; mkid(&f); gr_header_t h; gr_get_header(r, &h);
    att_dis(r, &ps[0], f.peer_id, f.public_key, h.group_id, h.version);
    gr_join_verify_result_t res; gr_join_evaluate(r, &res);
    T(gr_sign(r, &o) == GR_ERR_JOIN_UNVERIFIED, "sign after FAILED"); gr_close(r);
}

/* ═══════════════════════════════════════════════════════════════════
 *  9. Attestation integrity
 * ═══════════════════════════════════════════════════════════════════ */
static void test_replay_nonce(void) {
    SEC("wrong nonce -> SIG_INVALID"); gr_identity_t o; mkid(&o);
    gr_registrar_t *r = mkg(&o, "A1");
    gr_identity_t ps[4]; add_n(r, ps, 4, &o); gr_sign(r, &o); goprov(r, &o);
    uint8_t bad[GR_JOIN_NONCE_LEN]; yumi_randombytes(bad, GR_JOIN_NONCE_LEN);
    gr_header_t hdr; uint8_t sig[GR_SIGN_LEN];
    gr_join_export_header_attestation(r, &ps[0], bad, &hdr, sig);
    T(gr_join_submit_peer_header(r, &hdr, ps[0].peer_id, ps[0].public_key, sig)
      == GR_ERR_SIGNATURE_INVALID, "bad nonce"); gr_close(r);
}
static void test_forged(void) {
    SEC("forger -> SIG_INVALID"); gr_identity_t o, f; mkid(&o); mkid(&f);
    gr_registrar_t *r = mkg(&o, "A2");
    gr_identity_t ps[4]; add_n(r, ps, 4, &o); gr_sign(r, &o); goprov(r, &o);
    uint8_t nonce[GR_JOIN_NONCE_LEN]; gr_join_get_nonce(r, nonce);
    gr_header_t hdr; uint8_t sig[GR_SIGN_LEN];
    gr_join_export_header_attestation(r, &f, nonce, &hdr, sig);
    T(gr_join_submit_peer_header(r, &hdr, ps[0].peer_id, f.public_key, sig)
      == GR_ERR_SIGNATURE_INVALID, "forged"); gr_close(r);
}
static void test_dup_vote(void) {
    SEC("dup vote -> ALREADY_EXISTS"); gr_identity_t o; mkid(&o);
    gr_registrar_t *r = mkg(&o, "A3");
    gr_identity_t ps[4]; add_n(r, ps, 4, &o); gr_sign(r, &o); goprov(r, &o);
    T(att_ok(r, r, &ps[0]) == GR_OK, "first");
    T(att_ok(r, r, &ps[0]) == GR_ERR_ALREADY_EXISTS, "dup"); gr_close(r);
}
static void test_unknown_peer(void) {
    SEC("unknown peer -> NOT_FOUND"); gr_identity_t o, u; mkid(&o); mkid(&u);
    gr_registrar_t *r = mkg(&o, "A4");
    gr_identity_t ps[4]; add_n(r, ps, 4, &o); gr_sign(r, &o); goprov(r, &o);
    T(att_ok(r, r, &u) == GR_ERR_NOT_FOUND, "unknown"); gr_close(r);
}
static void test_roundtrip(void) {
    SEC("attestation roundtrip"); gr_identity_t o; mkid(&o);
    gr_registrar_t *r = mkg(&o, "A5"); gr_sign(r, &o); goprov(r, &o);
    uint8_t nonce[GR_JOIN_NONCE_LEN]; gr_join_get_nonce(r, nonce);
    gr_header_t hdr; uint8_t sig[GR_SIGN_LEN];
    gr_join_export_header_attestation(r, &o, nonce, &hdr, sig);
    uint8_t ad[GR_JOIN_ATTESTATION_LEN]; size_t p = 0;
    memcpy(ad+p, hdr.group_id, GR_HASH_LEN); p += GR_HASH_LEN;
    memcpy(ad+p, hdr.owner_id, GR_PEER_ID_LEN); p += GR_PEER_ID_LEN;
    memcpy(ad+p, hdr.owner_sign_key, GR_PUBLIC_KEY_LEN); p += GR_PUBLIC_KEY_LEN;
    uint32_t vle = gr_htole32(hdr.version); memcpy(ad+p, &vle, 4); p += 4;
    memcpy(ad+p, nonce, GR_JOIN_NONCE_LEN); p += GR_JOIN_NONCE_LEN;
    T(yumi_mldsa_verify(sig, ad, p, o.public_key) == YUMI_CRYPTO_OK, "ok");
    ad[0] ^= 0xFF;
    T(yumi_mldsa_verify(sig, ad, p, o.public_key) != YUMI_CRYPTO_OK, "tampered");
    gr_close(r);
}

/* ═══════════════════════════════════════════════════════════════════
 *  10. Unattested peers
 * ═══════════════════════════════════════════════════════════════════ */
static void test_ua_initial(void) {
    SEC("unattested: all minus inviter"); gr_identity_t o; mkid(&o);
    gr_registrar_t *r = mkg(&o, "U1");
    gr_identity_t ps[4]; add_n(r, ps, 4, &o); gr_sign(r, &o); goprov(r, &o);
    uint8_t buf[10][GR_PEER_ID_LEN]; uint32_t c;
    gr_join_list_unattested_peers(r, (uint8_t*)buf, 10, &c);
    T(c == 4, "4 unattested"); gr_close(r);
}
static void test_ua_shrinks(void) {
    SEC("unattested: shrinks"); gr_identity_t o; mkid(&o);
    gr_registrar_t *r = mkg(&o, "U2");
    gr_identity_t ps[4]; add_n(r, ps, 4, &o); gr_sign(r, &o); goprov(r, &o);
    att_ok(r, r, &ps[0]);
    uint8_t buf[10][GR_PEER_ID_LEN]; uint32_t c;
    gr_join_list_unattested_peers(r, (uint8_t*)buf, 10, &c);
    T(c == 3, "3 after 1 attests"); gr_close(r);
}
static void test_ua_no_join(void) {
    SEC("unattested: no join -> INVALID"); gr_identity_t o; mkid(&o);
    gr_registrar_t *r = mkg(&o, "U3");
    uint8_t buf[10][GR_PEER_ID_LEN]; uint32_t c;
    T(gr_join_list_unattested_peers(r, (uint8_t*)buf, 10, &c)
      == GR_ERR_INVALID_PARAM, "invalid"); gr_close(r);
}

/* ═══════════════════════════════════════════════════════════════════
 *  11. Edge cases
 * ═══════════════════════════════════════════════════════════════════ */
static void test_no_timeout(void) {
    SEC("no timeout: stays PROVISIONAL"); gr_identity_t o; mkid(&o);
    gr_registrar_t *r = mkg(&o, "E1");
    gr_identity_t ps[4]; add_n(r, ps, 4, &o); gr_sign(r, &o); goprov(r, &o);
    att_ok(r, r, &ps[0]);
    r->join_state->started_at = gr_timestamp_ms() - 3600000LL;
    gr_join_verify_result_t res; gr_join_evaluate(r, &res);
    T(res.state == GR_JOIN_PROVISIONAL, "still prov"); gr_close(r);
}
static void test_double_begin(void) {
    SEC("double begin -> ALREADY_EXISTS"); gr_identity_t o; mkid(&o);
    gr_registrar_t *r = mkg(&o, "E2"); gr_sign(r, &o); goprov(r, &o);
    gr_invite_ticket_t t; tkt(r, &o, &t);
    T(gr_join_begin(r, &t) == GR_ERR_ALREADY_EXISTS, "dup"); gr_close(r);
}
static void test_submit_non_prov(void) {
    SEC("submit to non-provisional -> INVALID"); gr_identity_t o, p; mkid(&o); mkid(&p);
    gr_registrar_t *r = mkg(&o, "E3"); addp(r, &p, &o); gr_sign(r, &o);
    uint8_t n[GR_JOIN_NONCE_LEN] = {0}; gr_header_t h; uint8_t sig[GR_SIGN_LEN];
    gr_join_export_header_attestation(r, &p, n, &h, sig);
    T(gr_join_submit_peer_header(r, &h, p.peer_id, p.public_key, sig)
      == GR_ERR_INVALID_PARAM, "invalid"); gr_close(r);
}
static void test_null_params(void) {
    SEC("NULL params"); gr_identity_t id; mkid(&id);
    gr_invite_ticket_t t; memset(&t, 0, sizeof(t));
    T(gr_join_begin(NULL, &t) == GR_ERR_INVALID_PARAM, "a");
    gr_registrar_t *r = mkg(&id, "NP");
    T(gr_join_begin(r, NULL) == GR_ERR_INVALID_PARAM, "b");
    uint8_t n[GR_JOIN_NONCE_LEN];
    T(gr_join_get_nonce(r, n) == GR_ERR_INVALID_PARAM, "c");
    T(!gr_is_trusted(NULL), "d"); gr_close(r);
}

/* ═══════════════════════════════════════════════════════════════════ */
int main(void) {
    if (yumi_crypto_init() != YUMI_CRYPTO_OK) { fprintf(stderr, "yumi_crypto_init failed\n"); return 1; }
    fprintf(stdout, "═══ join_verify test suite ═══\n\n");

    test_legit_join(); test_local_trusted();
    test_doc_ticket_owner(); test_doc_ticket_gid();
    test_doc_peer_disagrees(); test_doc_all_disagree();
    test_doc_single_dissenter(); test_doc_3_colluders();
    test_doc_deser_sync_ok(); test_doc_delta_sync_ok();
    test_doc_foreign_deser_blocked(); test_doc_owner_swap_established();
    test_inv_outsider(); test_inv_no_perm();
    test_inv_wrong_perm(); test_inv_kicked();
    test_sg_2(); test_sg_4(); test_sg_5_no();
    test_sg_ticket_check(); test_sg_inviter_check(); test_sg_mutations_work();
    test_q5(); test_q26(); test_q102(); test_q_partial();
    test_inv_excl_owner(); test_inv_excl_mod();
    test_cb_accept(); test_cb_reject(); test_cb_none_fails(); test_cb_fires_once();
    test_mut_prov(); test_mut_failed();
    test_replay_nonce(); test_forged(); test_dup_vote();
    test_unknown_peer(); test_roundtrip();
    test_ua_initial(); test_ua_shrinks(); test_ua_no_join();
    test_no_timeout(); test_double_begin();
    test_submit_non_prov(); test_null_params();

    fprintf(stdout, "\n═══ %d/%d passed ═══\n", g_run - g_fail, g_run);
    if (g_fail) { fprintf(stderr, "%d FAILED\n", g_fail); return 1; }
    fprintf(stdout, "All passed.\n"); return 0;
}
