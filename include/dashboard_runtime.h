/**
 * @file dashboard_runtime.h
 * @brief Dashboard supervisor runtime for Yumi Browser.
 *
 * The dashboard is the privileged native layer that owns the SDL
 * window, manages webapp surfaces via tiling, mediates clipboard
 * access, controls group lifecycle (connect / disconnect / invite),
 * handles recovery mode, friend list, settings, and export.
 *
 * Webapps are sandboxed `WebAppRuntime` instances that render to
 * offscreen textures composited by the dashboard.
 */

#ifndef DASHBOARD_RUNTIME_H
#define DASHBOARD_RUNTIME_H

#include "deps.h"
#include "gpu.h"
#include "clipboard_bindings.h"
#include "webapp_runtime.h"
#include "yumi_client.h"
#include "deps/duckdb/src/include/duckdb.h"

#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Dynamic storage policy ──────────────────────────────────────
 *
 *  Slots, groups and friends are stored in heap-allocated arrays
 *  that grow on demand — the dashboard imposes no upper bound.
 *  Callers that access the arrays directly (e.g. unit tests) must
 *  reserve capacity first via @ref dashboard_reserve_slots,
 *  @ref dashboard_reserve_groups or @ref dashboard_reserve_friends.
 */

/** Initial capacities reserved by @ref dashboard_init. */
#define DASHBOARD_INITIAL_SLOT_CAP     8
#define DASHBOARD_INITIAL_GROUP_CAP    8
#define DASHBOARD_INITIAL_FRIEND_CAP  16

/** Stable webapp identifier length (mirrors YUMI_WEBAPP_ID_LEN in sdk/). */
#define YUMI_WEBAPP_ID_LEN      16

/** Webapp-visible group handle: truncated 32-byte prefix of a full
 *  GR_HASH_LEN (128-byte) Skein-1024 group hash (mirrors
 *  YUMI_GROUP_ID_LEN in sdk/wasm_dashboard.h). */
#define YUMI_GROUP_ID_LEN       32

/** Minimum group size before reconnect packets from non-friends are filtered. */
#define DASHBOARD_RECONNECT_FILTER_THRESHOLD  100

/* ── Reconnect protocol ─────────────────────────────────────────── */

/** 16-byte reconnect notification: [4-byte magic][12-byte peer_id prefix] */
#define DASHBOARD_RECONNECT_MAGIC       0x59524E43  /* "YRNC" */
#define DASHBOARD_RECONNECT_PACKET_LEN  16

/* ── Dashboard UI state ──────────────────────────────────────────── */

typedef enum {
    DASHBOARD_VIEW_NORMAL = 0,    /**< Normal tiled webapp view. */
    DASHBOARD_VIEW_SETTINGS,      /**< Settings panel open. */
    DASHBOARD_VIEW_RECOVERY,      /**< Recovery mode (greyed-out groups). */
} DashboardView;

/* ── Group connection state ──────────────────────────────────────── */

typedef enum {
    GROUP_STATE_DISCONNECTED = 0,
    GROUP_STATE_CONNECTING,
    GROUP_STATE_CONNECTED,
    GROUP_STATE_FAILED,
    GROUP_STATE_REMOVED_KICKED,   /**< Removed from group (kicked). */
    GROUP_STATE_REMOVED_BANNED,   /**< Removed from group (banned). */
} GroupState;

/* ── Per-group info ──────────────────────────────────────────────── */

typedef struct {
    yumi_client_t  *client;       /**< High-level network client (NULL if disconnected). */
    GroupState       state;
    char             db_path[4096];
    uint8_t          group_id[GR_HASH_LEN];
    bool             recovery_readonly;  /**< True if group is in recovery/read-only. */
    bool             background;         /**< True when marked background by supervisor. */
    uint32_t         member_count;       /**< Cached member count (for reconnect filter). */
    char             name[64];           /**< Display name (UTF-8, NUL-padded). */

    /** Per-group intra-webapp link buffer. */
    uint8_t          link_buf[WEBAPP_LINK_BUF_SIZE];
    uint32_t         link_buf_len;
} DashboardGroup;

/* ── Friend entry ────────────────────────────────────────────────── */

typedef struct {
    uint8_t  peer_id[GR_PEER_ID_LEN];
    char     display_name[128];
    int64_t  added_at;               /**< Unix timestamp. */
} DashboardFriend;

/* ── Webapp slot (tiling cell) ───────────────────────────────────── */

typedef struct {
    WebAppRuntime *rt;              /**< The webapp runtime (NULL if empty). */
    WebAppViewport viewport;        /**< Position and size in window coords. */
    bool           focused;
    uint32_t       group_index;     /**< Which DashboardGroup this slot belongs to. */

    /** Stable 16-byte webapp identifier (derived from wasm path / manifest). */
    uint8_t        app_id[YUMI_WEBAPP_ID_LEN];

    /** Display name of the webapp (UTF-8, NUL-padded; basename of wasm_path). */
    char           app_name[64];

    /* Offscreen surface (owned by dashboard, handed to webapp) */
    WGPUTexture     offscreen_tex;
    WGPUTextureView offscreen_view;
} WebAppSlot;

