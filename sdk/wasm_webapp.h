/*
    Yumi SDK — Webapp → Dashboard IPC Imports
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

#ifndef WASM_WEBAPP_H
#define WASM_WEBAPP_H

/**
 * @file wasm_webapp.h
 * @brief Guest-side IPC imports a normal webapp uses to talk to the
 *        Dashboard host.
 *
 * This header carries the "bottom half" of the dashboard IPC surface:
 * the calls that any webapp running in a dashboard slot may issue to
 * request user-mediated services (clipboard, file dialogs, friends,
 * notifications, self-identity queries, shared link buffer).
 *
 * The heavier privileged surface — group enumeration / switching,
 * Group Registrar proxy, audit log, behaviour analysis, IP blocklist
 * — lives in @c wasm_dashboard.h and is only meaningful to the
 * dashboard webapp itself.
 *
 * No guest-export callback declarations live here. Callbacks that the
 * host may invoke on a webapp (for example @c on_paste_result,
 * @c on_file_result, @c on_folder_result, @c on_friend_list_result,
 * @c on_db_quota_changed, @c on_group_switched,
 * @c on_group_background_changed) are documented in the SDK template
 * files under @c sdk/templates/.
 */

#include <stdint.h>
#include "wasm_file_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Shared identifiers                                                 */
/* ================================================================== */

/** Peer ID length in bytes (matches GR_PEER_ID_LEN on the host). */
#ifndef YUMI_PEER_ID_LEN
#define YUMI_PEER_ID_LEN 32
#endif

/** Group ID length in bytes (matches GR_HASH_LEN on the host). */
#ifndef YUMI_GROUP_ID_LEN
#define YUMI_GROUP_ID_LEN 32
#endif

/** WebApp ID length in bytes — stable identifier for a webapp binary
 *  (typically a prefix of the SHA-256 of the manifest or .wasm). */
#ifndef YUMI_WEBAPP_ID_LEN
#define YUMI_WEBAPP_ID_LEN 16
#endif

/** Opaque group identifier. */
#ifndef YUMI_GROUP_ID_DEFINED
#define YUMI_GROUP_ID_DEFINED
typedef struct YumiGroupId
{
    uint8_t bytes[YUMI_GROUP_ID_LEN];
} YumiGroupId;
#endif

/** Opaque webapp identifier — stable across restarts. */
#ifndef YUMI_WEBAPP_ID_DEFINED
#define YUMI_WEBAPP_ID_DEFINED
typedef struct YumiWebAppId
{
    uint8_t bytes[YUMI_WEBAPP_ID_LEN];
} YumiWebAppId;
#endif

/** Peer identity — 32-byte opaque identifier. */
#ifndef YUMI_PEER_ID_DEFINED
#define YUMI_PEER_ID_DEFINED
typedef struct YumiPeerId
{
    union
    {
        uint8_t bytes[YUMI_PEER_ID_LEN];
        struct
        {
            uint64_t a;
            uint64_t b;
            uint64_t c;
            uint64_t d;
        };
    };
} YumiPeerId;
#endif

/* ================================================================== */
/*  Clipboard IPC                                                      */
/* ================================================================== */

/**
 * @brief Request a paste from the system clipboard.
 *
 * The host will read the clipboard and invoke the guest's
 * @c on_paste_result export with the payload.
 */
__attribute__((import_module("env"), import_name("dashboard_request_paste")))
void dashboard_request_paste(void);

/**
 * @brief Copy text to the system clipboard.
 *
 * @param ptr  Pointer to UTF-8 text in WASM linear memory.
 * @param len  Byte length of the text (no NUL required).
 */
__attribute__((import_module("env"), import_name("dashboard_request_copy")))
void dashboard_request_copy(const void *ptr, uint32_t len);

/* ================================================================== */
/*  Intra-group link buffer                                            */
/* ================================================================== */

/**
 * @brief Write an opaque link into the group's shared link buffer.
 *
 * Other webapps in the same group can read this via @ref
 * webapp_paste_link. Cross-group reads return empty.  The format is
 * opaque — only the webapps within the group need to understand it.
 *
 * @param ptr  Pointer to link data in WASM linear memory.
 * @param len  Byte length.
 */
__attribute__((import_module("env"), import_name("webapp_copy_link")))
void webapp_copy_link(const void *ptr, uint32_t len);

/**
 * @brief Read the group's shared link buffer.
 *
 * @param buf_ptr  Destination buffer in WASM linear memory.
 * @param buf_len  Capacity of the destination buffer.
 * @return         Actual length of the link data (may exceed @p buf_len
 *                 if the buffer is too small; data is truncated to
 *                 @p buf_len).  Returns 0 if no link is set or if the
 *                 caller is not in the owning group.
 */
__attribute__((import_module("env"), import_name("webapp_paste_link")))
int32_t webapp_paste_link(void *buf_ptr, uint32_t buf_len);

/* ================================================================== */
/*  File dialogs (asynchronous)                                        */
/* ================================================================== */

/**
 * @brief Open a file-open dialog.
 *
 * Result delivered via @c on_file_result(info_ptr) export, where
 * @c info_ptr points to a @c DashboardFileInfo in WASM memory.  The
 * struct includes the file name, extension, size, and loaded content
 * buffer.
 */
