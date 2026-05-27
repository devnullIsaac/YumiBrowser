/*
 * join.c - Group Registrar join verification state machine for tamper-proof invitations (PROVISIONAL → PENDING → VERIFIED / REJECTED).
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
 * @file join_verify.c
 * @brief Join verification state machine for tamper-proof invitations.
 */

#include "internal.h"
#include "buf.h"

/* ═══════════════════════════════════════════════════════════════════
 *  Internal helpers
 * ═══════════════════════════════════════════════════════════════════ */

bool gr_join_is_untrusted(const gr_registrar_t *reg) {
    if (!reg->join_state) return false;
    return reg->join_state->state != GR_JOIN_VERIFIED;
}

static uint32_t compute_quorum(uint32_t active_peers) {
    if (active_peers <= 2) return GR_JOIN_MIN_QUORUM;
    uint32_t attestable = active_peers - 2;
    double sq = ceil(sqrt((double)attestable));
    uint32_t q = (uint32_t)sq;
    if (q < GR_JOIN_MIN_QUORUM) q = GR_JOIN_MIN_QUORUM;
    if (q > GR_JOIN_MAX_QUORUM) q = GR_JOIN_MAX_QUORUM;
    return q;
}

static bool has_voted(const gr_join_verify_state_t *js,
                      const uint8_t peer_id[GR_PEER_ID_LEN]) {
    for (uint32_t i = 0; i < js->vote_count; i++) {
        if (yumi_memcmp(js->votes[i].peer_id, peer_id,
                          GR_PEER_ID_LEN) == 0)
            return true;
    }
    return false;
}

static bool headers_agree_on_authority(const gr_header_t *provisional,
                                       const gr_header_t *peer_header) {
    if (yumi_memcmp(provisional->group_id, peer_header->group_id,
                      GR_HASH_LEN) != 0)
        return false;
    if (yumi_memcmp(provisional->owner_id, peer_header->owner_id,
                      GR_PEER_ID_LEN) != 0)
        return false;
    if (yumi_memcmp(provisional->owner_sign_key,
                      peer_header->owner_sign_key,
                      GR_PUBLIC_KEY_LEN) != 0)
        return false;
    return true;
}

static bool ticket_matches_header(const gr_join_verify_state_t *js,
                                  const gr_header_t *header) {
    if (yumi_memcmp(js->ticket_group_id, header->group_id,
                      GR_HASH_LEN) != 0)
        return false;
    if (yumi_memcmp(js->ticket_owner_key, header->owner_sign_key,
                      GR_PUBLIC_KEY_LEN) != 0)
        return false;
    return true;
}

/**
 * @brief Build attestation data buffer for signing.
 *
 * Format: group_id(32) || owner_id(32) || owner_sign_key(32)
 *         || version_le(4) || nonce(32) = 132 bytes
 */