/* ── Pending IPC request (clipboard / file dialog) ───────────────── */

typedef enum {
    IPC_NONE = 0,
    IPC_PASTE_PENDING,            /**< Webapp requested paste, awaiting user confirm. */
    IPC_COPY_PENDING,             /**< Webapp provided text, awaiting copy. */
    IPC_FILE_OPEN_PENDING,
    IPC_FILE_SAVE_PENDING,
    IPC_FOLDER_OPEN_PENDING,
    IPC_FRIEND_ADD_PENDING,       /**< Webapp requested friend add, awaiting confirm. */
} IPCRequestType;

typedef struct {
    IPCRequestType type;
    uint32_t       slot_index;     /**< Which webapp slot made the request. */
    uint8_t        peer_id[GR_PEER_ID_LEN]; /**< For friend-add requests. */
    char           text[4096];     /**< For copy/paste text. */
    uint32_t       text_len;
} IPCRequest;

/* ── Folder scan (streaming directory iteration) ────────────────── */

#define DASHBOARD_MAX_FOLDER_SCANS  4

typedef struct {
    bool        active;
    uint32_t    slot_index;          /**< Which webapp owns this scan. */
    char        dir_path[4096];
    void       *dir_handle;          /**< Platform DIR* (opaque). */
} DashboardFolderScan;

/* ── IPC result struct sizes (WASM32 packed layout) ──────────────── */

#define DASHBOARD_FILE_INFO_SIZE      36
#define DASHBOARD_FOLDER_ENTRY_SIZE   28
#define DASHBOARD_PASTE_INFO_SIZE     16

/* ── Dashboard runtime ───────────────────────────────────────────── */

struct DashboardRuntime {
    /* ── Core ── */
    GpuContext     *gpu;            /**< Shared GPU context (owned). */
    SDL_Window     *window;         /**< SDL window (owned). */

    /* ── Groups (heap-allocated, grows on demand) ── */
    DashboardGroup *groups;
    uint32_t        group_count;
    uint32_t        group_cap;

    /* ── Webapp slots / tiling (heap-allocated, grows on demand) ── */
    WebAppSlot     *slots;
    uint32_t        slot_count;
    uint32_t        slot_cap;
    int             focused_slot;   /**< -1 = dashboard UI has focus. */

    /** Index of the foreground group (UINT32_MAX = none). */
    uint32_t        foreground_group;

    /* ── Privileged bindings ── */
    ClipboardBindings clipboard;

    /* ── Settings / export DuckDB (privileged, external access allowed) ── */
    duckdb_database    settings_db;
    duckdb_connection  settings_conn;
    bool               settings_db_active;

    /* ── Friend list (heap-allocated, grows on demand) ── */
    DashboardFriend *friends;
    uint32_t         friend_count;
    uint32_t         friend_cap;

    /* ── UI state ── */
    DashboardView    view;
    bool             recovery_mode;

    /* ── Pending IPC dialog ── */
    IPCRequest       pending_ipc;

    /* ── Folder scans ── */
    DashboardFolderScan  folder_scans[DASHBOARD_MAX_FOLDER_SCANS];

    /* ── Compositing pipeline ── */
    GpuCompositor compositor;           /**< Textured-quad compositor for offscreen blitting. */
};

/* ═════════════════════════════════════════════════════════════════════
 *  Lifecycle
 * ═════════════════════════════════════════════════════════════════════ */

/**
 * @brief Initialize the dashboard runtime.
 *
 * Creates the GPU context, opens the settings DuckDB, initializes
 * the clipboard, and loads the friend list.
 *
 * @param[out] d       Dashboard to initialize.
 * @param[in]  gpu     Initialized GPU context.
 * @param[in]  window  SDL window.
 * @param[in]  settings_db_path  Path to the global settings DuckDB.
 * @return true on success.
 */
bool dashboard_init(DashboardRuntime *d,
                    GpuContext *gpu,
                    SDL_Window *window,
                    const char *settings_db_path);

/** @brief Destroy the dashboard and all webapp slots. */
void dashboard_destroy(DashboardRuntime *d);

/**
 * @brief Ensure the slot / group / friend array can hold at least
 *        @p want entries, reallocating if needed.
 *
 * These helpers are safe to call before @ref dashboard_init has
 * populated the arrays — a freshly-zeroed @c DashboardRuntime is a
 * valid starting point and the backing buffer will be allocated on
 * first use.  Returns @c false only if @c realloc fails.
 *
 * @{ */
bool dashboard_reserve_slots  (DashboardRuntime *d, uint32_t want);
bool dashboard_reserve_groups (DashboardRuntime *d, uint32_t want);
bool dashboard_reserve_friends(DashboardRuntime *d, uint32_t want);
/** @} */

/* ═════════════════════════════════════════════════════════════════════
 *  Webapp slot management
 * ═════════════════════════════════════════════════════════════════════ */

