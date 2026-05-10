/*
    Yumi Secure UDP Client — Encrypted Session Layer
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
 * yumi_sudp_client.h — Yumi Secure UDP Client
 *
 * Encrypted session layer over yumi_udp_client with:
 *   - Triple-hybrid handshake (ML-KEM-1024 + FrodoKEM-1344 + BrainPool-512)
 *     for first contact, dual-hybrid (ML-KEM-1024 + BrainPool-512) for
 *     subsequent sessions.
 *   - Threefish-1024-CTR + Skein-1024-MAC (AEAD) for all wire data.
 *   - Epoch key integration via Group Registrar.
 *   - ML-DSA-87 mutual authentication.
 *   - Per-packet nonce + anti-replay window.
 *   - 1024-bit rekey seed for session rekeying (gen-based + epoch rotation).
 *   - Handshake timeout: 30 s (first contact) / 10 s (subsequent).
 *   - Protocol version byte on all handshake messages.
 *   - Kicked peer detection and session teardown.
 *   - Optional logging via -DYUMI_SUDP_LOG.
 *
 * Wire format (on wire — entire datagram encrypted at UDP level):
 *   [nonce(16)] [AEAD( [flags(1)][ch(1)][seq(4)][payload_len(4)]
 *                       [epoch_id(4)][channel(1)][data...] ) || tag(128)]
 *
 * The pkt_encrypt/pkt_decrypt hooks on the inner UDP client encrypt the
 * entire datagram (transport header + SUDP payload).  Flags, seq, channel,
 * payload_len are never exposed on the wire.
 */

#ifndef YUMI_SUDP_CLIENT_H
#define YUMI_SUDP_CLIENT_H

#include "network/yumi_udp_client.h"
#include "group_registrar.h"
#include "crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constants ─────────────────────────────────────────────────────────── */

/* Protocol version — 8-bit prefix on every handshake message. */
#define YUMI_SUDP_PROTOCOL_VERSION  1

/* Handshake timeouts (microseconds).
 * First contact (triple-hybrid) gets a longer window because FrodoKEM
 * key generation and encapsulation are heavyweight operations. */
#define YUMI_SUDP_HS_TIMEOUT_FIRST_US   (30ULL * 1000000)  /* 30 s */
#define YUMI_SUDP_HS_TIMEOUT_SUBSEQ_US  (10ULL * 1000000)  /* 10 s */

/* Rekey seed length — 1024 bits, derived alongside the transport key
 * during the handshake.  Used for epoch-rotation rekeying and periodic
 * nonce-counter-based key rotation. */
#define YUMI_SUDP_REKEY_SEED_LEN   YUMI_AEAD_KEY_LEN  /* 128 bytes */

/* Overhead added by the SUDP envelope inside the inner payload.
 * Datagram-level crypto (nonce + tag = 144) is applied at the UDP layer
 * via pkt_crypto_overhead and does not appear here. */
#define YUMI_SUDP_ENVELOPE_OVERHEAD \
    (4 /* epoch_id */ + 1 /* channel */)   /* = 5 bytes */

/* Crypto overhead added at the datagram level by the pkt_encrypt hook */
#define YUMI_SUDP_PKT_CRYPTO_OVERHEAD \
    (YUMI_AEAD_NONCE_LEN + YUMI_AEAD_TAG_LEN)  /* 16 + 128 = 144 bytes */

/* ── Session state ─────────────────────────────────────────────────────── */

typedef enum {
    YUMI_SUDP_DISCONNECTED = 0,
    YUMI_SUDP_HANDSHAKING,
    YUMI_SUDP_ESTABLISHED,
    YUMI_SUDP_FAILED,
} yumi_sudp_state_t;

/* ── Callbacks ─────────────────────────────────────────────────────────── */

typedef void (*yumi_sudp_recv_cb_t)(void *user, uint8_t channel,
                                     const void *data, uint32_t len,
                                     bool reliable);

typedef void (*yumi_sudp_state_cb_t)(void *user, yumi_sudp_state_t state);

/* ── Configuration ─────────────────────────────────────────────────────── */

typedef struct {
    /* Inner transport config.
     * The recv_cb and recv_user fields are ignored — the SUDP layer
     * intercepts all incoming data from the inner client. */
    yumi_udp_client_config_t transport;

    /* Group context (borrowed — must outlive the SUDP client) */
    gr_registrar_t      *registrar;
    const gr_identity_t *identity;

    /* true = triple-hybrid (ML-KEM + FrodoKEM + BrainPool-512),
     * false = dual-hybrid (ML-KEM + BrainPool-512). */
    bool                 first_contact;

    /* SUDP callbacks */
    yumi_sudp_recv_cb_t  recv_cb;
    yumi_sudp_state_cb_t state_cb;
    void                *user;
} yumi_sudp_config_t;

/* ── Opaque client handle ──────────────────────────────────────────────── */

typedef struct yumi_sudp_client yumi_sudp_client_t;

/* ── API ───────────────────────────────────────────────────────────────── */

/**
 * Create a secure UDP client.  Heap-allocates internal state including
 * the inner transport.  Starts the worker thread immediately.
 */
int  yumi_sudp_client_create(yumi_sudp_client_t **out,
                              const yumi_sudp_config_t *cfg);

/**
 * Destroy the client, join threads, wipe all key material.
 */
void yumi_sudp_client_destroy(yumi_sudp_client_t *c);

/**
 * Initiate the handshake with a remote peer.
 * peer_id must be present in the registrar as an active peer.
 * Fires state_cb(YUMI_SUDP_HANDSHAKING) immediately, then
 * state_cb(YUMI_SUDP_ESTABLISHED) or state_cb(YUMI_SUDP_FAILED)
 * asynchronously.
 */
int  yumi_sudp_client_connect(yumi_sudp_client_t *c,
                               const uint8_t peer_id[GR_PEER_ID_LEN]);

/* Data send — blocks until the envelope is enqueued (not until ACK). */
int  yumi_sudp_client_send(yumi_sudp_client_t *c,
                            const void *data, uint32_t len);
int  yumi_sudp_client_send_reliable(yumi_sudp_client_t *c,
                                     const void *data, uint32_t len);
int  yumi_sudp_client_send_channel(yumi_sudp_client_t *c, uint8_t channel,
                                    const void *data, uint32_t len);
int  yumi_sudp_client_send_reliable_channel(yumi_sudp_client_t *c,
                                             uint8_t channel,
                                             const void *data, uint32_t len);

/**
 * Notify the SUDP client that its remote peer has been kicked.
 * Verifies the peer's status in the registrar; if the peer is
 * KICKED or BANNED, tears down the session (FAILED state).
 * Safe to call from any thread.
 *
 * @return 0 if the session was torn down, -1 if the peer is still active.
 */
int  yumi_sudp_client_notify_kick(yumi_sudp_client_t *c);

/* Queries */
yumi_sudp_state_t yumi_sudp_client_get_state(const yumi_sudp_client_t *c);
uint32_t          yumi_sudp_client_get_max_payload(const yumi_sudp_client_t *c);

/* ICE passthrough (delegates to inner client) */
int  yumi_sudp_client_ice_get_local_sdp(yumi_sudp_client_t *c,
                                         char *buf, size_t size);
int  yumi_sudp_client_ice_set_remote_sdp(yumi_sudp_client_t *c,
                                          const char *sdp);

#ifdef __cplusplus
}
#endif

#endif /* YUMI_SUDP_CLIENT_H */
