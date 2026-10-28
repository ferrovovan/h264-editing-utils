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

/* One picture (frame), in decoding order. */
typedef struct {
    size_t nal_first, nal_last;  /* inclusive range of NALs belonging to this frame */
    int is_idr;
    char pic_type;               /* 'I','P','B','S','X' or '?' if unparsable */
    uint32_t frame_num;
    long long poc;               /* PicOrderCnt, reset at every IDR; -1 if unknown */
    long long display_index;     /* position in display order; -1 if unknown */
    int parse_error;             /* slice header parse failed for this frame */
} frame_t;

typedef struct {
    const uint8_t *buf;          /* the whole file buffer, owned by the caller */
    size_t size;
    nal_list_t nals;
    param_store_t params;        /* final parameter set state after the scan */
    frame_t *frames;
    size_t frame_count;
    int poc_supported;           /* 0 if pic_order_cnt_type == 1 anywhere */
    int has_fields;              /* interlaced content seen (fields counted as frames) */
    int timing_valid;            /* first parsed SPS carried VUI timing */
    uint32_t time_scale;
    uint32_t num_units_in_tick;
    int fixed_frame_rate;
} stream_index_t;

/*
 * Scan the stream, group slices into frames, compute POC and display order.
 * Slices are grouped into the current frame unless (spec 7.4.1.2.4):
 *   - the NAL is an IDR whose idr_pic_id differs from the previous IDR, or
 *   - first_mb_in_slice == 0, or
 *   - frame_num / field flags / nal_ref_idc / pps_id differ from the current
 *     frame, or the NAL is type 5.
 * display_index is computed per IDR segment (closed-GOP assumption) by
 * sorting on (poc, decoding order). Unparsable slice NALs form their own
 * frame with pic_type '?', parse_error = 1.
 * Returns 0 on success (even for partially unparsable streams), -1 on
 * allocation failure.
 */
int index_build(stream_index_t *idx, const uint8_t *buf, size_t size);
void index_free(stream_index_t *idx);

#endif /* H264EU_INDEX_H */
