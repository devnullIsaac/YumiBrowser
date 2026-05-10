/*
    Yumi SDK — System Clipboard WASM Imports
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

#ifndef WASM_CLIPBOARD_H
#define WASM_CLIPBOARD_H

/**
 * @file wasm_clipboard.h
 * @brief WASM guest imports for system clipboard access.
 *
 * The dashboard app mediates all clipboard access. Webapps call
 * these functions, but Yumi Browser enforces that no clipboard
 * data leaks without the user's knowledge.
 *
 * Typical flow:
 *   Copy:  Guest detects Ctrl/Cmd+C → reads its own selection →
 *          calls clipboard_set(ptr, len).
 *   Cut:   Same as copy, then guest deletes the selection.
 *   Paste: Guest detects Ctrl/Cmd+V → calls clipboard_get(buf, max) →
 *          inserts the returned text.
 */

#include <stdint.h>

#define IMPORT __attribute__((import_module("env")))

/**
 * @brief Check if the system clipboard contains text.
 * @return 1 if clipboard has text, 0 otherwise.
 */
IMPORT __attribute__((import_name("clipboard_available")))
int clipboard_available(void);

/**
 * @brief Read system clipboard text (UTF-8) into guest memory.
 *
 * Always null-terminates if max_len > 0.
 *
 * @param out_ptr  Buffer in wasm memory for the string.
 * @param max_len  Buffer capacity in bytes (max 1 MB).
 * @return Bytes written (excluding null terminator), 0 on empty/failure.
 */
IMPORT __attribute__((import_name("clipboard_get")))
int clipboard_get(char *out_ptr, int max_len);

/**
 * @brief Write UTF-8 text from guest memory to the system clipboard.
 *
 * @param ptr  Pointer to UTF-8 string in wasm memory (need not be null-terminated).
 * @param len  Byte length of the string (max 1 MB).
 * @return 1 on success, 0 on failure.
 */
IMPORT __attribute__((import_name("clipboard_set")))
int clipboard_set(const char *ptr, int len);

#undef IMPORT

#endif /* WASM_CLIPBOARD_H */
