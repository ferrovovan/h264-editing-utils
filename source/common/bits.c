/*
 * bits.c - MSB-first bit reader with Exp-Golomb support over a byte buffer.
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

#include "bits.h"

void bits_init(bits_t *b, const uint8_t *data, size_t size)
{
    b->data = data;
    b->size = size;
    b->bit_pos = 0;
    b->error = 0;
}

uint32_t bits_read_u(bits_t *b, int n)
{
    uint32_t val = 0;
    size_t total_bits;
    int i;

    if (n < 1 || n > 32) {
        b->error = 1;
        return 0;
    }
    total_bits = b->size * 8;
    if (b->bit_pos > total_bits || (size_t)n > total_bits - b->bit_pos) {
        b->error = 1;
        return 0;
    }
    for (i = 0; i < n; i++) {
        size_t pos = b->bit_pos + (size_t)i;
        uint32_t bit = (uint32_t)((b->data[pos >> 3] >> (7 - (pos & 7))) & 1u);
        val = (val << 1) | bit;
    }
    b->bit_pos += (size_t)n;
    return val;
}

uint32_t bits_read_ue(bits_t *b)
{
    uint64_t val;
    int lz = 0;

    while (bits_read_u(b, 1) == 0) {
        if (b->error) {
            return 0;
        }
        lz++;
        if (lz > 32) {
            b->error = 1;
            return 0;
        }
    }
    if (lz == 0) {
        return 0;
    }
    val = (uint64_t)bits_read_u(b, lz);
    if (b->error) {
        return 0;
    }
    val += ((uint64_t)1 << lz) - 1;
    if (val > UINT32_MAX) {
        b->error = 1;
        return 0;
    }
    return (uint32_t)val;
}

int32_t bits_read_se(bits_t *b)
{
    uint32_t k = bits_read_ue(b);

    if (b->error) {
        return 0;
    }
    if (k & 1u) {
        return (int32_t)((k + 1) / 2);
    }
    return -(int32_t)(k / 2);
}

int bits_error(const bits_t *b)
{
    return b->error != 0;
}
