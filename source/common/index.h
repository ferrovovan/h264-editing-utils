/*
 * index.h - map NAL units of an Annex B stream onto pictures (frames).
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

#ifndef H264EU_INDEX_H
#define H264EU_INDEX_H

#include <stdint.h>
#include <stddef.h>

#include "annexb.h"
#include "params.h"

/*
 * One picture in decoding order: a progressive/MBAFF frame, or a field pair
 * (top + bottom field, spec 8.21) — a field pair is ONE picture, the unit of
 * display order and of cutting. An orphan field (the other half of the pair
 * is missing, e.g. a truncated stream) is a picture of its own.
 */
typedef struct {
    size_t nal_first, nal_last;  /* inclusive NAL range (both fields of a pair) */
    int is_idr;
    char pic_type;               /* 'I','P','B','S','X' or '?' if unparsable */
    uint32_t frame_num;
    long long poc;               /* PicOrderCnt of the picture: the TopFieldOrderCnt
                                  * of a field pair (8.21.1), reset at every IDR;
                                  * -1 if unknown */
    long long display_index;     /* position in display order; -1 if unknown */
    int parse_error;             /* slice header parse failed for this picture */
    /* Open GOP (recovery point SEI, spec D.1.7): */
    int recovery_point;          /* AU carries a recovery_point SEI */
    uint32_t recovery_frame_cnt;
    int exact_match_flag;
    int broken_link_flag;
    int usable_anchor;           /* IDR, or recovery usable to cut from:
                                  * recovery_frame_cnt == 0 && exact_match
                                  * && !broken_link */
    int leading_picture;         /* non-ref AU right after an anchor with
                                  * poc < poc(anchor): decodes with broken
                                  * references when cut starts at the anchor */
    /* SEI picture timing (E.1.2, cnt_type 0 / n_90kHz 0), unwrapped: */
    int has_clock_ts;
    uint64_t clock_ts_90k;       /* 1/90000 s units, day-wraps unwrapped */
} frame_t;

typedef struct {
    const uint8_t *buf;          /* the whole file buffer, owned by the caller */
    size_t size;
    nal_list_t nals;
    param_store_t params;        /* final parameter set state after the scan */
    frame_t *frames;             /* pictures, in decoding order */
    size_t frame_count;          /* picture count (a field pair counts once) */
    int poc_supported;           /* 0 if an unknown pic_order_cnt_type was seen */
    int has_fields;              /* field_pic_flag seen (a field pair is still one
                                  * picture; --smart refuses such streams) */
    int timing_valid;            /* first parsed SPS carried VUI timing */
    uint32_t time_scale;
    uint32_t num_units_in_tick;
    int fixed_frame_rate;
    int clock_timing;            /* frame_count > 0 and every picture has_clock_ts */
} stream_index_t;

/*
 * Scan the stream, group slices into pictures, compute POC and display order.
 * Slices are grouped into the current field unless (spec 7.4.1.2.4):
 *   - the NAL is an IDR whose idr_pic_id differs from the current IDR field, or
 *   - first_mb_in_slice == 0, or
 *   - frame_num / field flags / nal_ref_idc / pps_id differ from the current
 *     field, or the NAL is type 5.
 * A bottom field whose top field is the current one (same frame_num / pps_id /
 * nal_ref_idc / field_pic_flag, picture without a bottom field yet; IDR pairs
 * by the same rule through the shared idr_pic_id) continues the SAME picture
 * (8.21.1), so a field pair is one frame_t whose NAL range spans both fields.
 * POC type 0 keeps its wrap state per field parity (8.21.1), the picture POC
 * is the top field's. display_index is computed per IDR segment (closed-GOP
 * assumption) by sorting on (poc, decoding order). Unparsable slice NALs form
 * their own picture with pic_type '?', parse_error = 1.
 * Returns 0 on success (even for partially unparsable streams), -1 on
 * allocation failure.
 */
int index_build(stream_index_t *idx, const uint8_t *buf, size_t size);
void index_free(stream_index_t *idx);

#endif /* H264EU_INDEX_H */
