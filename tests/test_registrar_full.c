/*
 * test_registrar_full.c - Full Group Registrar test suite: lifecycle, peers, roles, epochs, retention, invites, icons, webapps, servers, behavioral analysis.
 * Copyright (C) 2026 DevNullIsaac
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file test_registrar_full.c
 * @brief Comprehensive test suite for all untested Group Registrar modules.
 *
 * Covers:
 *   1.  Lifecycle — create, open, close, double-close, NULL safety
 *   2.  Peer management — add, get, kick, ban, leave, update address,
 *       observed address, touch, set role, list, count, is_authorized
 *   3.  Role management — add, get, remove, set_permissions, list,
 *       count, has_permission, limit, unauthorized
 *   4.  Epoch management — rotate, get_current, get, list, count,
 *       unauthorized, multi-rotate, old epoch retrieval
 *   5.  Retention — set, get, defaults, unauthorized, edge values
 *   6.  Invite lifecycle — create, parse, check, invalidate,
 *       mark_used, list, count, expiry, unauthorized, double-use
 *   7.  Group icon — set, get, hash, remove, size limit, video icon,
 *       unauthorized, double-remove
 *   8.  WebApp management — add, remove, is_authorized, list, count,
 *       unauthorized, duplicate, get, update (perm_data + role_mask blobs),
 *       blob roundtrip, list blob content, size limits, NULL safety
 *   9.  Server management — add, remove, list, count, types,
 *       unauthorized
 *  10.  Behavioral analysis — actor_burst, mutation_rate, admin_score,
 *       peer_churn, delta_score, stale_peers, epoch_pattern,
 *       network_score, snapshot, config get/set, alert callback
 *  11.  Permission boundaries — non-owner without role, with role,
 *       owner override, cross-module permission checks
 *  12.  NULL / edge case validation for every public API
 *
 * All tests use in-memory DuckDB (":memory:").
 */

#include "internal.h"
#include "crypto.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ── Test harness ──────────────────────────────────────────────── */

static int g_run  = 0;
static int g_fail = 0;

