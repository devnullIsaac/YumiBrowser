/*
    Group Registrar — Growable Write Buffer / Sequential Reader API
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
 * @file buf.h
 * @brief Growable write buffer and sequential reader for serialization.
 *
 * Provides two complementary primitives for binary serialization:
 *
 * - **gr_buf_t** (writer): a growable byte buffer that doubles in capacity
 *   on overflow. Used by serialize.c, invite.c, and crypto.c to build
 *   wire-format blobs for network transfer and on-disk storage.
 *
 * - **gr_reader_t** (reader): a zero-copy sequential cursor over a
 *   read-only byte array. Used to parse blobs produced by gr_buf_t.
 *
 * @section wire_format Wire Format
 *
 * All multi-byte integers are stored in **little-endian** byte order.
 * This is the native byte order for x86, ARM (default), RISC-V, and
 * Apple Silicon — which covers effectively all target platforms.
 *
 * On the rare big-endian host, the gr_htoleNN / gr_leNNtoh helpers
 * perform byte-swapping. On little-endian hosts these compile to no-ops
 * and the optimizer eliminates them entirely, so there is zero overhead
 * on the common path.
 *
 * Strings are length-prefixed: a u32 byte count followed by the raw
 * bytes (no null terminator in the wire format). On read, the null
 * terminator is restored into the caller's buffer.
 *
 * Raw byte arrays (keys, hashes, peer IDs) are written and read as-is
 * with no length prefix — the caller knows the fixed size from the
 * group_registrar.h constants.
 *
 * @section ownership Ownership
 *
 * - gr_buf_t owns its data pointer. Call gr_buf_free() when done,
 *   or transfer ownership by taking buf.data and NOT calling gr_buf_free().
 *   (gr_serialize and gr_invite_create do this — the caller frees with
 *   gr_free().)
 *
 * - gr_reader_t does NOT own its data pointer. The underlying buffer
 *   must remain valid for the lifetime of the reader.
 *
 * @section thread_safety Thread Safety
 *
 * Neither gr_buf_t nor gr_reader_t is thread-safe. Each instance must
 * be used from a single thread at a time. This matches the single-
 * connection constraint of the gr_registrar_t handle.
 */

