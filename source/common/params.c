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
        int i;

        if (id < 0 || id >= 32) {
            return -1;
        }
        st = &store->sps[id];
        st->profile_idc = s->profile_idc;
        st->chroma_format_idc = s->chroma_format_idc;
        st->separate_colour_plane_flag = s->residual_colour_transform_flag;
        st->frame_mbs_only_flag = s->frame_mbs_only_flag;
        st->log2_max_frame_num = 4 + s->log2_max_frame_num_minus4;
        st->max_frame_num = 1 << st->log2_max_frame_num;
        st->pic_order_cnt_type = s->pic_order_cnt_type;
        st->log2_max_poc_lsb = 4 + s->log2_max_pic_order_cnt_lsb_minus4;
        st->max_poc_lsb = 1 << st->log2_max_poc_lsb;
        st->has_timing = s->vui_parameters_present_flag && s->vui.timing_info_present_flag;
        st->pic_struct_present_flag = s->vui_parameters_present_flag && s->vui.pic_struct_present_flag;
        st->time_scale = (uint32_t)s->vui.time_scale;
        st->num_units_in_tick = (uint32_t)s->vui.num_units_in_tick;
        st->fixed_frame_rate_flag = s->vui.fixed_frame_rate_flag;
        st->pic_width_in_mbs_minus1 = s->pic_width_in_mbs_minus1;
        st->pic_height_in_map_units_minus1 = s->pic_height_in_map_units_minus1;
        st->bit_depth_luma_minus8 = s->bit_depth_luma_minus8;
        st->bit_depth_chroma_minus8 = s->bit_depth_chroma_minus8;
        st->frame_cropping_flag = s->frame_cropping_flag;
        st->frame_crop_left_offset = s->frame_crop_left_offset;
        st->frame_crop_right_offset = s->frame_crop_right_offset;
        st->frame_crop_top_offset = s->frame_crop_top_offset;
        st->frame_crop_bottom_offset = s->frame_crop_bottom_offset;
        /* pic_order_cnt_type == 1 (spec 7.3.2.1.1); stale values are never
         * read for other poc types, but keep the state tidy anyway. */
        if (s->pic_order_cnt_type == 1) {
            st->delta_pic_order_always_zero_flag = s->delta_pic_order_always_zero_flag;
            st->offset_for_non_ref_pic = (int32_t)s->offset_for_non_ref_pic;
            st->offset_for_top_to_bottom_field = (int32_t)s->offset_for_top_to_bottom_field;
            st->num_ref_frames_in_pic_order_cnt_cycle =
                s->num_ref_frames_in_pic_order_cnt_cycle > H264_POC1_CYCLE_MAX
                    ? H264_POC1_CYCLE_MAX
                    : s->num_ref_frames_in_pic_order_cnt_cycle;
            for (i = 0; i < st->num_ref_frames_in_pic_order_cnt_cycle; i++) {
                st->offset_for_ref_frame[i] = (int32_t)s->offset_for_ref_frame[i];
            }
        } else {
            st->delta_pic_order_always_zero_flag = 0;
            st->offset_for_non_ref_pic = 0;
            st->offset_for_top_to_bottom_field = 0;
            st->num_ref_frames_in_pic_order_cnt_cycle = 0;
        }
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
        st->entropy_coding_mode_flag = p->entropy_coding_mode_flag;
        st->redundant_pic_cnt_present_flag = p->redundant_pic_cnt_present_flag;
        st->weighted_pred_flag = p->weighted_pred_flag;
        st->weighted_bipred_idc = p->weighted_bipred_idc;
        st->num_ref_idx_l0_active_minus1 = p->num_ref_idx_l0_active_minus1;
        st->num_ref_idx_l1_active_minus1 = p->num_ref_idx_l1_active_minus1;
        st->valid = 1;
    }
    return 0;
}

/*
 * Profiles that force ref_pic_list_mvc_modification_present_flag to 1
 * (spec 7.3, inferred values). ref_pic_list_mvc_modification() is not
 * implemented, so the deep path rejects these streams.
 */
static int profile_high_family(int profile_idc)
{
    switch (profile_idc) {
    case 44:
    case 83:
    case 86:
    case 118:
    case 128:
    case 134:
    case 135:
    case 138:
    case 139:
        return 1;
    default:
        return 0;
    }
}

/* ref_pic_list_modification() for one list (spec 7.3.3.1). */
static int ref_pic_list_modification(bits_t *b)
{
    for (;;) {
        uint32_t idc = bits_read_ue(b);
        if (bits_error(b) || idc > 3) {
            return -1;
        }
        if (idc == 3) {
            return 0;
        }
        /* idc 0/1: abs_diff_pic_num_minus1; idc 2: long_term_pic_num */
        (void)bits_read_ue(b);
        if (bits_error(b)) {
            return -1;
        }
    }
}

/* One l0/l1 half of pred_weight_table() (spec 7.3.3.1). */
static int pred_weight_list(bits_t *b, uint32_t ref_count, int chroma_array_type)
{
    uint32_t i;

    for (i = 0; i < ref_count; i++) {
        int j;

        if (bits_read_u(b, 1)) {           /* luma_weight_flag */
            (void)bits_read_se(b);         /* luma_weight */
            (void)bits_read_se(b);         /* luma_offset */
        }
        if (chroma_array_type != 0 && bits_read_u(b, 1)) { /* chroma_weight_flag */
            for (j = 0; j < 2; j++) {
                (void)bits_read_se(b);     /* chroma_weight */
                (void)bits_read_se(b);     /* chroma_offset */
            }
        }
        if (bits_error(b)) {
            return -1;
        }
    }
    return 0;
}

