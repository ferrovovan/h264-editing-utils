/*
 * bits.h - MSB-first bit reader with Exp-Golomb support over a byte buffer.
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

#ifndef H264EU_BITS_H
#define H264EU_BITS_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    const uint8_t *data;
    size_t size;     /* size of data in bytes */
    size_t bit_pos;  /* current position in bits from the start of data */
    int error;       /* set to 1 once a read exceeded the buffer */
} bits_t;

/* Initialize the reader over [data, data+size). */
void bits_init(bits_t *b, const uint8_t *data, size_t size);

/* Read n bits (1 <= n <= 32), MSB first. Returns 0 and sets error on overrun. */
uint32_t bits_read_u(bits_t *b, int n);

/* Unsigned Exp-Golomb (ue(v), spec 9.1). Sets error on malformed/overrun data. */
uint32_t bits_read_ue(bits_t *b);

/* Signed Exp-Golomb (se(v), spec 9.1.1). Sets error on malformed/overrun data. */
int32_t bits_read_se(bits_t *b);

/* Nonzero if any read went past the end of the buffer or the data was invalid. */
int bits_error(const bits_t *b);

#endif /* H264EU_BITS_H */
