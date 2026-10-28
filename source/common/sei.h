/*
 * sei.h - SEI message iteration and the payload parsers we consume.
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

#ifndef H264EU_SEI_H
#define H264EU_SEI_H

#include <stdint.h>
#include <stddef.h>

/* Payload types consumed by this project (spec D.1, table D-1). */
enum {
    SEI_PAYLOAD_BUFFERING_PERIOD = 0,
    SEI_PAYLOAD_PICTURE_TIMING   = 1,
    SEI_PAYLOAD_RECOVERY_POINT   = 6
};

/* Iterates sei_message() syntax inside one SEI NAL RBSP. */
typedef struct {
    const uint8_t *rbsp;   /* RBSP AFTER the nal header byte */
    size_t size;
    size_t pos;            /* private: cursor */
} sei_iter_t;

typedef struct {
    uint32_t type;         /* payloadType */
    const uint8_t *payload;
    size_t size;           /* payloadSize bytes */
} sei_msg_t;

void sei_iter_init(sei_iter_t *it, const uint8_t *rbsp, size_t size);

/* Next message. Returns 1 while messages remain, 0 at the end of the RBSP
 * or on malformed data (iteration simply stops; never a crash). */
int sei_iter_next(sei_iter_t *it, sei_msg_t *out);

/* sei_recovery_point_payload (spec D.1.7 in the 2014 edition, D.1.8 later),
 * payloadType 6. 0 on success. */
typedef struct {
    uint32_t recovery_frame_cnt;
    int exact_match_flag;
    int broken_link_flag;
    uint32_t changing_slice_group_idc;
} sei_recovery_point_t;

int sei_parse_recovery_point(const sei_msg_t *msg, sei_recovery_point_t *out);

/*
 * sei_picture_timing_payload (spec E.1.2 of the 2014 edition; later editions
 * replaced the clock_timestamp syntax with SMPTE timecode - we implement the
 * classic clock_timestamp variant that x264/ATSC-style encoders write).
 * `pic_struct_present` comes from the active SPS VUI
 * (sps_state_t.pic_struct_present_flag).
 *
 * Supported for time base purposes: first clock timestamp with
 * cnt_type == 0 and n_90kHz_flag == 0 -> clock_ts_90k, absolute value in
 * 1/90000 s units:
 *   clock_ts_90k = full_timestamp_u24
 *                + 90000 * (hours * 3600 + minutes * 60 + seconds)
 * (semantics D.2; one day period, the CALLER unwraps the wrap across
 * pictures).
 * Any other combination (cnt_type 1/2/3, n_90kHz_flag == 1) parses fine but
 * sets supported = 0.
 * Returns 0 when the payload is syntactically OK, -1 on malformed data.
 */
typedef struct {
    uint32_t pic_struct;
    int pic_struct_present;
    uint32_t cnt_type;
    int n_90kHz_flag;
    int has_clock_timestamp;   /* first clock_timestamp_flag */
    int supported;             /* clock ts usable as time base (see above) */
    uint64_t clock_ts_90k;     /* only meaningful when supported */
} sei_picture_timing_t;

int sei_parse_picture_timing(const sei_msg_t *msg, int pic_struct_present,
                             sei_picture_timing_t *out);

#endif /* H264EU_SEI_H */