/* dec_ref_pic_marking() (spec 7.3.3.1.1). */
static int dec_ref_pic_marking(bits_t *b, uint8_t nal_type, slice_hdr_t *out)
{
    if (nal_type == 5) {
        (void)bits_read_u(b, 1);           /* no_output_of_prior_pics_flag */
        (void)bits_read_u(b, 1);           /* long_term_reference_flag */
        return bits_error(b) ? -1 : 0;
    }
    if (bits_read_u(b, 1)) {               /* adaptive_ref_pic_marking_mode_flag */
        for (;;) {
            uint32_t mmco = bits_read_ue(b);
            if (bits_error(b) || mmco >= 7) {
                return -1;
            }
            if (mmco == 0) {
                return 0;
            }
            if (mmco == 1 || mmco == 2) {
                (void)bits_read_ue(b);     /* difference_of_pic_nums_minus1 */
            } else if (mmco == 3) {
                (void)bits_read_ue(b);     /* long_term_pic_num */
                (void)bits_read_ue(b);     /* long_term_frame_idx */
                out->unreferenced = 1;
            } else if (mmco == 4 || mmco == 6) {
                (void)bits_read_ue(b);     /* long_term_frame_idx */
            }
            /* mmco == 5: no operands */
            if (bits_error(b)) {
                return -1;
            }
        }
    }
    return bits_error(b) ? -1 : 0;
}

int slice_header_parse(const param_store_t *store, uint8_t nal_type,
                       uint8_t nal_ref_idc,
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

    if (sps->pic_order_cnt_type != 1) {
        if (bits_error(&b)) {
            return -1;
        }
        return 0;
    }

    /*
     * Deep path, pic_order_cnt_type == 1 only (spec 7.3.3 in full). The fast
     * path above stays byte-identical for every other stream. Semantics
     * cross-checked against FFmpeg decode_slice_header / pred_weight_table /
     * decode_ref_pic_list_reordering; the parse itself uses our bits.h.
     */
    if (!sps->delta_pic_order_always_zero_flag) {
        out->delta_poc0 = bits_read_se(&b);
        if (pps->bottom_field_pic_order_in_frame_present_flag && !out->field_pic_flag) {
            out->delta_poc1 = bits_read_se(&b);
        }
    }
    if (pps->redundant_pic_cnt_present_flag) {
        out->redundant_pic_cnt = bits_read_ue(&b);
    }
    if (bits_error(&b)) {
        return -1;
    }

    /* P / SP / B: ref list signalling. */
    {
        uint32_t eff_l0 = (uint32_t)pps->num_ref_idx_l0_active_minus1 + 1;
        uint32_t eff_l1 = (uint32_t)pps->num_ref_idx_l1_active_minus1 + 1;
        int overridden = 0;
        uint32_t l0 = 0, l1 = 0;

        if (out->pic_type == 'P' || out->pic_type == 'S' || out->pic_type == 'B') {
            if (profile_high_family(sps->profile_idc)) {
                return -1; /* ref_pic_list_mvc_modification() not implemented */
            }
            if (out->pic_type == 'B') {
                (void)bits_read_u(&b, 1);  /* direct_spatial_mv_pred_flag */
            }
            overridden = (int)bits_read_u(&b, 1);
            if (overridden) {
                l0 = bits_read_ue(&b);
                if (out->pic_type == 'B') {
                    l1 = bits_read_ue(&b);
                }
            }
            if (bits_error(&b)) {
                return -1;
            }
            if (ref_pic_list_modification(&b) != 0) {
                return -1;
            }
            if (out->pic_type == 'B' && ref_pic_list_modification(&b) != 0) {
                return -1;
            }
            if (overridden) {
                eff_l0 = l0 + 1;
                eff_l1 = l1 + 1;
            }
        }

        if (pps->entropy_coding_mode_flag && out->pic_type != 'I' && out->pic_type != 'X') {
            (void)bits_read_ue(&b);        /* cabac_init_idc */
        }
        out->slice_qp_delta = bits_read_se(&b);
        if (bits_error(&b)) {
            return -1;
        }

        if (((out->pic_type == 'P' || out->pic_type == 'S') && pps->weighted_pred_flag) ||
            (out->pic_type == 'B' && pps->weighted_bipred_idc == 1)) {
            int chroma_array_type;

            if (sps->profile_idc == 88 || sps->profile_idc == 244) {
                return -1; /* Extended profile pred_weight_table differs */
            }
            chroma_array_type = sps->separate_colour_plane_flag ? 0 : sps->chroma_format_idc;
            (void)bits_read_ue(&b);        /* luma_log2_weight_denom */
            if (chroma_array_type != 0) {
                (void)bits_read_ue(&b);    /* chroma_log2_weight_denom */
            }
            if (bits_error(&b)) {
                return -1;
            }
            if (pred_weight_list(&b, eff_l0, chroma_array_type) != 0) {
                return -1;
            }
            if (out->pic_type == 'B' &&
                pred_weight_list(&b, eff_l1, chroma_array_type) != 0) {
                return -1;
            }
        }
    }

    if (nal_ref_idc != 0 && dec_ref_pic_marking(&b, nal_type, out) != 0) {
        return -1;
    }
    return bits_error(&b) ? -1 : 0;
}