/**
 * @brief Add a webapp slot and load a WASM module into it.
 *
 * Allocates an offscreen texture, creates a WebAppRuntime, and
 * loads the given .wasm file.
 *
 * @param[in,out] d            Dashboard.
 * @param[in]     group_index  Group this webapp belongs to.
 * @param[in]     wasm_path    Path to the .wasm file.
 * @param[in]     vp           Initial viewport.
 * @param[in]     is_dashboard True when the slot hosts the dashboard
 *                             webapp itself.  Dashboard slots get the
 *                             privileged admin IPC surface and are
 *                             denied both the webapp→dashboard request
 *                             surface and peer-to-peer networking.
 * @return Slot index (>=0) on success, -1 on failure.
 */
int dashboard_add_slot(DashboardRuntime *d,
                       uint32_t group_index,
                       const char *wasm_path,
                       const WebAppViewport *vp,
                       bool is_dashboard);

/**
 * @brief Remove and destroy a webapp slot.
 * @param[in,out] d      Dashboard.
 * @param[in]     index  Slot index to remove.
 */
void dashboard_remove_slot(DashboardRuntime *d, uint32_t index);

/* ═════════════════════════════════════════════════════════════════════
 *  Tiling layout
 * ═════════════════════════════════════════════════════════════════════ */

/** @brief Recalculate all slot viewports after a window resize. */
void dashboard_layout_recalc(DashboardRuntime *d);

/** @brief Set a single slot to full-screen. */
void dashboard_layout_fullscreen(DashboardRuntime *d, uint32_t slot_index);

/** @brief Split horizontally: left/right. */
void dashboard_layout_split_h(DashboardRuntime *d,
                               uint32_t slot_a, uint32_t slot_b);

/** @brief Split vertically: top/bottom. */
void dashboard_layout_split_v(DashboardRuntime *d,
                               uint32_t slot_a, uint32_t slot_b);

/* ═════════════════════════════════════════════════════════════════════
 *  Frame & event dispatch
 * ═════════════════════════════════════════════════════════════════════ */

/**
 * @brief Render one frame.
 *
 * Draws dashboard UI, calls each webapp's frame(), composites
 * offscreen textures onto the swapchain.
 */
void dashboard_frame(DashboardRuntime *d);

/**
 * @brief Per-iteration maintenance tick.
 *
 * Called once per main-loop iteration to drive time-sensitive work
 * that must run more often than the render frame (currently: refilling
 * each slot's audio stream).  Cheap to call; safe at any rate.
 */
void dashboard_tick(DashboardRuntime *d);

/**
 * @brief Dispatch an SDL event.
 *
 * Dashboard-level hotkeys and clipboard interception are handled
 * first.  Mouse events are hit-tested against slots; keyboard and
 * text input go to the focused slot only.
 *
 * @return false if the event is SDL_EVENT_QUIT.
 */
bool dashboard_dispatch_event(DashboardRuntime *d, const SDL_Event *ev);

/* ═════════════════════════════════════════════════════════════════════
 *  Group lifecycle
 * ═════════════════════════════════════════════════════════════════════ */

/**
 * @brief Connect (open or join) a group.
 * @param[in,out] d           Dashboard.
 * @param[in]     group_index Index of group to connect.
 * @return 0 on success, -1 on failure.
 */
int dashboard_connect_group(DashboardRuntime *d, uint32_t group_index);

/** @brief Disconnect from a group gracefully. */
void dashboard_disconnect_group(DashboardRuntime *d, uint32_t group_index);

/**
 * @brief Process an invite code and join the group.
 * @return Group index (>=0) on success, -1 on failure.
 */
int dashboard_process_invite(DashboardRuntime *d,
                             const uint8_t *invite_blob,
                             size_t invite_len,
                             const char *db_path);

/** @brief Generate an invite code for a group. Caller frees *out_blob. */
int dashboard_generate_invite(DashboardRuntime *d,
                              uint32_t group_index,
                              int64_t expiry_ms,
                              uint8_t **out_blob,
                              size_t *out_len);

void dashboard_invalidate_invite(DashboardRuntime *d, uint32_t group_index);
void dashboard_clear_invite(DashboardRuntime *d, uint32_t group_index);

/** @brief Set bandwidth limit for a group's SUDP layer. */
void dashboard_set_bandwidth_limit(DashboardRuntime *d,
                                   uint32_t group_index,
                                   uint32_t bytes_per_sec);

/* ═════════════════════════════════════════════════════════════════════
 *  Clipboard mediation
 * ═════════════════════════════════════════════════════════════════════ */

/**
 * @brief Handle a paste request from a webapp (shows confirmation dialog).
 * @param[in,out] d           Dashboard.
 * @param[in]     slot_index  Requesting webapp slot.
 */
void dashboard_handle_paste_request(DashboardRuntime *d, uint32_t slot_index);

/**
 * @brief Handle a copy request from a webapp.
 * @param[in,out] d       Dashboard.
 * @param[in]     text    Text to copy to system clipboard.
 * @param[in]     len     Length of text.
 */
void dashboard_handle_copy_request(DashboardRuntime *d,
                                   const char *text, uint32_t len);

/* ═════════════════════════════════════════════════════════════════════
 *  Intra-group link buffer
 * ═════════════════════════════════════════════════════════════════════ */

/**
 * @brief Write a link to the group's shared link buffer.
 *
 * Only webapps in the same group can read this link.
 *
 * @param[in,out] d            Dashboard.
 * @param[in]     group_index  Group whose link buffer to write.
 * @param[in]     data         Link data (opaque to dashboard).
 * @param[in]     len          Length of link data.
 */
