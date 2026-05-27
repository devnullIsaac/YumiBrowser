# Networking API Reference

The networking stack (`include/network/`, `include/yumi_client.h`, `src/network/`, `src/yumi_client.c`) provides encrypted peer-to-peer transport built in three layers, plus a congestion control module.

## Layer Stack

```
┌──────────────────────────────────────────────────────────┐
│  yumi_client_t   (High-Level Client)                     │
│  Auto-sync, attestation, bootstrap, messaging, meshnet   │
├──────────────────────────────────────────────────────────┤
│  yumi_sudp_client_t   (Secure UDP)                       │
│  Post-quantum handshake, AEAD encryption, epoch keys     │
├──────────────────────────────────────────────────────────┤
│  yumi_udp_client_t   (Raw UDP Transport)                 │
│  Channels, reliability, congestion control, ICE/STUN     │
├──────────────────────────────────────────────────────────┤
│  UDP socket + libjuice (ICE)                             │
└──────────────────────────────────────────────────────────┘
```

---

## Layer 1: Wire Protocol — `include/network/net.h`

### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `YUMI_UDP_HDR_SIZE` | 10 | Wire header size (1+1+4+4 bytes) |
| `YUMI_UDP_MAX_PAYLOAD` | 1442 | Max payload: 1500 − 40 (IPv6) − 8 (UDP) − 10 (hdr) |
| `YUMI_UDP_MAX_DGRAM` | 1452 | Header + max payload |
| `YUMI_UDP_MAX_CHANNELS` | 256 | Channel IDs 0–255 |

### Flag Bits

| Flag | Value | Description |
|------|-------|-------------|
| `YUMI_UDP_FLAG_RELIABLE` | `0x01` | Reliable delivery requested |
| `YUMI_UDP_FLAG_ACK` | `0x02` | This is an acknowledgement |
| `YUMI_UDP_FLAG_PROBE` | `0x04` | Internal CC probe packet |

### Wire Header — `yumi_udp_hdr_t`

Packed, exactly 10 bytes:

```c
typedef struct __attribute__((packed)) {
    uint8_t  flags;         /* reliability flags (RELIABLE, ACK, PROBE) */
    uint8_t  channel;       /* channel ID (0-255) */
    uint32_t seq;           /* monotonic sequence number (wraps) */
    uint32_t payload_len;   /* payload byte count */
} yumi_udp_hdr_t;
```

### Work Item Types — `yumi_work_type_t`

```c
typedef enum {
    YUMI_WORK_SEND_UNRELIABLE = 0,
    YUMI_WORK_SEND_RELIABLE   = 1,
    YUMI_WORK_SHUTDOWN        = 2,
    YUMI_WORK_USER_CB         = 3,
    YUMI_WORK_SEND_RAW        = 4,  /* pre-built header: encrypt+sendto only */
} yumi_work_type_t;
```

### Work Item — `yumi_work_item_t`

```c
typedef struct {
    yumi_work_type_t    type;
    uint32_t            len;
    uint8_t             channel;
    uint8_t             raw_flags;   /* SEND_RAW: wire flags */
    uint32_t            raw_seq;     /* SEND_RAW: sequence number */
    bool                has_dest;
    struct sockaddr_in6 dest;
    uint8_t             data[YUMI_UDP_MAX_PAYLOAD];
} yumi_work_item_t;
```

### MPSC Ring Buffer — `yumi_ring_t`

Lock-free ring buffer with backpressure for send worker dispatch.

#### Ring Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `YUMI_RING_CAPACITY` | 4096 | Slot count (power of 2) |
| `YUMI_RING_MASK` | 4095 | `CAPACITY - 1` for masking |
| `YUMI_RING_SPIN_MAX` | 256 | CAS spins before sleeping |
| `YUMI_RING_SLEEP_US` | 1000 | Sleep interval (1 ms) |
| `YUMI_RING_SLEEP_MAX` | 1000 | Max sleeps (~1 s total) |

#### Ring Structure

```c
typedef struct {
    _Alignas(64) atomic_uint_fast64_t head;   /* producers CAS here */
    _Alignas(64) atomic_uint_fast64_t tail;   /* single consumer owns this */
    yumi_work_item_t                 *slots;
    atomic_int                       *ready;
} yumi_ring_t;
```

`_Alignas(64)` prevents false sharing between head and tail.

#### Ring API

##### `yumi_ring_create`

Heap-allocate and initialize a ring buffer.

```c
yumi_ring_t *yumi_ring_create(void);
```

##### `yumi_ring_destroy`

Free all ring resources.

```c
void yumi_ring_destroy(yumi_ring_t *r);
```

##### `yumi_ring_init`

Initialize an already-allocated ring (reset head/tail).

```c
void yumi_ring_init(yumi_ring_t *r);
```

##### `yumi_ring_push` / `yumi_ring_pop`

