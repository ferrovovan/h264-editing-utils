/*
 * params.h - active SPS/PPS state and H.264 slice header parsing.
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

#ifndef H264EU_PARAMS_H
#define H264EU_PARAMS_H

#include <stdint.h>
#include <stddef.h>

#include "annexb.h"

/* Cap for num_ref_frames_in_pic_order_cnt_cycle / offset_for_ref_frame (as upstream). */
#define H264_POC1_CYCLE_MAX 256

/*
 * Subset of SPS fields needed to parse slice headers and compute POC.
 * Filled from the bitstream via h264bitstream (h264_stream_t scratch object,
 * kept private in params.c).
 */
typedef struct {
    int valid;
    int profile_idc;                  /* gates the poc_type==1 deep slice path */
    int chroma_format_idc;            /* defaults to 1 for non-high profiles */
    int separate_colour_plane_flag;   /* sps->residual_colour_transform_flag in h264bitstream */
    int frame_mbs_only_flag;
    int log2_max_frame_num;           /* 4 + log2_max_frame_num_minus4 */
    int max_frame_num;                /* 1 << log2_max_frame_num */
    int pic_order_cnt_type;
    int log2_max_poc_lsb;             /* 4 + log2_max_pic_order_cnt_lsb_minus4 */
    int max_poc_lsb;                  /* 1 << log2_max_poc_lsb */
    int has_timing;                   /* VUI timing_info_present_flag */
    int pic_struct_present_flag;      /* VUI pic_struct_present_flag (SEI picture timing) */
    uint32_t time_scale;
    uint32_t num_units_in_tick;
    int fixed_frame_rate_flag;
    /* pic_order_cnt_type == 1 fields (spec 7.3.2.1.1), zeroed for other types */
    int delta_pic_order_always_zero_flag;
    int32_t offset_for_non_ref_pic;
    int32_t offset_for_top_to_bottom_field;
    int num_ref_frames_in_pic_order_cnt_cycle;   /* clamped to H264_POC1_CYCLE_MAX */
    int32_t offset_for_ref_frame[H264_POC1_CYCLE_MAX];
} sps_state_t;

/* Subset of PPS fields needed for slice header parsing. */
typedef struct {
    int valid;
    int sps_id;
    /* pps->pic_order_present_flag in h264bitstream (old spec name) */
    int bottom_field_pic_order_in_frame_present_flag;
    int num_slice_groups_minus1;
    int entropy_coding_mode_flag;     /* gates cabac_init_idc in the deep slice path */
    int redundant_pic_cnt_present_flag;
    int weighted_pred_flag;
    int weighted_bipred_idc;
    int num_ref_idx_l0_active_minus1;
    int num_ref_idx_l1_active_minus1;
} pps_state_t;

typedef struct {
    sps_state_t sps[32];
    pps_state_t pps[256];
    void *scratch;  /* private h264_stream_t*, created by param_store_init() */
} param_store_t;

/* Returns 0 on success, -1 on allocation failure. */
int param_store_init(param_store_t *store);
void param_store_free(param_store_t *store);

/*
 * Parse an SPS or PPS NAL and store it under its id (overwriting any previous
 * set with the same id). NALs of other types are ignored. `buf` is the whole
 * file buffer, `nal` indexes into it. Returns 0 on success (including the
 * ignored case), -1 on parse failure (the stored set is left unchanged).
 */
int params_feed_nal(param_store_t *store, const uint8_t *buf, const nal_t *nal);

/*
 * Parsed slice header (spec 7.3.3). For pic_order_cnt_type != 0/2 the parse
 * stops after the common prefix (fast path, byte-identical behaviour to the
 * previous version). For pic_order_cnt_type == 1 parsing continues through the
 * full 7.3.3 grammar so that the POC inputs below are available.
 */
typedef struct {
    uint32_t first_mb_in_slice;
    uint32_t slice_type;    /* raw value before the mod-5 mapping */
    uint32_t pps_id;
    uint32_t frame_num;     /* valid for nal_type != 5 */
    uint32_t idr_pic_id;    /* valid for nal_type == 5 */
    uint32_t poc_lsb;       /* valid for pic_order_cnt_type == 0 */
    int field_pic_flag;     /* always 0 when frame_mbs_only_flag */
    int bottom_field_flag;
    char pic_type;          /* 'P','B','I','S'(SP),'X'(SI) via slice_type % 5 */
    /* deep path only (pic_order_cnt_type == 1); zero otherwise */
    int32_t delta_poc0;     /* delta_pic_order_cnt[0] (0 when delta_pic_order_always_zero_flag) */
    int32_t delta_poc1;     /* delta_pic_order_cnt[1] */
    int unreferenced;       /* dec_ref_pic_marking contains mmco 1 or 3 */
    int32_t slice_qp_delta;
    uint32_t redundant_pic_cnt;
} slice_hdr_t;

/*
 * Parse the syntax elements of a slice header.
 * `rbsp` points to the slice RBSP *after* the nal header byte
 * (i.e. annexb_rbsp() output + 1). `nal_ref_idc` is needed for the
 * dec_ref_pic_marking gate.
 * Fast path (pic_order_cnt_type != 1): stops after the common prefix.
 * Deep path (pic_order_cnt_type == 1, spec 7.3.3 in full): additionally
 * parses delta_pic_order_cnt, redundant_pic_cnt, direct_spatial_mv_pred,
 * num_ref_idx_active_override, ref_pic_list_modification, cabac_init_idc,
 * slice_qp_delta, pred_weight_table and dec_ref_pic_marking.
 * Returns 0 on success, -1 on parse error (missing PPS/SPS, overrun, ...)
 * and on streams the deep path refuses by design:
 *   - High-family profiles (44,83,86,118,128,134,135,138,139), where
 *     ref_pic_list_mvc_modification_present_flag is inferred as 1 (spec 7.3)
 *     and ref_pic_list_mvc_modification() is not implemented;
 *   - Extended profiles (88,244) with a pred_weight_table (different syntax);
 *   - memory_management_control_operation >= 7.
 */
int slice_header_parse(const param_store_t *store, uint8_t nal_type,
                       uint8_t nal_ref_idc,
                       const uint8_t *rbsp_after_nal_header, size_t size,
                       slice_hdr_t *out);

#endif /* H264EU_PARAMS_H */