void dashboard_set_group_link(DashboardRuntime *d,
                              uint32_t group_index,
                              const void *data, uint32_t len);

/**
 * @brief Read the group's shared link buffer.
 *
 * @param[in]  d            Dashboard.
 * @param[in]  group_index  Group whose link buffer to read.
 * @param[out] out_data     Receives pointer to link data (valid until next set).
 * @param[out] out_len      Receives length.
 * @return true if a link is available, false if empty.
 */
bool dashboard_get_group_link(const DashboardRuntime *d,
                              uint32_t group_index,
                              const void **out_data,
                              uint32_t *out_len);

/* ═════════════════════════════════════════════════════════════════════
 *  File dialog portal API
 * ═════════════════════════════════════════════════════════════════════ */

void dashboard_open_file_dialog(DashboardRuntime *d, uint32_t slot_index);
void dashboard_save_file_dialog(DashboardRuntime *d, uint32_t slot_index);
void dashboard_open_folder_dialog(DashboardRuntime *d, uint32_t slot_index);

/**
 * @brief Read the next entry from a folder scan.
 *
 * Writes a DASHBOARD_FOLDER_ENTRY_SIZE-byte struct + string data
 * into the guest's WASM linear memory at the given offset.
 *
 * @param[in,out] d          Dashboard.
 * @param[in]     scan_id    Folder scan slot index.
 * @param[in]     slot_index Webapp slot (for memory access).
 * @param[in]     out_wasm_ptr  WASM pointer where the entry struct is written.
 * @return 1 = entry written, 0 = done, -1 = error.
 */
int dashboard_folder_scan_next(DashboardRuntime *d,
                               uint32_t scan_id,
                               uint32_t slot_index,
                               uint32_t out_wasm_ptr);

/** @brief Close a folder scan and release resources. */
void dashboard_folder_scan_close(DashboardRuntime *d, uint32_t scan_id);

/* ═════════════════════════════════════════════════════════════════════
 *  Settings & export
 * ═════════════════════════════════════════════════════════════════════ */

bool dashboard_view_settings(DashboardRuntime *d);
bool dashboard_change_setting(DashboardRuntime *d,
                              const uint8_t *group_id,
                              const char *key,
                              const char *value);
bool dashboard_reset_settings(DashboardRuntime *d);

/**
 * @brief Export group data as LZMA archive to a user-chosen path.
 * @param[in,out] d            Dashboard.
 * @param[in]     group_index  Group to export.
 * @return true on success.
 */
bool dashboard_export_group(DashboardRuntime *d, uint32_t group_index);

/* ═════════════════════════════════════════════════════════════════════
 *  Recovery mode
 * ═════════════════════════════════════════════════════════════════════ */

/** @brief Toggle recovery mode view (greyed-out banned/kicked groups). */
void dashboard_toggle_recovery_mode(DashboardRuntime *d);

/**
 * @brief Send a 16-byte reconnect notification to a peer.
 *
 * @param[in,out] d            Dashboard.
 * @param[in]     group_index  Group the peer was formerly in.
 * @param[in]     peer_id      Peer to notify.
 * @return 0 on success, -1 on failure.
 */
int dashboard_send_reconnect(DashboardRuntime *d,
                             uint32_t group_index,
                             const uint8_t peer_id[GR_PEER_ID_LEN]);

/**
 * @brief Handle an incoming reconnect request.
 *
 * Checks friend list and group size filter.  If accepted, shows
 * a notification to the user.
 *
 * @param[in,out] d            Dashboard.
 * @param[in]     peer_id      Requesting peer.
 * @param[in]     group_index  Former group of the peer.
 */
void dashboard_handle_reconnect_request(DashboardRuntime *d,
                                        const uint8_t peer_id[GR_PEER_ID_LEN],
                                        uint32_t group_index);

/** @brief Delete a banned/kicked group's data permanently. */
void dashboard_delete_removed_group(DashboardRuntime *d, uint32_t group_index);

/* ═════════════════════════════════════════════════════════════════════
 *  Friend system
 * ═════════════════════════════════════════════════════════════════════ */

/**
 * @brief Request to add a friend (shows confirmation dialog).
 *
 * Called by a webapp via IPC import.  Dashboard prompts:
 * "Are you sure you want to add X as friend?"
 */
void dashboard_request_add_friend(DashboardRuntime *d,
                                  uint32_t slot_index,
                                  const uint8_t peer_id[GR_PEER_ID_LEN]);

/** @brief Confirm a pending friend-add request. */
bool dashboard_confirm_add_friend(DashboardRuntime *d,
                                  const uint8_t peer_id[GR_PEER_ID_LEN],
                                  const char *display_name);

/** @brief Remove a friend (called by webapp via IPC, shows confirmation). */
void dashboard_request_remove_friend(DashboardRuntime *d,
                                     uint32_t slot_index,
                                     const uint8_t peer_id[GR_PEER_ID_LEN]);

/** @brief Remove a friend (direct, after confirmation). */
void dashboard_remove_friend(DashboardRuntime *d,
                             const uint8_t peer_id[GR_PEER_ID_LEN]);