Standard copy-based enqueue/dequeue.

```c
bool yumi_ring_push(yumi_ring_t *r, const yumi_work_item_t *item);
bool yumi_ring_pop(yumi_ring_t *r, yumi_work_item_t *out);
```

Returns `false` on full (push) or empty (pop).

##### `yumi_ring_peek` / `yumi_ring_advance`

Zero-copy consumer: peek at the next committed slot without copying. Call `yumi_ring_advance()` after processing to release.

```c
const yumi_work_item_t *yumi_ring_peek(yumi_ring_t *r);
void                     yumi_ring_advance(yumi_ring_t *r);
```

Returns `NULL` if empty or slot not committed yet.

##### `yumi_ring_reserve` / `yumi_ring_try_reserve` / `yumi_ring_commit`

Zero-copy enqueue: reserve a slot (CAS on head), fill it directly, then commit. Eliminates per-enqueue copy.

```c
yumi_work_item_t *yumi_ring_reserve(yumi_ring_t *r);
yumi_work_item_t *yumi_ring_try_reserve(yumi_ring_t *r);
void              yumi_ring_commit(yumi_ring_t *r, yumi_work_item_t *slot);
```

`yumi_ring_reserve()` applies spin→sleep backpressure when full. `yumi_ring_try_reserve()` returns `NULL` immediately if full.

---

## Layer 1: Raw UDP Transport — `include/network/yumi_udp_client.h`

### Reliability Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `YUMI_REORDER_WINDOW` | 32 | Per-channel reorder buffer window |
| `YUMI_REORDER_MASK` | 31 | `WINDOW - 1` |
| `YUMI_RELIABLE_INIT_SLOTS` | 4096 | Initial reliable retransmit table capacity |

### Data Structures

#### `yumi_reorder_slot_t`

Per-channel reliable reorder buffer slot:

```c
typedef struct {
    bool     occupied;
    uint32_t seq;
    uint32_t len;
    uint8_t  data[YUMI_UDP_MAX_PAYLOAD];
} yumi_reorder_slot_t;
```

#### `yumi_retx_entry_t`

Retransmit min-heap entry (lazy deletion, growable):

```c
typedef struct {
    uint64_t expiry;
    uint32_t table_idx;
    uint32_t seq;
    uint8_t  flags;
    uint8_t  channel;
} yumi_retx_entry_t;
```

#### `yumi_retx_heap_t`

```c
typedef struct {
    yumi_retx_entry_t *entries;
    int count;
    int capacity;
} yumi_retx_heap_t;
```

#### `yumi_reliable_entry_t`

Per-packet reliable retransmit state:

```c
typedef struct {
    uint32_t seq;
    uint64_t send_time_us;
    uint64_t timeout_us;
    uint32_t retransmit_count;
    uint32_t max_retransmits;
    uint32_t payload_len;
    uint8_t  flags;
    uint8_t  channel;
    bool     active;
    yumi_packet_t cc_pkt;                    /* BBR delivery state snapshot */
    struct sockaddr_in6 dest;                /* target address for retransmit */
    uint8_t  payload[YUMI_UDP_MAX_PAYLOAD];
} yumi_reliable_entry_t;
```

### Enumerations

#### `yumi_ice_state_t`

ICE transport state (via libjuice):

```c
typedef enum {
    YUMI_ICE_DISCONNECTED = 0,
    YUMI_ICE_GATHERING,
    YUMI_ICE_CONNECTING,
    YUMI_ICE_CONNECTED,
    YUMI_ICE_COMPLETED,
    YUMI_ICE_FAILED,
} yumi_ice_state_t;
```

### Callback Typedefs

#### `yumi_recv_cb_t`

Channel receive callback:

```c
typedef void (*yumi_recv_cb_t)(void *user, const void *data,
                                uint32_t len, bool reliable, uint32_t seq);
```

#### ICE Callbacks

```c
typedef void (*yumi_ice_state_cb_t)(void *user, yumi_ice_state_t state);
typedef void (*yumi_ice_candidate_cb_t)(void *user, const char *sdp);
typedef void (*yumi_ice_gathering_done_cb_t)(void *user);
```

### `yumi_turn_server_t`

TURN server entry:

```c
typedef struct {
    const char *host;
    const char *username;
    const char *password;
    uint16_t    port;
} yumi_turn_server_t;
```

### Configuration — `yumi_udp_client_config_t`

