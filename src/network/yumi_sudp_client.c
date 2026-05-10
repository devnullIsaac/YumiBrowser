/*
    Yumi Secure UDP Client (Implementation)
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
 * yumi_sudp_client.c — Yumi Secure UDP Client
 *
 * Encrypted session layer over yumi_udp_client.
 *
 *   - Handshake: triple-hybrid (ML-KEM-1024 + FrodoKEM-1344 + BrainPool-512)
 *     or dual-hybrid (ML-KEM-1024 + BrainPool-512).
 *   - Data: Threefish-1024-CTR + Skein-1024-MAC AEAD on every packet.
 *   - Authentication: ML-DSA-87 mutual signatures over handshake transcript.
 *   - Epoch binding: transport_key = HKDF(handshake_secret, epoch_key).
 *   - Session rekeying: 1024-bit rekey_seed derived during handshake;
 *     periodic gen-based key rotation every ~1M packets, plus epoch-
 *     rotation rekeying (HKDF(seed || new_epoch, "yumi-epoch-rekey-v1")).
 *   - Anti-replay: 64-entry sliding window on nonce counter.
 *   - Handshake timeout: 30 s (first contact) / 10 s (subsequent).
 *   - Protocol version: 8-bit prefix on every handshake message.
 *   - Kicked peer detection: checked on send, recv, and via notify_kick().
 *   - Optional logging: compile with -DYUMI_SUDP_LOG.
 *
 * Wire format (on wire — entire datagram encrypted):
 *
 *   [nonce(16)] [AEAD( transport_header(10) + inner_payload ) || tag(128)]
 *
 *   Inner payloads (decrypted content after transport header):
 *
 *     Data:      [epoch_id(4)][channel(1)][user_data...]
 *     Handshake: [0x00000000(4)][msg_type(1)][frag_total(1)][frag_index(1)][0(1)][data...]
 *
 *   Handshake messages (reassembled, inside fragments):
 *
 *     HELLO:    [version(1)][peer_id(32)][epoch_id(4)][first_contact(1)][keys...]
 *     RESPONSE: [version(1)][peer_id(32)][ciphertexts...][signature]
 *     CONFIRM:  [version(1)][signature]
 *
 *   Key selection: epoch_id == 0 → epoch key, epoch_id > 0 → transport key.
 */

#define _GNU_SOURCE
#include "network/yumi_sudp_client.h"

#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <time.h>
#include <pthread.h>

/* ════════════════════════════════════════════════════════════════════════
 *  Optional logging — compile with -DYUMI_SUDP_LOG to enable
 * ════════════════════════════════════════════════════════════════════════ */

#ifdef YUMI_SUDP_LOG
#include <stdio.h>
#define SUDP_LOG(fmt, ...) fprintf(stderr, "[SUDP] " fmt "\n", ##__VA_ARGS__)
#else
#define SUDP_LOG(fmt, ...) ((void)0)
#endif

/* ════════════════════════════════════════════════════════════════════════
 *  Constants
 * ════════════════════════════════════════════════════════════════════════ */

#define SUDP_HS_FRAG_HDR    8      /* 4 (epoch=0) + 1+1+1+1 frag header */
#define SUDP_HS_BUF_MAX     (48 * 1024)   /* max reassembled message     */
#define SUDP_REPLAY_WINDOW  256

/* Periodic nonce-counter-based rekey interval (~1 M packets per gen) */
#define SUDP_REKEY_INTERVAL (1ULL << 20)

/* Epoch-rotation check interval — query the registrar at most this often.
 * 30 seconds, in microseconds. */
#define SUDP_EPOCH_CHECK_INTERVAL_US  (30ULL * 1000000)

/* Forward declarations */
static void sudp_worker_connect(void *ctx, const void *data, uint32_t len);
static int  sudp_derive_gen_key(uint8_t out[YUMI_AEAD_KEY_LEN],
                                const uint8_t seed[YUMI_SUDP_REKEY_SEED_LEN],
                                uint32_t gen, bool init_direction);

/* Handshake message types */
#define SUDP_MSG_HELLO      0x01
#define SUDP_MSG_RESPONSE   0x02
#define SUDP_MSG_CONFIRM    0x03

/* Handshake internal states */
enum {
    HS_IDLE = 0,
    HS_HELLO_SENT,        /* initiator: waiting for RESPONSE              */
    HS_WAIT_CONFIRM,      /* responder: sent RESPONSE, waiting for CONFIRM */
    HS_DONE,
};

/* ════════════════════════════════════════════════════════════════════════
 *  Time helper
 * ════════════════════════════════════════════════════════════════════════ */

static uint64_t sudp_monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Internal types
 * ════════════════════════════════════════════════════════════════════════ */

/* Fragment reassembly slot */
typedef struct {
    uint8_t  *data;
    uint16_t  len;
} sudp_frag_slot_t;

/* Handshake context — heap-allocated, freed after establishment */
typedef struct {
    int      state;
    bool     first_contact;
    uint8_t  peer_id[GR_PEER_ID_LEN];

    /* Ephemeral ML-KEM-1024 */
    uint8_t  mlkem_pk[YUMI_MLKEM_PUBLIC_KEY_LEN];
    uint8_t  mlkem_sk[YUMI_MLKEM_SECRET_KEY_LEN];

    /* Ephemeral BrainPool-512 */
    yumi_bp512_keypair_t *bp512;
    uint8_t  bp512_pk[YUMI_BP512_PUBLIC_KEY_LEN];
    size_t   bp512_pk_len;

    /* Ephemeral FrodoKEM-1344 (first_contact only, heap) */
    uint8_t *frodo_pk;           /* YUMI_FRODO_PUBLIC_KEY_LEN  (21520) */
    uint8_t *frodo_sk;           /* YUMI_FRODO_SECRET_KEY_LEN  (43088) */

    /* Transcript — streaming hash of handshake key material */
    yumi_skein_ctx_t *transcript;
    uint8_t  transcript_hash_saved[YUMI_SKEIN_HASH_LEN];

    /* Fragment reassembly */
    sudp_frag_slot_t frags[256];
    uint8_t  frag_total;
    uint8_t  frag_received;
    uint64_t frag_start_us;      /* monotonic timestamp of first frag  */
    uint8_t  expected_msg;       /* which msg_type we expect next       */
} sudp_hs_ctx_t;

/* Anti-replay window (256-bit bitmap) */
typedef struct {
    uint64_t max_seq;
    uint64_t bitmap[4];  /* 256 bits */
} sudp_replay_t;

/* Top-level SUDP client */
struct yumi_sudp_client {
    /* Inner transport (embedded, not a pointer) */
    yumi_udp_client_t       inner;

    /* Group context (borrowed) */
    gr_registrar_t         *registrar;
    const gr_identity_t    *identity;

    /* Session state — written by worker, read by user */
    _Alignas(64) atomic_int state;           /* yumi_sudp_state_t       */

    /* Crypto session (valid after ESTABLISHED) */
    uint8_t                 transport_key[YUMI_AEAD_KEY_LEN];   /* 128 */
    bool                    transport_key_ready;
    uint8_t                 epoch_key[YUMI_AEAD_KEY_LEN];       /* 128 */
    uint8_t                 nonce_token[8];
    _Alignas(64) atomic_uint_fast64_t nonce_counter;
    _Atomic uint32_t        epoch_id;

    /* Cached epoch check — avoids hitting the DB on every packet.
     * The encrypt path only queries gr_epoch_get_current() every
     * SUDP_EPOCH_CHECK_INTERVAL_US microseconds (30 s). */
    uint64_t                epoch_check_last_us;

    /* Rekey seed — 1024 bits, used for epoch-rotation and periodic rekey.
     * transport_key is the "send" key; recv_transport_key is the
     * cached "receive" key.  Both derive from this seed + generation. */
    uint8_t                 rekey_seed[YUMI_SUDP_REKEY_SEED_LEN];
    uint8_t                 recv_transport_key[YUMI_AEAD_KEY_LEN];
    uint32_t                send_rekey_gen;
    uint32_t                recv_rekey_gen;

    /* Pre-derived AEAD subkeys — avoids per-packet key derivation.
     * Invalidated and re-derived on rekey / epoch rotation. */
    yumi_aead_subkeys_t     send_subkeys;
    yumi_aead_subkeys_t     recv_subkeys;
    yumi_aead_subkeys_t     epoch_subkeys;
    bool                    epoch_subkeys_ready;

    /* Handshake timeout (µs since boot) */
    uint64_t                hs_deadline_us;

    /* Anti-replay */
    sudp_replay_t           replay;

    /* Crypto lock — protects rekey_seed, epoch_key, epoch_subkeys,
     * and the epoch_id write during epoch rotation.  Encrypt/decrypt
     * take a read lock; sudp_epoch_rekey takes a write lock.
     * Gen-based send/recv rekey only touches per-direction fields and
     * reads rekey_seed under the read lock. */
    pthread_rwlock_t        crypto_lock;

    /* Handshake (heap, freed after establishment) */
    sudp_hs_ctx_t          *hs;
    pthread_mutex_t         hs_mutex;  /* serialises send/recv threads  */

    /* Peer */
    uint8_t                 peer_id[GR_PEER_ID_LEN];
    bool                    first_contact;
    bool                    is_initiator;  /* lower peer_id = initiator  */
    atomic_bool             peer_active;

    /* User callbacks */
    yumi_sudp_recv_cb_t     recv_cb;
    yumi_sudp_state_cb_t    state_cb;
    void                   *user;

    /* Derived from inner client max_payload */
    uint32_t                max_secure_payload;
};