/**
 * @brief Deliver the friend list to a webapp as peer IDs.
 *
 * Calls the guest's on_friend_list_result export with a contiguous
 * array of GR_PEER_ID_LEN-byte peer IDs.
 */
void dashboard_send_friend_list(DashboardRuntime *d, uint32_t slot_index);

/** @brief Check if a peer is in the friend list. */
bool dashboard_is_friend(const DashboardRuntime *d,
                         const uint8_t peer_id[GR_PEER_ID_LEN]);

/* ═════════════════════════════════════════════════════════════════════
 *  Profile / social updates
 * ═════════════════════════════════════════════════════════════════════ */

void dashboard_update_profile_picture(DashboardRuntime *d,
                                      uint32_t group_index,
                                      const void *data, uint32_t len);
void dashboard_update_status(DashboardRuntime *d,
                             uint32_t group_index,
                             const char *status);
void dashboard_update_bios(DashboardRuntime *d,
                           uint32_t group_index,
                           const char *bios);
void dashboard_update_quotes(DashboardRuntime *d,
                             uint32_t group_index,
                             const char *quotes);
void dashboard_update_activity(DashboardRuntime *d,
                               uint32_t group_index,
                               const char *activity);
void dashboard_add_emoji(DashboardRuntime *d,
                         uint32_t group_index,
                         const char *emoji);
void dashboard_remove_emoji(DashboardRuntime *d,
                            uint32_t group_index,
                            const char *emoji);

/* ═════════════════════════════════════════════════════════════════════
 *  Group tab notifications
 * ═════════════════════════════════════════════════════════════════════ */

#define DASHBOARD_NOTIFY_TEXT_MAX  256
#define DASHBOARD_NOTIFY_FLAG_CRITICAL  (1u << 0)

/**
 * @brief Post a notification on a group's tab.
 *
 * @param[in,out] d          Dashboard.
 * @param[in]     group_index Group whose tab receives the notification.
 * @param[in]     text       UTF-8 notification text (max DASHBOARD_NOTIFY_TEXT_MAX bytes).
 * @param[in]     text_len   Byte length of text.
 * @param[in]     img        Optional image data (NULL if none).
 * @param[in]     img_len    Byte length of image data (0 if none).
 * @param[in]     flags      Bitfield (DASHBOARD_NOTIFY_FLAG_CRITICAL, etc.).
 */
void dashboard_group_notify(DashboardRuntime *d,
                            uint32_t group_index,
                            const char *text, uint32_t text_len,
                            const void *img,  uint32_t img_len,
                            uint32_t flags);

/* ═════════════════════════════════════════════════════════════════════
 *  DuckDB disk-quota + group/webapp management
 * ═════════════════════════════════════════════════════════════════════ */

/** Absolute ceiling the dashboard will ever hand out to a webapp. */
#define DASHBOARD_DB_QUOTA_HARD_MAX   ((uint64_t)4ull * 1024 * 1024 * 1024)

/** WASM-facing YumiGroupInfo struct size (see sdk/wasm_dashboard.h). */
#define YUMI_GROUP_INFO_SIZE          112

/** WASM-facing YumiWebAppInfo struct size (see sdk/wasm_dashboard.h). */
#define YUMI_WEBAPP_INFO_SIZE         104

/**
 * @brief Set the DuckDB disk quota for one slot.
 *
 * Clamps to DASHBOARD_DB_QUOTA_HARD_MAX, updates the bindings, and
 * notifies the webapp via `on_db_quota_changed` if it exports it.
 *
 * @return 0 on success, negative on error.
 */
int32_t dashboard_set_slot_db_quota(DashboardRuntime *d,
                                    uint32_t slot_index,
                                    uint64_t quota_bytes);

/**
 * @brief Fill a YumiGroupInfo array in guest memory.
 *
 * @param[in]  d        Dashboard.
 * @param[in]  rt       Guest runtime whose memory to write into (for
 *                      bounds checks and base-pointer resolution).
 * @param[in]  out_ptr  WASM pointer to array (may be 0 for count-only).
 * @param[in]  out_cap  Number of entries the guest allocated.
 * @return              Total group count (even if > out_cap).
 */
int32_t dashboard_fill_group_info(DashboardRuntime *d,
                                  WebAppRuntime *rt,
                                  uint32_t out_ptr,
                                  uint32_t out_cap);

/**
 * @brief Fill a YumiWebAppInfo array in guest memory.
 *
 * @param[in]  d            Dashboard.
 * @param[in]  rt           Guest runtime (for memory + self-marking).
 * @param[in]  group_index  Group to enumerate.
 * @param[in]  out_ptr      WASM pointer (may be 0 for count-only).
 * @param[in]  out_cap      Number of entries allocated.
 * @return                  Total webapp count.
 */
int32_t dashboard_fill_webapp_info(DashboardRuntime *d,
                                   WebAppRuntime *rt,
                                   uint32_t group_index,
                                   uint32_t out_ptr,
                                   uint32_t out_cap);

/**
 * @brief Switch the foreground group.
 *
 * The previous foreground group is marked background (if different),
 * and every running webapp receives `on_group_switched`.
 */