```c
typedef struct {
    /* Raw-mode peer address (ignored when ICE is enabled) */
    struct sockaddr_in6 peer_addr;
    uint16_t            local_port;          /* 0 = ephemeral */
    uint64_t            reliable_timeout_us; /* 0 = default (200 ms) */
    uint32_t            max_retransmits;     /* 0 = default (5) */
    yumi_recv_cb_t      recv_cb;
    void               *recv_user;

    /* ICE / STUN / TURN (NULL stun_server = raw mode) */
    const char                  *stun_server;
    uint16_t                     stun_port;          /* 0 = default (3478) */
    yumi_turn_server_t          *turn_servers;
    int                          turn_servers_count;
    yumi_ice_state_cb_t          ice_state_cb;
    yumi_ice_candidate_cb_t      ice_candidate_cb;
    yumi_ice_gathering_done_cb_t ice_gathering_done_cb;
    void                        *ice_user;

    /* Datagram-level encryption (optional) */
    int (*pkt_encrypt)(void *ctx, const uint8_t *pt, uint32_t pt_len,
                       uint8_t *ct, uint32_t *ct_len);
    int (*pkt_decrypt)(void *ctx, const uint8_t *ct, uint32_t ct_len,
                       uint8_t *pt, uint32_t *pt_len);
    void    *pkt_crypto_ctx;
    uint32_t pkt_crypto_overhead;

    /* Worker-thread user callback */
    void    (*user_work_cb)(void *ctx, const void *data, uint32_t len);
    void    *user_work_ctx;

    /* Worker thread pool (defaults: 1 send, 1 recv) */
    uint32_t num_send_workers;     /* 0 = 1 */
    uint32_t num_recv_workers;     /* 0 = 1 */
} yumi_udp_client_config_t;
```

When `pkt_encrypt`/`pkt_decrypt` are set, ALL datagrams (transport header + payload) are encrypted before `sendto()` and decrypted before header parsing. Flags, seq, channel, payload_len are never exposed on the wire. `pkt_crypto_overhead` is deducted from `max_payload` so encrypted datagrams fit in MTU.

### Client Structure — `yumi_udp_client_t`

Heap-allocated via `calloc`. Key fields:

| Field Group | Description |
|-------------|-------------|
| `fd`, `epoll_fd`, `timer_fd`, `recv_event_fd` | File descriptors |
| `peer_addr` | Remote peer address |
| `send_wk` / `recv_wk` | Worker thread pool arrays |
| `state_lock` (spinlock) | Protects `reliable_table`, `retx_heap`, `cc` |
| `channel_locks[256]` (spinlocks) | Per-channel recv ordering |
| `cc_mode_cache`, `cc_rate_cache` | Lockless CC reading for send workers |
| `reliable_table`, `retx_heap` | Reliable retransmit state |
| `link_mtu`, `max_payload` | MTU discovered from NIC at create time |
| `rel_seq[256]`, `unrel_seq[256]` | Per-channel, per-mode atomic seq counters |
| `reorder_buf` | `[MAX_CHANNELS × REORDER_WINDOW]` reorder slots |
| `cc` (`yumi_cc_t`) | BBR → TDS congestion control |
| `stat_tx_*`, `stat_rx_*`, `stat_retransmits` | Lockless atomic statistics |
| `ice_agent`, `ice_*` | ICE transport state (NULL in raw mode) |

### Transport API

#### `yumi_udp_client_create`

Create and start the transport. Binds socket, discovers MTU, spawns worker threads.

```c
int yumi_udp_client_create(yumi_udp_client_t *client,
                            const yumi_udp_client_config_t *cfg);
```

**Returns**: 0 on success, -1 on error.

#### `yumi_udp_client_destroy`

Shut down workers, close sockets, free resources.

```c
void yumi_udp_client_destroy(yumi_udp_client_t *client);
```

#### Send — Default Channel (0)

```c
int yumi_udp_client_send(yumi_udp_client_t *client,
                          const void *data, uint32_t len);
int yumi_udp_client_send_reliable(yumi_udp_client_t *client,
                                   const void *data, uint32_t len);
```

#### Send — Specific Channel (0–255)

```c
int yumi_udp_client_send_channel(yumi_udp_client_t *client,
                                  uint8_t channel,
                                  const void *data, uint32_t len);
int yumi_udp_client_send_reliable_channel(yumi_udp_client_t *client,
                                           uint8_t channel,
                                           const void *data, uint32_t len);
```

#### Send — Specific Destination (multi-peer)

```c
int yumi_udp_client_send_to(yumi_udp_client_t *client,
                             const struct sockaddr_in6 *dest,
                             const void *data, uint32_t len);
int yumi_udp_client_send_reliable_to(yumi_udp_client_t *client,
                                      const struct sockaddr_in6 *dest,
                                      const void *data, uint32_t len);
int yumi_udp_client_send_channel_to(yumi_udp_client_t *client,
                                     uint8_t channel,
                                     const struct sockaddr_in6 *dest,
                                     const void *data, uint32_t len);
int yumi_udp_client_send_reliable_channel_to(yumi_udp_client_t *client,
                                              uint8_t channel,
                                              const struct sockaddr_in6 *dest,
                                              const void *data, uint32_t len);
```

#### MTU Queries

