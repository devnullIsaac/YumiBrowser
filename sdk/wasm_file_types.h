/*
    Yumi SDK — Shared IPC Result Types for File / Folder / Paste Operations
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

#ifndef WASM_FILE_TYPES_H
#define WASM_FILE_TYPES_H

/**
 * @file wasm_file_types.h
 * @brief Shared IPC result types for file, folder, and paste operations.
 *
 * These packed structs define the binary layout written by the host into
 * WASM linear memory and read by the guest in export callbacks.  Both
 * sides MUST agree on the layout — do not reorder or add padding.
 *
 * All pointer fields are WASM32 pointers (4 bytes) pointing into the
 * guest's linear memory.  The host writes the referenced data into
 * linear memory before filling in the pointer values.
 */

#include <stdint.h>

/* ================================================================== */
/*  File result                                                        */
/* ================================================================== */

/**
 * @brief Result delivered to on_file_result(info_ptr).
 *
 * Populated by the host after a file-open or file-save dialog.
 * For save dialogs, data/data_len are 0 (guest writes via handle).
 *
 * Layout (packed, 36 bytes):
 *   offset  0: handle     (int32_t)   — opaque handle, 0 = cancelled
 *   offset  4: name       (ptr)       — file name without extension
 *   offset  8: name_len   (uint32_t)
 *   offset 12: ext        (ptr)       — extension without leading dot
 *   offset 16: ext_len    (uint32_t)
 *   offset 20: file_size  (uint64_t)  — total size on disk
 *   offset 28: data       (ptr)       — file content (NULL for save)
 *   offset 32: data_len   (uint32_t)  — bytes of content loaded
 */
typedef struct __attribute__((packed)) {
    int32_t     handle;
    const char *name;
    uint32_t    name_len;
    const char *ext;
    uint32_t    ext_len;
    uint64_t    file_size;
    const void *data;
    uint32_t    data_len;
} DashboardFileInfo;

#define DASHBOARD_FILE_INFO_SIZE  36

/* ================================================================== */
/*  Folder entry (streaming iteration)                                 */
/* ================================================================== */

/** Opaque handle for a folder scan session. */
typedef uint32_t folder_scan_t;

/**
 * @brief One directory entry returned by dashboard_folder_next().
 *
 * Layout (packed, 28 bytes):
 *   offset  0: name       (ptr)       — entry name without extension
 *   offset  4: name_len   (uint32_t)
 *   offset  8: ext        (ptr)       — extension without dot (empty for dirs)
 *   offset 12: ext_len    (uint32_t)
 *   offset 16: file_size  (uint64_t)  — 0 for directories
 *   offset 24: is_dir     (uint32_t)  — 1 = directory, 0 = regular file
 */
typedef struct __attribute__((packed)) {
    const char *name;
    uint32_t    name_len;
    const char *ext;
    uint32_t    ext_len;
    uint64_t    file_size;
    uint32_t    is_dir;
} DashboardFolderEntry;

#define DASHBOARD_FOLDER_ENTRY_SIZE  28

/* ================================================================== */
/*  Paste result                                                       */
/* ================================================================== */

/**
 * @brief Result delivered to on_paste_result(info_ptr).
 *
 * Layout (packed, 16 bytes):
 *   offset  0: data       (ptr)       — paste content
 *   offset  4: data_len   (uint32_t)  — byte length
 *   offset  8: mime_type  (ptr)       — MIME type (e.g. "text/plain")
 *   offset 12: mime_len   (uint32_t)  — byte length of MIME string
 */
typedef struct __attribute__((packed)) {
    const void *data;
    uint32_t    data_len;
    const char *mime_type;
    uint32_t    mime_len;
} DashboardPasteInfo;

#define DASHBOARD_PASTE_INFO_SIZE  16

#endif /* WASM_FILE_TYPES_H */
