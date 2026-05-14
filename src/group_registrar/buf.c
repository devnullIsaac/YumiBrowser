/**
 * @file buf.c
 * @brief Implementation of the growable write buffer and sequential reader.
 *
 * See buf.h for the full API documentation, wire format specification,
 * and ownership semantics.
 *
 * @section growth_strategy Growth Strategy
 *
 * The write buffer uses a doubling strategy: when a write would exceed
 * the current capacity, the buffer doubles in size until the new data
 * fits. This gives amortized O(1) append performance at the cost of
 * up to 2x memory overhead. For the registrar's use case (blobs in
 * the low KB to low MB range, allocated once per serialize/invite
 * call), this is the right tradeoff.
 *
 * @section endian_cost Endian Conversion Cost
 *
 * On little-endian hosts (x86, ARM default, RISC-V, Apple Silicon),
 * the gr_htoleNN / gr_leNNtoh calls inline to identity functions.
 * The compiler eliminates the branch entirely at -O1 or above,
 * leaving a plain memcpy for each integer write/read. On x86 this
 * typically compiles to a single MOV instruction.
 *
 * On big-endian hosts, the swap functions add a small constant cost
 * per integer (a few shifts and ORs, typically 3-5 instructions).
 * This is negligible compared to the I/O and crypto costs elsewhere
 * in the registrar.
 */

#include "buf.h"
#include <stdlib.h>

/* ═══════════════════════════════════════════════════════════════════
 *  Writer implementation
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @copydoc gr_buf_init
 */
int gr_buf_init(gr_buf_t *b, size_t cap) {
    b->data = (uint8_t *)malloc(cap);
    if (!b->data) return -1;
    b->len = 0;
    b->cap = cap;
    b->error = false;
    return 0;
}

/**
 * @copydoc gr_buf_free
 */