```c
uint32_t yumi_udp_client_get_mtu(const yumi_udp_client_t *client);
uint32_t yumi_udp_client_get_max_payload(const yumi_udp_client_t *client);
```

#### User Callback Dispatch

Post a work item to the send worker ring. The `user_work_cb` from config is invoked on the worker thread.

```c
int yumi_udp_client_post_user(yumi_udp_client_t *client,
                               const void *data, uint32_t len);
```

#### Statistics (lockless atomic reads)

```c
uint64_t yumi_udp_client_stat_tx_packets(const yumi_udp_client_t *client);
uint64_t yumi_udp_client_stat_tx_bytes(const yumi_udp_client_t *client);
uint64_t yumi_udp_client_stat_rx_packets(const yumi_udp_client_t *client);
uint64_t yumi_udp_client_stat_rx_bytes(const yumi_udp_client_t *client);
```

### ICE API

Only valid when `stun_server` was set in config.

#### `yumi_udp_client_ice_get_local_sdp`

Get the local SDP description (call after create, before setting remote).

```c
int yumi_udp_client_ice_get_local_sdp(yumi_udp_client_t *client,
                                       char *buf, size_t size);
```

#### `yumi_udp_client_ice_set_remote_sdp`

Set the remote peer's SDP description (triggers connectivity checks).

```c
int yumi_udp_client_ice_set_remote_sdp(yumi_udp_client_t *client,
                                        const char *sdp);
```

#### `yumi_udp_client_ice_add_remote_candidate`

Add a remote ICE candidate (trickle ICE).

```c
int yumi_udp_client_ice_add_remote_candidate(yumi_udp_client_t *client,
                                              const char *sdp);
```

#### `yumi_udp_client_ice_set_remote_gathering_done`

Signal that remote gathering is complete.

```c
int yumi_udp_client_ice_set_remote_gathering_done(yumi_udp_client_t *client);
```

#### `yumi_udp_client_ice_get_state`

```c
yumi_ice_state_t yumi_udp_client_ice_get_state(const yumi_udp_client_t *client);
```

#### `yumi_udp_client_ice_enabled`

```c
bool yumi_udp_client_ice_enabled(const yumi_udp_client_t *client);
```

---

## Layer 2: Secure UDP — `include/network/yumi_sudp_client.h`

Encrypted session layer over `yumi_udp_client_t` with post-quantum handshake, AEAD encryption, epoch key integration, and mutual authentication.

### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `YUMI_SUDP_PROTOCOL_VERSION` | 1 | 8-bit prefix on every handshake message |
| `YUMI_SUDP_HS_TIMEOUT_FIRST_US` | 30,000,000 | 30 s — first contact (triple-hybrid) |
| `YUMI_SUDP_HS_TIMEOUT_SUBSEQ_US` | 10,000,000 | 10 s — subsequent (dual-hybrid) |
| `YUMI_SUDP_REKEY_SEED_LEN` | 128 | 1024-bit rekey seed (`YUMI_AEAD_KEY_LEN`) |
| `YUMI_SUDP_ENVELOPE_OVERHEAD` | 5 | `epoch_id(4) + channel(1)` |
| `YUMI_SUDP_PKT_CRYPTO_OVERHEAD` | 144 | `nonce(16) + tag(128)` |

### Handshake Modes

| Mode | Algorithms | Timeout |
|------|-----------|---------|
| **First contact** (`first_contact = true`) | Triple-hybrid: ML-KEM-1024 + FrodoKEM-1344 + BrainPool-512 | 30 s |
| **Subsequent** (`first_contact = false`) | Dual-hybrid: ML-KEM-1024 + BrainPool-512 | 10 s |

First contact uses FrodoKEM (plain LWE, conservative post-quantum choice) alongside the lattice-based ML-KEM for defense in depth. Subsequent connections skip FrodoKEM for performance.

### Wire Format

All data on wire is encrypted at the UDP datagram level:

```
[nonce(16)] [AEAD( [flags(1)][ch(1)][seq(4)][payload_len(4)]
                    [epoch_id(4)][channel(1)][data...] ) || tag(128)]
```

### Session State — `yumi_sudp_state_t`

```c
typedef enum {
    YUMI_SUDP_DISCONNECTED = 0,
    YUMI_SUDP_HANDSHAKING,        /* key exchange in progress */
    YUMI_SUDP_ESTABLISHED,        /* encrypted session active */
    YUMI_SUDP_FAILED,             /* handshake failed or peer kicked */
} yumi_sudp_state_t;
```

### Callback Typedefs

#### `yumi_sudp_recv_cb_t`

```c
typedef void (*yumi_sudp_recv_cb_t)(void *user, uint8_t channel,
                                     const void *data, uint32_t len,
                                     bool reliable);
```

#### `yumi_sudp_state_cb_t`

```c
typedef void (*yumi_sudp_state_cb_t)(void *user, yumi_sudp_state_t state);
```

### Configuration — `yumi_sudp_config_t`