#ifndef GR_BUF_H
#define GR_BUF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════
 * @defgroup endian Endian Detection and Byte Swapping
 * @{
 *
 * Runtime endianness detection and conversion helpers.
 *
 * The wire format is little-endian. On little-endian hosts (the vast
 * majority), all conversion functions compile to identity operations.
 * On big-endian hosts, they perform the necessary byte swap.
 *
 * These are defined as static inline in the header so the compiler
 * can constant-fold and eliminate dead branches at every call site.
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief Detect host byte order at runtime.
 * @return true if the host is little-endian, false if big-endian.
 *
 * Uses a union-style probe: writes 1 to a uint16_t and checks
 * whether the low byte is 1 (little-endian) or 0 (big-endian).
 * Compilers recognize this pattern and constant-fold it at -O1+.
 */
static inline bool gr_is_little_endian(void) {
    uint16_t x = 1;
    return *(uint8_t *)&x == 1;
}

/**
 * @brief Unconditionally swap bytes of a 16-bit value.
 * @param v Value to swap.
 * @return Byte-swapped value.
 */
static inline uint16_t gr_swap16(uint16_t v) {
    return (v >> 8) | (v << 8);
}

/**
 * @brief Unconditionally swap bytes of a 32-bit value.
 * @param v Value to swap.
 * @return Byte-swapped value.
 */
static inline uint32_t gr_swap32(uint32_t v) {
    return ((v >> 24) & 0x000000FF) |
           ((v >>  8) & 0x0000FF00) |
           ((v <<  8) & 0x00FF0000) |
           ((v << 24) & 0xFF000000);
}

/**
 * @brief Unconditionally swap bytes of a 64-bit value.
 * @param v Value to swap.
 * @return Byte-swapped value.
 */
static inline uint64_t gr_swap64(uint64_t v) {
    return ((v >> 56) & 0x00000000000000FFULL) |
           ((v >> 40) & 0x000000000000FF00ULL) |
           ((v >> 24) & 0x0000000000FF0000ULL) |
           ((v >>  8) & 0x00000000FF000000ULL) |
           ((v <<  8) & 0x000000FF00000000ULL) |
           ((v << 24) & 0x0000FF0000000000ULL) |
           ((v << 40) & 0x00FF000000000000ULL) |
           ((v << 56) & 0xFF00000000000000ULL);
}

/**
 * @brief Convert a host-order 16-bit value to little-endian wire format.
 * @param v Host-order value.
 * @return Little-endian value (identity on LE hosts).
 */
static inline uint16_t gr_htole16(uint16_t v) {
    return gr_is_little_endian() ? v : gr_swap16(v);
}

/**
 * @brief Convert a host-order 32-bit value to little-endian wire format.
 * @param v Host-order value.
 * @return Little-endian value (identity on LE hosts).
 */
static inline uint32_t gr_htole32(uint32_t v) {
    return gr_is_little_endian() ? v : gr_swap32(v);
}

/**
 * @brief Convert a host-order 64-bit value to little-endian wire format.
 * @param v Host-order value.
 * @return Little-endian value (identity on LE hosts).
 */
static inline uint64_t gr_htole64(uint64_t v) {
    return gr_is_little_endian() ? v : gr_swap64(v);
}

/**
 * @brief Convert a little-endian 16-bit wire value to host order.
 * @param v Little-endian value.
 * @return Host-order value (identity on LE hosts).
 */
static inline uint16_t gr_le16toh(uint16_t v) {
    return gr_is_little_endian() ? v : gr_swap16(v);
}

/**
 * @brief Convert a little-endian 32-bit wire value to host order.
 * @param v Little-endian value.
 * @return Host-order value (identity on LE hosts).
 */
static inline uint32_t gr_le32toh(uint32_t v) {
    return gr_is_little_endian() ? v : gr_swap32(v);
}

/**
 * @brief Convert a little-endian 64-bit wire value to host order.
 * @param v Little-endian value.
 * @return Host-order value (identity on LE hosts).
 */
static inline uint64_t gr_le64toh(uint64_t v) {
    return gr_is_little_endian() ? v : gr_swap64(v);
}

/** @} */ /* end endian group */

/* ═══════════════════════════════════════════════════════════════════
 * @defgroup writer Growable Write Buffer
 * @{
 *
 * A simple growable byte buffer for building serialized blobs.
 *
 * Usage:
 * @code
 *   gr_buf_t buf;
 *   gr_buf_init(&buf, 4096);       // initial capacity
 *   gr_buf_write_u32(&buf, 42);    // appends 4 bytes (LE)
 *   gr_buf_write_str(&buf, name, GR_MAX_NAME_LEN);
 *   // ... transfer buf.data / buf.len ...
 *   gr_buf_free(&buf);             // or hand off buf.data to caller
 * @endcode
 *
 * The buffer doubles in capacity each time it runs out of space.
 * Initial capacity should be a reasonable estimate to avoid early
 * reallocations — 4096 for small blobs, 65536 for full serialization.
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief Growable write buffer.
 *
 * @var gr_buf_t::data  Pointer to the allocated byte array. Owned by
 *                      the buffer; freed by gr_buf_free(). May be NULL
 *                      after gr_buf_free() or if gr_buf_init() failed.
 * @var gr_buf_t::len   Number of bytes currently written. Always <= cap.
 * @var gr_buf_t::cap   Allocated capacity in bytes.
 */
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
    bool     error;
} gr_buf_t;

/**
 * @brief Initialize a write buffer with the given initial capacity.
 *
 * Allocates the backing array via malloc(). The buffer starts empty
 * (len = 0) with the requested capacity.
 *
 * @param[out] b    Buffer to initialize. Must not be NULL.
 * @param      cap  Initial capacity in bytes. Must be > 0.
 * @return 0 on success, -1 if malloc fails.
 *
 * @note The caller must eventually call gr_buf_free() to release the
 *       backing memory, unless ownership of b->data is transferred
 *       (e.g. to the caller of gr_serialize via gr_free).
 */
int gr_buf_init(gr_buf_t *b, size_t cap);

