/*
 * params.c - active SPS/PPS state and H.264 slice header parsing.
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
#include <string.h>

#include "bits.h"
#include "params.h"

/*
 * h264_stream.h defines its own nal_t which clashes with ours (annexb.h).
 * Neither header may be edited, so rename theirs within this file only.
 */
#define nal_t h264bitstream_nal_t_
#include <h264_stream.h>
#undef nal_t

int param_store_init(param_store_t *store)
{
    memset(store, 0, sizeof(*store));
    store->scratch = h264_new();
    return store->scratch ? 0 : -1;
}

void param_store_free(param_store_t *store)
{
    if (store->scratch) {
        h264_free((h264_stream_t *)store->scratch);
        store->scratch = NULL;
    }
}

int params_feed_nal(param_store_t *store, const uint8_t *buf, const nal_t *nal)
{
    h264_stream_t *h = (h264_stream_t *)store->scratch;
    uint8_t *tmp;
    int rc;

    if (nal->type != NAL_SPS && nal->type != NAL_PPS) {
        return 0;
    }
    if (!h) {
        return -1;
    }
    tmp = malloc(nal->size ? nal->size : 1);
    if (!tmp) {
        return -1;
    }
    memcpy(tmp, buf + nal->off, nal->size);
    rc = read_nal_unit(h, tmp, (int)nal->size);
    free(tmp);
    if (rc < 0) {
        return -1;
    }

    if (nal->type == NAL_SPS) {
        sps_t *s = h->sps;
        sps_state_t *st;
        int id = s->seq_parameter_set_id;

        if (id < 0 || id >= 32) {
            return -1;
        }
        st = &store->sps[id];
        st->chroma_format_idc = s->chroma_format_idc;
        st->separate_colour_plane_flag = s->residual_colour_transform_flag;
        st->frame_mbs_only_flag = s->frame_mbs_only_flag;
        st->log2_max_frame_num = 4 + s->log2_max_frame_num_minus4;
        st->max_frame_num = 1 << st->log2_max_frame_num;
        st->pic_order_cnt_type = s->pic_order_cnt_type;
        st->log2_max_poc_lsb = 4 + s->log2_max_pic_order_cnt_lsb_minus4;
        st->max_poc_lsb = 1 << st->log2_max_poc_lsb;
        st->has_timing = s->vui_parameters_present_flag && s->vui.timing_info_present_flag;
        st->time_scale = (uint32_t)s->vui.time_scale;
        st->num_units_in_tick = (uint32_t)s->vui.num_units_in_tick;
        st->fixed_frame_rate_flag = s->vui.fixed_frame_rate_flag;
        st->valid = 1;
    } else {
        pps_t *p = h->pps;
        pps_state_t *st;
        int id = p->pic_parameter_set_id;

        if (id < 0 || id >= 256) {
            return -1;
        }
        st = &store->pps[id];
        st->sps_id = p->seq_parameter_set_id;
        st->bottom_field_pic_order_in_frame_present_flag = p->pic_order_present_flag;
        st->num_slice_groups_minus1 = p->num_slice_groups_minus1;
        st->valid = 1;
    }
    return 0;
}

int slice_header_parse(const param_store_t *store, uint8_t nal_type,
                       const uint8_t *rbsp_after_nal_header, size_t size,
                       slice_hdr_t *out)
{
    static const char pic_types[5] = { 'P', 'B', 'I', 'S', 'X' };
    bits_t b;
    const pps_state_t *pps;
    const sps_state_t *sps;

    memset(out, 0, sizeof(*out));
    bits_init(&b, rbsp_after_nal_header, size);

    out->first_mb_in_slice = bits_read_ue(&b);
    out->slice_type = bits_read_ue(&b);
    out->pps_id = bits_read_ue(&b);
    if (bits_error(&b)) {
        return -1;
    }
    if (out->pps_id >= 256 || !store->pps[out->pps_id].valid) {
        return -1;
    }
    pps = &store->pps[out->pps_id];
    if (pps->sps_id < 0 || pps->sps_id >= 32 || !store->sps[pps->sps_id].valid) {
        return -1;
    }
    sps = &store->sps[pps->sps_id];

    if (sps->separate_colour_plane_flag) {
        bits_read_u(&b, 2);
    }
    /*
     * Deviation from the task spec (verified against x264 slice_header_write,
     * FFmpeg decode_slice_header and h264bitstream): encoders write frame_num
     * (and the field flags) for IDR slices as well, so it must be read here
     * for every slice type or the rest of the header would be misparsed.
     */
    out->frame_num = bits_read_u(&b, sps->log2_max_frame_num);
    if (!sps->frame_mbs_only_flag) {
        out->field_pic_flag = (int)bits_read_u(&b, 1);
        if (out->field_pic_flag) {
            out->bottom_field_flag = (int)bits_read_u(&b, 1);
        }
    }
    if (nal_type == 5) {
        out->idr_pic_id = bits_read_ue(&b);
    }
    if (sps->pic_order_cnt_type == 0) {
        out->poc_lsb = bits_read_u(&b, sps->log2_max_poc_lsb);
    }
    out->pic_type = pic_types[out->slice_type % 5];

    if (bits_error(&b)) {
        return -1;
    }
    return 0;
}