```c
typedef struct {
    /* Inner transport config (recv_cb/recv_user ignored — SUDP intercepts) */
    yumi_udp_client_config_t transport;

    /* Group context (borrowed — must outlive the SUDP client) */
    gr_registrar_t      *registrar;
    const gr_identity_t *identity;

    /* true = triple-hybrid, false = dual-hybrid */
    bool                 first_contact;

    /* SUDP callbacks */
    yumi_sudp_recv_cb_t  recv_cb;
    yumi_sudp_state_cb_t state_cb;
    void                *user;
} yumi_sudp_config_t;
```

### Opaque Handle

```c
typedef struct yumi_sudp_client yumi_sudp_client_t;
```

### SUDP API

#### `yumi_sudp_client_create`

Create a secure UDP client. Heap-allocates internal state including the inner transport. Starts the worker thread immediately.

```c
int yumi_sudp_client_create(yumi_sudp_client_t **out,
                             const yumi_sudp_config_t *cfg);
```

**Returns**: 0 on success, -1 on error.

#### `yumi_sudp_client_destroy`

Destroy the client, join threads, wipe all key material.

```c
void yumi_sudp_client_destroy(yumi_sudp_client_t *c);
```

#### `yumi_sudp_client_connect`

Initiate the handshake with a remote peer. `peer_id` must be present in the registrar as an active peer. Fires `state_cb(YUMI_SUDP_HANDSHAKING)` immediately, then `state_cb(YUMI_SUDP_ESTABLISHED)` or `state_cb(YUMI_SUDP_FAILED)` asynchronously.

```c
int yumi_sudp_client_connect(yumi_sudp_client_t *c,
                              const uint8_t peer_id[GR_PEER_ID_LEN]);
```

#### Send Functions

All block until the envelope is enqueued (not until ACK).

```c
int yumi_sudp_client_send(yumi_sudp_client_t *c,
                           const void *data, uint32_t len);
int yumi_sudp_client_send_reliable(yumi_sudp_client_t *c,
                                    const void *data, uint32_t len);
int yumi_sudp_client_send_channel(yumi_sudp_client_t *c, uint8_t channel,
                                   const void *data, uint32_t len);
int yumi_sudp_client_send_reliable_channel(yumi_sudp_client_t *c,
                                            uint8_t channel,
                                            const void *data, uint32_t len);
```

#### `yumi_sudp_client_notify_kick`

Notify the SUDP client that its remote peer has been kicked. Verifies the peer's status in the registrar; if KICKED or BANNED, tears down the session (FAILED state). Safe to call from any thread.

```c
int yumi_sudp_client_notify_kick(yumi_sudp_client_t *c);
```

**Returns**: 0 if the session was torn down, -1 if the peer is still active.

#### Queries

```c
yumi_sudp_state_t yumi_sudp_client_get_state(const yumi_sudp_client_t *c);
uint32_t          yumi_sudp_client_get_max_payload(const yumi_sudp_client_t *c);
```

#### ICE Passthrough

Delegates to the inner `yumi_udp_client_t`:

```c
int yumi_sudp_client_ice_get_local_sdp(yumi_sudp_client_t *c,
                                        char *buf, size_t size);
int yumi_sudp_client_ice_set_remote_sdp(yumi_sudp_client_t *c,
                                         const char *sdp);
```

---

## Layer 3: Yumi Client — `include/yumi_client.h`

High-level API wrapping Group Registrar + Secure UDP into a single interface.

### Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `YUMI_CLIENT_MAX_PEERS` | 64 | Max simultaneous peer connections |

#### Channel Allocation

| Constant | Value | Purpose |
|----------|-------|---------|
| `YUMI_CLIENT_SYNC_CH` | 0 | Registrar delta sync protocol |
| `YUMI_CLIENT_MESH_CH` | 1 | Meshnet relay |
| `YUMI_CLIENT_USER_CH` | 2 | Default user/webapp data |
| `YUMI_CLIENT_ATTEST_CH` | 254 | Join attestation protocol |
| `YUMI_CLIENT_RESERVED_CH` | 255 | Reserved |

Channels 2–253 are available for user data.

#### Meshnet Relay

| Constant | Value | Description |
|----------|-------|-------------|
| `YUMI_MESH_MIN_TTL` | 2 | Minimum hops |
| `YUMI_MESH_RATE_BYTES_SEC` | 10240 | 10 KB/s default throttle per forwarding peer |
| `YUMI_MESH_RATE_WINDOW_MS` | 1000 | Token bucket refill window |

TTL = `max(2, ceil(log2(active_peers)))` — scales logarithmically with group size.

#### Bootstrap Wire Protocol

| Constant | Value | Description |
|----------|-------|-------------|
| `YUMI_BOOT_CHUNK_SIZE` | 1000 | Bootstrap chunk size (bytes) |
| `YUMI_BOOT_TIMEOUT_MS` | 30000 | Bootstrap timeout (30 s) |
| `YUMI_BOOT_RETRY_MS` | 2000 | Bootstrap retry interval (2 s) |