/**
 * @brief Free the buffer's backing memory and reset all fields to zero.
 *
 * After this call, b->data is NULL and b->len / b->cap are 0.
 * Safe to call on an already-freed or zero-initialized buffer.
 *
 * @param[in,out] b  Buffer to free. Must not be NULL.
 */
void gr_buf_free(gr_buf_t *b);

/**
 * @brief Append raw bytes to the buffer.
 *
 * Grows the buffer if necessary (capacity doubles until sufficient).
 * Bytes are copied as-is with no endian conversion — use this for
 * fixed-size binary fields like keys, hashes, and peer IDs.
 *
 * @param[in,out] b     Target buffer.
 * @param[in]     data  Source bytes to append. Must not be NULL if len > 0.
 * @param         len   Number of bytes to append. May be 0.
 * @return 0 on success, -1 if realloc fails.
 */
int gr_buf_write(gr_buf_t *b, const void *data, size_t len);

/**
 * @brief Append a single byte.
 *
 * @param[in,out] b  Target buffer.
 * @param         v  Byte value to append.
 * @return 0 on success, -1 on allocation failure.
 */
int gr_buf_write_u8(gr_buf_t *b, uint8_t v);

/**
 * @brief Append a 16-bit unsigned integer in little-endian wire format.
 *
 * Converts from host byte order to little-endian before writing.
 * On little-endian hosts this is a direct memcpy of 2 bytes.
 *
 * @param[in,out] b  Target buffer.
 * @param         v  Host-order value to write.
 * @return 0 on success, -1 on allocation failure.
 */
int gr_buf_write_u16(gr_buf_t *b, uint16_t v);

/**
 * @brief Append a 32-bit unsigned integer in little-endian wire format.
 *
 * Converts from host byte order to little-endian before writing.
 * On little-endian hosts this is a direct memcpy of 4 bytes.
 *
 * @param[in,out] b  Target buffer.
 * @param         v  Host-order value to write.
 * @return 0 on success, -1 on allocation failure.
 */
int gr_buf_write_u32(gr_buf_t *b, uint32_t v);

/**
 * @brief Append a 64-bit signed integer in little-endian wire format.
 *
 * The value is cast to uint64_t for the byte-order conversion, then
 * written as 8 bytes. The signed-to-unsigned cast is lossless and
 * reversed identically on read by gr_read_i64().
 *
 * Used for all timestamp fields (unix milliseconds) and retention
 * policy values throughout the registrar.
 *
 * @param[in,out] b  Target buffer.
 * @param         v  Host-order signed value to write.
 * @return 0 on success, -1 on allocation failure.
 */
int gr_buf_write_i64(gr_buf_t *b, int64_t v);

/**
 * @brief Append a length-prefixed string.
 *
 * Wire format: [u32 byte_count] [raw bytes, no null terminator].
 *
 * The string length is determined by strnlen(s, maxlen), so the
 * output is clamped to at most maxlen bytes regardless of the
 * actual string length. This prevents buffer overreads when
 * serializing fixed-size char arrays from structs (e.g.
 * gr_peer_t.ip which is GR_MAX_IP_LEN bytes).
 *
 * @param[in,out] b       Target buffer.
 * @param[in]     s       Null-terminated string to write. Must not be NULL.
 * @param         maxlen  Maximum number of bytes to read from s.
 *                        Should match the size of the source char array
 *                        (e.g. GR_MAX_NAME_LEN, GR_MAX_IP_LEN).
 * @return 0 on success, -1 on allocation failure.
 */
int gr_buf_write_str(gr_buf_t *b, const char *s, size_t maxlen);

/** @} */ /* end writer group */