/* ════════════════════════════════════════════════════════════════════════
 *  Handshake context lifecycle
 * ════════════════════════════════════════════════════════════════════════ */

static sudp_hs_ctx_t *hs_ctx_create(bool first_contact)
{
    sudp_hs_ctx_t *h = calloc(1, sizeof(*h));
    if (!h) return NULL;

    h->state = HS_IDLE;
    h->first_contact = first_contact;

    if (first_contact) {
        h->frodo_pk = calloc(1, YUMI_FRODO_PUBLIC_KEY_LEN);
        h->frodo_sk = calloc(1, YUMI_FRODO_SECRET_KEY_LEN);
        if (!h->frodo_pk || !h->frodo_sk) {
            free(h->frodo_pk);
            free(h->frodo_sk);
            free(h);
            return NULL;
        }
    }

    if (yumi_skein_init(&h->transcript) != YUMI_CRYPTO_OK) {
        free(h->frodo_pk);
        free(h->frodo_sk);
        free(h);
        return NULL;
    }

    return h;
}

static void hs_ctx_destroy(sudp_hs_ctx_t *h)
{
    if (!h) return;

    /* Wipe ephemeral secret keys */
    yumi_memzero(h->mlkem_sk, sizeof(h->mlkem_sk));
    if (h->frodo_sk) {
        yumi_memzero(h->frodo_sk, YUMI_FRODO_SECRET_KEY_LEN);
        free(h->frodo_sk);
    }
    if (h->frodo_pk) free(h->frodo_pk);
    if (h->bp512) yumi_bp512_keypair_free(h->bp512);
    if (h->transcript) yumi_skein_free(h->transcript);

    /* Free fragment buffers */
    for (int i = 0; i < 256; i++)
        free(h->frags[i].data);

    yumi_memzero(h, sizeof(*h));
    free(h);
}