### Client State — `yumi_client_state_t`

```c
typedef enum {
    YUMI_CLIENT_INITIALIZING = 0,
    YUMI_CLIENT_CONNECTING,       /* connecting to peers */
    YUMI_CLIENT_SYNCING,          /* downloading registrar (join) */
    YUMI_CLIENT_RUNNING,          /* fully operational */
    YUMI_CLIENT_CLOSED,
} yumi_client_state_t;
```

### Events — `yumi_client_event_t`

```c
typedef enum {
    YUMI_EVENT_PEER_CONNECTED      = 1,
    YUMI_EVENT_PEER_DISCONNECTED   = 2,
    YUMI_EVENT_SYNC_COMPLETE       = 3,   /* registrar up-to-date */
    YUMI_EVENT_VERIFIED            = 4,   /* join attestation passed */
    YUMI_EVENT_VERIFICATION_FAILED = 5,
    YUMI_EVENT_PEER_KICKED         = 6,
    YUMI_EVENT_EPOCH_ROTATED       = 7,
    YUMI_EVENT_DUPLICATE_PEER      = 8,   /* duplicate connection blocked */
    YUMI_EVENT_MESH_ABUSE          = 9,   /* meshnet relay abuse detected */
    YUMI_EVENT_BOOT_BLOCKED        = 10,  /* IP blocked after failed attempts */
} yumi_client_event_t;
```

### Callback Typedefs

#### `yumi_client_message_fn`

Fired for every user-data message received on channels 2–253. Channels 0–1 and 254–255 are reserved for internal protocols.

```c
typedef void (*yumi_client_message_fn)(
    void        *user,
    const uint8_t peer_id[GR_PEER_ID_LEN],
    uint8_t      channel,
    const void  *data,
    uint32_t     len,
    bool         reliable);
```

#### `yumi_client_event_fn`

Fired for lifecycle events. `peer_id` may be NULL for non-peer-specific events.

```c
typedef void (*yumi_client_event_fn)(
    void                   *user,
    yumi_client_event_t     event,
    const uint8_t          *peer_id);
```

### Configuration — `yumi_client_config_t`

```c
typedef struct {
    /* Storage */
    const char          *db_path;       /* DuckDB path (":memory:" ok) */
    const char          *group_name;    /* creation only */
    gr_group_type_t      group_type;    /* GR_GROUP_PRIVATE / PUBLIC */

    /* Callbacks */
    yumi_client_message_fn on_message;
    yumi_client_event_fn   on_event;
    void                  *user;

    /* Network (optional) */
    const char *stun_server;            /* NULL = direct mode */
    uint16_t    stun_port;              /* 0 = default 3478 */
    uint16_t    local_port;             /* 0 = ephemeral */

    /* STUN fallback list (overrides stun_server when set) */
    const char        **stun_servers;
    uint32_t            stun_server_count;
} yumi_client_config_t;
```

### Opaque Handle

```c
typedef struct yumi_client yumi_client_t;
```

### Lifecycle API

#### `yumi_client_create_group`

Create a new group. Caller becomes owner. Starts the bootstrap listener and sync worker automatically.

```c
int yumi_client_create_group(yumi_client_t **out,
                              const yumi_client_config_t *cfg);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `out` | out | Receives the allocated client |
| `cfg` | in | Configuration (`db_path`, `group_name` required) |

**Returns**: 0 on success, -1 on error.

#### `yumi_client_open`

Open an existing group from a local registrar DB. Auto-connects to all active peers found in the registrar.

```c
int yumi_client_open(yumi_client_t **out,
                     const yumi_client_config_t *cfg,
                     const uint8_t group_id[GR_HASH_LEN],
                     const gr_identity_t *identity);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `out` | out | Receives the allocated client |
| `cfg` | in | Configuration (`db_path` required) |
| `group_id` | in | Expected group ID (`GR_HASH_LEN` bytes) |
| `identity` | in | Your identity (must already be a member) |

#### `yumi_client_join`

Join a group via an invite blob. Automatically: parses invite → connects to bootstrap peer → downloads registrar → begins join verification → starts sync. Blocks until the registrar is fetched (typically 1–5 s). Attestation continues asynchronously; `YUMI_EVENT_VERIFIED` fires when complete.

```c
int yumi_client_join(yumi_client_t **out,
                     const yumi_client_config_t *cfg,
                     const uint8_t *invite_blob, size_t invite_len);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `out` | out | Receives the allocated client |
| `cfg` | in | Configuration (`db_path` required) |
| `invite_blob` | in | Raw invite blob from `yumi_client_invite()` |
| `invite_len` | in | Length of the invite blob |

#### `yumi_client_destroy`

Shut down all connections and free resources. Blocks until background threads finish.

```c
void yumi_client_destroy(yumi_client_t *c);
```

### Messaging API

#### `yumi_client_broadcast`

Unreliable broadcast to all connected peers (channel 2).

```c
int yumi_client_broadcast(yumi_client_t *c,
                           const void *data, uint32_t len);