int32_t dashboard_switch_group(DashboardRuntime *d, uint32_t group_index);

/**
 * @brief Mark / unmark a group as background.
 *
 * Webapps in the group receive `on_group_background_changed` if they
 * export it.  Background groups are skipped by the frame loop but
 * their network client keeps running.
 */
int32_t dashboard_group_set_background(DashboardRuntime *d,
                                       uint32_t group_index,
                                       bool background);

/* ═════════════════════════════════════════════════════════════════════
 *  Group Registrar proxy (surface-level, caller-identity-signed)
 *
 *  Each verb resolves @p group_index to the group's yumi_client and
 *  signs mutations with that group's identity.  Only user-facing
 *  fields cross the WASM boundary — secret key material is never
 *  exposed.
 * ═════════════════════════════════════════════════════════════════════ */

/* ── Peer management ────────────────────────────────────────────── */
int32_t dashboard_gr_peer_kick(DashboardRuntime *d, uint32_t gi,
                               const uint8_t peer_id[GR_PEER_ID_LEN],
                               const char *reason);
int32_t dashboard_gr_peer_ban(DashboardRuntime *d, uint32_t gi,
                              const uint8_t peer_id[GR_PEER_ID_LEN],
                              const char *reason);
int32_t dashboard_gr_peer_touch(DashboardRuntime *d, uint32_t gi,
                                const uint8_t peer_id[GR_PEER_ID_LEN]);
int32_t dashboard_gr_peer_set_role(DashboardRuntime *d, uint32_t gi,
                                   const uint8_t peer_id[GR_PEER_ID_LEN],
                                   uint32_t role_id);
int32_t dashboard_gr_peer_get(DashboardRuntime *d, uint32_t gi,
                              const uint8_t peer_id[GR_PEER_ID_LEN],
                              uint8_t out_record[280]);
int32_t dashboard_gr_peer_count(DashboardRuntime *d, uint32_t gi,
                                int32_t status_filter);
int32_t dashboard_gr_peer_list(DashboardRuntime *d, uint32_t gi,
                               int32_t status_filter,
                               uint8_t *out_records, uint32_t out_cap);
int32_t dashboard_gr_peer_is_authorized(DashboardRuntime *d, uint32_t gi,
                                        const uint8_t peer_id[GR_PEER_ID_LEN]);

/* ── Role management ───────────────────────────────────────────── */
int32_t dashboard_gr_role_add(DashboardRuntime *d, uint32_t gi,
                              const char *name, uint32_t permissions,
                              uint32_t *role_id_out);
int32_t dashboard_gr_role_remove(DashboardRuntime *d, uint32_t gi,
                                 uint32_t role_id);
int32_t dashboard_gr_role_set_permissions(DashboardRuntime *d, uint32_t gi,
                                          uint32_t role_id,
                                          uint32_t permissions);
int32_t dashboard_gr_role_get(DashboardRuntime *d, uint32_t gi,
                              uint32_t role_id, uint8_t out_record[152]);
int32_t dashboard_gr_role_list(DashboardRuntime *d, uint32_t gi,
                               uint8_t *out_records, uint32_t out_cap);
int32_t dashboard_gr_role_count(DashboardRuntime *d, uint32_t gi);
int32_t dashboard_gr_has_permission(DashboardRuntime *d, uint32_t gi,
                                    uint32_t permission);

/* ── Group-registrar webapp manifest ───────────────────────────── */
int32_t dashboard_gr_webapp_add(DashboardRuntime *d, uint32_t gi,
                                const uint8_t *record /* 304 bytes */);
int32_t dashboard_gr_webapp_remove(DashboardRuntime *d, uint32_t gi,
                                   const uint8_t hash[128]);
int32_t dashboard_gr_webapp_is_authorized(DashboardRuntime *d, uint32_t gi,
                                          const uint8_t hash[128]);
int32_t dashboard_gr_webapp_list(DashboardRuntime *d, uint32_t gi,
                                 uint8_t *out_records, uint32_t out_cap);
int32_t dashboard_gr_webapp_count(DashboardRuntime *d, uint32_t gi);

/* ── Server management ─────────────────────────────────────────── */
int32_t dashboard_gr_server_add(DashboardRuntime *d, uint32_t gi,
                                const uint8_t *record /* 4480 bytes */);
int32_t dashboard_gr_server_remove(DashboardRuntime *d, uint32_t gi,
                                   const uint8_t id_hash[128]);
int32_t dashboard_gr_server_list(DashboardRuntime *d, uint32_t gi,
                                 uint32_t server_type,
                                 uint8_t *out_records, uint32_t out_cap);
int32_t dashboard_gr_server_count(DashboardRuntime *d, uint32_t gi,
                                  uint32_t server_type);

/* ── Epoch rotation ────────────────────────────────────────────── */
int32_t dashboard_gr_epoch_rotate(DashboardRuntime *d, uint32_t gi);

/* ── Retention policy ──────────────────────────────────────────── */
int32_t dashboard_gr_retention_get(DashboardRuntime *d, uint32_t gi,
                                   int64_t out[3]);
int32_t dashboard_gr_retention_set(DashboardRuntime *d, uint32_t gi,
                                   const int64_t policy[3]);