__attribute__((import_module("env"), import_name("dashboard_open_file_dialog")))
void dashboard_open_file_dialog(void);

/**
 * @brief Open a file-save dialog.
 *
 * Result delivered via @c on_file_result(info_ptr) export.  For save
 * dialogs, @c DashboardFileInfo::data is NULL and @c data_len is 0 —
 * use the returned handle for subsequent write operations.
 */
__attribute__((import_module("env"), import_name("dashboard_save_file_dialog")))
void dashboard_save_file_dialog(void);

/**
 * @brief Open a folder-select dialog.
 *
 * Result delivered via @c on_folder_result(scan_handle) export.  The
 * guest iterates entries by calling @ref dashboard_folder_next in a
 * loop, then releases the handle with @ref dashboard_folder_close.
 */
__attribute__((import_module("env"), import_name("dashboard_open_folder_dialog")))
void dashboard_open_folder_dialog(void);

/**
 * @brief Read the next entry from a folder scan.
 *
 * Fills the @c DashboardFolderEntry struct at @p out_ptr with the
 * next directory entry.  String data (name, ext) is written into
 * WASM memory adjacent to the struct.
 *
 * @param scan     Folder scan handle from @c on_folder_result.
 * @param out_ptr  Pointer to a @c DashboardFolderEntry in WASM memory.
 * @return         1 = entry written, 0 = no more entries, -1 = error.
 */
__attribute__((import_module("env"), import_name("dashboard_folder_next")))
int32_t dashboard_folder_next(folder_scan_t scan, DashboardFolderEntry *out_ptr);

/**
 * @brief Close a folder scan and release host resources.
 *
 * @param scan  Folder scan handle from @c on_folder_result.
 */
__attribute__((import_module("env"), import_name("dashboard_folder_close")))
void dashboard_folder_close(folder_scan_t scan);

/* ================================================================== */
/*  Social / Friends                                                   */
/* ================================================================== */

/**
 * @brief Request adding a peer as a friend.
 *
 * The dashboard will display a confirmation dialog.  If accepted, the
 * peer is inserted into the friends table.
 *
 * @param peer_id  Pointer to a @c YumiPeerId in WASM linear memory.
 */
__attribute__((import_module("env"), import_name("dashboard_add_friend")))
void dashboard_add_friend(const YumiPeerId *peer_id);

/**
 * @brief Request the friend list from the dashboard.
 *
 * The host will deliver the list to the guest via the
 * @c on_friend_list_result(peer_ids, count) export, where
 * @c peer_ids is a contiguous array of @c YumiPeerId structs.
 */
__attribute__((import_module("env"), import_name("dashboard_get_friend_list")))
void dashboard_get_friend_list(void);

/**
 * @brief Request removal of a peer from the friend list.
 *
 * The dashboard will display a confirmation dialog.  If confirmed,
 * the peer is removed from the friends table.
 *
 * @param peer_id  Pointer to a @c YumiPeerId in WASM linear memory.
 */
__attribute__((import_module("env"), import_name("dashboard_remove_friend")))
void dashboard_remove_friend(const YumiPeerId *peer_id);

/* ================================================================== */
/*  Group tab notifications                                            */
/* ================================================================== */

/** Maximum length of notification text (bytes, excluding NUL). */
#define DASHBOARD_NOTIFY_TEXT_MAX 256

/** Flag: treat the notification as critical / alert. */
#define DASHBOARD_NOTIFY_FLAG_CRITICAL (1u << 0)

/**
 * @brief Post a notification on the group tab.
 *
 * The notification appears as a badge / toast on the group's tab in
 * the dashboard UI.  An optional small image (e.g. icon) can be
 * attached.
 *
 * @param text_ptr   Pointer to UTF-8 notification text in WASM memory.
 * @param text_len   Byte length (max @ref DASHBOARD_NOTIFY_TEXT_MAX).
 * @param img_ptr    Pointer to image data in WASM memory, or 0 (NULL)
 *                   if no image is attached.
 * @param img_len    Byte length of image data, or 0 if no image.
 * @param flags      Bitfield.  Set @ref DASHBOARD_NOTIFY_FLAG_CRITICAL
 *                   for an alert-level notification.
 */
__attribute__((import_module("env"), import_name("dashboard_group_notify")))
void dashboard_group_notify(const void *text_ptr, uint32_t text_len,
                            const void *img_ptr, uint32_t img_len,
                            uint32_t flags);

/* ================================================================== */
/*  Self identity                                                      */
/* ================================================================== */

/**
 * @brief Write the caller's webapp ID into @p out.
 *
 * Stable across runs.  Zeroed if the host has no ID assigned yet.
 */
__attribute__((import_module("env"), import_name("dashboard_self_webapp_id")))
void dashboard_self_webapp_id(YumiWebAppId *out);

/**
 * @brief Write the caller's current group ID into @p out.
 *
 * Always refers to the group the caller belongs to — unaffected by
 * foreground / background changes.
 */
__attribute__((import_module("env"), import_name("dashboard_self_group_id")))
void dashboard_self_group_id(YumiGroupId *out);

#ifdef __cplusplus
}
#endif

#endif /* WASM_WEBAPP_H */
