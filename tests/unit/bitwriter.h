/*
 * bitwriter.h - MSB-first bit writer for synthetic RBSP buffers in unit
 * tests. Strict inverse of the reader in source/common/bits.c: bits are
 * written MSB-first into the byte buffer, Exp-Golomb codes follow
 * spec 9.1/9.1.1 (ue: lz zeros + (v+1) in lz+1 bits; se per 9.1.1 map).
 *
 * Copyright (C) 2026 h264-editing-utils contributors
 *
 * This file is part of h264-editing-utils.
 *
 * h264-editing-utils is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * h264-editing-utils is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with h264-editing-utils.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef H264EU_TEST_BITWRITER_H
#define H264EU_TEST_BITWRITER_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    uint8_t *buf;
    size_t cap;      /* capacity of buf in bytes */
    size_t bit_pos;  /* bits written so far */
    int error;       /* set once a write did not fit into cap (or bad n);
                        once set, all further writes are dropped, so an
                        overflowing test never writes past cap */
} bitwriter_t;

/* Zeroes buf[0..cap) and resets the writer. */
static inline void bw_init(bitwriter_t *bw, uint8_t *buf, size_t cap)
{
    bw->buf = buf;
    bw->cap = cap;
    bw->bit_pos = 0;
    bw->error = 0;
    memset(buf, 0, cap);
}

static inline void bw_write_bit(bitwriter_t *bw, int bit)
{
    if (bw->bit_pos >= bw->cap * 8) {
        bw->error = 1;
        return;
    }
    if (bit) {
        bw->buf[bw->bit_pos >> 3] |= (uint8_t)(1u << (7 - (bw->bit_pos & 7)));
    }
    bw->bit_pos++;
}

/* Write the low n bits of val, MSB first (1 <= n <= 32). */
static inline void bw_u(bitwriter_t *bw, uint32_t val, int n)
{
    int i;

    if (n < 1 || n > 32) {
        bw->error = 1;
        return;
    }
    for (i = n - 1; i >= 0; i--) {
        bw_write_bit(bw, (int)((val >> i) & 1u));
    }
}

/*
 * bw_finish() does NOT pad the buffer: neither rbsp_stop_one_bit nor
 * zero-alignment is appended — trailing bits belong to the RBSP under
 * construction, not to the writer (a test that needs them calls
 * bw_u(bw, 1, 1) itself). Since bw_init zeroes the buffer and the writer
 * only sets one-bits, unused low bits of a partial last byte are 0.
 * Returns the number of bytes occupied by the bits written, i.e.
 * (bit_pos + 7) / 8 — pass it straight to bits_init() for a round-trip.
 */
static inline size_t bw_finish(bitwriter_t *bw)
{
    return (bw->bit_pos + 7) / 8;
}

/* Unsigned Exp-Golomb ue(v), spec 9.1: inverse of bits_read_ue(). */
static inline void bw_ue(bitwriter_t *bw, uint32_t v)
{
    uint64_t x = (uint64_t)v + 1; /* written as lz zeros + x in lz+1 bits */
    int lz = 0;
    int z;
    int n;

    while ((x >> (lz + 1)) != 0) {
        lz++; /* lz = floor(log2(v + 1)) */
    }
    for (z = lz; z > 0; z--) {
        bw_write_bit(bw, 0);
    }
    n = lz + 1; /* x fits exactly in n bits, n <= 33 (v = UINT32_MAX) */
    while (n > 0) {
        int k = n > 32 ? 32 : n;
        n -= k;
        bw_u(bw, (uint32_t)(x >> (unsigned)n), k);
    }
}

/* Signed Exp-Golomb se(v), spec 9.1.1: inverse of bits_read_se(). */
static inline void bw_se(bitwriter_t *bw, int32_t v)
{
    int64_t x = v;
    uint32_t k = (uint32_t)((x > 0) ? 2 * x - 1 : -2 * x);

    bw_ue(bw, k);
}

static inline void bw_u8(bitwriter_t *bw, uint8_t byte)
{
    bw_u(bw, byte, 8);
}

static inline void bw_bytes(bitwriter_t *bw, const uint8_t *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        bw_u8(bw, p[i]);
    }
}

#endif /* H264EU_TEST_BITWRITER_H */
