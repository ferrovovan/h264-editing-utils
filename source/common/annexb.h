/*
 * annexb.h - Annex B byte stream scanning and writing.
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

#ifndef H264EU_ANNEXB_H
#define H264EU_ANNEXB_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* nal_unit_type values used by this project (spec table 7-1). */
enum {
    NAL_SLICE       = 1,   /* coded slice of a non-IDR picture */
    NAL_SLICE_DPA   = 2,
    NAL_SLICE_DPB   = 3,
    NAL_SLICE_DPC   = 4,
    NAL_IDR         = 5,   /* coded slice of an IDR picture */
    NAL_SEI         = 6,
    NAL_SPS         = 7,
    NAL_PPS         = 8,
    NAL_AUD         = 9,
    NAL_END_SEQ     = 10,
    NAL_END_STREAM  = 11,
    NAL_FILLER      = 12,
    NAL_PREFIX      = 14,
    NAL_SUBSET_SPS  = 15
};

typedef struct {
    uint8_t type;    /* nal_unit_type (5 bits of the nal header byte) */
    uint8_t ref_idc; /* nal_ref_idc (2 bits of the nal header byte) */
    size_t off;      /* file offset of the nal header byte (first byte after the start code) */
    size_t size;     /* nal header byte + EBSP size in bytes, trailing zero bytes excluded */
} nal_t;

typedef struct {
    nal_t *items;
    size_t count;
} nal_list_t;

/*
 * Scan the whole Annex B buffer and record every NAL unit.
 * A NAL unit runs from the start code up to the next start code (or EOF);
 * trailing zero_8bits are excluded from size. zero_byte before a nal
 * (a 0x00 immediately after a start code) is skipped. NALs whose header
 * byte is missing (empty payload) are skipped.
 * Returns 0 on success, -1 on allocation failure. The caller keeps owning buf.
 */
int annexb_scan(const uint8_t *buf, size_t size, nal_list_t *out);
void annexb_list_free(nal_list_t *list);

/*
 * Copy the NAL payload (header byte + EBSP, as stored in the file) of `nal`
 * from `buf` and remove emulation_prevention_three_byte values (spec 7.4.1),
 * producing the RBSP. Returns a malloc'd buffer the caller must free, or NULL
 * on allocation failure. *rbsp_size receives the output size.
 */
uint8_t *annexb_rbsp(const uint8_t *buf, const nal_t *nal, size_t *rbsp_size);

/*
 * Write one NAL from `buf` to `out`, prefixed with a 4-byte start code
 * (00 00 00 01). The NAL payload itself is written byte-for-byte.
 * Returns 0 on success, -1 on write error.
 */
int annexb_write_nal(FILE *out, const uint8_t *buf, const nal_t *nal);

#endif /* H264EU_ANNEXB_H */