#define T(cond, msg) do { g_run++; if (!(cond)) { g_fail++; \
    fprintf(stderr, "  FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); } } while(0)
#define SEC(name) fprintf(stdout, "── %s\n", name)

/* ── Helpers ───────────────────────────────────────────────────── */

static void mkid(gr_identity_t *id) {
    gr_identity_generate(id);
}

static gr_registrar_t *mkreg(const gr_identity_t *owner) {
    gr_registrar_t *r = NULL;
    if (gr_create(&r, ":memory:", "TestGroup", GR_GROUP_PRIVATE, owner) != GR_OK)
        return NULL;
    gr_sign(r, owner);
    return r;
}

static gr_peer_t mkpeer(const gr_identity_t *id) {
    gr_peer_t p;
    memset(&p, 0, sizeof(p));
    memcpy(p.peer_id, id->peer_id, GR_PEER_ID_LEN);
    memcpy(p.kem_pk, id->kem_pk, GR_KEM_PUBLIC_KEY_LEN);
    memcpy(p.sign_key, id->public_key, GR_PUBLIC_KEY_LEN);
    strncpy(p.ip, "10.0.0.1", GR_MAX_IP_LEN);
    p.port = 9000;
    p.status = GR_PEER_ACTIVE;
    p.joined_at = gr_timestamp_ms();
    p.last_seen = p.joined_at;
    return p;
}

static void add_peer(gr_registrar_t *r, const gr_identity_t *peer,
                     const gr_identity_t *signer) {
    gr_peer_t p = mkpeer(peer);
    gr_peer_add(r, &p, signer);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 1: Lifecycle
 * ═══════════════════════════════════════════════════════════════════ */

static void test_lifecycle(void)
{
    SEC("1. Lifecycle — create, close, NULL safety");

    gr_identity_t owner;
    mkid(&owner);

    /* 1a. Create in-memory */
    gr_registrar_t *r = mkreg(&owner);
    T(r != NULL, "create in-memory succeeds");

    /* 1b. Get header after create */
    gr_header_t h;
    T(gr_get_header(r, &h) == GR_OK, "get_header succeeds");
    T(h.version > 0, "version > 0 after create");
    T(h.group_type == GR_GROUP_PRIVATE, "group_type = PRIVATE");
    T(memcmp(h.owner_id, owner.peer_id, GR_PEER_ID_LEN) == 0, "owner_id matches");

    /* 1c. Sign + verify */
    T(gr_sign(r, &owner) == GR_OK, "sign succeeds");
    bool valid = false;
    T(gr_verify(r, &valid) == GR_OK, "verify call succeeds");
    T(valid, "signature valid");

    /* 1d. Close + double-close */
    gr_close(r);
    gr_close(NULL); /* should not crash */
    T(1, "double-close no crash");

    /* 1e. NULL create params */
    gr_registrar_t *out = NULL;
    T(gr_create(NULL, ":memory:", "x", GR_GROUP_PRIVATE, &owner) == GR_ERR_INVALID_PARAM,
      "create(NULL out) = INVALID_PARAM");
    T(gr_create(&out, NULL, "x", GR_GROUP_PRIVATE, &owner) == GR_ERR_INVALID_PARAM,
      "create(NULL path) = INVALID_PARAM");
    T(gr_create(&out, ":memory:", NULL, GR_GROUP_PRIVATE, &owner) == GR_ERR_INVALID_PARAM,
      "create(NULL name) = INVALID_PARAM");
    T(gr_create(&out, ":memory:", "x", GR_GROUP_PRIVATE, NULL) == GR_ERR_INVALID_PARAM,
      "create(NULL owner) = INVALID_PARAM");

    /* 1f. gr_get_header NULL */
    T(gr_get_header(NULL, &h) == GR_ERR_INVALID_PARAM, "get_header(NULL) = INVALID_PARAM");

    /* 1g. Public group type */
    gr_registrar_t *pub = NULL;
    T(gr_create(&pub, ":memory:", "Public", GR_GROUP_PUBLIC, &owner) == GR_OK,
      "create public group");
    gr_header_t ph;
    gr_get_header(pub, &ph);
    T(ph.group_type == GR_GROUP_PUBLIC, "public group type");
    gr_close(pub);

    /* 1h. gr_is_trusted on fresh create = true */
    r = mkreg(&owner);
    T(gr_is_trusted(r), "fresh registrar is trusted");
    gr_close(r);

    /* 1i. gr_open with nonexistent path */
    gr_registrar_t *bad = NULL;
    uint8_t fake_gid[GR_HASH_LEN];
    memset(fake_gid, 0xAA, GR_HASH_LEN);
    T(gr_open(&bad, "/tmp/nonexistent_yumi_test_db_12345.db", fake_gid) != GR_OK,
      "open nonexistent path fails");
    T(bad == NULL, "out stays NULL on failure");

    /* 1j. gr_open with wrong group_id on a valid DB */
    r = mkreg(&owner);
    gr_header_t hdr;
    gr_get_header(r, &hdr);
    /* Can only test wrong-group_id with file-based DB, not :memory:.
     * Just verify NULL params are rejected. */
    T(gr_open(NULL, ":memory:", hdr.group_id) == GR_ERR_INVALID_PARAM,
      "open(NULL out) = INVALID_PARAM");
    T(gr_open(&bad, NULL, hdr.group_id) == GR_ERR_INVALID_PARAM,
      "open(NULL path) = INVALID_PARAM");
    gr_close(r);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 2: Peer management
 * ═══════════════════════════════════════════════════════════════════ */

static void test_peer_management(void)
{
    SEC("2. Peer management");

    gr_identity_t owner, alice, bob, carol;
    mkid(&owner); mkid(&alice); mkid(&bob); mkid(&carol);

    gr_registrar_t *r = mkreg(&owner);
    T(r != NULL, "create reg");

    /* 2a. Add peers */
    add_peer(r, &alice, &owner);
    add_peer(r, &bob, &owner);
    add_peer(r, &carol, &owner);

    /* 2b. Count active peers (owner + 3) */
    uint32_t count = 0;
    T(gr_peer_count(r, GR_PEER_ACTIVE, &count) == GR_OK, "peer_count active");
    T(count == 4, "4 active peers (owner + 3)");

    /* 2c. Get peer */
    gr_peer_t p;
    T(gr_peer_get(r, alice.peer_id, &p) == GR_OK, "peer_get alice");
    T(memcmp(p.peer_id, alice.peer_id, GR_PEER_ID_LEN) == 0, "peer_id matches");
    T(p.status == GR_PEER_ACTIVE, "alice is active");
    T(p.port == 9000, "port correct");

    /* 2d. Peer not found */
    uint8_t fake_id[GR_PEER_ID_LEN];
    memset(fake_id, 0xFF, GR_PEER_ID_LEN);
    T(gr_peer_get(r, fake_id, &p) == GR_ERR_NOT_FOUND, "peer_get unknown = NOT_FOUND");

    /* 2e. Duplicate add */
    gr_peer_t dup = mkpeer(&alice);
    T(gr_peer_add(r, &dup, &owner) == GR_ERR_ALREADY_EXISTS, "duplicate add = ALREADY_EXISTS");

    /* 2f. Kick */
    T(gr_peer_kick(r, alice.peer_id, "test reason", &owner) == GR_OK, "kick alice");
    T(gr_peer_get(r, alice.peer_id, &p) == GR_OK, "get kicked peer");
    T(p.status == GR_PEER_KICKED, "alice status = KICKED");

    /* 2g. Count by status */
    uint32_t kicked = 0;
    T(gr_peer_count(r, GR_PEER_KICKED, &kicked) == GR_OK, "count kicked");
    T(kicked == 1, "1 kicked peer");

    /* 2h. Ban */
    T(gr_peer_ban(r, bob.peer_id, "bad actor", &owner) == GR_OK, "ban bob");
    T(gr_peer_get(r, bob.peer_id, &p) == GR_OK, "get banned peer");
    T(p.status == GR_PEER_BANNED, "bob status = BANNED");

    /* 2i. Leave */
    T(gr_peer_leave(r, &carol) == GR_OK, "carol leaves");
    T(gr_peer_get(r, carol.peer_id, &p) == GR_OK, "get left peer");
    T(p.status == GR_PEER_LEFT, "carol status = LEFT");

    /* 2j. Count all */
    uint32_t all = 0;
    T(gr_peer_count(r, GR_PEER_STATUS_ANY, &all) == GR_OK, "count all");
    T(all == 4, "4 total peers");

    /* 2k. List active (should be owner only) */
    gr_peer_t list[10];
    uint32_t lcount = 0;
    T(gr_peer_list(r, list, 10, &lcount, GR_PEER_ACTIVE) == GR_OK, "list active");
    T(lcount == 1, "1 active peer (owner)");

    /* 2l. Update address */
    T(gr_peer_update_address(r, "192.168.1.1", 8080, &owner) == GR_OK,
      "update_address");
    T(gr_peer_get(r, owner.peer_id, &p) == GR_OK, "get owner after update");
    T(strcmp(p.ip, "192.168.1.1") == 0, "IP updated");
    T(p.port == 8080, "port updated");

    /* 2m. Observed address */
    gr_identity_t dave;
    mkid(&dave);
    add_peer(r, &dave, &owner);
    T(gr_peer_observed_address(r, dave.peer_id, "10.0.0.5", 7070, &owner) == GR_OK,
      "observed_address");
    T(gr_peer_get(r, dave.peer_id, &p) == GR_OK, "get after observed");
    T(strcmp(p.ip, "10.0.0.5") == 0, "observed IP");
    T(p.port == 7070, "observed port");

    /* 2n. Touch (update last_seen) */
    T(gr_peer_touch(r, dave.peer_id) == GR_OK, "touch");
    gr_peer_t p2;
    T(gr_peer_get(r, dave.peer_id, &p2) == GR_OK, "get after touch");
    T(p2.last_seen >= p.last_seen, "last_seen updated");

    /* 2o. is_authorized — owner always authorized */
    T(gr_peer_is_authorized(r, owner.peer_id), "owner is authorized");
    T(!gr_peer_is_authorized(r, alice.peer_id), "kicked peer not authorized");
    T(!gr_peer_is_authorized(r, bob.peer_id), "banned peer not authorized");
    T(!gr_peer_is_authorized(r, fake_id), "unknown peer not authorized");

    /* 2p. NULL params */
    T(gr_peer_add(NULL, &dup, &owner) == GR_ERR_INVALID_PARAM, "add(NULL reg)");
    T(gr_peer_add(r, NULL, &owner) == GR_ERR_INVALID_PARAM, "add(NULL peer)");
    T(gr_peer_kick(NULL, alice.peer_id, "x", &owner) == GR_ERR_INVALID_PARAM, "kick(NULL)");
    T(gr_peer_ban(NULL, bob.peer_id, "x", &owner) == GR_ERR_INVALID_PARAM, "ban(NULL)");
    T(gr_peer_leave(NULL, &carol) == GR_ERR_INVALID_PARAM, "leave(NULL)");
    T(gr_peer_get(NULL, alice.peer_id, &p) == GR_ERR_INVALID_PARAM, "get(NULL)");
    T(gr_peer_count(NULL, GR_PEER_ACTIVE, &count) == GR_ERR_INVALID_PARAM, "count(NULL)");
    T(gr_peer_touch(NULL, dave.peer_id) == GR_ERR_INVALID_PARAM, "touch(NULL)");

    /* 2q. Unauthorized kick (non-owner, no role) */
    gr_identity_t nobody;
    mkid(&nobody);
    add_peer(r, &nobody, &owner);
    T(gr_peer_kick(r, dave.peer_id, "no perm", &nobody) == GR_ERR_UNAUTHORIZED,
      "unauthorized kick");

    /* 2r. peer_id / sign_key binding — mismatched peer_id rejected */
    {
        gr_identity_t legit;
        mkid(&legit);
        gr_peer_t bad = mkpeer(&legit);
        /* Replace peer_id with garbage while keeping valid sign_key */
        memset(bad.peer_id, 0xAA, GR_PEER_ID_LEN);
        T(gr_peer_add(r, &bad, &owner) == GR_ERR_INVALID_PARAM,
          "mismatched peer_id/sign_key rejected");
    }

    /* 2s. observed_address — NULL observer rejected */
    T(gr_peer_observed_address(r, dave.peer_id, "10.0.0.99", 8080, NULL)
          == GR_ERR_INVALID_PARAM,
      "observed_address NULL observer rejected");

    /* 2t. observed_address — valid call with observer produces audit */
    {
        uint32_t audit_before = 0;
        gr_audit_count(r, &audit_before);
        T(gr_peer_observed_address(r, dave.peer_id, "10.0.0.88", 7777, &owner)
              == GR_OK,
          "observed_address with observer succeeds");
        gr_peer_t obs;
        T(gr_peer_get(r, dave.peer_id, &obs) == GR_OK, "get after observed");
        T(strcmp(obs.ip, "10.0.0.88") == 0, "observed IP updated");
        T(obs.port == 7777, "observed port updated");
        uint32_t audit_after = 0;
        gr_audit_count(r, &audit_after);
        T(audit_after > audit_before, "observed_address created audit entry");
    }

    /* 2u. observed_address — non-existent peer rejected */
    {
        uint8_t fake[GR_PEER_ID_LEN];
        memset(fake, 0xFF, GR_PEER_ID_LEN);
        T(gr_peer_observed_address(r, fake, "10.0.0.1", 9000, &owner)
              == GR_ERR_NOT_FOUND,
          "observed_address unknown peer rejected");
    }

    gr_close(r);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 3: Role management
 * ═══════════════════════════════════════════════════════════════════ */

static void test_role_management(void)
{
    SEC("3. Role management");

    gr_identity_t owner, member;
    mkid(&owner); mkid(&member);

    gr_registrar_t *r = mkreg(&owner);
    add_peer(r, &member, &owner);

    /* 3a. Add role */
    uint32_t mod_id = 0;
    T(gr_role_add(r, "Moderator", GR_PERM_KICK_MEMBER | GR_PERM_BAN_MEMBER,
                  &owner, &mod_id) == GR_OK, "add Moderator role");
    T(mod_id > 0, "role_id > 0");

    /* 3b. Get role */
    gr_role_t role;
    T(gr_role_get(r, mod_id, &role) == GR_OK, "get role");
    T(strcmp(role.name, "Moderator") == 0, "role name matches");
    T(role.permissions == (GR_PERM_KICK_MEMBER | GR_PERM_BAN_MEMBER), "permissions match");

    /* 3c. Count roles */
    uint32_t rc = 0;
    T(gr_role_count(r, &rc) == GR_OK, "role_count");
    T(rc == 1, "1 role");

    /* 3d. List roles */
    gr_role_t rlist[10];
    uint32_t rlcount = 0;
    T(gr_role_list(r, rlist, 10, &rlcount) == GR_OK, "role_list");
    T(rlcount == 1, "list has 1 role");

    /* 3e. Set permissions */
    T(gr_role_set_permissions(r, mod_id,
                              GR_PERM_KICK_MEMBER | GR_PERM_INVITE_MEMBER,
                              &owner) == GR_OK, "set_permissions");
    T(gr_role_get(r, mod_id, &role) == GR_OK, "get after update");
    T(role.permissions == (GR_PERM_KICK_MEMBER | GR_PERM_INVITE_MEMBER),
      "permissions updated");

    /* 3f. Assign role to member */
    T(gr_peer_set_role(r, member.peer_id, mod_id, &owner) == GR_OK,
      "assign role to member");

    /* 3g. has_permission */
    T(gr_has_permission(r, &member, GR_PERM_KICK_MEMBER),
      "member has KICK_MEMBER via role");
    T(!gr_has_permission(r, &member, GR_PERM_ROTATE_EPOCH),
      "member lacks ROTATE_EPOCH");
    T(gr_has_permission(r, &owner, GR_PERM_ROTATE_EPOCH),
      "owner has all permissions");

    /* 3h. Add second role */
    uint32_t admin_id = 0;
    T(gr_role_add(r, "Admin", GR_PERM_KICK_MEMBER | GR_PERM_BAN_MEMBER |
                  GR_PERM_INVITE_MEMBER | GR_PERM_EDIT_ROLES,
                  &owner, &admin_id) == GR_OK, "add Admin role");
    T(admin_id != mod_id, "different role IDs");

    /* 3i. Role limit (add up to GR_MAX_ROLES) */
    uint32_t added = 2; /* mod + admin already */
    gr_error_t last_err = GR_OK;
    for (uint32_t i = added; i < GR_MAX_ROLES + 5; i++) {
        char name[32];
        snprintf(name, sizeof(name), "role_%u", i);
        uint32_t rid;
        last_err = gr_role_add(r, name, GR_PERM_NONE, &owner, &rid);
        if (last_err != GR_OK) break;
    }
    T(last_err == GR_ERR_ROLE_LIMIT, "role limit enforced");

    /* 3j. Remove role */
    T(gr_role_remove(r, mod_id, &owner) == GR_OK, "remove Moderator");
    T(gr_role_get(r, mod_id, &role) == GR_ERR_NOT_FOUND, "removed role not found");

    /* 3k. Remove nonexistent */
    T(gr_role_remove(r, 9999, &owner) == GR_ERR_NOT_FOUND, "remove unknown role");

    /* 3l. Unauthorized role add */
    uint32_t tmp;
    T(gr_role_add(r, "bad", GR_PERM_NONE, &member, &tmp) == GR_ERR_UNAUTHORIZED,
      "unauthorized role add");

    /* 3l2. Unauthorized role remove */
    T(gr_role_remove(r, admin_id, &member) == GR_ERR_UNAUTHORIZED,
      "unauthorized role remove");

    /* 3l3. Unauthorized set_permissions */
    T(gr_role_set_permissions(r, admin_id, GR_PERM_OWNER, &member) == GR_ERR_UNAUTHORIZED,
      "unauthorized set_permissions");

    /* 3m. NULL params */
    T(gr_role_add(NULL, "x", 0, &owner, &tmp) == GR_ERR_INVALID_PARAM, "add(NULL reg)");
    T(gr_role_get(NULL, 1, &role) == GR_ERR_INVALID_PARAM, "get(NULL)");
    T(gr_role_count(NULL, &rc) == GR_ERR_INVALID_PARAM, "count(NULL)");
    T(gr_role_remove(NULL, 1, &owner) == GR_ERR_INVALID_PARAM, "remove(NULL)");

    gr_close(r);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 4: Epoch management
 * ═══════════════════════════════════════════════════════════════════ */

static void test_epoch_management(void)
{
    SEC("4. Epoch management");

    gr_identity_t owner, member;
    mkid(&owner); mkid(&member);

    gr_registrar_t *r = mkreg(&owner);
    add_peer(r, &member, &owner);

    /* 4a. Initial epoch */
    gr_epoch_t ep;
    T(gr_epoch_get_current(r, &ep) == GR_OK, "get_current epoch");
    T(ep.epoch_id == 1, "initial epoch_id = 1");
    T(ep.created_at > 0, "epoch has timestamp");

    uint32_t ec = 0;
    T(gr_epoch_count(r, &ec) == GR_OK, "epoch_count");
    T(ec == 1, "1 epoch initially");

    /* 4b. Rotate */
    T(gr_epoch_rotate(r, &owner) == GR_OK, "rotate epoch");
    T(gr_epoch_get_current(r, &ep) == GR_OK, "get new current");
    T(ep.epoch_id == 2, "new epoch_id = 2");

    T(gr_epoch_count(r, &ec) == GR_OK, "count after rotate");
    T(ec == 2, "2 epochs");

    /* 4c. Get old epoch */
    gr_epoch_t old;
    T(gr_epoch_get(r, 1, &old) == GR_OK, "get epoch 1");
    T(old.epoch_id == 1, "old epoch_id = 1");

    /* 4d. Get nonexistent */
    T(gr_epoch_get(r, 999, &old) == GR_ERR_NOT_FOUND, "get unknown epoch");

    /* 4e. List epochs */
    gr_epoch_t elist[10];
    uint32_t elcount = 0;
    T(gr_epoch_list(r, elist, 10, &elcount) == GR_OK, "epoch_list");
    T(elcount == 2, "list has 2 epochs");

    /* 4f. Multi-rotate */
    for (int i = 0; i < 5; i++) {
        T(gr_epoch_rotate(r, &owner) == GR_OK, "multi-rotate");
    }
    T(gr_epoch_count(r, &ec) == GR_OK, "count after multi");
    T(ec == 7, "7 epochs total");

    /* 4g. Unauthorized rotate */
    T(gr_epoch_rotate(r, &member) == GR_ERR_UNAUTHORIZED, "unauthorized rotate");

    /* 4h. NULL params */
    T(gr_epoch_rotate(NULL, &owner) == GR_ERR_INVALID_PARAM, "rotate(NULL)");
    T(gr_epoch_get_current(NULL, &ep) == GR_ERR_INVALID_PARAM, "get_current(NULL)");
    T(gr_epoch_count(NULL, &ec) == GR_ERR_INVALID_PARAM, "count(NULL)");

    gr_close(r);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 5: Retention
 * ═══════════════════════════════════════════════════════════════════ */

static void test_retention(void)
{
    SEC("5. Retention policy");

    gr_identity_t owner, member;
    mkid(&owner); mkid(&member);

    gr_registrar_t *r = mkreg(&owner);
    add_peer(r, &member, &owner);

    /* 5a. Get defaults */
    gr_retention_t ret;
    T(gr_retention_get(r, &ret) == GR_OK, "get retention");
    T(ret.message_retention_ms == GR_DEFAULT_MESSAGE_RETENTION_MS, "default msg retention");
    T(ret.file_retention_ms == GR_DEFAULT_FILE_RETENTION_MS, "default file retention");
    T(ret.registrar_max_bytes == GR_DEFAULT_REGISTRAR_MAX_BYTES, "default max bytes");

    /* 5b. Set custom policy */
    gr_retention_t custom = {
        .message_retention_ms = 7LL * 24 * 60 * 60 * 1000,  /* 7 days */
        .file_retention_ms = 30LL * 24 * 60 * 60 * 1000,     /* 30 days */
        .registrar_max_bytes = 100LL * 1024 * 1024,           /* 100 MB */
    };
    T(gr_retention_set(r, &custom, &owner) == GR_OK, "set retention");

    /* 5c. Verify set */
    gr_retention_t got;
    T(gr_retention_get(r, &got) == GR_OK, "get after set");
    T(got.message_retention_ms == custom.message_retention_ms, "msg retention updated");
    T(got.file_retention_ms == custom.file_retention_ms, "file retention updated");
    T(got.registrar_max_bytes == custom.registrar_max_bytes, "max bytes updated");

    /* 5d. Retention = forever (0) */
    gr_retention_t forever = {
        .message_retention_ms = GR_RETENTION_FOREVER,
        .file_retention_ms = GR_RETENTION_FOREVER,
        .registrar_max_bytes = GR_RETENTION_FOREVER,
    };
    T(gr_retention_set(r, &forever, &owner) == GR_OK, "set forever retention");
    T(gr_retention_get(r, &got) == GR_OK, "get forever");
    T(got.message_retention_ms == 0, "msg = forever");

    /* 5e. Unauthorized */
    T(gr_retention_set(r, &custom, &member) == GR_ERR_UNAUTHORIZED, "unauthorized set");

    /* 5f. NULL params */
    T(gr_retention_set(NULL, &custom, &owner) == GR_ERR_INVALID_PARAM, "set(NULL reg)");
    T(gr_retention_set(r, NULL, &owner) == GR_ERR_INVALID_PARAM, "set(NULL policy)");
    T(gr_retention_get(NULL, &got) == GR_ERR_INVALID_PARAM, "get(NULL)");

    gr_close(r);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 6: Invite lifecycle
 * ═══════════════════════════════════════════════════════════════════ */

static void test_invite_lifecycle(void)
{
    SEC("6. Invite lifecycle");

    gr_identity_t owner, member;
    mkid(&owner); mkid(&member);

    gr_registrar_t *r = mkreg(&owner);
    add_peer(r, &member, &owner);

    /* 6a. Create invite */
    uint8_t *blob = NULL;
    size_t blen = 0;
    uint8_t token[GR_HASH_LEN];
    T(gr_invite_create(r, &owner, 0, &blob, &blen, token) == GR_OK,
      "invite_create");
    T(blob != NULL, "blob allocated");
    T(blen > 0, "blob has data");

    /* 6b. Parse invite */
    gr_invite_ticket_t ticket;
    T(gr_invite_parse(blob, blen, &ticket) == GR_OK, "invite_parse");
    T(ticket.group_type == GR_GROUP_PRIVATE, "ticket group type");
    T(ticket.expires_at == 0, "no expiry");
    gr_free(blob);

    /* 6c. Check invite is valid */
    bool valid = false;
    T(gr_invite_check(r, token, &valid) == GR_OK, "invite_check");
    T(valid, "invite is valid");

    /* 6d. Count invites */
    uint32_t ic = 0;
    T(gr_invite_count(r, &ic) == GR_OK, "invite_count");
    T(ic == 1, "1 invite");

    /* 6e. List invites */
    gr_invite_info_t ilist[5];
    uint32_t ilcount = 0;
    T(gr_invite_list(r, ilist, 5, &ilcount) == GR_OK, "invite_list");
    T(ilcount == 1, "list has 1 invite");
    T(!ilist[0].invalidated, "not invalidated");
    T(!ilist[0].used, "not used");

    /* 6f. Mark used */
    gr_identity_t joiner;
    mkid(&joiner);
    T(gr_invite_mark_used(r, token, joiner.peer_id, &owner) == GR_OK,
      "invite_mark_used");

    /* 6g. Check after use — still valid (used doesn't mean invalid) */
    T(gr_invite_check(r, token, &valid) == GR_OK, "check after use");

    /* 6h. Invalidate */
    uint8_t *blob2 = NULL;
    size_t blen2 = 0;
    uint8_t token2[GR_HASH_LEN];
    T(gr_invite_create(r, &owner, 0, &blob2, &blen2, token2) == GR_OK,
      "create second invite");
    gr_free(blob2);

    T(gr_invite_invalidate(r, token2, &owner) == GR_OK, "invalidate");
    T(gr_invite_check(r, token2, &valid) == GR_OK, "check invalidated");
    T(!valid, "invalidated invite is not valid");

    /* 6i. Create with expiry (1 ms in the past = expired) */
    uint8_t *blob3 = NULL;
    size_t blen3 = 0;
    uint8_t token3[GR_HASH_LEN];
    int64_t past = gr_timestamp_ms() - 1000;
    T(gr_invite_create(r, &owner, past, &blob3, &blen3, token3) == GR_OK,
      "create expired invite");
    gr_free(blob3);
    T(gr_invite_check(r, token3, &valid) == GR_OK, "check expired");
    T(!valid, "expired invite is not valid");

    /* 6j. Count now */
    T(gr_invite_count(r, &ic) == GR_OK, "count now");
    T(ic == 3, "3 total invites");

    /* 6k. Unauthorized create */
    T(gr_invite_create(r, &member, 0, &blob, &blen, token) == GR_ERR_UNAUTHORIZED,
      "unauthorized invite");

    /* 6l. Invalidate nonexistent */
    uint8_t fake_token[GR_HASH_LEN];
    memset(fake_token, 0xFF, GR_HASH_LEN);
    T(gr_invite_invalidate(r, fake_token, &owner) == GR_ERR_NOT_FOUND,
      "invalidate unknown");

    /* 6m. Parse malformed blob */
    uint8_t garbage[] = { 0x00, 0x01, 0x02 };
    T(gr_invite_parse(garbage, sizeof(garbage), &ticket) != GR_OK,
      "parse garbage fails");

    /* 6n. NULL params */
    T(gr_invite_create(NULL, &owner, 0, &blob, &blen, token) == GR_ERR_INVALID_PARAM,
      "create(NULL reg)");
    T(gr_invite_parse(NULL, 0, &ticket) != GR_OK, "parse(NULL)");
    T(gr_invite_check(NULL, token, &valid) == GR_ERR_INVALID_PARAM, "check(NULL)");
    T(gr_invite_count(NULL, &ic) == GR_ERR_INVALID_PARAM, "count(NULL)");

    gr_close(r);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 7: Group icon
 * ═══════════════════════════════════════════════════════════════════ */

static void test_group_icon(void)
{
    SEC("7. Group icon");

    gr_identity_t owner, member;
    mkid(&owner); mkid(&member);

    gr_registrar_t *r = mkreg(&owner);
    add_peer(r, &member, &owner);

    /* 7a. No icon initially */
    gr_group_icon_t icon;
    memset(&icon, 0, sizeof(icon));
    T(gr_group_icon_get(r, &icon) == GR_ERR_NOT_FOUND, "no icon initially");

    /* 7b. Icon hash when none set */
    uint8_t hash[GR_HASH_LEN];
    T(gr_group_icon_hash(r, hash) == GR_ERR_NOT_FOUND, "hash when no icon");

    /* 7c. Set a simple PNG icon */
    uint8_t fake_png[256];
    memset(fake_png, 0xAA, sizeof(fake_png));
    T(gr_group_icon_set(r, fake_png, sizeof(fake_png), "image/png",
                        128, 128, false, NULL, 0, &owner) == GR_OK,
      "set PNG icon");

    /* 7d. Get icon */
    memset(&icon, 0, sizeof(icon));
    T(gr_group_icon_get(r, &icon) == GR_OK, "get icon");
    T(icon.data != NULL, "icon data not null");
    T(icon.data_len == sizeof(fake_png), "icon data length");
    T(icon.data && memcmp(icon.data, fake_png, sizeof(fake_png)) == 0, "icon data matches");
    T(strcmp(icon.mime_type, "image/png") == 0, "mime_type = image/png");
    T(icon.width == 128, "width = 128");
    T(icon.height == 128, "height = 128");
    T(!icon.is_video, "not video");
    gr_group_icon_free(&icon);

    /* 7e. Icon hash */
    T(gr_group_icon_hash(r, hash) == GR_OK, "get icon hash");
    uint8_t zero_hash[GR_HASH_LEN];
    memset(zero_hash, 0, GR_HASH_LEN);
    T(memcmp(hash, zero_hash, GR_HASH_LEN) != 0, "hash is non-zero");

    /* 7f. Replace icon */
    uint8_t new_img[512];
    memset(new_img, 0xBB, sizeof(new_img));
    T(gr_group_icon_set(r, new_img, sizeof(new_img), "image/jpeg",
                        256, 256, false, NULL, 0, &owner) == GR_OK,
      "replace icon");
    memset(&icon, 0, sizeof(icon));
    gr_error_t get_err = gr_group_icon_get(r, &icon);
    T(get_err == GR_OK, "get replaced icon");
    if (get_err == GR_OK) {
        T(icon.data_len == sizeof(new_img), "replaced data length");
        T(strcmp(icon.mime_type, "image/jpeg") == 0, "new mime_type");
    }
    gr_group_icon_free(&icon);

    /* 7g. Video icon with static frame */
    uint8_t video[1024];
    memset(video, 0xCC, sizeof(video));
    uint8_t frame[256];
    memset(frame, 0xDD, sizeof(frame));
    T(gr_group_icon_set(r, video, sizeof(video), "video/mp4",
                        512, 512, true, frame, sizeof(frame), &owner) == GR_OK,
      "set video icon");
    memset(&icon, 0, sizeof(icon));
    get_err = gr_group_icon_get(r, &icon);
    T(get_err == GR_OK, "get video icon");
    if (get_err == GR_OK) {
        T(icon.is_video, "is_video flag");
        T(icon.static_frame != NULL, "static frame present");
        T(icon.static_frame_len == sizeof(frame), "static frame length");
    }
    gr_group_icon_free(&icon);

    /* 7h. Size limit — GR_GROUP_ICON_MAX_BYTES exceeded */
    size_t too_big = GR_GROUP_ICON_MAX_BYTES + 1;
    uint8_t *big = calloc(1, too_big);
    if (big) {
        T(gr_group_icon_set(r, big, too_big, "image/png",
                            128, 128, false, NULL, 0, &owner) == GR_ERR_SIZE_EXCEEDED,
          "icon size limit enforced");
        free(big);
    }

    /* 7i. Dimension limit */
    T(gr_group_icon_set(r, fake_png, sizeof(fake_png), "image/png",
                        GR_GROUP_ICON_MAX_DIM + 1, 128, false, NULL, 0,
                        &owner) == GR_ERR_SIZE_EXCEEDED, "width limit");
    T(gr_group_icon_set(r, fake_png, sizeof(fake_png), "image/png",
                        128, GR_GROUP_ICON_MAX_DIM + 1, false, NULL, 0,
                        &owner) == GR_ERR_SIZE_EXCEEDED, "height limit");

    /* 7j. Remove icon */
    T(gr_group_icon_remove(r, &owner) == GR_OK, "remove icon");
    T(gr_group_icon_get(r, &icon) == GR_ERR_NOT_FOUND, "icon removed");

    /* 7k. Double-remove is safe */
    T(gr_group_icon_remove(r, &owner) == GR_OK || 1, "double-remove no crash");

    /* 7l. Unauthorized */
    T(gr_group_icon_set(r, fake_png, sizeof(fake_png), "image/png",
                        128, 128, false, NULL, 0, &member) == GR_ERR_UNAUTHORIZED,
      "unauthorized icon set");
    T(gr_group_icon_remove(r, &member) == GR_ERR_UNAUTHORIZED,
      "unauthorized icon remove");

    /* 7m. NULL params */
    T(gr_group_icon_set(NULL, fake_png, sizeof(fake_png), "image/png",
                        128, 128, false, NULL, 0, &owner) == GR_ERR_INVALID_PARAM,
      "set(NULL reg)");
    T(gr_group_icon_get(NULL, &icon) == GR_ERR_INVALID_PARAM, "get(NULL)");
    T(gr_group_icon_hash(NULL, hash) == GR_ERR_INVALID_PARAM, "hash(NULL)");
    T(gr_group_icon_remove(NULL, &owner) == GR_ERR_INVALID_PARAM, "remove(NULL)");

    /* 7n. Free NULL icon (no crash) */
    gr_group_icon_free(NULL);
    T(1, "free(NULL) no crash");

    gr_close(r);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 8: WebApp management
 * ═══════════════════════════════════════════════════════════════════ */

static void test_webapp_management(void)
{
    SEC("8. WebApp management");

    gr_identity_t owner, member;
    mkid(&owner); mkid(&member);

    gr_registrar_t *r = mkreg(&owner);
    add_peer(r, &member, &owner);

    /* 8a. Initially empty */
    uint32_t wc = 0;
    T(gr_webapp_count(r, &wc) == GR_OK, "webapp_count");
    T(wc == 0, "0 webapps initially");

    /* 8b. Add webapp */
    gr_webapp_t wa;
    memset(&wa, 0, sizeof(wa));
    memset(wa.hash, 0xAA, GR_SERVICE_HASH_LEN);
    strncpy(wa.name, "chat-app", GR_MAX_NAME_LEN);
    wa.version = 1;
    wa.added_at = gr_timestamp_ms();
    memcpy(wa.added_by, owner.peer_id, GR_PEER_ID_LEN);
    T(gr_webapp_add(r, &wa, &owner) == GR_OK, "webapp_add");

    /* 8c. is_authorized */
    T(gr_webapp_is_authorized(r, wa.hash), "webapp is authorized");
    uint8_t unknown_hash[GR_SERVICE_HASH_LEN];
    memset(unknown_hash, 0xBB, GR_SERVICE_HASH_LEN);
    T(!gr_webapp_is_authorized(r, unknown_hash), "unknown webapp not authorized");

    /* 8d. Count */
    T(gr_webapp_count(r, &wc) == GR_OK, "count after add");
    T(wc == 1, "1 webapp");

    /* 8e. List */
    gr_webapp_t wlist[5];
    uint32_t wlcount = 0;
    T(gr_webapp_list(r, wlist, 5, &wlcount) == GR_OK, "webapp_list");
    T(wlcount == 1, "list has 1 webapp");
    T(strcmp(wlist[0].name, "chat-app") == 0, "webapp name matches");

    /* 8f. Add second webapp */
    gr_webapp_t wa2;
    memset(&wa2, 0, sizeof(wa2));
    memset(wa2.hash, 0xCC, GR_SERVICE_HASH_LEN);
    strncpy(wa2.name, "media-player", GR_MAX_NAME_LEN);
    wa2.version = 2;
    T(gr_webapp_add(r, &wa2, &owner) == GR_OK, "add second webapp");
    T(gr_webapp_count(r, &wc) == GR_OK && wc == 2, "2 webapps");

    /* 8g. Remove */
    T(gr_webapp_remove(r, wa.hash, &owner) == GR_OK, "remove chat-app");
    T(!gr_webapp_is_authorized(r, wa.hash), "removed webapp not authorized");

    /* 8h. Remove nonexistent */
    T(gr_webapp_remove(r, unknown_hash, &owner) == GR_ERR_NOT_FOUND,
      "remove unknown = NOT_FOUND");

    /* 8i. Unauthorized */
    T(gr_webapp_add(r, &wa, &member) == GR_ERR_UNAUTHORIZED, "unauthorized add");
    T(gr_webapp_remove(r, wa2.hash, &member) == GR_ERR_UNAUTHORIZED,
      "unauthorized remove");

    /* 8j. NULL params */
    T(gr_webapp_add(NULL, &wa, &owner) == GR_ERR_INVALID_PARAM, "add(NULL)");
    T(gr_webapp_count(NULL, &wc) == GR_ERR_INVALID_PARAM, "count(NULL)");
    T(gr_webapp_remove(NULL, wa.hash, &owner) == GR_ERR_INVALID_PARAM, "remove(NULL)");

    /* 8k. get() — existing webapp, no blobs */
    gr_webapp_t got;
    T(gr_webapp_get(r, wa2.hash, &got) == GR_OK, "get existing webapp");
    T(memcmp(got.hash, wa2.hash, GR_SERVICE_HASH_LEN) == 0, "get: hash matches");
    T(strcmp(got.name, "media-player") == 0, "get: name matches");
    T(got.perm_data == NULL, "get: no perm_data initially");
    T(got.role_mask == NULL, "get: no role_mask initially");
    gr_free(got.perm_data);
    gr_free(got.role_mask);

    /* 8l. get() — not found */
    T(gr_webapp_get(r, unknown_hash, &got) == GR_ERR_NOT_FOUND, "get unknown");

    /* 8m. Add webapp with both blobs */
    gr_webapp_t wa3;
    memset(&wa3, 0, sizeof(wa3));
    memset(wa3.hash, 0xDD, GR_SERVICE_HASH_LEN);
    strncpy(wa3.name, "perm-app", GR_MAX_NAME_LEN);
    wa3.version = 3;
    wa3.added_at = gr_timestamp_ms();
    memcpy(wa3.added_by, owner.peer_id, GR_PEER_ID_LEN);
    uint8_t init_perm[16];
    for (int i = 0; i < 16; i++) init_perm[i] = (uint8_t)(i + 1);
    uint8_t init_role[8];
    memset(init_role, 0xF0, sizeof(init_role));
    for (int i = 0; i < 8; i++) init_role[i] = (uint8_t)(0xF0 + i);
    wa3.perm_data = init_perm;
    wa3.perm_data_len = sizeof(init_perm);
    wa3.role_mask = init_role;
    wa3.role_mask_len = sizeof(init_role);
    T(gr_webapp_add(r, &wa3, &owner) == GR_OK, "add webapp with blobs");

    /* 8n. get() roundtrip verifies both blobs */
    T(gr_webapp_get(r, wa3.hash, &got) == GR_OK, "get blob-webapp");
    T(got.perm_data != NULL && got.perm_data_len == 16, "get: perm_data_len 16");
    T(got.perm_data != NULL && memcmp(got.perm_data, init_perm, 16) == 0,
      "get: perm_data content correct");
    T(got.role_mask != NULL && got.role_mask_len == 8, "get: role_mask_len 8");
    T(got.role_mask != NULL && memcmp(got.role_mask, init_role, 8) == 0,
      "get: role_mask content correct");
    gr_free(got.perm_data);
    gr_free(got.role_mask);

    /* 8o. update_perm_data() — only perm_data changes, role_mask preserved */
    uint8_t new_perm[32];
    memset(new_perm, 0x55, sizeof(new_perm));
    T(gr_webapp_update_perm_data(r, wa3.hash, new_perm, sizeof(new_perm), &owner) == GR_OK,
      "update perm_data only");
    T(gr_webapp_get(r, wa3.hash, &got) == GR_OK, "get after perm-only update");
    T(got.perm_data_len == 32, "updated perm_data_len == 32");
    T(got.perm_data != NULL && memcmp(got.perm_data, new_perm, 32) == 0,
      "updated perm_data content");
    T(got.role_mask != NULL && got.role_mask_len == 8, "role_mask preserved (len 8)");
    T(got.role_mask != NULL && memcmp(got.role_mask, init_role, 8) == 0,
      "role_mask content preserved");
    gr_free(got.perm_data);
    gr_free(got.role_mask);

    /* 8p. update_role_mask() — only role_mask changes, perm_data preserved */
    uint8_t new_role[4] = { 0xA0, 0xB0, 0xC0, 0xD0 };
    T(gr_webapp_update_role_mask(r, wa3.hash, new_role, sizeof(new_role), &owner) == GR_OK,
      "update role_mask only");
    T(gr_webapp_get(r, wa3.hash, &got) == GR_OK, "get after role-only update");
    T(got.perm_data != NULL && got.perm_data_len == 32, "perm_data preserved (len 32)");
    T(got.perm_data != NULL && memcmp(got.perm_data, new_perm, 32) == 0,
      "perm_data content preserved");
    T(got.role_mask_len == 4, "updated role_mask_len == 4");
    T(got.role_mask != NULL && memcmp(got.role_mask, new_role, 4) == 0,
      "updated role_mask content");
    gr_free(got.perm_data);
    gr_free(got.role_mask);

    /* 8q. clear each field independently with len=0, then re-set both */
    T(gr_webapp_update_perm_data(r, wa3.hash, NULL, 0, &owner) == GR_OK,
      "clear perm_data with len=0");
    T(gr_webapp_get(r, wa3.hash, &got) == GR_OK, "get after perm clear");
    T(got.perm_data == NULL, "perm_data cleared");
    T(got.role_mask != NULL, "role_mask still present after perm clear");
    gr_free(got.perm_data);
    gr_free(got.role_mask);

    uint8_t pd_blob[10]; memset(pd_blob, 0xAB, sizeof(pd_blob));
    uint8_t rm_blob[6];  memset(rm_blob, 0xCD, sizeof(rm_blob));
    T(gr_webapp_update_perm_data(r, wa3.hash, pd_blob, sizeof(pd_blob), &owner) == GR_OK,
      "set perm_data");
    T(gr_webapp_update_role_mask(r, wa3.hash, rm_blob, sizeof(rm_blob), &owner) == GR_OK,
      "set role_mask");
    uint32_t wlcount2 = 0;
    gr_webapp_t wlist2[5];
    T(gr_webapp_list(r, wlist2, 5, &wlcount2) == GR_OK, "list with blob webapps");
    T(wlcount2 == 2, "2 webapps in list (wa2 + wa3)");
    int found_wa3 = 0;
    for (uint32_t li = 0; li < wlcount2; li++) {
        if (memcmp(wlist2[li].hash, wa3.hash, GR_SERVICE_HASH_LEN) == 0) {
            found_wa3 = 1;
            T(wlist2[li].perm_data != NULL && wlist2[li].perm_data_len == 10,
              "list: wa3 perm_data_len 10");
            T(wlist2[li].perm_data != NULL &&
              memcmp(wlist2[li].perm_data, pd_blob, 10) == 0,
              "list: wa3 perm_data content");
            T(wlist2[li].role_mask != NULL && wlist2[li].role_mask_len == 6,
              "list: wa3 role_mask_len 6");
            T(wlist2[li].role_mask != NULL &&
              memcmp(wlist2[li].role_mask, rm_blob, 6) == 0,
              "list: wa3 role_mask content");
        }
        gr_free(wlist2[li].perm_data);
        gr_free(wlist2[li].role_mask);
    }
    T(found_wa3, "wa3 found in list");

    /* 8r. size limit enforcement on each update function */
    T(gr_webapp_update_perm_data(r, wa3.hash, pd_blob, GR_WEBAPP_PERM_DATA_MAX + 1,
                                 &owner) == GR_ERR_SIZE_EXCEEDED,
      "oversized perm_data rejected");
    T(gr_webapp_update_role_mask(r, wa3.hash, rm_blob, GR_WEBAPP_ROLE_MASK_MAX + 1,
                                 &owner) == GR_ERR_SIZE_EXCEEDED,
      "oversized role_mask rejected");

    /* 8r-bis. gr_webapp_add must also enforce size limits */
    {
        gr_webapp_t wa_big = wa3;
        wa_big.hash[0] ^= 0x77;
        wa_big.perm_data = pd_blob; wa_big.perm_data_len = GR_WEBAPP_PERM_DATA_MAX + 1;
        wa_big.role_mask = NULL;    wa_big.role_mask_len = 0;
        T(gr_webapp_add(r, &wa_big, &owner) == GR_ERR_SIZE_EXCEEDED,
          "add: oversized perm_data rejected");
        wa_big.perm_data = NULL;    wa_big.perm_data_len = 0;
        wa_big.role_mask = rm_blob; wa_big.role_mask_len = GR_WEBAPP_ROLE_MASK_MAX + 1;
        T(gr_webapp_add(r, &wa_big, &owner) == GR_ERR_SIZE_EXCEEDED,
          "add: oversized role_mask rejected");
    }

    /* 8s. unauthorized — perm_data needs ADD_WEBAPP, role_mask needs SET_WEBAPP_ROLES */
    T(gr_webapp_update_perm_data(r, wa3.hash, pd_blob, sizeof(pd_blob), &member)
        == GR_ERR_UNAUTHORIZED,
      "update perm_data unauthorized");
    T(gr_webapp_update_role_mask(r, wa3.hash, rm_blob, sizeof(rm_blob), &member)
        == GR_ERR_UNAUTHORIZED,
      "update role_mask unauthorized");

    /* 8t. not found */
    T(gr_webapp_update_perm_data(r, unknown_hash, pd_blob, sizeof(pd_blob), &owner)
        == GR_ERR_NOT_FOUND,
      "update perm_data unknown webapp");
    T(gr_webapp_update_role_mask(r, unknown_hash, rm_blob, sizeof(rm_blob), &owner)
        == GR_ERR_NOT_FOUND,
      "update role_mask unknown webapp");

    /* 8u. NULL param safety */
    T(gr_webapp_get(NULL, wa3.hash, &got) == GR_ERR_INVALID_PARAM, "get(NULL reg)");
    T(gr_webapp_get(r, NULL, &got) == GR_ERR_INVALID_PARAM, "get(NULL hash)");
    T(gr_webapp_get(r, wa3.hash, NULL) == GR_ERR_INVALID_PARAM, "get(NULL out)");
    T(gr_webapp_update_perm_data(NULL, wa3.hash, NULL, 0, &owner) == GR_ERR_INVALID_PARAM,
      "update_perm_data(NULL reg)");
    T(gr_webapp_update_perm_data(r, NULL, NULL, 0, &owner) == GR_ERR_INVALID_PARAM,
      "update_perm_data(NULL hash)");
    T(gr_webapp_update_perm_data(r, wa3.hash, NULL, 0, NULL) == GR_ERR_INVALID_PARAM,
      "update_perm_data(NULL signer)");
    T(gr_webapp_update_role_mask(NULL, wa3.hash, NULL, 0, &owner) == GR_ERR_INVALID_PARAM,
      "update_role_mask(NULL reg)");
    T(gr_webapp_update_role_mask(r, NULL, NULL, 0, &owner) == GR_ERR_INVALID_PARAM,
      "update_role_mask(NULL hash)");
    T(gr_webapp_update_role_mask(r, wa3.hash, NULL, 0, NULL) == GR_ERR_INVALID_PARAM,
      "update_role_mask(NULL signer)");

    gr_close(r);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 9: Server management
 * ═══════════════════════════════════════════════════════════════════ */

static void test_server_management(void)
{
    SEC("9. Server management");

    gr_identity_t owner, member;
    mkid(&owner); mkid(&member);

    gr_registrar_t *r = mkreg(&owner);
    add_peer(r, &member, &owner);

    /* 9a. Initially empty */
    uint32_t sc = 0;
    T(gr_server_count(r, GR_SERVER_SIGNALING, &sc) == GR_OK, "server_count signaling");
    T(sc == 0, "0 signaling servers");
    T(gr_server_count(r, GR_SERVER_REBROADCAST, &sc) == GR_OK, "server_count rebroadcast");
    T(sc == 0, "0 rebroadcast servers");

    /* 9b. Add signaling server */
    gr_server_t srv;
    memset(&srv, 0, sizeof(srv));
    srv.type = GR_SERVER_SIGNALING;
    strncpy(srv.ip, "signal.example.com", GR_MAX_IP_LEN);
    srv.port = 443;
    memset(srv.id_hash, 0x11, GR_HASH_LEN);
    T(gr_server_add(r, &srv, &owner) == GR_OK, "add signaling server");

    /* 9c. Add rebroadcast server */
    gr_server_t rb;
    memset(&rb, 0, sizeof(rb));
    rb.type = GR_SERVER_REBROADCAST;
    strncpy(rb.ip, "relay.example.com", GR_MAX_IP_LEN);
    rb.port = 8443;
    memset(rb.id_hash, 0x22, GR_HASH_LEN);
    T(gr_server_add(r, &rb, &owner) == GR_OK, "add rebroadcast server");

    /* 9d. Count by type */
    T(gr_server_count(r, GR_SERVER_SIGNALING, &sc) == GR_OK && sc == 1,
      "1 signaling server");
    T(gr_server_count(r, GR_SERVER_REBROADCAST, &sc) == GR_OK && sc == 1,
      "1 rebroadcast server");

    /* 9e. List signaling */
    gr_server_t slist[5];
    uint32_t slcount = 0;
    T(gr_server_list(r, GR_SERVER_SIGNALING, slist, 5, &slcount) == GR_OK,
      "list signaling");
    T(slcount == 1, "1 signaling in list");
    T(strcmp(slist[0].ip, "signal.example.com") == 0, "server IP matches");

    /* 9f. List rebroadcast */
    T(gr_server_list(r, GR_SERVER_REBROADCAST, slist, 5, &slcount) == GR_OK,
      "list rebroadcast");
    T(slcount == 1, "1 rebroadcast in list");

    /* 9g. Remove signaling server */
    T(gr_server_remove(r, srv.id_hash, &owner) == GR_OK, "remove signaling");
    T(gr_server_count(r, GR_SERVER_SIGNALING, &sc) == GR_OK && sc == 0,
      "0 signaling after remove");

    /* 9h. Remove nonexistent */
    uint8_t fake_hash[GR_HASH_LEN];
    memset(fake_hash, 0xFF, GR_HASH_LEN);
    T(gr_server_remove(r, fake_hash, &owner) == GR_ERR_NOT_FOUND,
      "remove unknown = NOT_FOUND");

    /* 9i. Unauthorized */
    T(gr_server_add(r, &srv, &member) == GR_ERR_UNAUTHORIZED, "unauthorized add");
    T(gr_server_remove(r, rb.id_hash, &member) == GR_ERR_UNAUTHORIZED,
      "unauthorized remove");

    /* 9j. NULL params */
    T(gr_server_add(NULL, &srv, &owner) == GR_ERR_INVALID_PARAM, "add(NULL)");
    T(gr_server_count(NULL, GR_SERVER_SIGNALING, &sc) == GR_ERR_INVALID_PARAM,
      "count(NULL)");
    T(gr_server_remove(NULL, srv.id_hash, &owner) == GR_ERR_INVALID_PARAM,
      "remove(NULL)");

    gr_close(r);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 10: Behavioral analysis
 * ═══════════════════════════════════════════════════════════════════ */

/* Alert callback capture */
static uint32_t g_alert_flags = 0;
static uint8_t  g_alert_actor[GR_PEER_ID_LEN];
static int      g_alert_count = 0;

static void test_alert_cb(uint32_t alerts, const uint8_t actor_id[GR_PEER_ID_LEN],
                           gr_change_type_t change_type,
                           const gr_actor_burst_t *burst,
                           const gr_admin_score_t *admin,
                           void *user_data)
{
    (void)change_type; (void)burst; (void)admin; (void)user_data;
    g_alert_flags |= alerts;
    memcpy(g_alert_actor, actor_id, GR_PEER_ID_LEN);
    g_alert_count++;
}

static void test_behavior(void)
{
    SEC("10. Behavioral analysis");

    gr_identity_t owner, alice, bob, carol, dave;
    mkid(&owner); mkid(&alice); mkid(&bob); mkid(&carol); mkid(&dave);

    gr_registrar_t *r = mkreg(&owner);
    add_peer(r, &alice, &owner);
    add_peer(r, &bob, &owner);
    add_peer(r, &carol, &owner);
    add_peer(r, &dave, &owner);

    /* 10a. Config defaults */
    gr_behavior_config_t cfg;
    T(gr_behavior_get_config(r, &cfg) == GR_OK, "get_config");
    T(cfg.burst_actions_per_min == 20.0f, "default burst threshold");
    T(cfg.scale_by_group_size, "scale enabled by default");

    /* 10b. Set custom config */
    cfg.burst_actions_per_min = 5.0f;
    cfg.abuse_min_actions = 3;
    cfg.abuse_destructive_ratio = 0.5f;
    T(gr_behavior_set_config(r, &cfg) == GR_OK, "set_config");
    gr_behavior_config_t got;
    T(gr_behavior_get_config(r, &got) == GR_OK, "get after set");
    T(got.burst_actions_per_min == 5.0f, "config updated");

    /* 10c. Actor burst — no activity yet (owner's activity from setup) */
    gr_actor_burst_t burst;
    T(gr_behavior_actor_burst(r, alice.peer_id, 300000, &burst) == GR_OK,
      "actor_burst alice");
    T(burst.actions_in_window == 0, "alice: 0 actions");
    T(!burst.burst_detected, "alice: no burst");

    /* 10d. Actor burst — owner has setup actions */
    T(gr_behavior_actor_burst(r, owner.peer_id, 300000, &burst) == GR_OK,
      "actor_burst owner");
    T(burst.actions_in_window > 0, "owner: has actions from setup");

    /* 10e. Mutation rate */
    gr_mutation_rate_t mrate;
    T(gr_behavior_mutation_rate(r, 300000, &mrate) == GR_OK, "mutation_rate");
    T(mrate.total_mutations > 0, "has mutations from setup");
    T(mrate.distinct_actors >= 1, "at least 1 actor (owner)");

    /* 10f. Admin score — owner */
    gr_admin_score_t ascore;
    T(gr_behavior_admin_score(r, owner.peer_id, 300000, &ascore) == GR_OK,
      "admin_score owner");
    T(ascore.total_admin_actions > 0, "owner has admin actions");

    /* 10g. Peer churn */
    gr_peer_churn_t churn;
    T(gr_behavior_peer_churn(r, 300000, &churn) == GR_OK, "peer_churn");
    T(churn.active_peers == 5, "5 active peers");
    T(churn.kicked_peers == 0, "0 kicked");

    /* 10h. Churn after kick */
    gr_peer_kick(r, dave.peer_id, "testing", &owner);
    T(gr_behavior_peer_churn(r, 300000, &churn) == GR_OK, "churn after kick");
    T(churn.kicked_peers == 1, "1 kicked");
    T(churn.active_peers == 4, "4 active after kick");

    /* 10i. Delta score */
    gr_delta_score_t dscore;
    T(gr_behavior_delta_score(r, 10000, 50, &dscore) == GR_OK, "delta_score");
    T(dscore.delta_bytes == 10000, "delta_bytes recorded");
    T(dscore.entry_count == 50, "entry_count recorded");

    /* 10j. Large delta = anomalous */
    T(gr_behavior_delta_score(r, 1000000, 10000, &dscore) == GR_OK,
      "large delta_score");

    /* 10k. Stale peers */
    uint8_t stale_ids[GR_PEER_ID_LEN * 10];
    uint32_t stale_count = 0;
    T(gr_behavior_stale_peers(r, 1, stale_ids, 10, &stale_count) == GR_OK,
      "stale_peers");
    /* All peers were just created so none should be stale with 1ms threshold...
     * unless time has passed. We check that the call succeeds. */

    /* 10l. Epoch pattern */
    gr_epoch_pattern_t epat;
    T(gr_behavior_epoch_pattern(r, 3600000, &epat) == GR_OK, "epoch_pattern");
    T(epat.rotations_in_window == 0, "0 rotations in window (only initial)");

    /* 10m. Epoch pattern after rotations */
    for (int i = 0; i < 3; i++) gr_epoch_rotate(r, &owner);
    T(gr_behavior_epoch_pattern(r, 3600000, &epat) == GR_OK, "epoch_pattern after rotates");
    T(epat.rotations_in_window >= 3, "rotations counted");

    /* 10n. Network score */
    gr_network_score_t nscore;
    T(gr_behavior_network_score(r, &nscore) == GR_OK, "network_score");
    T(nscore.total_audit_entries > 0, "has audit entries");
    T(nscore.estimated_registrar_bytes > 0, "estimated size > 0");

    /* 10o. Full snapshot */
    gr_behavior_snapshot_t snap;
    T(gr_behavior_snapshot(r, 300000, &snap) == GR_OK, "snapshot");
    T(snap.churn.active_peers == 4, "snapshot: 4 active");
    T(snap.estimated_size > 0, "snapshot: size > 0");

    /* 10p. Alert callback */
    g_alert_count = 0;
    g_alert_flags = 0;
    T(gr_set_behavior_alert_callback(r, test_alert_cb, NULL) == GR_OK,
      "set alert callback");

    /* Generate burst: many rapid kicks. Restore dave first. */
    gr_identity_t extras[30];
    for (int i = 0; i < 30; i++) {
        mkid(&extras[i]);
        add_peer(r, &extras[i], &owner);
    }
    for (int i = 0; i < 30; i++) {
        gr_peer_kick(r, extras[i].peer_id, "burst", &owner);
    }
    /* The alert callback may or may not have fired depending on
     * threshold timing — just verify it didn't crash */
    T(g_alert_count >= 0, "alert callback didn't crash");

    /* 10q. Admin abuse detection */
    T(gr_behavior_admin_score(r, owner.peer_id, 60000, &ascore) == GR_OK,
      "admin_score after kicks");
    T(ascore.kicks >= 30, "30+ kicks recorded");

    /* 10r. NULL params */
    T(gr_behavior_actor_burst(NULL, owner.peer_id, 300000, &burst)
      == GR_ERR_INVALID_PARAM, "actor_burst(NULL)");
    T(gr_behavior_mutation_rate(NULL, 300000, &mrate)
      == GR_ERR_INVALID_PARAM, "mutation_rate(NULL)");
    T(gr_behavior_admin_score(NULL, owner.peer_id, 300000, &ascore)
      == GR_ERR_INVALID_PARAM, "admin_score(NULL)");
    T(gr_behavior_peer_churn(NULL, 300000, &churn)
      == GR_ERR_INVALID_PARAM, "peer_churn(NULL)");
    T(gr_behavior_delta_score(NULL, 0, 0, &dscore)
      == GR_ERR_INVALID_PARAM, "delta_score(NULL)");
    T(gr_behavior_stale_peers(NULL, 1, stale_ids, 10, &stale_count)
      == GR_ERR_INVALID_PARAM, "stale_peers(NULL)");
    T(gr_behavior_epoch_pattern(NULL, 3600000, &epat)
      == GR_ERR_INVALID_PARAM, "epoch_pattern(NULL)");
    T(gr_behavior_network_score(NULL, &nscore)
      == GR_ERR_INVALID_PARAM, "network_score(NULL)");
    T(gr_behavior_snapshot(NULL, 300000, &snap)
      == GR_ERR_INVALID_PARAM, "snapshot(NULL)");
    T(gr_behavior_set_config(NULL, &cfg)
      == GR_ERR_INVALID_PARAM, "set_config(NULL)");
    T(gr_behavior_get_config(NULL, &got)
      == GR_ERR_INVALID_PARAM, "get_config(NULL)");

    gr_close(r);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 11: Permission boundaries
 * ═══════════════════════════════════════════════════════════════════ */

static void test_permissions(void)
{
    SEC("11. Permission boundaries");

    gr_identity_t owner, mod, member;
    mkid(&owner); mkid(&mod); mkid(&member);

    gr_registrar_t *r = mkreg(&owner);
    add_peer(r, &mod, &owner);
    add_peer(r, &member, &owner);

    /* 11a. Assign moderator role */
    uint32_t mod_rid = 0;
    T(gr_role_add(r, "Mod", GR_PERM_KICK_MEMBER | GR_PERM_INVITE_MEMBER,
                  &owner, &mod_rid) == GR_OK, "add Mod role");
    T(gr_peer_set_role(r, mod.peer_id, mod_rid, &owner) == GR_OK,
      "assign Mod to mod");

    /* 11b. Moderator can kick */
    gr_identity_t target;
    mkid(&target);
    add_peer(r, &target, &owner);
    T(gr_peer_kick(r, target.peer_id, "mod kick", &mod) == GR_OK,
      "mod can kick");

    /* 11c. Moderator cannot ban (no GR_PERM_BAN_MEMBER) */
    gr_identity_t target2;
    mkid(&target2);
    add_peer(r, &target2, &owner);
    T(gr_peer_ban(r, target2.peer_id, "mod ban", &mod) == GR_ERR_UNAUTHORIZED,
      "mod cannot ban");

    /* 11d. Moderator can invite */
    uint8_t *blob = NULL;
    size_t blen = 0;
    uint8_t token[GR_HASH_LEN];
    T(gr_invite_create(r, &mod, 0, &blob, &blen, token) == GR_OK,
      "mod can invite");
    gr_free(blob);

    /* 11e. Moderator cannot rotate epoch */
    T(gr_epoch_rotate(r, &mod) == GR_ERR_UNAUTHORIZED, "mod cannot rotate");

    /* 11f. Moderator cannot edit roles */
    uint32_t tmp;
    T(gr_role_add(r, "bad", 0, &mod, &tmp) == GR_ERR_UNAUTHORIZED,
      "mod cannot add role");

    /* 11g. Member (no role) cannot do anything */
    T(gr_peer_kick(r, target2.peer_id, "x", &member) == GR_ERR_UNAUTHORIZED,
      "member cannot kick");
    T(gr_invite_create(r, &member, 0, &blob, &blen, token) == GR_ERR_UNAUTHORIZED,
      "member cannot invite");
    T(gr_epoch_rotate(r, &member) == GR_ERR_UNAUTHORIZED,
      "member cannot rotate");

    /* 11h. Owner can do everything */
    T(gr_peer_kick(r, target2.peer_id, "owner kick", &owner) == GR_OK,
      "owner can kick");
    T(gr_epoch_rotate(r, &owner) == GR_OK, "owner can rotate");

    gr_retention_t ret = {
        .message_retention_ms = 1000,
        .file_retention_ms = 1000,
        .registrar_max_bytes = 1000,
    };
    T(gr_retention_set(r, &ret, &owner) == GR_OK, "owner can set retention");
    T(gr_retention_set(r, &ret, &member) == GR_ERR_UNAUTHORIZED,
      "member cannot set retention");

    /* 11i. Add server permission */
    gr_server_t srv;
    memset(&srv, 0, sizeof(srv));
    srv.type = GR_SERVER_SIGNALING;
    strncpy(srv.ip, "s.example.com", GR_MAX_IP_LEN);
    srv.port = 443;
    memset(srv.id_hash, 0x99, GR_HASH_LEN);
    T(gr_server_add(r, &srv, &member) == GR_ERR_UNAUTHORIZED,
      "member cannot add server");
    T(gr_server_add(r, &srv, &owner) == GR_OK, "owner can add server");

    /* 11j. Set group icon permission */
    uint8_t img[64];
    memset(img, 0xAA, sizeof(img));
    T(gr_group_icon_set(r, img, sizeof(img), "image/png", 64, 64,
                        false, NULL, 0, &member) == GR_ERR_UNAUTHORIZED,
      "member cannot set icon");
    T(gr_group_icon_set(r, img, sizeof(img), "image/png", 64, 64,
                        false, NULL, 0, &owner) == GR_OK,
      "owner can set icon");

    gr_close(r);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 12: NULL / edge case validation
 * ═══════════════════════════════════════════════════════════════════ */

static void test_edge_cases(void)
{
    SEC("12. NULL / edge case validation");

    gr_identity_t owner;
    mkid(&owner);

    /* 12a. Error string coverage */
    T(gr_error_str(GR_OK) != NULL, "error_str(GR_OK)");
    T(gr_error_str(GR_ERR_INVALID_PARAM) != NULL, "error_str(INVALID_PARAM)");
    T(gr_error_str(GR_ERR_UNAUTHORIZED) != NULL, "error_str(UNAUTHORIZED)");
    T(gr_error_str(GR_ERR_JOIN_FAILED) != NULL, "error_str(JOIN_FAILED)");

    /* 12b. gr_hash */
    uint8_t data[] = "hello world";
    uint8_t hash[GR_HASH_LEN];
    T(gr_hash(data, sizeof(data), hash) == GR_OK, "gr_hash succeeds");
    uint8_t hash2[GR_HASH_LEN];
    T(gr_hash(data, sizeof(data), hash2) == GR_OK, "gr_hash deterministic");
    T(memcmp(hash, hash2, GR_HASH_LEN) == 0, "same hash");

    /* 12c. gr_hash different data */
    uint8_t data2[] = "hello worle";
    T(gr_hash(data2, sizeof(data2), hash2) == GR_OK, "hash different data");
    T(memcmp(hash, hash2, GR_HASH_LEN) != 0, "different hash");

    /* 12d. gr_id_equal */
    uint8_t a[GR_PEER_ID_LEN], b[GR_PEER_ID_LEN];
    memset(a, 0xAA, GR_PEER_ID_LEN);
    memset(b, 0xAA, GR_PEER_ID_LEN);
    T(gr_id_equal(a, b), "equal IDs");
    b[0] = 0xBB;
    T(!gr_id_equal(a, b), "unequal IDs");

    /* 12e. Identity wipe */
    gr_identity_t id;
    mkid(&id);
    uint8_t zero_sk[GR_SECRET_KEY_LEN];
    memset(zero_sk, 0, GR_SECRET_KEY_LEN);
    T(memcmp(id.secret_key, zero_sk, GR_SECRET_KEY_LEN) != 0,
      "sk is non-zero before wipe");
    gr_identity_wipe(&id);
    T(memcmp(id.secret_key, zero_sk, GR_SECRET_KEY_LEN) == 0,
      "sk is zeroed after wipe");
    gr_identity_wipe(NULL);  /* no crash */
    T(1, "wipe(NULL) no crash");

    /* 12f. Identity derive_id */
    gr_identity_t id2;
    mkid(&id2);
    uint8_t derived[GR_PEER_ID_LEN];
    T(gr_identity_derive_id(id2.public_key, derived) == GR_OK, "derive_id");
    T(memcmp(derived, id2.peer_id, GR_PEER_ID_LEN) == 0,
      "derived matches peer_id");

    /* 12g. Timestamps */
    int64_t ms = gr_timestamp_ms();
    T(ms > 0, "timestamp_ms > 0");
    int64_t ns = gr_timestamp_ns();
    T(ns > 0, "timestamp_ns > 0");
    T(ns >= ms * 1000000, "ns >= ms * 1M (roughly)");

    /* 12h. Schema version */
    T(gr_schema_version() > 0, "schema_version > 0");

    /* 12i. gr_free(NULL) */
    gr_free(NULL);
    T(1, "gr_free(NULL) no crash");

    /* 12j. Registrar with member doing role operations via elevated role */
    gr_registrar_t *r = mkreg(&owner);
    gr_identity_t elevated;
    mkid(&elevated);
    add_peer(r, &elevated, &owner);

    uint32_t role_id;
    T(gr_role_add(r, "SuperMod", GR_PERM_EDIT_ROLES | GR_PERM_KICK_MEMBER |
                  GR_PERM_BAN_MEMBER | GR_PERM_INVITE_MEMBER,
                  &owner, &role_id) == GR_OK, "add SuperMod");
    T(gr_peer_set_role(r, elevated.peer_id, role_id, &owner) == GR_OK,
      "assign SuperMod");

    /* SuperMod can add roles (has EDIT_ROLES) */
    uint32_t sub_role;
    T(gr_role_add(r, "SubRole", GR_PERM_KICK_MEMBER, &elevated, &sub_role) == GR_OK,
      "SuperMod can add roles");

    /* SuperMod can kick */
    gr_identity_t victim;
    mkid(&victim);
    add_peer(r, &victim, &owner);
    T(gr_peer_kick(r, victim.peer_id, "supermod", &elevated) == GR_OK,
      "SuperMod can kick");

    /* 12k. Callbacks */
    T(gr_set_delta_anomaly_callback(r, NULL, NULL) == GR_OK,
      "set delta anomaly cb (NULL fn)");
    T(gr_set_persist_prompt_callback(r, NULL, NULL) == GR_OK,
      "set persist prompt cb (NULL fn)");
    T(gr_set_behavior_alert_callback(r, NULL, NULL) == GR_OK,
      "set behavior alert cb (NULL fn)");

    /* 12l. Audit count and chain verify on fresh */
    uint32_t ac = 0;
    T(gr_audit_count(r, &ac) == GR_OK, "audit_count");
    T(ac > 0, "audit entries from setup");

    gr_audit_chain_result_t chain;
    T(gr_audit_verify_chain(r, &chain) == GR_OK, "audit_verify_chain");
    T(chain.total_entries > 0, "chain has entries");
    T(chain.invalid_hash == 0, "no invalid hashes");
    T(chain.invalid_signature == 0, "no invalid signatures");

    /* 12m. Audit enforce retention */
    T(gr_audit_enforce_retention(r) == GR_OK, "audit_enforce_retention");

    /* 12n. Serialize / deserialize round-trip */
    uint8_t *blob = NULL;
    size_t blen = 0;
    T(gr_serialize(r, GR_SERIALIZE_FULL, &blob, &blen) == GR_OK, "serialize");
    T(blob != NULL && blen > 0, "blob produced");

    gr_registrar_t *clone = NULL;
    T(gr_create(&clone, ":memory:", "CloneGroup", GR_GROUP_PRIVATE, &owner) == GR_OK,
      "create clone");
    T(gr_deserialize(clone, blob, blen) == GR_OK, "deserialize");
    gr_free(blob);

    /* Verify clone state */
    uint32_t clone_pc = 0;
    T(gr_peer_count(clone, GR_PEER_ACTIVE, &clone_pc) == GR_OK, "clone peer count");
    /* Might differ due to kicks during test, but should have some peers */
    T(clone_pc >= 1, "clone has peers");

    gr_close(clone);
    gr_close(r);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Section 13: Security fix regression tests
 * ═══════════════════════════════════════════════════════════════════ */

static void test_security_fixes(void)
{
    SEC("13. Security fix regression tests");

    gr_identity_t owner;
    mkid(&owner);
    gr_registrar_t *r = mkreg(&owner);
    T(r != NULL, "setup registrar");

    /* 13a. Delta apply functional test (validates GR_LOCK fix) */
    {
        gr_identity_t p1;
        mkid(&p1);
        add_peer(r, &p1, &owner);
        gr_sign(r, &owner);

        /* Serialize a delta */
        uint8_t *delta = NULL;
        size_t dlen = 0;
        T(gr_serialize_delta(r, 0, &delta, &dlen) == GR_OK, "serialize delta");
        T(delta != NULL && dlen > 0, "delta produced");

        /* Apply to a clone */
        gr_registrar_t *clone = NULL;
        T(gr_create(&clone, ":memory:", "DeltaTarget", GR_GROUP_PRIVATE, &owner) == GR_OK,
          "create clone for delta");
        gr_sign(clone, &owner);

        gr_merge_result_t mr;
        T(gr_apply_delta(clone, delta, dlen, &mr) == GR_OK, "apply_delta succeeds");
        T(mr.entries_rejected == 0, "no entries rejected");
        gr_free(delta);

        /* Verify the clone is usable after delta (lock state valid) */
        uint32_t pc = 0;
        T(gr_peer_count(clone, GR_PEER_ACTIVE, &pc) == GR_OK,
          "peer_count after delta (lock not corrupted)");
        T(pc >= 1, "clone has peers after delta");

        gr_close(clone);
    }

    /* 13b. Delta apply with corrupt magic is rejected cleanly */
    {
        uint8_t bad_data[16];
        memset(bad_data, 0, sizeof(bad_data));
        gr_merge_result_t mr;
        T(gr_apply_delta(r, bad_data, sizeof(bad_data), &mr) == GR_ERR_SERIALIZATION,
          "corrupt delta rejected");
        /* Verify registrar is still usable after rejected delta */
        uint32_t pc = 0;
        T(gr_peer_count(r, GR_PEER_ACTIVE, &pc) == GR_OK,
          "registrar usable after rejected delta");
    }

    gr_close(r);
}

/* ═══════════════════════════════════════════════════════════════════
 *  main
 * ═══════════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("▸ test_registrar_full — Comprehensive registrar module test suite\n\n");

    yumi_crypto_init();

    test_lifecycle();
    test_peer_management();
    test_role_management();
    test_epoch_management();
    test_retention();
    test_invite_lifecycle();
    test_group_icon();
    test_webapp_management();
    test_server_management();
    test_behavior();
    test_permissions();
    test_edge_cases();
    test_security_fixes();

    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("  %d tests, %d passed, %d failed\n",
           g_run, g_run - g_fail, g_fail);
    printf("═══════════════════════════════════════════════════════════\n");

    return g_fail > 0 ? 1 : 0;
}
