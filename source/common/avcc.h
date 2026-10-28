/*
 * avcc.h - AVCDecoderConfigurationRecord (ISO/IEC 14496-15) parse/build and
 * the length-prefixed ("AVCC") <-> Annex B byte stream pipeline.
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

#ifndef H264EU_AVCC_H
#define H264EU_AVCC_H

#include <stdint.h>
#include <stddef.h>

/*
 * Storage cap of the SPS/PPS lists. The 14496-15 count fields are u5
 * (0..31); records with more sets than this cap are rejected by
 * avcc_parse as unsupported (real records never approach even 16).
 */
#define AVCC_MAX_SETS 16

/*
 * One parsed AVCDecoderConfigurationRecord (14496-15 5.3.3.1.2).
 *
 * The SPS/PPS payloads are byte-for-byte copies of the NAL units as stored
 * in the record: nal header byte + EBSP (emulation_prevention_three_bytes
 * intact - NOT unescaped), without a start code. All payload buffers are
 * malloc'd and owned by the struct; avcc_config_free() releases them.
 */
typedef struct {
    uint8_t profile_idc;           /* AVCProfileIndication          */
    uint8_t profile_compat;        /* profile_compatibility         */
    uint8_t level_idc;             /* AVCLevelIndication            */
    uint8_t length_size_minus_one; /* 0..3, NAL length field = 1..4 bytes */
    /* Extended tail (High-family profiles, 14496-15:2013 revision);
     * zero for other profiles: */
    uint8_t chroma_format_idc;
    uint8_t bit_depth_luma_minus8;
    uint8_t bit_depth_chroma_minus8;
    /*
     * 1 when the record carried the extended tail. avcc_parse sets it
     * exactly when profile_idc belongs to the High family (the extended
     * tail is mandatory for those profiles and forbidden for the rest,
     * see avcc_parse); informational - avcc_build decides on the profile,
     * not on this flag.
     */
    int has_ext_fields;
    uint8_t *sps[AVCC_MAX_SETS];
    size_t sps_len[AVCC_MAX_SETS];
    int sps_count;
    uint8_t *pps[AVCC_MAX_SETS];
    size_t pps_len[AVCC_MAX_SETS];
    int pps_count;
} avcc_config_t;

/*
 * Parse an AVCDecoderConfigurationRecord from buf[0..size). Returns 0 and
 * fills `out` (payloads are malloc'd copies) or -1 on malformed input.
 *
 * Syntax (14496-15 5.3.3.1.2) and the project's reading of it:
 *  - configurationVersion must be 1;
 *  - byte 4: the 6 reserved bits must be 0b111111, lengthSizeMinusOne is
 *    the low 2 bits;
 *  - numOfSequenceParameterSets is byte 5 & 0x1F; the top 3 bits are
 *    reserved ('111' in the spec text) but NOT enforced - muxers vary;
 *  - each SPS/PPS is u16 length + that many bytes; a zero-length entry or
 *    a length reaching past the end of the buffer is an error. A count of
 *    0 is allowed (the grammar permits empty lists; e.g. parameter sets
 *    signalled in-band);
 *  - counts above AVCC_MAX_SETS are rejected as unsupported;
 *  - for the High-family profiles {44, 83, 86, 100, 110, 118, 122, 128,
 *    134, 135, 138, 139, 144, 244} the extended tail
 *    (chroma_format_idc, bit_depth_luma_minus8, bit_depth_chroma_minus8,
 *    numOfSequenceParameterSetExt + the ext SPS list) is MANDATORY; a
 *    record of such a profile that stops after the PPS list is truncated
 *    -> -1. The ext SPS payloads are length-checked but not stored
 *    (subset SPSes matter only for parsing, not for the byte pipeline);
 *  - for all other profiles the record must END right after the PPS list
 *    (no ext tail, no trailing garbage);
 *  - reserved bits inside the extended tail are not enforced either; the
 *    3-bit value fields are extracted from the low bits of their bytes.
 *
 * On error everything allocated for `out` is released and `out` ends in the
 * empty (freeable) state. `out` must be zeroed or freshly parsed.
 */
int avcc_parse(const uint8_t *buf, size_t size, avcc_config_t *out);

/*
 * Serialize `c` per the same grammar; `out`/`out_size` receive a malloc'd
 * buffer the caller must free. For a High-family profile_idc the extended
 * tail is written (has_ext_fields is ignored); for the rest it is not.
 * The ext tail is written with the canonical reserved-bit patterns and
 * numOfSequenceParameterSetExt = 0: ext SPS payloads are not kept by
 * avcc_parse, so building a parsed record normalizes an ext SPS list to
 * empty (the round-trip property below covers what the struct can carry).
 * 0 on success, -1 on invalid input (count outside [0, AVCC_MAX_SETS],
 * missing/empty entry with count > 0, entry longer than 65535 bytes) or
 * allocation failure.
 * avcc_build -> avcc_parse -> avcc_build round-trips byte-identically for
 * every input avcc_build accepts, and avcc_build(avcc_parse(rec)) is
 * byte-identical to `rec` for every record avcc_parse accepts whose ext
 * SPS list is empty.
 */
int avcc_build(const avcc_config_t *c, uint8_t **out, size_t *out_size);

/*
 * Length-prefixed stream -> Annex B: every NAL is prefixed with a 4-byte
 * start code (00 00 00 01) and copied byte-for-byte (trapping 6 applies to
 * the other direction only - payloads here pass through untouched).
 * nal_len is the length field width in bytes (2..4), big-endian lengths.
 * A truncated final NAL (length past the end of the buffer) is an error.
 * Returns 0 with a malloc'd *out, or -1 on malformed input / OOM.
 */
int avcc_to_annexb(const uint8_t *in, size_t in_size, unsigned nal_len,
                   uint8_t **out, size_t *out_size);

/*
 * Annex B -> length-prefixed stream: annexb_scan (start codes of any
 * length, trailing zero_8bits dropped) then u(nal_len*8) length + payload.
 * nal_len is 2..4; a NAL payload that does not fit the width is an error.
 * The output round-tripped through avcc_to_annexb equals the Annex B input
 * normalized to 4-byte start codes without trailing zero_8bits.
 * Returns 0 with a malloc'd *out, or -1 on malformed input / OOM.
 */
int annexb_to_avcc(const uint8_t *in, size_t in_size, unsigned nal_len,
                   uint8_t **out, size_t *out_size);

void avcc_config_free(avcc_config_t *cfg);

#endif /* H264EU_AVCC_H */