```

#### `yumi_client_broadcast_reliable`

Reliable broadcast to all connected peers (channel 2).

```c
int yumi_client_broadcast_reliable(yumi_client_t *c,
                                    const void *data, uint32_t len);
```

#### `yumi_client_send`

Unreliable send to one peer (channel 2).

```c
int yumi_client_send(yumi_client_t *c,
                     const uint8_t peer_id[GR_PEER_ID_LEN],
                     const void *data, uint32_t len);
```

#### `yumi_client_send_reliable`

Reliable send to one peer (channel 2).

```c
int yumi_client_send_reliable(yumi_client_t *c,
                               const uint8_t peer_id[GR_PEER_ID_LEN],
                               const void *data, uint32_t len);
```

#### `yumi_client_send_channel`

Send on a specific channel (2–253) to one peer. Channels 0–1 and 254–255 are reserved.

```c
int yumi_client_send_channel(yumi_client_t *c,
                              const uint8_t peer_id[GR_PEER_ID_LEN],
                              uint8_t channel,
                              const void *data, uint32_t len,
                              bool reliable);
```

### Invitation API

#### `yumi_client_invite`

Create an invite blob for a new peer. Caller must free `*out_blob` with `free()`.

```c
int yumi_client_invite(yumi_client_t *c, int64_t expiry_ms,
                        uint8_t **out_blob, size_t *out_len);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `c` | in | Client (must be owner or have `GR_PERM_INVITE_MEMBER`) |
| `expiry_ms` | in | Expiry timestamp (0 = no expiry) |
| `out_blob` | out | Receives heap-allocated invite blob |
| `out_len` | out | Receives blob length |

### Query API

#### `yumi_client_get_state`

```c
yumi_client_state_t yumi_client_get_state(const yumi_client_t *c);
```

#### `yumi_client_get_registrar`

Get the underlying registrar handle (for direct group operations).

```c
gr_registrar_t *yumi_client_get_registrar(yumi_client_t *c);
```

#### `yumi_client_get_identity`

```c
const gr_identity_t *yumi_client_get_identity(const yumi_client_t *c);
```

#### `yumi_client_connected_peers`

```c
uint32_t yumi_client_connected_peers(const yumi_client_t *c);
```

#### `yumi_client_is_verified`

```c
bool yumi_client_is_verified(const yumi_client_t *c);
```

#### `yumi_client_get_port`

```c
uint16_t yumi_client_get_port(const yumi_client_t *c);
```

### Threading Model

- One SUDP worker thread per peer connection
- One sync worker thread (polls registrar version, auto-connects)
- One bootstrap listener thread (raw UDP, serves registrar to joiners)
- All registrar access serialized by the registrar's `db_lock`

### Duplicate Connection Detection

If a second instance connects using the same identity, the duplicate handshake is rejected and `YUMI_EVENT_DUPLICATE_PEER` fires.

---

## Congestion Control — `include/network/bbr.h`

Custom **Probe-then-Cruise** congestion control model. All time values are in microseconds (`uint64_t`). All bandwidth values are in bytes/second (`uint64_t`).

### Top-level Mode — `yumi_mode_t`

```c
typedef enum {
    YUMI_MODE_PROBE,       /* BBR-like discovery in progress */
    YUMI_MODE_TDS,         /* Time Division Sending (high throughput) */
} yumi_mode_t;
```

### BBR Probe Sub-states — `bbr_state_t`

```c
typedef enum {
    BBR_STARTUP,
    BBR_DRAIN,
    BBR_PROBE_BW,
    BBR_PROBE_RTT,
} bbr_state_t;
```

Standard BBR flow: `STARTUP` → `DRAIN` → `PROBE_BW` ↔ `PROBE_RTT`.

### Per-packet Metadata — `yumi_packet_t`

Caller stores this, feeds it back on ACK:

```c
typedef struct {
    uint32_t seq;
    uint64_t send_time_us;
    uint64_t size;
    /* Only used during PROBE mode */
    uint64_t delivered;
    uint64_t delivered_time;
    uint64_t first_sent_time;    /* send_time of pkt that set delivered */
    bool     is_app_limited;
    bool     is_probe;           /* true = sent during PROBE phase */
} yumi_packet_t;
```

### Windowed-max Filter — `bbr_max_filter_t`

For bottleneck bandwidth estimation:

```c
#define BBR_FILTER_LEN 10

typedef struct {
    bbr_filter_sample_t samples[BBR_FILTER_LEN];
    int                 count;
} bbr_max_filter_t;
```