/* ═══════════════════════════════════════════════════════════════════
 * @defgroup reader Sequential Read Cursor
 * @{
 *
 * A zero-copy sequential reader over a read-only byte array.
 *
 * Usage:
 * @code
 *   gr_reader_t r = { .data = blob, .len = blob_len, .pos = 0 };
 *   uint32_t version;
 *   gr_read_u32(&r, &version);
 *   char name[128];
 *   gr_read_str(&r, name, sizeof(name));
 * @endcode
 *
 * Every read function checks bounds before advancing. If a read
 * would go past the end of the buffer, it returns -1 and the
 * reader position is unchanged — the caller can detect the error
 * and abort deserialization.
 *
 * The reader is the inverse of the writer: every gr_buf_write_*
 * function has a matching gr_read_* that decodes the same wire
 * format. Round-trip correctness (write then read recovers the
 * original value) is a critical invariant for serialization.
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief Zero-copy sequential read cursor over a byte array.
 *
 * @var gr_reader_t::data  Pointer to the source byte array. NOT owned
 *                         by the reader; must remain valid for the
 *                         reader's lifetime. Must not be NULL.
 * @var gr_reader_t::len   Total length of the source array in bytes.
 * @var gr_reader_t::pos   Current read position. Advances with each
 *                         successful read. Always <= len.
 */
typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
} gr_reader_t;

/**
 * @brief Read exactly n raw bytes from the current position.
 *
 * Copies n bytes into out and advances the position by n.
 * No endian conversion — use this for fixed-size binary fields.
 *
 * @param[in,out] r    Reader cursor.
 * @param[out]    out  Destination buffer. Must have room for n bytes.
 * @param         n    Number of bytes to read.
 * @return 0 on success, -1 if fewer than n bytes remain.
 */
int gr_read_bytes(gr_reader_t *r, void *out, size_t n);

/**
 * @brief Read a single byte.
 *
 * @param[in,out] r    Reader cursor.
 * @param[out]    out  Destination for the byte value.
 * @return 0 on success, -1 if no bytes remain.
 */
int gr_read_u8(gr_reader_t *r, uint8_t *out);

/**
 * @brief Read a 16-bit unsigned integer from little-endian wire format.
 *
 * Reads 2 bytes, converts from little-endian to host byte order.
 * On little-endian hosts this is a direct memcpy.
 *
 * @param[in,out] r    Reader cursor.
 * @param[out]    out  Destination for the host-order value.
 * @return 0 on success, -1 if fewer than 2 bytes remain.
 */
int gr_read_u16(gr_reader_t *r, uint16_t *out);

/**
 * @brief Read a 32-bit unsigned integer from little-endian wire format.
 *
 * Reads 4 bytes, converts from little-endian to host byte order.
 * On little-endian hosts this is a direct memcpy.
 *
 * @param[in,out] r    Reader cursor.
 * @param[out]    out  Destination for the host-order value.
 * @return 0 on success, -1 if fewer than 4 bytes remain.
 */
int gr_read_u32(gr_reader_t *r, uint32_t *out);

/**
 * @brief Read a 64-bit signed integer from little-endian wire format.
 *
 * Reads 8 bytes as uint64_t, converts from little-endian to host order,
 * then casts to int64_t. This is the inverse of gr_buf_write_i64().
 *
 * @param[in,out] r    Reader cursor.
 * @param[out]    out  Destination for the host-order signed value.
 * @return 0 on success, -1 if fewer than 8 bytes remain.
 */
int gr_read_i64(gr_reader_t *r, int64_t *out);

/**
 * @brief Read a length-prefixed string and null-terminate it.
 *
 * Wire format: [u32 byte_count] [raw bytes].
 *
 * Reads the u32 length prefix, validates that byte_count < maxlen
 * (to ensure room for the null terminator), copies byte_count bytes
 * into out, and appends '\\0'.
 *
 * This is the inverse of gr_buf_write_str(). The maxlen parameter
 * should match the destination buffer size (e.g. GR_MAX_NAME_LEN
 * for a char[128] field).
 *
 * @param[in,out] r       Reader cursor.
 * @param[out]    out     Destination buffer. Must have room for maxlen bytes.
 * @param         maxlen  Size of the destination buffer. The decoded
 *                        string length must be strictly less than this
 *                        (to fit the null terminator).
 * @return 0 on success, -1 if the length prefix exceeds maxlen-1
 *         or if insufficient bytes remain.
 */
int gr_read_str(gr_reader_t *r, char *out, size_t maxlen);

/** @} */ /* end reader group */

#ifdef __cplusplus
}
#endif

#endif /* GR_BUF_H */