static size_t build_attestation_data(uint8_t *out,
                                     const uint8_t group_id[GR_HASH_LEN],
                                     const uint8_t owner_id[GR_PEER_ID_LEN],
                                     const uint8_t owner_sign_key[GR_PUBLIC_KEY_LEN],
                                     uint32_t version,
                                     const uint8_t nonce[GR_JOIN_NONCE_LEN]) {
    size_t pos = 0;
    memcpy(out + pos, group_id, GR_HASH_LEN);       pos += GR_HASH_LEN;
    memcpy(out + pos, owner_id, GR_PEER_ID_LEN);    pos += GR_PEER_ID_LEN;
    memcpy(out + pos, owner_sign_key, GR_PUBLIC_KEY_LEN);
    pos += GR_PUBLIC_KEY_LEN;
    uint32_t ver_le = gr_htole32(version);
    memcpy(out + pos, &ver_le, 4);                  pos += 4;
    memcpy(out + pos, nonce, GR_JOIN_NONCE_LEN);     pos += GR_JOIN_NONCE_LEN;
    return pos;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════ */

gr_error_t gr_join_begin(gr_registrar_t *reg,
                         const gr_invite_ticket_t *ticket) {
    if (!reg || !ticket) return GR_ERR_INVALID_PARAM;
    if (reg->join_state)  return GR_ERR_ALREADY_EXISTS;

    gr_join_verify_state_t *js = (gr_join_verify_state_t *)
        calloc(1, sizeof(gr_join_verify_state_t));
    if (!js) return GR_ERR_OUT_OF_MEMORY;

    js->state = GR_JOIN_PROVISIONAL;
    js->started_at = gr_timestamp_ms();
    if (yumi_randombytes(js->nonce, GR_JOIN_NONCE_LEN) != YUMI_CRYPTO_OK) {
        free(js);
        return GR_ERR_CRYPTO;
    }

    memcpy(js->ticket_owner_key, ticket->owner_sign_key, GR_PUBLIC_KEY_LEN);
    memcpy(js->ticket_group_id, ticket->group_id, GR_HASH_LEN);
    memcpy(js->ticket_registrar_hash, ticket->registrar_hash, GR_HASH_LEN);

    /* Cross-check 1: ticket vs fetched registrar */
    if (!ticket_matches_header(js, &reg->header)) {
        js->state = GR_JOIN_FAILED;
        reg->join_state = js;
        return GR_ERR_JOIN_FAILED;
    }

    /* Cross-check 2: inviter authorization */
    gr_identity_derive_id(ticket->inviter_sign_pk, js->inviter_peer_id);

    gr_peer_t inviter_peer;
    if (gr_peer_get(reg, js->inviter_peer_id, &inviter_peer) != GR_OK ||
        inviter_peer.status != GR_PEER_ACTIVE) {
        js->state = GR_JOIN_FAILED;
        reg->join_state = js;
        return GR_ERR_JOIN_FAILED;
    }

    uint32_t inviter_perms = gr_get_peer_permissions(reg, js->inviter_peer_id);
    if (!(inviter_perms & GR_PERM_INVITE_MEMBER)) {
        js->state = GR_JOIN_FAILED;
        reg->join_state = js;
        return GR_ERR_JOIN_FAILED;
    }

    /* Small group bypass */
    uint32_t active_count = 0;
    gr_peer_count(reg, GR_PEER_ACTIVE, &active_count);

    if (active_count < GR_JOIN_SMALL_GROUP_THRESHOLD) {
        js->state = GR_JOIN_VERIFIED;
        js->small_group_bypass = true;
        js->required_attestations = 0;
        reg->join_state = js;
        return GR_OK;
    }

    js->required_attestations = compute_quorum(active_count);
    reg->join_state = js;
    return GR_OK;
}

gr_error_t gr_join_set_dissent_callback(gr_registrar_t *reg,
                                        gr_join_dissent_fn callback,
                                        void *user_data) {
    if (!reg || !reg->join_state) return GR_ERR_INVALID_PARAM;
    reg->join_state->dissent_callback = callback;
    reg->join_state->dissent_user_data = user_data;
    return GR_OK;
}

gr_error_t gr_join_submit_peer_header(gr_registrar_t *reg,
                                      const gr_header_t *peer_header,
                                      const uint8_t peer_id[GR_PEER_ID_LEN],
                                      const uint8_t peer_pk[GR_PUBLIC_KEY_LEN],
                                      const uint8_t peer_signature[GR_SIGN_LEN]) {
    if (!reg || !peer_header || !peer_id || !peer_pk || !peer_signature)
        return GR_ERR_INVALID_PARAM;

    gr_join_verify_state_t *js = reg->join_state;
    if (!js || js->state != GR_JOIN_PROVISIONAL)
        return GR_ERR_INVALID_PARAM;

    if (yumi_memcmp(peer_id, js->inviter_peer_id, GR_PEER_ID_LEN) == 0)
        return GR_ERR_JOIN_INVITER_EXCLUDED;

    if (has_voted(js, peer_id))
        return GR_ERR_ALREADY_EXISTS;

    gr_peer_t peer;
    if (gr_peer_get(reg, peer_id, &peer) != GR_OK)
        return GR_ERR_NOT_FOUND;
    if (peer.status != GR_PEER_ACTIVE)
        return GR_ERR_NOT_FOUND;

    if (yumi_memcmp(peer.sign_key, peer_pk, GR_PUBLIC_KEY_LEN) != 0)
        return GR_ERR_SIGNATURE_INVALID;

    uint8_t attestation_data[GR_JOIN_ATTESTATION_LEN];
    size_t alen = build_attestation_data(attestation_data,
        peer_header->group_id, peer_header->owner_id,
        peer_header->owner_sign_key, peer_header->version,
        js->nonce);

    if (yumi_mldsa_verify(peer_signature, attestation_data,
                           alen, peer_pk) != YUMI_CRYPTO_OK)
        return GR_ERR_SIGNATURE_INVALID;

    if (js->vote_count >= GR_JOIN_MAX_TRACKED_PEERS)
        return GR_ERR_SIZE_EXCEEDED;

    gr_join_peer_vote_t *vote = &js->votes[js->vote_count];
    memset(vote, 0, sizeof(*vote));
    memcpy(vote->peer_id, peer_id, GR_PEER_ID_LEN);
    memcpy(vote->owner_id, peer_header->owner_id, GR_PEER_ID_LEN);
    memcpy(vote->owner_sign_key, peer_header->owner_sign_key,
           GR_PUBLIC_KEY_LEN);
    memcpy(vote->group_id, peer_header->group_id, GR_HASH_LEN);
    memcpy(vote->registrar_hash, peer_header->hash, GR_HASH_LEN);
    vote->version = peer_header->version;
    vote->received_at = gr_timestamp_ms();
    vote->agrees = headers_agree_on_authority(&reg->header, peer_header);
    js->vote_count++;

    if (vote->agrees) {
        js->agree_count++;
    } else {
        js->disagree_count++;
        if (!js->has_dissent) {
            js->has_dissent = true;
            memcpy(js->dissent_peer_id, peer_id, GR_PEER_ID_LEN);
            memcpy(js->dissent_owner_id, peer_header->owner_id,
                   GR_PEER_ID_LEN);
            memcpy(js->dissent_owner_key, peer_header->owner_sign_key,
                   GR_PUBLIC_KEY_LEN);
        }
    }

    return GR_OK;
}

gr_error_t gr_join_evaluate(gr_registrar_t *reg,
                            gr_join_verify_result_t *result_out) {
    if (!reg || !result_out) return GR_ERR_INVALID_PARAM;

    memset(result_out, 0, sizeof(*result_out));
    gr_join_verify_state_t *js = reg->join_state;

    if (!js) { result_out->state = GR_JOIN_NONE; return GR_OK; }

    if (js->state != GR_JOIN_PROVISIONAL) {
        result_out->state = js->state;
        result_out->started_at = js->started_at;
        result_out->peers_checked = js->vote_count;
        result_out->peers_agreed = js->agree_count;
        result_out->peers_disagreed = js->disagree_count;
        result_out->required_attestations = js->required_attestations;
        result_out->small_group_bypass = js->small_group_bypass;
        result_out->user_override = js->user_override;
        return GR_OK;
    }

    /* Dissent: fire callback once */
    if (js->disagree_count > 0 && !js->dissent_callback_fired) {
        js->dissent_callback_fired = true;

        /* Pre-populate result for callback */
        result_out->state = GR_JOIN_PROVISIONAL;
        result_out->started_at = js->started_at;
        result_out->peers_checked = js->vote_count;
        result_out->peers_agreed = js->agree_count;
        result_out->peers_disagreed = js->disagree_count;
        result_out->required_attestations = js->required_attestations;
        memcpy(result_out->provisional_owner_id, reg->header.owner_id,
               GR_PEER_ID_LEN);
        memcpy(result_out->provisional_owner_key, reg->header.owner_sign_key,
               GR_PUBLIC_KEY_LEN);
        memcpy(result_out->dissent_peer_id, js->dissent_peer_id,
               GR_PEER_ID_LEN);
        memcpy(result_out->dissent_owner_id, js->dissent_owner_id,
               GR_PEER_ID_LEN);
        memcpy(result_out->dissent_owner_key, js->dissent_owner_key,
               GR_PUBLIC_KEY_LEN);

        if (js->dissent_callback) {
            bool stay = js->dissent_callback(result_out,
                                              js->dissent_user_data);
            if (stay) {
                js->state = GR_JOIN_VERIFIED;
                js->user_override = true;
            } else {
                js->state = GR_JOIN_FAILED;
            }
        } else {
            js->state = GR_JOIN_FAILED;
        }
    } else if (js->agree_count >= js->required_attestations &&
               js->disagree_count == 0) {
        js->state = GR_JOIN_VERIFIED;
    }

    /* Fill result */
    result_out->state = js->state;
    result_out->started_at = js->started_at;
    result_out->peers_checked = js->vote_count;
    result_out->peers_agreed = js->agree_count;
    result_out->peers_disagreed = js->disagree_count;
    result_out->required_attestations = js->required_attestations;
    result_out->small_group_bypass = js->small_group_bypass;
    result_out->user_override = js->user_override;
    memcpy(result_out->provisional_owner_id, reg->header.owner_id,
           GR_PEER_ID_LEN);
    memcpy(result_out->provisional_owner_key, reg->header.owner_sign_key,
           GR_PUBLIC_KEY_LEN);
    if (js->has_dissent) {
        memcpy(result_out->dissent_peer_id, js->dissent_peer_id,
               GR_PEER_ID_LEN);
        memcpy(result_out->dissent_owner_id, js->dissent_owner_id,
               GR_PEER_ID_LEN);
        memcpy(result_out->dissent_owner_key, js->dissent_owner_key,
               GR_PUBLIC_KEY_LEN);
    }

    return GR_OK;
}

gr_error_t gr_join_get_state(const gr_registrar_t *reg,
                             gr_join_state_t *state_out) {
    if (!reg || !state_out) return GR_ERR_INVALID_PARAM;
    *state_out = reg->join_state ? reg->join_state->state : GR_JOIN_NONE;
    return GR_OK;
}

gr_error_t gr_join_get_nonce(const gr_registrar_t *reg,
                             uint8_t nonce_out[GR_JOIN_NONCE_LEN]) {
    if (!reg || !nonce_out) return GR_ERR_INVALID_PARAM;
    if (!reg->join_state) return GR_ERR_INVALID_PARAM;
    memcpy(nonce_out, reg->join_state->nonce, GR_JOIN_NONCE_LEN);
    return GR_OK;
}

gr_error_t gr_join_export_header_attestation(
    const gr_registrar_t *reg, const gr_identity_t *self,
    const uint8_t joiner_nonce[GR_JOIN_NONCE_LEN],
    gr_header_t *header_out, uint8_t signature_out[GR_SIGN_LEN]) {
    if (!reg || !self || !joiner_nonce || !header_out || !signature_out)
        return GR_ERR_INVALID_PARAM;

    memcpy(header_out, &reg->header, sizeof(gr_header_t));

    uint8_t attestation_data[GR_JOIN_ATTESTATION_LEN];
    size_t alen = build_attestation_data(attestation_data,
        reg->header.group_id, reg->header.owner_id,
        reg->header.owner_sign_key, reg->header.version,
        joiner_nonce);

    if (yumi_mldsa_sign(signature_out, attestation_data,
                         alen, self->secret_key) != YUMI_CRYPTO_OK)
        return GR_ERR_CRYPTO;
    return GR_OK;
}

gr_error_t gr_join_list_unattested_peers(
    const gr_registrar_t *reg, uint8_t *peer_ids_out,
    uint32_t max_count, uint32_t *actual_count) {
    if (!reg || !peer_ids_out || !actual_count)
        return GR_ERR_INVALID_PARAM;
    *actual_count = 0;
    const gr_join_verify_state_t *js = reg->join_state;
    if (!js || js->state != GR_JOIN_PROVISIONAL)
        return GR_ERR_INVALID_PARAM;

    uint32_t total;
    gr_error_t err = gr_peer_count(reg, GR_PEER_ACTIVE, &total);
    if (err != GR_OK) return err;
    if (total == 0) return GR_OK;

    gr_peer_t *all = (gr_peer_t *)calloc(total, sizeof(gr_peer_t));
    if (!all) return GR_ERR_OUT_OF_MEMORY;

    uint32_t fetched;
    err = gr_peer_list(reg, all, total, &fetched, GR_PEER_ACTIVE);
    if (err != GR_OK) { free(all); return err; }

    uint32_t count = 0;
    for (uint32_t i = 0; i < fetched && count < max_count; i++) {
        if (yumi_memcmp(all[i].peer_id, js->inviter_peer_id,
                          GR_PEER_ID_LEN) == 0)
            continue;
        if (has_voted(js, all[i].peer_id))
            continue;
        memcpy(peer_ids_out + (count * GR_PEER_ID_LEN),
               all[i].peer_id, GR_PEER_ID_LEN);
        count++;
    }

    free(all);
    *actual_count = count;
    return GR_OK;
}

bool gr_is_trusted(const gr_registrar_t *reg) {
    if (!reg) return false;
    if (!reg->join_state) return true;
    return reg->join_state->state == GR_JOIN_VERIFIED;
}