/* ── Group icon ────────────────────────────────────────────────── */
int32_t dashboard_gr_icon_set(DashboardRuntime *d, uint32_t gi,
                              const uint8_t *data, uint32_t data_len,
                              const char *mime,
                              uint16_t width, uint16_t height,
                              bool is_video,
                              const uint8_t *static_frame,
                              uint32_t static_frame_len);
int32_t dashboard_gr_icon_get(DashboardRuntime *d, uint32_t gi,
                              uint8_t out_info[248],
                              uint8_t *data_out, uint32_t data_cap,
                              uint8_t *sf_out, uint32_t sf_cap);
int32_t dashboard_gr_icon_remove(DashboardRuntime *d, uint32_t gi);
int32_t dashboard_gr_icon_hash(DashboardRuntime *d, uint32_t gi,
                               uint8_t hash_out[128]);

/* ── Invite management ─────────────────────────────────────────── */
int32_t dashboard_gr_invite_create(DashboardRuntime *d, uint32_t gi,
                                   int64_t expiry_ms,
                                   uint8_t *blob_out, uint32_t blob_cap,
                                   uint8_t verification_token_out[128]);
int32_t dashboard_gr_invite_invalidate(DashboardRuntime *d, uint32_t gi,
                                       const uint8_t token[128]);
int32_t dashboard_gr_invite_check(DashboardRuntime *d, uint32_t gi,
                                  const uint8_t token[128]);
int32_t dashboard_gr_invite_list(DashboardRuntime *d, uint32_t gi,
                                 uint8_t *out_records, uint32_t out_cap);
int32_t dashboard_gr_invite_count(DashboardRuntime *d, uint32_t gi);
int32_t dashboard_gr_invite_parse(const uint8_t *blob, uint32_t blob_len,
                                  uint8_t out_ticket[312]);

/* ── Audit log ──────────────────────────────────────────────────
 *
 *  Audit entries are delivered in a 600-byte wire layout matching
 *  the SDK's @c YumiAuditEntry.  The ML-DSA signature is stripped
 *  — chain verification is performed host-side.
 */
#define DASHBOARD_AUDIT_ENTRY_SIZE        600
#define DASHBOARD_AUDIT_CHAIN_RESULT_SIZE 32
#define DASHBOARD_AUDIT_FORK_SIZE         1544

int32_t dashboard_gr_audit_list(DashboardRuntime *d, uint32_t gi,
                                int64_t since_ms,
                                uint8_t *out_records, uint32_t out_cap);
int32_t dashboard_gr_audit_count(DashboardRuntime *d, uint32_t gi);
int32_t dashboard_gr_audit_verify_chain(DashboardRuntime *d, uint32_t gi,
                                        uint8_t out_record[DASHBOARD_AUDIT_CHAIN_RESULT_SIZE]);
int32_t dashboard_gr_audit_list_forks(DashboardRuntime *d, uint32_t gi,
                                      uint8_t *out_records, uint32_t out_cap);

/* ── Behavior analysis (wire sizes mirror SDK structs) ─────────── */
#define DASHBOARD_BEHAVIOR_ACTOR_BURST_SIZE    32
#define DASHBOARD_BEHAVIOR_MUTATION_RATE_SIZE  24
#define DASHBOARD_BEHAVIOR_ADMIN_SCORE_SIZE    32
#define DASHBOARD_BEHAVIOR_PEER_CHURN_SIZE     32
#define DASHBOARD_BEHAVIOR_DELTA_SCORE_SIZE    40
#define DASHBOARD_BEHAVIOR_EPOCH_PATTERN_SIZE  24
#define DASHBOARD_BEHAVIOR_NETWORK_SCORE_SIZE  32
#define DASHBOARD_BEHAVIOR_SNAPSHOT_SIZE       200
#define DASHBOARD_BEHAVIOR_CONFIG_SIZE         40

int32_t dashboard_gr_behavior_actor_burst(DashboardRuntime *d, uint32_t gi,
                                          const uint8_t actor[GR_PEER_ID_LEN],
                                          int64_t window_ms,
                                          uint8_t out[DASHBOARD_BEHAVIOR_ACTOR_BURST_SIZE]);
int32_t dashboard_gr_behavior_mutation_rate(DashboardRuntime *d, uint32_t gi,
                                            int64_t window_ms,
                                            uint8_t out[DASHBOARD_BEHAVIOR_MUTATION_RATE_SIZE]);
int32_t dashboard_gr_behavior_admin_score(DashboardRuntime *d, uint32_t gi,
                                          const uint8_t admin[GR_PEER_ID_LEN],
                                          int64_t window_ms,
                                          uint8_t out[DASHBOARD_BEHAVIOR_ADMIN_SCORE_SIZE]);
int32_t dashboard_gr_behavior_peer_churn(DashboardRuntime *d, uint32_t gi,
                                         int64_t window_ms,
                                         uint8_t out[DASHBOARD_BEHAVIOR_PEER_CHURN_SIZE]);
int32_t dashboard_gr_behavior_delta_score(DashboardRuntime *d, uint32_t gi,
                                          uint64_t delta_bytes,
                                          uint32_t entry_count,
                                          uint8_t out[DASHBOARD_BEHAVIOR_DELTA_SCORE_SIZE]);