void gr_buf_free(gr_buf_t *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

/**
 * @brief Ensure the buffer has room for at least @p need more bytes.
 *
 * If the current free space (cap - len) is insufficient, doubles the
 * capacity repeatedly until the requirement is met. Uses realloc()
 * to resize in-place when possible.
 *
 * @param[in,out] b     Buffer to grow.
 * @param         need  Number of additional bytes required.
 * @return 0 on success (enough space available or realloc succeeded),
 *         -1 if realloc fails (buffer is unchanged).
 *
 * @warning If realloc fails, the existing buffer contents are preserved
 *          (realloc does not free the old pointer on failure). The caller
 *          can still read existing data or call gr_buf_free().
 */
static int buf_ensure(gr_buf_t *b, size_t need) {
    if (b->error) return -1;
    if (b->len + need <= b->cap) return 0;
    size_t newcap = b->cap * 2;
    while (newcap < b->len + need) newcap *= 2;
    uint8_t *p = (uint8_t *)realloc(b->data, newcap);
    if (!p) { b->error = true; return -1; }
    b->data = p;
    b->cap = newcap;
    return 0;
}

/**
 * @copydoc gr_buf_write
 */
int gr_buf_write(gr_buf_t *b, const void *data, size_t len) {
    if (buf_ensure(b, len) != 0) return -1;
    memcpy(b->data + b->len, data, len);
    b->len += len;
    return 0;
}

/**
 * @copydoc gr_buf_write_u8
 */
int gr_buf_write_u8(gr_buf_t *b, uint8_t v) {
    return gr_buf_write(b, &v, 1);
}

/**
 * @copydoc gr_buf_write_u16
 *
 * Stores the value in little-endian byte order via gr_htole16().
 * On LE hosts this is a no-op cast; the memcpy in gr_buf_write()
 * copies the native bytes directly.
 */
int gr_buf_write_u16(gr_buf_t *b, uint16_t v) {
    uint16_t le = gr_htole16(v);
    return gr_buf_write(b, &le, 2);
}

/**
 * @copydoc gr_buf_write_u32
 *
 * Stores the value in little-endian byte order via gr_htole32().
 */
int gr_buf_write_u32(gr_buf_t *b, uint32_t v) {
    uint32_t le = gr_htole32(v);
    return gr_buf_write(b, &le, 4);
}

/**
 * @copydoc gr_buf_write_i64
 *
 * Casts to uint64_t for the endian conversion (lossless for all
 * int64_t values via two's complement), then stores as 8 LE bytes.
 */
int gr_buf_write_i64(gr_buf_t *b, int64_t v) {
    uint64_t le = gr_htole64((uint64_t)v);
    return gr_buf_write(b, &le, 8);
}

/**
 * @copydoc gr_buf_write_str
 *
 * The string is measured with strnlen() clamped to maxlen, so even
 * if the source char array is not null-terminated within maxlen
 * bytes, the write is bounded and safe.
 *
 * Wire layout:
 * @code
 *   [4 bytes: u32 LE string length] [N bytes: raw string data]
 * @endcode
 */
int gr_buf_write_str(gr_buf_t *b, const char *s, size_t maxlen) {
    size_t slen = strnlen(s, maxlen);
    if (gr_buf_write_u32(b, (uint32_t)slen) != 0) return -1;
    return gr_buf_write(b, s, slen);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Reader implementation
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @copydoc gr_read_bytes
 *
 * Bounds check: if pos + n > len, returns -1 without advancing.
 * This is the foundation for all other read functions — every
 * typed read calls this and inherits the bounds safety.
 */
int gr_read_bytes(gr_reader_t *r, void *out, size_t n) {
    if (r->pos + n > r->len) return -1;
    memcpy(out, r->data + r->pos, n);
    r->pos += n;
    return 0;
}

/**
 * @copydoc gr_read_u8
 */
int gr_read_u8(gr_reader_t *r, uint8_t *out) {
    return gr_read_bytes(r, out, 1);
}

/**
 * @copydoc gr_read_u16
 *
 * Reads 2 bytes in little-endian wire order and converts to host
 * order via gr_le16toh(). On LE hosts the conversion is elided.
 */
int gr_read_u16(gr_reader_t *r, uint16_t *out) {
    uint16_t le;
    if (gr_read_bytes(r, &le, 2) != 0) return -1;
    *out = gr_le16toh(le);
    return 0;
}

/**
 * @copydoc gr_read_u32
 *
 * Reads 4 bytes in little-endian wire order and converts to host
 * order via gr_le32toh().
 */
int gr_read_u32(gr_reader_t *r, uint32_t *out) {
    uint32_t le;
    if (gr_read_bytes(r, &le, 4) != 0) return -1;
    *out = gr_le32toh(le);
    return 0;
}

/**
 * @copydoc gr_read_i64
 *
 * Reads 8 bytes as uint64_t in LE wire order, converts to host
 * order, then casts back to int64_t. The uint64_t→int64_t cast
 * is the exact inverse of the int64_t→uint64_t cast in
 * gr_buf_write_i64(), preserving the original signed value.
 */
int gr_read_i64(gr_reader_t *r, int64_t *out) {
    uint64_t le;
    if (gr_read_bytes(r, &le, 8) != 0) return -1;
    *out = (int64_t)gr_le64toh(le);
    return 0;
}

/**
 * @copydoc gr_read_str
 *
 * Decoding steps:
 *  1. Read a u32 length prefix (byte count of the string, no null).
 *  2. Validate: slen < maxlen (strict less-than, so out[slen] = '\\0' fits).
 *  3. Read slen raw bytes into out.
 *  4. Null-terminate: out[slen] = '\\0'.
 *
 * If validation fails (string too long for the destination buffer)
 * or if there aren't enough bytes in the reader, returns -1.
 * The reader position is partially advanced only in the case where
 * the u32 prefix was read successfully but the string body failed —
 * callers should treat any -1 return as a fatal deserialization error.
 */
int gr_read_str(gr_reader_t *r, char *out, size_t maxlen) {
    uint32_t slen;
    if (gr_read_u32(r, &slen) != 0) return -1;
    if (slen >= maxlen) return -1;
    if (gr_read_bytes(r, out, slen) != 0) return -1;
    out[slen] = '\0';
    return 0;
}