static void hs_reasm_reset(sudp_hs_ctx_t *h)
{
    for (int i = 0; i < 256; i++) {
        free(h->frags[i].data);
        h->frags[i].data = NULL;
        h->frags[i].len  = 0;
    }
    h->frag_total    = 0;
    h->frag_received = 0;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Fragment send — split a blob into inner-client reliable sends
 * ════════════════════════════════════════════════════════════════════════ */

static int sudp_frag_send(yumi_sudp_client_t *c, uint8_t msg_type,
                           const uint8_t *data, size_t data_len)
{
    uint32_t inner_max = yumi_udp_client_get_max_payload(&c->inner);
    if (inner_max <= SUDP_HS_FRAG_HDR) return -1;

    uint32_t frag_data_max = inner_max - SUDP_HS_FRAG_HDR;
    uint32_t frag_total = (uint32_t)((data_len + frag_data_max - 1) / frag_data_max);
    if (frag_total > 255 || frag_total == 0) return -1;

    uint8_t *pkt = malloc(inner_max);
    if (!pkt) return -1;

    /* First 4 bytes = epoch_id 0 (handshake discriminator) */
    memset(pkt, 0, 4);

    for (uint32_t i = 0; i < frag_total; i++) {
        size_t offset = (size_t)i * frag_data_max;
        size_t chunk  = data_len - offset;
        if (chunk > frag_data_max) chunk = frag_data_max;

        pkt[4] = msg_type;
        pkt[5] = (uint8_t)frag_total;
        pkt[6] = (uint8_t)i;
        pkt[7] = 0; /* reserved */
        memcpy(pkt + SUDP_HS_FRAG_HDR, data + offset, chunk);

        int r = yumi_udp_client_send_reliable(&c->inner, pkt,
                    (uint32_t)(SUDP_HS_FRAG_HDR + chunk));
        if (r != 0) { free(pkt); return -1; }
    }

    free(pkt);
    return 0;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Fragment receive — returns reassembled message or NULL
 *
 *  Returns a malloc'd buffer that the caller must free.
 * ════════════════════════════════════════════════════════════════════════ */

static uint8_t *sudp_frag_receive(sudp_hs_ctx_t *h,
                                   const uint8_t *data, uint32_t len,
                                   size_t *out_len)
{
    if (len < SUDP_HS_FRAG_HDR) return NULL;

    uint8_t frag_total = data[5];
    uint8_t frag_index = data[6];

    if (frag_total == 0 || frag_index >= frag_total) return NULL;

    /* First fragment of a new message — reset reassembly */
    if (h->frag_total == 0) {
        h->frag_total = frag_total;
        h->frag_start_us = sudp_monotonic_us();
    } else if (frag_total != h->frag_total) {
        return NULL; /* mismatched count */
    }

    /* Fragment reassembly timeout — 5 seconds */
    if (h->frag_start_us != 0) {
        uint64_t elapsed = sudp_monotonic_us() - h->frag_start_us;
        if (elapsed > 5ULL * 1000000) {
            hs_reasm_reset(h);
            h->frag_start_us = 0;
            return NULL;
        }
    }

    /* Already have this fragment? */
    if (h->frags[frag_index].data) return NULL;

    uint32_t frag_data_len = len - SUDP_HS_FRAG_HDR;
    if (frag_data_len > SUDP_HS_BUF_MAX) return NULL;

    h->frags[frag_index].data = malloc(frag_data_len);
    if (!h->frags[frag_index].data) return NULL;
    memcpy(h->frags[frag_index].data, data + SUDP_HS_FRAG_HDR, frag_data_len);
    h->frags[frag_index].len = (uint16_t)frag_data_len;
    h->frag_received++;

    if (h->frag_received < h->frag_total) return NULL;

    /* All fragments received — concatenate */
    size_t total = 0;
    for (int i = 0; i < h->frag_total; i++)
        total += h->frags[i].len;

    if (total > SUDP_HS_BUF_MAX) { hs_reasm_reset(h); return NULL; }

    uint8_t *msg = malloc(total);
    if (!msg) { hs_reasm_reset(h); return NULL; }

    size_t pos = 0;
    for (int i = 0; i < h->frag_total; i++) {
        memcpy(msg + pos, h->frags[i].data, h->frags[i].len);
        pos += h->frags[i].len;
    }

    hs_reasm_reset(h);
    *out_len = total;
    return msg;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Key derivation — combine handshake secrets + epoch key
 * ════════════════════════════════════════════════════════════════════════ */

static int sudp_derive_keys(yumi_sudp_client_t *c,
                             const uint8_t *mlkem_ss,
                             const uint8_t *frodo_ss,  /* NULL if dual */
                             const uint8_t *bp512_ss, size_t bp512_ss_len)
{
    uint8_t temp_key[YUMI_TRANSPORT_KEY_LEN];
    int r;

    if (frodo_ss) {
        r = yumi_combine_invite_keys(temp_key,
                mlkem_ss, YUMI_MLKEM_SHARED_SECRET_LEN,
                frodo_ss, YUMI_FRODO_SHARED_SECRET_LEN,
                bp512_ss, bp512_ss_len);
    } else {
        r = yumi_combine_peer_keys(temp_key,
                mlkem_ss, YUMI_MLKEM_SHARED_SECRET_LEN,
                bp512_ss, bp512_ss_len);
    }
    if (r != YUMI_CRYPTO_OK) { yumi_memzero(temp_key, sizeof(temp_key)); return -1; }

    /* Fetch current epoch key */
    gr_epoch_t epoch;
    if (gr_epoch_get_current(c->registrar, &epoch) != GR_OK) {
        yumi_memzero(temp_key, sizeof(temp_key)); return -1;
    }
    atomic_store_explicit(&c->epoch_id, epoch.epoch_id, memory_order_relaxed);

    /* Derive 1024-bit rekey seed from temp_key + epoch_key.
     * The rekey seed is the root from which all directional transport
     * keys are derived (both initial gen-0 and future gen advances). */
    static const uint8_t rekey_info[] = "yumi-rekey-seed-v1";
    r = yumi_hkdf(c->rekey_seed, YUMI_SUDP_REKEY_SEED_LEN,
                   temp_key, YUMI_TRANSPORT_KEY_LEN,
                   epoch.epoch_key, YUMI_EPOCH_KEY_LEN,
                   rekey_info, sizeof(rekey_info) - 1);

    yumi_memzero(temp_key, sizeof(temp_key));
    yumi_memzero(epoch.epoch_key, sizeof(epoch.epoch_key));
    if (r != YUMI_CRYPTO_OK) return -1;

    /* ── Directional keys ─────────────────────────────────────────
     * Derive separate initiator→responder and responder→initiator
     * gen-0 keys from the rekey seed.  Each side's send/recv
     * assignment is determined by is_initiator (set during handshake).
     * Uses the same sudp_derive_gen_key path that periodic rekey
     * uses, so gen transitions are seamless. */
    uint8_t init_key[YUMI_AEAD_KEY_LEN];
    uint8_t resp_key[YUMI_AEAD_KEY_LEN];

    r = sudp_derive_gen_key(init_key, c->rekey_seed, 0, true);
    if (r != YUMI_CRYPTO_OK) {
        return -1;
    }
    r = sudp_derive_gen_key(resp_key, c->rekey_seed, 0, false);
    if (r != YUMI_CRYPTO_OK) {
        yumi_memzero(init_key, sizeof(init_key));
        return -1;
    }

    /* Assign based on role */
    if (c->is_initiator) {
        memcpy(c->transport_key, init_key, YUMI_AEAD_KEY_LEN);
        memcpy(c->recv_transport_key, resp_key, YUMI_AEAD_KEY_LEN);
    } else {
        memcpy(c->transport_key, resp_key, YUMI_AEAD_KEY_LEN);
        memcpy(c->recv_transport_key, init_key, YUMI_AEAD_KEY_LEN);
    }
    yumi_memzero(init_key, sizeof(init_key));
    yumi_memzero(resp_key, sizeof(resp_key));

    /* Initialize rekey gen and pre-derive AEAD subkeys */
    c->send_rekey_gen = 0;
    c->recv_rekey_gen = 0;

    /* Pre-derive AEAD subkeys for the data path */
    yumi_aead_derive_subkeys(&c->send_subkeys, c->transport_key);
    yumi_aead_derive_subkeys(&c->recv_subkeys, c->recv_transport_key);

    c->transport_key_ready = true;
    SUDP_LOG("keys derived: epoch=%u, transport+rekey ready (initiator=%d)",
             atomic_load_explicit(&c->epoch_id, memory_order_relaxed),
             c->is_initiator);

    return 0;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Handshake — HELLO sizes (includes 1-byte protocol version prefix)
 * ════════════════════════════════════════════════════════════════════════ */

#define HELLO_BASE_LEN (1 /* version */ + GR_PEER_ID_LEN + 4 + 1 + \
                        YUMI_MLKEM_PUBLIC_KEY_LEN + \
                        YUMI_BP512_PUBLIC_KEY_LEN)
/* 1 + 32 + 4 + 1 + 1568 + 129 = 1735 */

/* ════════════════════════════════════════════════════════════════════════
 *  Gen-based rekey helper — derive a transport key for a given generation
 *
 *  Directional: uses "yumi-rekey-gen-init-v1" for initiator→responder
 *  and "yumi-rekey-gen-resp-v1" for responder→initiator.
 *  The 'sending' flag selects the caller's send direction.
 * ════════════════════════════════════════════════════════════════════════ */

static int sudp_derive_gen_key(uint8_t out[YUMI_AEAD_KEY_LEN],
                                const uint8_t seed[YUMI_SUDP_REKEY_SEED_LEN],
                                uint32_t gen,
                                bool init_direction)
{
    uint8_t gen_be[4];
    gen_be[0] = (uint8_t)((gen >> 24) & 0xFF);
    gen_be[1] = (uint8_t)((gen >> 16) & 0xFF);
    gen_be[2] = (uint8_t)((gen >>  8) & 0xFF);
    gen_be[3] = (uint8_t)(gen & 0xFF);
    static const uint8_t info_init[] = "yumi-rekey-gen-init-v1";
    static const uint8_t info_resp[] = "yumi-rekey-gen-resp-v1";
    const uint8_t *info = init_direction ? info_init : info_resp;
    size_t info_len = init_direction ? sizeof(info_init) - 1
                                     : sizeof(info_resp) - 1;
    return yumi_hkdf(out, YUMI_AEAD_KEY_LEN,
                      seed, YUMI_SUDP_REKEY_SEED_LEN,
                      gen_be, 4,
                      info, info_len);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Epoch rotation rekey — derive new rekey_seed and directional keys
 *
 *  new_rekey_seed = HKDF(old_seed || new_epoch_key, "yumi-epoch-rekey-v1")
 *  send_key = HKDF(new_seed, gen=0, "yumi-rekey-gen-{init,resp}-v1")
 *  recv_key = HKDF(new_seed, gen=0, "yumi-rekey-gen-{resp,init}-v1")
 * ════════════════════════════════════════════════════════════════════════ */

static int sudp_epoch_rekey(yumi_sudp_client_t *c, const gr_epoch_t *epoch)
{
    /* Derive new rekey seed from old seed + new epoch key */
    uint8_t ikm[YUMI_SUDP_REKEY_SEED_LEN + YUMI_EPOCH_KEY_LEN];
    memcpy(ikm, c->rekey_seed, YUMI_SUDP_REKEY_SEED_LEN);
    memcpy(ikm + YUMI_SUDP_REKEY_SEED_LEN, epoch->epoch_key,
           YUMI_EPOCH_KEY_LEN);

    uint8_t new_seed[YUMI_SUDP_REKEY_SEED_LEN];
    static const uint8_t info[] = "yumi-epoch-rekey-v1";
    int r = yumi_hkdf(new_seed, YUMI_SUDP_REKEY_SEED_LEN,
                       ikm, sizeof(ikm), NULL, 0,
                       info, sizeof(info) - 1);
    yumi_memzero(ikm, sizeof(ikm));
    if (r != YUMI_CRYPTO_OK) return -1;

    /* Derive directional gen-0 keys from new seed */
    uint8_t send_key[YUMI_AEAD_KEY_LEN];
    uint8_t recv_key[YUMI_AEAD_KEY_LEN];
    bool send_is_init = c->is_initiator;
    if (sudp_derive_gen_key(send_key, new_seed, 0, send_is_init)
            != YUMI_CRYPTO_OK ||
        sudp_derive_gen_key(recv_key, new_seed, 0, !send_is_init)
            != YUMI_CRYPTO_OK)
    {
        yumi_memzero(new_seed, sizeof(new_seed));
        yumi_memzero(send_key, sizeof(send_key));
        yumi_memzero(recv_key, sizeof(recv_key));
        return -1;
    }

    /* ── Write lock: commit all shared crypto state atomically ── */
    pthread_rwlock_wrlock(&c->crypto_lock);

    yumi_memzero(c->rekey_seed, YUMI_SUDP_REKEY_SEED_LEN);
    memcpy(c->rekey_seed, new_seed, YUMI_SUDP_REKEY_SEED_LEN);
    memcpy(c->transport_key, send_key, YUMI_AEAD_KEY_LEN);
    memcpy(c->recv_transport_key, recv_key, YUMI_AEAD_KEY_LEN);
    memcpy(c->epoch_key, epoch->epoch_key, YUMI_AEAD_KEY_LEN);
    atomic_store_explicit(&c->epoch_id, epoch->epoch_id, memory_order_release);
    c->send_rekey_gen = 0;
    c->recv_rekey_gen = 0;

    /* Refresh cached subkeys */
    yumi_aead_derive_subkeys(&c->send_subkeys, c->transport_key);
    yumi_aead_derive_subkeys(&c->recv_subkeys, c->recv_transport_key);
    yumi_aead_derive_subkeys(&c->epoch_subkeys, c->epoch_key);

    pthread_rwlock_unlock(&c->crypto_lock);

    yumi_memzero(new_seed, sizeof(new_seed));
    yumi_memzero(send_key, sizeof(send_key));
    yumi_memzero(recv_key, sizeof(recv_key));

    /* Re-validate peer status on epoch rotation */
    gr_peer_t epoch_peer;
    if (gr_peer_get(c->registrar, c->peer_id, &epoch_peer) == GR_OK) {
        if (epoch_peer.status != GR_PEER_ACTIVE)
            atomic_store_explicit(&c->peer_active, false, memory_order_release);
    }

    SUDP_LOG("epoch rekey: new epoch=%u", epoch->epoch_id);
    return 0;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Process HELLO — responder side
 *
 *  Receives the initiator's ephemeral public keys, encapsulates,
 *  computes shared secrets, derives transport key, sends RESPONSE.
 * ════════════════════════════════════════════════════════════════════════ */

static void sudp_set_state(yumi_sudp_client_t *c, yumi_sudp_state_t s)
{
    atomic_store_explicit(&c->state, (int)s, memory_order_release);
    if (c->state_cb)
        c->state_cb(c->user, s);
}

static void sudp_process_hello(yumi_sudp_client_t *c,
                                const uint8_t *msg, size_t msg_len)
{
    sudp_hs_ctx_t *h = c->hs;

    if (msg_len < HELLO_BASE_LEN) return;

    /* Check protocol version (first byte) */
    uint8_t version = msg[0];
    if (version != YUMI_SUDP_PROTOCOL_VERSION) {
        SUDP_LOG("HELLO: unsupported version %u", version);
        return;
    }

    /* Simultaneous-open tiebreaker: lower peer_id is the initiator.
     * peer_id is at offset 1 (after version byte). */
    if (h->state == HS_HELLO_SENT) {
        if (memcmp(c->identity->peer_id, msg + 1, GR_PEER_ID_LEN) < 0)
            return; /* we're the initiator, ignore their HELLO */
        /* They win — we become responder. Reset our handshake. */
        hs_reasm_reset(h);
        if (h->bp512) { yumi_bp512_keypair_free(h->bp512); h->bp512 = NULL; }
        if (h->transcript) yumi_skein_free(h->transcript);
        if (yumi_skein_init(&h->transcript) != YUMI_CRYPTO_OK) {
            sudp_set_state(c, YUMI_SUDP_FAILED);
            return;
        }
    }

    /* Parse HELLO: [version(1)][peer_id(32)][epoch_id(4)][first_contact(1)][...] */
    size_t off = 1; /* skip version */
    const uint8_t *init_peer_id = msg + off; off += GR_PEER_ID_LEN;
    uint32_t init_epoch;
    memcpy(&init_epoch, msg + off, 4); off += 4;
    bool first_contact = msg[off] != 0; off += 1;
    const uint8_t *init_mlkem_pk = msg + off; off += YUMI_MLKEM_PUBLIC_KEY_LEN;
    const uint8_t *init_bp512_pk = msg + off; off += YUMI_BP512_PUBLIC_KEY_LEN;

    const uint8_t *init_frodo_pk = NULL;
    if (first_contact) {
        if (msg_len < HELLO_BASE_LEN + YUMI_FRODO_PUBLIC_KEY_LEN) return;
        init_frodo_pk = msg + off; off += YUMI_FRODO_PUBLIC_KEY_LEN;
    }

    /* Verify peer is in the registrar */
    gr_peer_t peer;
    if (gr_peer_get(c->registrar, init_peer_id, &peer) != GR_OK) return;
    if (peer.status != GR_PEER_ACTIVE) return;
    atomic_store_explicit(&c->peer_active, true, memory_order_release);

    memcpy(h->peer_id, init_peer_id, GR_PEER_ID_LEN);
    memcpy(c->peer_id, init_peer_id, GR_PEER_ID_LEN);
    h->first_contact = first_contact;
    c->first_contact = first_contact;

    /* We are the responder — the HELLO sender is the initiator */
    c->is_initiator = false;

    /* Set handshake deadline for the responder side */
    if (c->hs_deadline_us == 0) {
        c->hs_deadline_us = sudp_monotonic_us()
            + (first_contact ? YUMI_SUDP_HS_TIMEOUT_FIRST_US
                             : YUMI_SUDP_HS_TIMEOUT_SUBSEQ_US);
    }

    /* ── Encapsulate / key exchange ────────────────────────────── */

    /* Declare all shared secrets up front so the fail label can wipe them */
    uint8_t mlkem_ct[YUMI_MLKEM_CIPHERTEXT_LEN];
    uint8_t mlkem_ss[YUMI_MLKEM_SHARED_SECRET_LEN];
    uint8_t bp512_ss[YUMI_BP512_SHARED_SECRET_LEN];
    uint8_t frodo_ss[YUMI_FRODO_SHARED_SECRET_LEN];
    memset(mlkem_ss, 0, sizeof(mlkem_ss));
    memset(bp512_ss, 0, sizeof(bp512_ss));
    memset(frodo_ss, 0, sizeof(frodo_ss));
    size_t bp512_ss_len = 0;
    uint8_t frodo_ct_buf[1]; /* placeholder if not used */
    uint8_t *frodo_ct = frodo_ct_buf;
    size_t frodo_ct_len = 0;

    /* ML-KEM-1024 */
    if (yumi_mlkem_encaps(mlkem_ct, mlkem_ss, init_mlkem_pk) != YUMI_CRYPTO_OK)
        goto fail;

    /* BrainPool-512 ECDH */
    if (yumi_bp512_keygen(&h->bp512) != YUMI_CRYPTO_OK) goto fail;
    if (yumi_bp512_get_public_key(h->bp512, h->bp512_pk, &h->bp512_pk_len)
        != YUMI_CRYPTO_OK) goto fail;
    if (yumi_bp512_ecdh(bp512_ss, &bp512_ss_len, h->bp512,
                         init_bp512_pk, YUMI_BP512_PUBLIC_KEY_LEN)
        != YUMI_CRYPTO_OK) goto fail;

    /* FrodoKEM-1344 (first contact only) */
    if (first_contact) {
        uint8_t *fct = malloc(YUMI_FRODO_CIPHERTEXT_LEN);
        if (!fct) goto fail;
        if (yumi_frodo_encaps(fct, frodo_ss, init_frodo_pk) != YUMI_CRYPTO_OK) {
            free(fct);
            goto fail;
        }
        frodo_ct = fct;
        frodo_ct_len = YUMI_FRODO_CIPHERTEXT_LEN;
    }

    /* ── Compute transcript hash ───────────────────────────────── */

    /* Feed HELLO data */
    yumi_skein_update(h->transcript, msg, msg_len);

    /* Feed RESPONSE key material (what we'll send, minus signature) */
    yumi_skein_update(h->transcript, c->identity->peer_id, GR_PEER_ID_LEN);
    yumi_skein_update(h->transcript, mlkem_ct, YUMI_MLKEM_CIPHERTEXT_LEN);
    yumi_skein_update(h->transcript, h->bp512_pk, (size_t)h->bp512_pk_len);
    if (first_contact)
        yumi_skein_update(h->transcript, frodo_ct, frodo_ct_len);

    uint8_t transcript_hash[YUMI_SKEIN_HASH_LEN];
    yumi_skein_final(h->transcript, transcript_hash);
    yumi_skein_free(h->transcript);
    h->transcript = NULL;

    /* ── Sign transcript with our ML-DSA-87 key ───────────────── */

    uint8_t signature[YUMI_MLDSA_SIGN_LEN];
    if (yumi_mldsa_sign(signature, transcript_hash, YUMI_SKEIN_HASH_LEN,
                         c->identity->secret_key) != YUMI_CRYPTO_OK) {
        if (first_contact) free(frodo_ct);
        goto fail;
    }

    /* ── Build and send RESPONSE message ───────────────────────── */

    size_t resp_len = 1 /* version */ + GR_PEER_ID_LEN
                    + YUMI_MLKEM_CIPHERTEXT_LEN
                    + YUMI_BP512_PUBLIC_KEY_LEN
                    + (first_contact ? YUMI_FRODO_CIPHERTEXT_LEN : 0)
                    + YUMI_MLDSA_SIGN_LEN;
    uint8_t *resp = malloc(resp_len);
    if (!resp) { if (first_contact) free(frodo_ct); goto fail; }

    size_t roff = 0;
    resp[roff++] = YUMI_SUDP_PROTOCOL_VERSION;
    memcpy(resp + roff, c->identity->peer_id, GR_PEER_ID_LEN);
    roff += GR_PEER_ID_LEN;
    memcpy(resp + roff, mlkem_ct, YUMI_MLKEM_CIPHERTEXT_LEN);
    roff += YUMI_MLKEM_CIPHERTEXT_LEN;
    memcpy(resp + roff, h->bp512_pk, YUMI_BP512_PUBLIC_KEY_LEN);
    roff += YUMI_BP512_PUBLIC_KEY_LEN;
    if (first_contact) {
        memcpy(resp + roff, frodo_ct, YUMI_FRODO_CIPHERTEXT_LEN);
        roff += YUMI_FRODO_CIPHERTEXT_LEN;
        free(frodo_ct);
        frodo_ct = NULL;
    }
    memcpy(resp + roff, signature, YUMI_MLDSA_SIGN_LEN);

    /* ── Derive transport key ──────────────────────────────────── */

    if (sudp_derive_keys(c, mlkem_ss,
                          first_contact ? frodo_ss : NULL,
                          bp512_ss, bp512_ss_len) != 0) {
        free(resp);
        goto fail;
    }

    /* Wipe shared secrets */
    yumi_memzero(mlkem_ss, sizeof(mlkem_ss));
    yumi_memzero(bp512_ss, sizeof(bp512_ss));
    if (first_contact) yumi_memzero(frodo_ss, sizeof(frodo_ss));

    /* Store transcript hash for CONFIRM verification */
    memcpy(h->transcript_hash_saved, transcript_hash,
           YUMI_SKEIN_HASH_LEN);

    /* Send RESPONSE */
    h->state = HS_WAIT_CONFIRM;
    h->expected_msg = SUDP_MSG_CONFIRM;
    if (sudp_frag_send(c, SUDP_MSG_RESPONSE, resp, resp_len) != 0) {
        free(resp);
        goto fail;
    }

    free(resp);
    return;

fail:
    yumi_memzero(mlkem_ss, sizeof(mlkem_ss));
    yumi_memzero(bp512_ss, sizeof(bp512_ss));
    yumi_memzero(frodo_ss, sizeof(frodo_ss));
    sudp_set_state(c, YUMI_SUDP_FAILED);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Process RESPONSE — initiator side
 *
 *  Decapsulates shared secrets, derives transport key, verifies
 *  responder signature, sends CONFIRM.
 * ════════════════════════════════════════════════════════════════════════ */

static void sudp_process_response(yumi_sudp_client_t *c,
                                   const uint8_t *msg, size_t msg_len)
{
    sudp_hs_ctx_t *h = c->hs;
    if (h->state != HS_HELLO_SENT) return;

    bool fc = h->first_contact;
    size_t min_len = 1 /* version */ + GR_PEER_ID_LEN
                   + YUMI_MLKEM_CIPHERTEXT_LEN
                   + YUMI_BP512_PUBLIC_KEY_LEN
                   + (fc ? YUMI_FRODO_CIPHERTEXT_LEN : 0)
                   + YUMI_MLDSA_SIGN_LEN;
    if (msg_len < min_len) return;

    /* Check protocol version */
    if (msg[0] != YUMI_SUDP_PROTOCOL_VERSION) {
        SUDP_LOG("RESPONSE: unsupported version %u", msg[0]);
        return;
    }

    /* Parse RESPONSE: [version(1)][peer_id][mlkem_ct][bp512_pk][frodo_ct?][sig] */
    size_t off = 1; /* skip version */
    const uint8_t *resp_peer_id  = msg + off; off += GR_PEER_ID_LEN;
    const uint8_t *resp_mlkem_ct = msg + off; off += YUMI_MLKEM_CIPHERTEXT_LEN;
    const uint8_t *resp_bp512_pk = msg + off; off += YUMI_BP512_PUBLIC_KEY_LEN;
    const uint8_t *resp_frodo_ct = NULL;
    if (fc) {
        resp_frodo_ct = msg + off; off += YUMI_FRODO_CIPHERTEXT_LEN;
    }
    const uint8_t *resp_sig = msg + off;

    /* Verify peer is in registrar */
    gr_peer_t peer;
    if (gr_peer_get(c->registrar, resp_peer_id, &peer) != GR_OK) return;
    if (peer.status != GR_PEER_ACTIVE) return;

    /* Verify this is the peer we expected */
    if (yumi_memcmp(resp_peer_id, h->peer_id, GR_PEER_ID_LEN) != 0) return;

    /* ── Decapsulate / ECDH ────────────────────────────────────── */

    /* Zero-initialize so the fail label can safely wipe them */
    uint8_t mlkem_ss[YUMI_MLKEM_SHARED_SECRET_LEN];
    uint8_t bp512_ss[YUMI_BP512_SHARED_SECRET_LEN];
    uint8_t frodo_ss[YUMI_FRODO_SHARED_SECRET_LEN];
    memset(mlkem_ss, 0, sizeof(mlkem_ss));
    memset(bp512_ss, 0, sizeof(bp512_ss));
    memset(frodo_ss, 0, sizeof(frodo_ss));

    /* ML-KEM-1024 */
    if (yumi_mlkem_decaps(mlkem_ss, resp_mlkem_ct, h->mlkem_sk)
        != YUMI_CRYPTO_OK) goto fail;

    /* BrainPool-512 ECDH */
    size_t bp512_ss_len;
    if (yumi_bp512_ecdh(bp512_ss, &bp512_ss_len, h->bp512,
                         resp_bp512_pk, YUMI_BP512_PUBLIC_KEY_LEN)
        != YUMI_CRYPTO_OK) goto fail;

    /* FrodoKEM-1344 */
    if (fc) {
        if (yumi_frodo_decaps(frodo_ss, resp_frodo_ct, h->frodo_sk)
            != YUMI_CRYPTO_OK) goto fail;
    }

    /* ── Compute and verify transcript hash ────────────────────── */

    /* The transcript was started in connect() with HELLO data.
     * Now add RESPONSE key material (without the signature). */
    yumi_skein_update(h->transcript, resp_peer_id, GR_PEER_ID_LEN);
    yumi_skein_update(h->transcript, resp_mlkem_ct, YUMI_MLKEM_CIPHERTEXT_LEN);
    yumi_skein_update(h->transcript, resp_bp512_pk, YUMI_BP512_PUBLIC_KEY_LEN);
    if (fc)
        yumi_skein_update(h->transcript, resp_frodo_ct,
                          YUMI_FRODO_CIPHERTEXT_LEN);

    uint8_t transcript_hash[YUMI_SKEIN_HASH_LEN];
    yumi_skein_final(h->transcript, transcript_hash);
    yumi_skein_free(h->transcript);
    h->transcript = NULL;

    /* Verify responder's ML-DSA-87 signature over transcript */
    if (yumi_mldsa_verify(resp_sig, transcript_hash, YUMI_SKEIN_HASH_LEN,
                           peer.sign_key) != YUMI_CRYPTO_OK) goto fail;

    /* ── Derive transport key ──────────────────────────────────── */

    if (sudp_derive_keys(c, mlkem_ss,
                          fc ? frodo_ss : NULL,
                          bp512_ss, bp512_ss_len) != 0) goto fail;

    /* Wipe shared secrets */
    yumi_memzero(mlkem_ss, sizeof(mlkem_ss));
    yumi_memzero(bp512_ss, sizeof(bp512_ss));
    if (fc) yumi_memzero(frodo_ss, sizeof(frodo_ss));

    /* ── Sign transcript and send CONFIRM ──────────────────────── */

    uint8_t confirm_msg[1 + YUMI_MLDSA_SIGN_LEN];
    confirm_msg[0] = YUMI_SUDP_PROTOCOL_VERSION;
    if (yumi_mldsa_sign(confirm_msg + 1, transcript_hash, YUMI_SKEIN_HASH_LEN,
                         c->identity->secret_key) != YUMI_CRYPTO_OK) goto fail;

    h->state = HS_DONE;
    if (sudp_frag_send(c, SUDP_MSG_CONFIRM, confirm_msg,
                        sizeof(confirm_msg)) != 0) goto fail;

    /* ── Session established ───────────────────────────────────── */

    sudp_set_state(c, YUMI_SUDP_ESTABLISHED);

    /* Free handshake context — keys are no longer needed */
    hs_ctx_destroy(c->hs);
    c->hs = NULL;
    return;

fail:
    yumi_memzero(mlkem_ss, sizeof(mlkem_ss));
    yumi_memzero(bp512_ss, sizeof(bp512_ss));
    yumi_memzero(frodo_ss, sizeof(frodo_ss));
    sudp_set_state(c, YUMI_SUDP_FAILED);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Process CONFIRM — responder side
 *
 *  Verifies the initiator's signature over the transcript.
 * ════════════════════════════════════════════════════════════════════════ */

static void sudp_process_confirm(yumi_sudp_client_t *c,
                                  const uint8_t *msg, size_t msg_len)
{
    sudp_hs_ctx_t *h = c->hs;
    if (h->state != HS_WAIT_CONFIRM) return;
    if (msg_len < 1 + YUMI_MLDSA_SIGN_LEN) return;

    /* Check protocol version */
    if (msg[0] != YUMI_SUDP_PROTOCOL_VERSION) {
        SUDP_LOG("CONFIRM: unsupported version %u", msg[0]);
        return;
    }
    const uint8_t *sig = msg + 1;  /* skip version byte */

    /* Look up the initiator's signing key */
    gr_peer_t peer;
    if (gr_peer_get(c->registrar, h->peer_id, &peer) != GR_OK) goto fail;
    if (peer.status != GR_PEER_ACTIVE) goto fail;

    /* Transcript hash was stored by process_hello */
    const uint8_t *transcript_hash = h->transcript_hash_saved;

    /* Verify initiator's ML-DSA-87 signature */
    if (yumi_mldsa_verify(sig, transcript_hash, YUMI_SKEIN_HASH_LEN,
                           peer.sign_key) != YUMI_CRYPTO_OK) goto fail;

    /* ── Session established ───────────────────────────────────── */

    h->state = HS_DONE;
    sudp_set_state(c, YUMI_SUDP_ESTABLISHED);

    hs_ctx_destroy(c->hs);
    c->hs = NULL;
    return;

fail:
    sudp_set_state(c, YUMI_SUDP_FAILED);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Handshake dispatcher
 * ════════════════════════════════════════════════════════════════════════ */

static void sudp_process_handshake_msg(yumi_sudp_client_t *c,
                                        uint8_t msg_type,
                                        const uint8_t *msg, size_t msg_len)
{
    switch (msg_type) {
    case SUDP_MSG_HELLO:    sudp_process_hello(c, msg, msg_len);    break;
    case SUDP_MSG_RESPONSE: sudp_process_response(c, msg, msg_len); break;
    case SUDP_MSG_CONFIRM:  sudp_process_confirm(c, msg, msg_len);  break;
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Anti-replay window
 * ════════════════════════════════════════════════════════════════════════ */

static void replay_shift_bitmap(sudp_replay_t *r, uint64_t shift)
{
    if (shift >= SUDP_REPLAY_WINDOW) {
        memset(r->bitmap, 0, sizeof(r->bitmap));
        return;
    }
    /* Shift the 256-bit bitmap left by 'shift' positions.
     * bitmap[0] is the most-significant word (newest bits). */
    uint64_t word_shift = shift / 64;
    uint64_t bit_shift  = shift % 64;

    /* Move words first */
    if (word_shift > 0) {
        for (int i = 0; i < 4; i++) {
            int src = (int)(i + word_shift);
            r->bitmap[i] = (src < 4) ? r->bitmap[src] : 0;
        }
    }
    /* Then shift bits within words */
    if (bit_shift > 0) {
        for (int i = 0; i < 3; i++)
            r->bitmap[i] = (r->bitmap[i] << bit_shift)
                         | (r->bitmap[i + 1] >> (64 - bit_shift));
        r->bitmap[3] <<= bit_shift;
    }
}

static bool sudp_replay_check(sudp_replay_t *r, uint64_t seq)
{
    if (seq > r->max_seq) {
        replay_shift_bitmap(r, seq - r->max_seq);
        r->bitmap[0] |= 1;  /* mark position 0 (current max) */
        r->max_seq = seq;
        return true;
    }
    uint64_t diff = r->max_seq - seq;
    if (diff >= SUDP_REPLAY_WINDOW) return false;
    uint64_t word = diff / 64;
    uint64_t bit  = diff % 64;
    uint64_t mask = 1ULL << bit;
    if (r->bitmap[word] & mask) return false;
    r->bitmap[word] |= mask;
    return true;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Datagram-level encrypt / decrypt hooks
 *
 *  Called by the inner UDP client's send/recv paths to wrap every
 *  datagram.  Transport headers are never exposed on the wire.
 * ════════════════════════════════════════════════════════════════════════ */

static int sudp_pkt_encrypt(void *ctx, const uint8_t *pt, uint32_t pt_len,
                             uint8_t *ct, uint32_t *ct_len)
{
    yumi_sudp_client_t *c = (yumi_sudp_client_t *)ctx;

    /* Mutable copy buffer for epoch-id patching (avoids const violation) */
    uint8_t pt_copy[YUMI_UDP_MAX_DGRAM];
    const uint8_t *enc_src = pt;

    /* ── Handshake timeout check ───────────────────────────────── */
    int st = atomic_load_explicit(&c->state, memory_order_acquire);
    if (st == (int)YUMI_SUDP_HANDSHAKING && c->hs_deadline_us != 0) {
        if (sudp_monotonic_us() > c->hs_deadline_us) {
            SUDP_LOG("handshake timed out (encrypt path)");
            sudp_set_state(c, YUMI_SUDP_FAILED);
            return -1;
        }
    }

    /* Key selection: epoch_id in the inner payload (after 10-byte hdr).
     * epoch_id == 0 → handshake → epoch key.
     * epoch_id >  0 → data      → transport key. */
    const yumi_aead_subkeys_t *subkeys;
    yumi_aead_subkeys_t epoch_sk_snap;  /* snapshot for epoch key path */
    if (pt_len > YUMI_UDP_HDR_SIZE + 4) {
        uint32_t eid_n;
        memcpy(&eid_n, pt + YUMI_UDP_HDR_SIZE, 4);
        if (eid_n == 0) {
            pthread_rwlock_rdlock(&c->crypto_lock);
            memcpy(&epoch_sk_snap, &c->epoch_subkeys, sizeof(epoch_sk_snap));
            pthread_rwlock_unlock(&c->crypto_lock);
            subkeys = &epoch_sk_snap;
        } else {
            subkeys = &c->send_subkeys;

            /* ── Epoch rotation rekey (worker thread only) ─────
             * Only query the registrar every 30 s to avoid
             * hitting DuckDB on every packet.  The decrypt
             * fallback path still catches epoch changes
             * immediately for incoming traffic. */
            if (c->transport_key_ready) {
                uint64_t now_enc = sudp_monotonic_us();
                if (now_enc - c->epoch_check_last_us
                        >= SUDP_EPOCH_CHECK_INTERVAL_US) {
                    c->epoch_check_last_us = now_enc;
                    gr_epoch_t cur_ep;
                    if (gr_epoch_get_current(c->registrar, &cur_ep)
                            == GR_OK) {
                        uint32_t cur_eid = atomic_load_explicit(
                            &c->epoch_id, memory_order_relaxed);
                        if (cur_ep.epoch_id != cur_eid) {
                            if (sudp_epoch_rekey(c, &cur_ep) == 0) {
                                memcpy(pt_copy, pt, pt_len);
                                uint32_t new_eid_n = htonl(
                                    atomic_load_explicit(&c->epoch_id,
                                        memory_order_relaxed));
                                memcpy(pt_copy + YUMI_UDP_HDR_SIZE,
                                       &new_eid_n, 4);
                                enc_src = pt_copy;
                            }
                        }
                        yumi_memzero(cur_ep.epoch_key,
                                     sizeof(cur_ep.epoch_key));
                    }
                }
            }
        }
    } else {
        /* ACK / SACK (header-only or small control) — during the handshake
         * the responder derives transport_key before the initiator has it,
         * so ACKs must stay epoch-key-encrypted until both sides are
         * ESTABLISHED and guaranteed to hold the transport key. */
        if (st == (int)YUMI_SUDP_ESTABLISHED && c->transport_key_ready) {
            subkeys = &c->send_subkeys;
        } else {
            pthread_rwlock_rdlock(&c->crypto_lock);
            memcpy(&epoch_sk_snap, &c->epoch_subkeys, sizeof(epoch_sk_snap));
            pthread_rwlock_unlock(&c->crypto_lock);
            subkeys = &epoch_sk_snap;
        }
    }

    /* Nonce: nonce_token(8) || counter(8 BE) */
    uint8_t nonce[YUMI_AEAD_NONCE_LEN];
    memcpy(nonce, c->nonce_token, 8);
    uint64_t ctr = atomic_fetch_add_explicit(&c->nonce_counter, 1,
                                              memory_order_relaxed);

    /* ── Periodic nonce-counter-based rekey ─────────────────────── */
    if (subkeys == &c->send_subkeys && c->transport_key_ready) {
        uint32_t gen = (uint32_t)(ctr / SUDP_REKEY_INTERVAL);
        if (gen != c->send_rekey_gen) {
            pthread_rwlock_rdlock(&c->crypto_lock);
            uint8_t seed_copy[YUMI_SUDP_REKEY_SEED_LEN];
            memcpy(seed_copy, c->rekey_seed, YUMI_SUDP_REKEY_SEED_LEN);
            pthread_rwlock_unlock(&c->crypto_lock);
            if (sudp_derive_gen_key(c->transport_key, seed_copy,
                                     gen, c->is_initiator) == YUMI_CRYPTO_OK) {
                c->send_rekey_gen = gen;
                yumi_aead_derive_subkeys(&c->send_subkeys, c->transport_key);
                SUDP_LOG("send rekey gen=%u (counter=%lu)", gen,
                         (unsigned long)ctr);
            }
            yumi_memzero(seed_copy, sizeof(seed_copy));
        }
    }

    for (int i = 7; i >= 0; i--) {
        nonce[8 + i] = (uint8_t)(ctr & 0xFF);
        ctr >>= 8;
    }

    /* Output: [nonce(16)] [AEAD(pt) || tag(128)] */
    memcpy(ct, nonce, YUMI_AEAD_NONCE_LEN);

    size_t aead_out;
    if (yumi_aead_encrypt_keyed(ct + YUMI_AEAD_NONCE_LEN, &aead_out,
                                 enc_src, pt_len,
                                 NULL, 0,
                                 nonce, subkeys) != YUMI_CRYPTO_OK)
        return -1;

    *ct_len = (uint32_t)(YUMI_AEAD_NONCE_LEN + aead_out);
    return 0;
}

static int sudp_pkt_decrypt(void *ctx, const uint8_t *ct_in, uint32_t ct_len,
                             uint8_t *pt, uint32_t *pt_len)
{
    yumi_sudp_client_t *c = (yumi_sudp_client_t *)ctx;

    /* ── Handshake timeout check ───────────────────────────────── */
    int st = atomic_load_explicit(&c->state, memory_order_acquire);
    if (st == (int)YUMI_SUDP_HANDSHAKING && c->hs_deadline_us != 0) {
        if (sudp_monotonic_us() > c->hs_deadline_us) {
            SUDP_LOG("handshake timed out (decrypt path)");
            sudp_set_state(c, YUMI_SUDP_FAILED);
            return -1;
        }
    }

    if (ct_len < YUMI_AEAD_NONCE_LEN + YUMI_AEAD_TAG_LEN + 1)
        return -1;

    const uint8_t *nonce     = ct_in;
    const uint8_t *aead_ct   = ct_in + YUMI_AEAD_NONCE_LEN;
    uint32_t       aead_len  = ct_len - YUMI_AEAD_NONCE_LEN;

    /* Extract sender's counter from the nonce for gen computation */
    uint64_t sender_ctr = 0;
    for (int i = 0; i < 8; i++)
        sender_ctr = (sender_ctr << 8) | nonce[8 + i];

    size_t out_len;

    /* Try transport key — with gen-based recv key selection */
    if (c->transport_key_ready) {
        uint32_t sender_gen = (uint32_t)(sender_ctr / SUDP_REKEY_INTERVAL);

        /* Fast path: sender's gen matches our cached recv gen */
        if (sender_gen == c->recv_rekey_gen) {
            if (yumi_aead_decrypt_keyed(pt, &out_len, aead_ct, aead_len,
                                         NULL, 0, nonce, &c->recv_subkeys)
                == YUMI_CRYPTO_OK)
            {
                *pt_len = (uint32_t)out_len;
                if (!sudp_replay_check(&c->replay, sender_ctr)) return -1;
                return 0;
            }
        } else {
            /* Sender has advanced to a new gen — derive their key.
             * The recv direction is the sender's send direction,
             * which is the opposite of our role. */
            uint8_t gen_key[YUMI_AEAD_KEY_LEN];
            pthread_rwlock_rdlock(&c->crypto_lock);
            uint8_t seed_copy[YUMI_SUDP_REKEY_SEED_LEN];
            memcpy(seed_copy, c->rekey_seed, YUMI_SUDP_REKEY_SEED_LEN);
            pthread_rwlock_unlock(&c->crypto_lock);
            if (sudp_derive_gen_key(gen_key, seed_copy, sender_gen,
                                     !c->is_initiator)
                == YUMI_CRYPTO_OK)
            {
                yumi_aead_subkeys_t gen_subkeys;
                yumi_aead_derive_subkeys(&gen_subkeys, gen_key);
                if (yumi_aead_decrypt_keyed(pt, &out_len, aead_ct, aead_len,
                                             NULL, 0, nonce, &gen_subkeys)
                    == YUMI_CRYPTO_OK)
                {
                    /* Accept the gen advance */
                    memcpy(c->recv_transport_key, gen_key, YUMI_AEAD_KEY_LEN);
                    c->recv_rekey_gen = sender_gen;
                    memcpy(&c->recv_subkeys, &gen_subkeys, sizeof(gen_subkeys));
                    yumi_memzero(gen_key, sizeof(gen_key));
                    yumi_aead_subkeys_wipe(&gen_subkeys);
                    *pt_len = (uint32_t)out_len;
                    SUDP_LOG("recv rekey gen=%u", sender_gen);
                    if (!sudp_replay_check(&c->replay, sender_ctr)) return -1;
                    return 0;
                }
                yumi_aead_subkeys_wipe(&gen_subkeys);
            }
            yumi_memzero(gen_key, sizeof(gen_key));
            yumi_memzero(seed_copy, sizeof(seed_copy));
        }
    }

    /* Fall back to epoch key (handshake packets) */
    pthread_rwlock_rdlock(&c->crypto_lock);
    yumi_aead_subkeys_t epoch_sk_copy;
    memcpy(&epoch_sk_copy, &c->epoch_subkeys, sizeof(epoch_sk_copy));
    pthread_rwlock_unlock(&c->crypto_lock);

    if (yumi_aead_decrypt_keyed(pt, &out_len, aead_ct, aead_len,
                                 NULL, 0, nonce, &epoch_sk_copy)
        == YUMI_CRYPTO_OK)
    {
        *pt_len = (uint32_t)out_len;
        if (!sudp_replay_check(&c->replay, sender_ctr)) return -1;
        return 0;
    }

    /* ── Epoch rotation fallback ───────────────────────────────── */
    if (c->transport_key_ready) {
        gr_epoch_t new_epoch;
        if (gr_epoch_get_current(c->registrar, &new_epoch) == GR_OK &&
            new_epoch.epoch_id != atomic_load_explicit(&c->epoch_id,
                                       memory_order_relaxed))
        {
            /* Epoch rotated — try rekeying and decrypting again
             * (sudp_epoch_rekey refreshes all cached subkeys) */
            if (sudp_epoch_rekey(c, &new_epoch) == 0) {
                yumi_memzero(new_epoch.epoch_key, sizeof(new_epoch.epoch_key));
                /* Try new transport key */
                if (yumi_aead_decrypt_keyed(pt, &out_len, aead_ct, aead_len,
                                             NULL, 0, nonce, &c->recv_subkeys)
                    == YUMI_CRYPTO_OK)
                {
                    *pt_len = (uint32_t)out_len;
                    if (!sudp_replay_check(&c->replay, sender_ctr)) return -1;
                    return 0;
                }
                /* Try new epoch key */
                if (yumi_aead_decrypt_keyed(pt, &out_len, aead_ct, aead_len,
                                             NULL, 0, nonce, &c->epoch_subkeys)
                    == YUMI_CRYPTO_OK)
                {
                    *pt_len = (uint32_t)out_len;
                    if (!sudp_replay_check(&c->replay, sender_ctr)) return -1;
                    return 0;
                }
            } else {
                yumi_memzero(new_epoch.epoch_key,
                             sizeof(new_epoch.epoch_key));
            }
        }
    }

    return -1;  /* all keys failed */
}

/* ════════════════════════════════════════════════════════════════════════
 *  Data encrypt + send (simplified — AEAD is at the datagram level)
 * ════════════════════════════════════════════════════════════════════════ */

static int sudp_encrypt_send(yumi_sudp_client_t *c, uint8_t channel,
                              const void *data, uint32_t len, bool reliable)
{
    int s = atomic_load_explicit(&c->state, memory_order_acquire);
    if (s != (int)YUMI_SUDP_ESTABLISHED) return -1;

    /* ── Cached peer status (updated by notify_kick / epoch rekey) ── */
    if (!atomic_load_explicit(&c->peer_active, memory_order_acquire)) {
        sudp_set_state(c, YUMI_SUDP_FAILED);
        return -1;
    }

    /* Inner payload: [epoch_id(4)][channel(1)][user_data] */
    uint32_t inner_len = 4 + 1 + len;
    uint32_t inner_max = yumi_udp_client_get_max_payload(&c->inner);
    if (inner_len > inner_max) return -1;

    uint8_t pkt[YUMI_UDP_MAX_PAYLOAD];

    uint32_t eid_n = htonl(
        atomic_load_explicit(&c->epoch_id, memory_order_acquire));
    memcpy(pkt, &eid_n, 4);
    pkt[4] = channel;
    if (len > 0) memcpy(pkt + 5, data, len);

    if (reliable)
        return yumi_udp_client_send_reliable(&c->inner, pkt, inner_len);
    return yumi_udp_client_send(&c->inner, pkt, inner_len);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Inner client callback — route handshake vs data
 *
 *  All incoming packets arrive already decrypted at the datagram level.
 *  epoch_id == 0 → handshake fragment, else → cleartext data envelope.
 * ════════════════════════════════════════════════════════════════════════ */

static void sudp_inner_recv(void *user, const void *data,
                             uint32_t len, bool reliable, uint32_t seq)
{
    (void)seq;
    yumi_sudp_client_t *c = (yumi_sudp_client_t *)user;
    if (len < 4) return;

    const uint8_t *pkt = (const uint8_t *)data;

    /* Check discriminator: first 4 bytes == 0 → handshake */
    uint32_t eid_n;
    memcpy(&eid_n, pkt, 4);

    if (eid_n == 0) {
        /* ── Handshake fragment ────────────────────────────────── */
        pthread_mutex_lock(&c->hs_mutex);
        if (!c->hs) { pthread_mutex_unlock(&c->hs_mutex); return; }
        if (len < SUDP_HS_FRAG_HDR) { pthread_mutex_unlock(&c->hs_mutex); return; }

        uint8_t msg_type = pkt[4];
        size_t msg_len;
        uint8_t *msg = sudp_frag_receive(c->hs, pkt, len, &msg_len);
        if (msg) {
            sudp_process_handshake_msg(c, msg_type, msg, msg_len);
            free(msg);
        }
        pthread_mutex_unlock(&c->hs_mutex);
    } else {
        /* ── Data (already decrypted at datagram level) ────────── */
        if (len < 5) return;  /* epoch_id(4) + channel(1) minimum */

        /* Cached peer status (updated by notify_kick / epoch rekey) */
        if (!atomic_load_explicit(&c->peer_active, memory_order_acquire)) {
            sudp_set_state(c, YUMI_SUDP_FAILED);
            return;
        }

        uint32_t eid = ntohl(eid_n);
        if (eid != atomic_load_explicit(&c->epoch_id,
                        memory_order_relaxed)) return;

        uint8_t channel = pkt[4];
        uint32_t payload_len = len - 5;
        const void *payload = payload_len > 0 ? pkt + 5 : NULL;

        if (c->recv_cb)
            c->recv_cb(c->user, channel, payload, payload_len, reliable);
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Public API — create
 * ════════════════════════════════════════════════════════════════════════ */

int yumi_sudp_client_create(yumi_sudp_client_t **out,
                             const yumi_sudp_config_t *cfg)
{
    if (!out || !cfg || !cfg->registrar || !cfg->identity) return -1;

    yumi_sudp_client_t *c = calloc(1, sizeof(*c));
    if (!c) return -1;

    c->registrar     = cfg->registrar;
    c->identity      = cfg->identity;
    c->first_contact = cfg->first_contact;
    c->recv_cb       = cfg->recv_cb;
    c->state_cb      = cfg->state_cb;
    c->user          = cfg->user;
    atomic_store(&c->state, (int)YUMI_SUDP_DISCONNECTED);
    atomic_store(&c->nonce_counter, 0);
    atomic_store(&c->peer_active, false);

    /* Create the handshake context */
    c->hs = hs_ctx_create(cfg->first_contact);
    if (!c->hs) { free(c); return -1; }
    pthread_mutex_init(&c->hs_mutex, NULL);
    pthread_rwlock_init(&c->crypto_lock, NULL);

    /* Fetch epoch key for datagram-level encryption */
    gr_epoch_t create_epoch;
    if (gr_epoch_get_current(c->registrar, &create_epoch) != GR_OK) {
        hs_ctx_destroy(c->hs);
        free(c);
        return -1;
    }
    memcpy(c->epoch_key, create_epoch.epoch_key, YUMI_AEAD_KEY_LEN);
    atomic_store(&c->epoch_id, create_epoch.epoch_id);
    yumi_aead_derive_subkeys(&c->epoch_subkeys, c->epoch_key);
    c->epoch_subkeys_ready = true;
    c->epoch_check_last_us = sudp_monotonic_us();
    yumi_memzero(create_epoch.epoch_key, sizeof(create_epoch.epoch_key));

    /* Random nonce prefix for this client's lifetime */
    yumi_randombytes(c->nonce_token, sizeof(c->nonce_token));

    /* Configure inner transport — override recv_cb and install crypto hooks */
    yumi_udp_client_config_t tcfg = cfg->transport;
    tcfg.recv_cb             = sudp_inner_recv;
    tcfg.recv_user           = c;
    tcfg.pkt_encrypt         = sudp_pkt_encrypt;
    tcfg.pkt_decrypt         = sudp_pkt_decrypt;
    tcfg.pkt_crypto_ctx      = c;
    tcfg.pkt_crypto_overhead = YUMI_SUDP_PKT_CRYPTO_OVERHEAD;
    tcfg.user_work_cb        = sudp_worker_connect;
    tcfg.user_work_ctx       = c;

    if (yumi_udp_client_create(&c->inner, &tcfg) != 0) {
        hs_ctx_destroy(c->hs);
        free(c);
        return -1;
    }

    /* Compute max secure payload from inner client's limit */
    uint32_t inner_max = yumi_udp_client_get_max_payload(&c->inner);
    if (inner_max > YUMI_SUDP_ENVELOPE_OVERHEAD)
        c->max_secure_payload = inner_max - YUMI_SUDP_ENVELOPE_OVERHEAD;
    else
        c->max_secure_payload = 0;

    *out = c;
    return 0;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Public API — destroy
 * ════════════════════════════════════════════════════════════════════════ */

void yumi_sudp_client_destroy(yumi_sudp_client_t *c)
{
    if (!c) return;

    yumi_udp_client_destroy(&c->inner);

    /* Wipe all key material */
    yumi_memzero(c->transport_key, sizeof(c->transport_key));
    yumi_memzero(c->recv_transport_key, sizeof(c->recv_transport_key));
    yumi_memzero(c->rekey_seed, sizeof(c->rekey_seed));
    yumi_memzero(c->epoch_key, sizeof(c->epoch_key));
    yumi_memzero(c->nonce_token, sizeof(c->nonce_token));
    yumi_aead_subkeys_wipe(&c->send_subkeys);
    yumi_aead_subkeys_wipe(&c->recv_subkeys);
    yumi_aead_subkeys_wipe(&c->epoch_subkeys);

    if (c->hs) hs_ctx_destroy(c->hs);
    pthread_mutex_destroy(&c->hs_mutex);
    pthread_rwlock_destroy(&c->crypto_lock);

    yumi_memzero(c, sizeof(*c));
    free(c);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Worker-thread user callback — handles deferred connect requests
 *
 *  Handshake mutations are serialised with recv-side processing by
 *  hs_mutex, since send and recv now run on separate threads.
 * ════════════════════════════════════════════════════════════════════════ */

static void sudp_worker_connect(void *ctx, const void *data, uint32_t len)
{
    yumi_sudp_client_t *c = (yumi_sudp_client_t *)ctx;
    if (len != GR_PEER_ID_LEN) return;
    const uint8_t *peer_id = (const uint8_t *)data;

    int s = atomic_load_explicit(&c->state, memory_order_acquire);
    if (s == (int)YUMI_SUDP_ESTABLISHED) return;

    pthread_mutex_lock(&c->hs_mutex);

    sudp_hs_ctx_t *h = c->hs;
    if (!h) {
        h = hs_ctx_create(c->first_contact);
        if (!h) {
            pthread_mutex_unlock(&c->hs_mutex);
            sudp_set_state(c, YUMI_SUDP_FAILED);
            return;
        }
        c->hs = h;
    }

    /* If the worker already processed an incoming HELLO (passive responder
     * or simultaneous-open resolved), don't clobber the handshake. */
    if (h->state != HS_IDLE) {
        pthread_mutex_unlock(&c->hs_mutex);
        if (atomic_load(&c->state) == (int)YUMI_SUDP_DISCONNECTED)
            sudp_set_state(c, YUMI_SUDP_HANDSHAKING);
        return;
    }

    memcpy(h->peer_id, peer_id, GR_PEER_ID_LEN);
    memcpy(c->peer_id, peer_id, GR_PEER_ID_LEN);
    atomic_store_explicit(&c->peer_active, true, memory_order_release);

    /* We are the initiator (connect caller).  The simultaneous-open
     * tiebreaker in process_hello may override this if we lose. */
    c->is_initiator = true;

    /* ── Generate ephemeral keys ───────────────────────────────── */

    if (yumi_mlkem_keygen(h->mlkem_pk, h->mlkem_sk) != YUMI_CRYPTO_OK)
        goto fail;

    if (yumi_bp512_keygen(&h->bp512) != YUMI_CRYPTO_OK) goto fail;
    if (yumi_bp512_get_public_key(h->bp512, h->bp512_pk, &h->bp512_pk_len)
        != YUMI_CRYPTO_OK) goto fail;

    if (h->first_contact) {
        if (yumi_frodo_keygen(h->frodo_pk, h->frodo_sk) != YUMI_CRYPTO_OK)
            goto fail;
    }

    /* ── Build HELLO message ───────────────────────────────────── */

    gr_epoch_t epoch;
    if (gr_epoch_get_current(c->registrar, &epoch) != GR_OK) goto fail;

    size_t hello_len = HELLO_BASE_LEN
                     + (h->first_contact ? YUMI_FRODO_PUBLIC_KEY_LEN : 0);
    uint8_t *hello = malloc(hello_len);
    if (!hello) goto fail;

    size_t off = 0;
    hello[off++] = YUMI_SUDP_PROTOCOL_VERSION;
    memcpy(hello + off, c->identity->peer_id, GR_PEER_ID_LEN);
    off += GR_PEER_ID_LEN;
    memcpy(hello + off, &epoch.epoch_id, 4);
    off += 4;
    hello[off++] = h->first_contact ? 1 : 0;
    memcpy(hello + off, h->mlkem_pk, YUMI_MLKEM_PUBLIC_KEY_LEN);
    off += YUMI_MLKEM_PUBLIC_KEY_LEN;
    memcpy(hello + off, h->bp512_pk, YUMI_BP512_PUBLIC_KEY_LEN);
    off += YUMI_BP512_PUBLIC_KEY_LEN;
    if (h->first_contact) {
        memcpy(hello + off, h->frodo_pk, YUMI_FRODO_PUBLIC_KEY_LEN);
        off += YUMI_FRODO_PUBLIC_KEY_LEN;
    }

    /* ── Feed HELLO into transcript ────────────────────────────── */

    yumi_skein_update(h->transcript, hello, hello_len);

    /* ── Send HELLO ────────────────────────────────────────────── */

    h->state = HS_HELLO_SENT;
    h->expected_msg = SUDP_MSG_RESPONSE;

    /* Set handshake deadline */
    c->hs_deadline_us = sudp_monotonic_us()
        + (h->first_contact ? YUMI_SUDP_HS_TIMEOUT_FIRST_US
                            : YUMI_SUDP_HS_TIMEOUT_SUBSEQ_US);

    sudp_set_state(c, YUMI_SUDP_HANDSHAKING);
    SUDP_LOG("HELLO sent (version=%u, first_contact=%d, timeout=%s)",
             YUMI_SUDP_PROTOCOL_VERSION, h->first_contact,
             h->first_contact ? "30s" : "10s");

    int r = sudp_frag_send(c, SUDP_MSG_HELLO, hello, hello_len);
    free(hello);
    if (r != 0) goto fail;
    pthread_mutex_unlock(&c->hs_mutex);
    return;

fail:
    pthread_mutex_unlock(&c->hs_mutex);
    sudp_set_state(c, YUMI_SUDP_FAILED);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Public API — connect (initiator)
 *
 *  Validates arguments on the caller's thread, then posts the actual
 *  keygen + HELLO work to the inner client's worker thread so that
 *  all handshake context mutations are single-threaded.
 * ════════════════════════════════════════════════════════════════════════ */

int yumi_sudp_client_connect(yumi_sudp_client_t *c,
                              const uint8_t peer_id[GR_PEER_ID_LEN])
{
    if (!c || !peer_id) return -1;

    int s = atomic_load_explicit(&c->state, memory_order_acquire);
    if (s == (int)YUMI_SUDP_ESTABLISHED) return -1;

    /* Verify peer exists in registrar (read-only, safe from any thread) */
    gr_peer_t peer;
    if (gr_peer_get(c->registrar, peer_id, &peer) != GR_OK) return -1;
    if (peer.status != GR_PEER_ACTIVE) return -1;

    /* Post connect request to the worker thread */
    return yumi_udp_client_post_user(&c->inner, peer_id, GR_PEER_ID_LEN);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Public API — send variants
 * ════════════════════════════════════════════════════════════════════════ */

int yumi_sudp_client_send(yumi_sudp_client_t *c,
                           const void *data, uint32_t len)
{
    return sudp_encrypt_send(c, 0, data, len, false);
}

int yumi_sudp_client_send_reliable(yumi_sudp_client_t *c,
                                    const void *data, uint32_t len)
{
    return sudp_encrypt_send(c, 0, data, len, true);
}

int yumi_sudp_client_send_channel(yumi_sudp_client_t *c, uint8_t channel,
                                   const void *data, uint32_t len)
{
    return sudp_encrypt_send(c, channel, data, len, false);
}

int yumi_sudp_client_send_reliable_channel(yumi_sudp_client_t *c,
                                            uint8_t channel,
                                            const void *data, uint32_t len)
{
    return sudp_encrypt_send(c, channel, data, len, true);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Public API — notify kick
 *
 *  Checks the remote peer's status in the registrar.  If the peer is
 *  KICKED or BANNED, tears down the session.  Safe from any thread
 *  because it only reads the registrar (read-only) and does an atomic
 *  state store.
 * ════════════════════════════════════════════════════════════════════════ */

int yumi_sudp_client_notify_kick(yumi_sudp_client_t *c)
{
    if (!c) return -1;

    int s = atomic_load_explicit(&c->state, memory_order_acquire);
    if (s != (int)YUMI_SUDP_ESTABLISHED &&
        s != (int)YUMI_SUDP_HANDSHAKING)
        return -1; /* already torn down */

    gr_peer_t peer;
    if (gr_peer_get(c->registrar, c->peer_id, &peer) != GR_OK)
        return -1;

    if (peer.status == GR_PEER_KICKED || peer.status == GR_PEER_BANNED) {
        SUDP_LOG("notify_kick: peer is %s — tearing down",
                 peer.status == GR_PEER_KICKED ? "KICKED" : "BANNED");
        atomic_store_explicit(&c->peer_active, false, memory_order_release);
        sudp_set_state(c, YUMI_SUDP_FAILED);
        return 0;
    }

    return -1; /* peer is still active */
}

/* ════════════════════════════════════════════════════════════════════════
 *  Public API — queries
 * ════════════════════════════════════════════════════════════════════════ */

yumi_sudp_state_t yumi_sudp_client_get_state(const yumi_sudp_client_t *c)
{
    return (yumi_sudp_state_t)atomic_load_explicit(
        (atomic_int *)&c->state, memory_order_acquire);
}

uint32_t yumi_sudp_client_get_max_payload(const yumi_sudp_client_t *c)
{
    return c->max_secure_payload;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Public API — ICE passthrough
 * ════════════════════════════════════════════════════════════════════════ */

int yumi_sudp_client_ice_get_local_sdp(yumi_sudp_client_t *c,
                                        char *buf, size_t size)
{
    return yumi_udp_client_ice_get_local_sdp(&c->inner, buf, size);
}

int yumi_sudp_client_ice_set_remote_sdp(yumi_sudp_client_t *c,
                                         const char *sdp)
{
    return yumi_udp_client_ice_set_remote_sdp(&c->inner, sdp);
}