int32_t dashboard_gr_behavior_stale_peers(DashboardRuntime *d, uint32_t gi,
                                          int64_t stale_threshold_ms,
                                          uint8_t *out_ids, uint32_t max_count);
int32_t dashboard_gr_behavior_epoch_pattern(DashboardRuntime *d, uint32_t gi,
                                            int64_t window_ms,
                                            uint8_t out[DASHBOARD_BEHAVIOR_EPOCH_PATTERN_SIZE]);
int32_t dashboard_gr_behavior_network_score(DashboardRuntime *d, uint32_t gi,
                                            uint8_t out[DASHBOARD_BEHAVIOR_NETWORK_SCORE_SIZE]);
int32_t dashboard_gr_behavior_snapshot(DashboardRuntime *d, uint32_t gi,
                                       int64_t window_ms,
                                       uint8_t out[DASHBOARD_BEHAVIOR_SNAPSHOT_SIZE]);
int32_t dashboard_gr_behavior_get_config(DashboardRuntime *d, uint32_t gi,
                                         uint8_t out[DASHBOARD_BEHAVIOR_CONFIG_SIZE]);
int32_t dashboard_gr_behavior_set_config(DashboardRuntime *d, uint32_t gi,
                                         const uint8_t in[DASHBOARD_BEHAVIOR_CONFIG_SIZE]);

/* ── IP blocklist ──────────────────────────────────────────────── */

int32_t dashboard_gr_blocklist_check(DashboardRuntime *d, uint32_t gi,
                                     const char *ip,
                                     int64_t block_duration_ms);
int32_t dashboard_gr_blocklist_record_fail(DashboardRuntime *d, uint32_t gi,
                                           const char *ip,
                                           int32_t max_fails);
int32_t dashboard_gr_blocklist_reset(DashboardRuntime *d, uint32_t gi,
                                     const char *ip);
int32_t dashboard_gr_blocklist_cleanup(DashboardRuntime *d, uint32_t gi,
                                       int64_t block_duration_ms);

/* ── Group-Registrar change broadcast ───────────────────────────
 *
 *  Fired synchronously after every successful mutation issued
 *  through the dashboard_gr_* proxy.  The dashboard records the
 *  change for its own audit log and leaves peer notification to the
 *  group-registrar / networking subsystem — the webapp runtime is
 *  deliberately not involved.
 */
void dashboard_gr_emit_change(DashboardRuntime *d, uint32_t gi,
                              uint32_t change_type,
                              const uint8_t actor_id[GR_PEER_ID_LEN],
                              const uint8_t target_id[GR_PEER_ID_LEN],
                              const char *detail);

/* ═════════════════════════════════════════════════════════════════════
 *  Dashboard IPC table
 *
 *  All dashboard-facing IPC imports ("env.dashboard_*", "env.webapp_copy_link",
 *  "env.webapp_paste_link") are implemented in src/dashboard_runtime.c.
 *  The webapp runtime pulls the table from here at instantiate time —
 *  it must not hold a direct reference to any dashboard IPC function.
 * ═════════════════════════════════════════════════════════════════════ */

struct wasm_val_vec_t;
struct wasm_trap_t;

typedef struct wasm_trap_t *(*DashboardIPCCallback)(
    void *env,
    const struct wasm_val_vec_t *args,
    struct wasm_val_vec_t *results);

typedef struct DashboardIPCEntry {
    const char          *name;
    DashboardIPCCallback cb;
    uint8_t              np;            /**< Param count (0..5). */
    uint8_t              params[10];    /**< wasm_valkind_t values. */
    uint8_t              nr;            /**< Result count (0..1). */
    uint8_t              results[1];    /**< wasm_valkind_t values. */
} DashboardIPCEntry;

/**
 * @brief Retrieve the webapp→dashboard IPC import table.
 *
 * Bound into *regular* webapp slots only.  Exposes the user-mediated
 * request surface a normal webapp needs (clipboard, shared link
 * buffer, file dialogs, friend management, group notifications,
 * self-identity queries).  The dashboard webapp itself does not
 * receive these bindings.
 *
 * Entries use `WebAppRuntime *` as the host env pointer.
 *
 * @param[out] count_out  Receives the number of entries.
 * @return Pointer to a static table of entries.
 */
const DashboardIPCEntry *webapp_ipc_table(size_t *count_out);

/**
 * @brief Retrieve the dashboard-only IPC import table.
 *
 * Bound into the dashboard webapp slot only.  Exposes the privileged
 * admin surface: group enumeration / switching, registrar DB
 * open/close, per-webapp DuckDB quota targeting, full Group
 * Registrar proxy, audit log, behaviour analysis, and the IP
 * blocklist.  Regular webapp slots do not receive these bindings.
 *
 * Entries use `WebAppRuntime *` as the host env pointer.
 *
 * @param[out] count_out  Receives the number of entries.
 * @return Pointer to a static table of entries.
 */
const DashboardIPCEntry *dashboard_only_ipc_table(size_t *count_out);

#ifdef __cplusplus
}
#endif

#endif /* DASHBOARD_RUNTIME_H */