Where each sample is:

```c
typedef struct {
    uint64_t val;
    uint64_t stamp;
} bbr_filter_sample_t;
```

### BBR Probe State — `bbr_probe_t`

Internal state for the PROBE phase:

```c
typedef struct {
    bbr_state_t      state;
    bbr_max_filter_t btl_bw_filter;    /* windowed-max for BtlBw */
    uint64_t         btl_bw;           /* current bottleneck bandwidth */
    uint64_t         rtprop_us;        /* propagation delay */
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
    uint64_t         first_sent_time;
    /* Loss-aware STARTUP exit (BBRv2-style) */
    uint64_t         startup_bytes_sent;
    uint64_t         startup_bytes_lost;
    bool             startup_loss_exit;  /* true = exited via loss */
} bbr_probe_t;
```

### TDS State — `tds_state_t`

Time Division Sending — fixed-rate cruise mode once bottleneck rate is known:

```c
typedef struct {
    uint64_t send_rate;          /* bytes/sec — the discovered rate */
    uint64_t rtprop_us;          /* propagation delay snapshot */
    uint64_t slot_interval_us;   /* time between send bursts */
    uint64_t burst_bytes;        /* bytes per slot */
    uint64_t last_slot_time;     /* timestamp of last burst */
    /* Lightweight loss watchdog */
    uint64_t total_sent;
    uint64_t total_acked;
    uint64_t total_lost;
    uint64_t window_sent;        /* sent in current watchdog window */
    uint64_t window_lost;        /* lost in current watchdog window */
    uint64_t window_start;       /* timestamp when window started */
} tds_state_t;
```

### Main Controller — `yumi_cc_t`

```c
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
    uint64_t      last_probe_time;      /* when PROBE last completed */
    uint64_t      reprobe_interval_us;  /* how often to re-probe */
    /* TDS tuning */
    double        tds_rate_factor;      /* fraction of BtlBw to use (≤1) */
    uint64_t      tds_slot_target_us;   /* desired slot granularity */
    /* Loss threshold to trigger re-probe (0.0–1.0) */
    double        loss_reprobe_thresh;
} yumi_cc_t;
```

### Congestion Control API

#### `yumi_cc_init`

Initialize the congestion controller.

```c
void yumi_cc_init(yumi_cc_t *cc, uint64_t mss);
```

| Parameter | Dir | Description |
|-----------|-----|-------------|
| `cc` | out | Controller to initialize |
| `mss` | in | Maximum segment size |

#### `yumi_cc_on_send`

Called before sending a packet. Returns pacing delay in microseconds.

```c
uint64_t yumi_cc_on_send(yumi_cc_t *cc, yumi_packet_t *pkt, uint64_t now_us);
```

#### `yumi_cc_on_ack`

Called when an ACK is received.

```c
void yumi_cc_on_ack(yumi_cc_t *cc, const yumi_packet_t *pkt,
                    uint64_t acked_bytes, uint64_t rtt_us, uint64_t now_us);
```

#### `yumi_cc_on_loss`

Called when packet loss is detected.

```c
void yumi_cc_on_loss(yumi_cc_t *cc, uint64_t lost_bytes);
```

#### `yumi_cc_get_mode`

```c
yumi_mode_t yumi_cc_get_mode(const yumi_cc_t *cc);
```

#### `yumi_cc_get_send_rate`

Get the current send rate in bytes/second.

```c
uint64_t yumi_cc_get_send_rate(const yumi_cc_t *cc);
```

#### `yumi_cc_get_cwnd`

Get the current congestion window.

```c
uint64_t yumi_cc_get_cwnd(const yumi_cc_t *cc);
```

#### `yumi_cc_get_slot_interval`

Get the TDS slot interval in microseconds.

```c
uint64_t yumi_cc_get_slot_interval(const yumi_cc_t *cc);
```

#### `yumi_cc_force_reprobe`

Manually trigger a re-probe.

```c
void yumi_cc_force_reprobe(yumi_cc_t *cc, uint64_t now_us);
```

---

## Implementation Files

| File | Responsibility |
|------|---------------|
| `src/network/yumi_udp.c` | UDP socket, ring buffer, worker threads |
| `src/network/yumi_udp_client.c` | Client API, reliability, reorder, ICE integration |
| `src/network/yumi_sudp_client.c` | Secure session, handshake, AEAD, epoch keys |
| `src/network/bbr.c` | Congestion control (BBR probe + TDS cruise) |
| `src/yumi_client.c` | High-level client, sync, attestation, bootstrap, meshnet |
| `include/network/net.h` | Wire protocol, MPSC ring, work items |
| `include/network/bbr.h` | Congestion control types and API |
| `include/network/yumi_udp_client.h` | Raw transport types and API |
| `include/network/yumi_sudp_client.h` | Secure transport types and API |
| `include/yumi_client.h` | High-level client types and API |
