/*
 * annexb.c - Annex B byte stream scanning and writing.
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

#include <stdlib.h>

#include "annexb.h"

/*
 * Find the next occurrence of the 3-byte start code prefix 00 00 01 at or
 * after index `from`. Returns 1 and sets *pos to the index of the first 0x00,
 * or 0 if not found.
 */
static int find_start_code(const uint8_t *buf, size_t size, size_t from, size_t *pos)
{
    size_t i;

    if (from + 2 >= size) {
        return 0;
    }
    for (i = from; i + 2 < size; i++) {
        if (buf[i] == 0x00 && buf[i + 1] == 0x00 && buf[i + 2] == 0x01) {
            *pos = i;
            return 1;
        }
    }
    return 0;
}

int annexb_scan(const uint8_t *buf, size_t size, nal_list_t *out)
{
    nal_t *items = NULL;
    size_t count = 0;
    size_t cap = 0;
    size_t i = 0;

    out->items = NULL;
    out->count = 0;

    while (i + 2 < size) {
        size_t p;
        size_t sc_end;
        size_t q;
        size_t end;
        uint8_t hdr;
        nal_t *dst;

        if (!find_start_code(buf, size, i, &p)) {
            break;
        }
        sc_end = p + 3; /* index of the nal header byte (or past EOF) */
        if (sc_end >= size) {
            break;
        }
        if (buf[sc_end] == 0x00) {
            /* zero_byte before the nal: no nal unit here, keep scanning. */
            i = sc_end + 1 - 2;
            continue;
        }
        hdr = buf[sc_end];
        if ((hdr & 0x1f) == 0) {
            /* unspecified nal_unit_type 0: skip */
            i = sc_end + 1;
            continue;
        }

        if (find_start_code(buf, size, sc_end + 1, &q)) {
            end = q;
        } else {
            end = size;
        }
        /* strip trailing zero_8bits */
        while (end > sc_end + 1 && buf[end - 1] == 0x00) {
            end--;
        }

        if (count == cap) {
            size_t ncap = cap ? cap * 2 : 64;
            nal_t *ni = realloc(items, ncap * sizeof(*ni));
            if (!ni) {
                free(items);
                return -1;
            }
            items = ni;
            cap = ncap;
        }
        dst = &items[count++];
        dst->type = (uint8_t)(hdr & 0x1f);
        dst->ref_idc = (uint8_t)((hdr >> 5) & 0x03);
        dst->off = sc_end;
        dst->size = end - sc_end;

        i = (end > sc_end + 1) ? end : sc_end + 1;
    }

    out->items = items;
    out->count = count;
    return 0;
}

void annexb_list_free(nal_list_t *list)
{
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

uint8_t *annexb_rbsp(const uint8_t *buf, const nal_t *nal, size_t *rbsp_size)
{
    uint8_t *out;
    size_t limit;
    size_t i;
    size_t w;

    if (nal == NULL || nal->size == 0) {
        return NULL;
    }
    out = malloc(nal->size);
    if (!out) {
        return NULL;
    }
    out[0] = buf[nal->off];
    w = 1;
    limit = nal->off + nal->size;
    for (i = nal->off + 1; i < limit; i++) {
        uint8_t cur = buf[i];
        if (cur == 0x03 && w >= 2 && out[w - 1] == 0x00 && out[w - 2] == 0x00 &&
            i + 1 < limit && buf[i + 1] <= 0x03) {
            continue; /* emulation_prevention_three_byte */
        }
        out[w++] = cur;
    }
    *rbsp_size = w;
    return out;
}

int annexb_write_nal(FILE *out, const uint8_t *buf, const nal_t *nal)
{
    static const uint8_t start_code[4] = { 0x00, 0x00, 0x00, 0x01 };

    if (fwrite(start_code, 1, sizeof(start_code), out) != sizeof(start_code)) {
        return -1;
    }
    if (fwrite(buf + nal->off, 1, nal->size, out) != nal->size) {
        return -1;
    }
    return 0;
}
