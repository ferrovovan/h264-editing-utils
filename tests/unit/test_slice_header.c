/*
 * test_slice_header.c - unit tests for the pic_order_cnt_type == 1 deep path
 * of slice_header_parse() (spec 7.3.3), on synthetic RBSP built with
 * bitwriter.h. The param_store is filled by hand (the state structs are
 * public; no h264_stream parsing involved).
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

#include <stdint.h>
#include <string.h>

#include "unity.h"
#include "params.h"
#include "bitwriter.h"

void setUp(void)
{
}

void tearDown(void)
{
}

/*
 * Store state shared by the tests: SPS 0 (Main profile, poc type 1,
 * MaxFrameNum 16, poc cycle [2, 2]) and PPS 0. Individual tests tweak the
 * flags the deep path consults.
 */
static void fill_store(param_store_t *store, int profile_idc)
{
    sps_state_t *sps;
    pps_state_t *pps;

    TEST_ASSERT_EQUAL_INT(0, param_store_init(store));

    sps = &store->sps[0];
    sps->valid = 1;
    sps->profile_idc = profile_idc;
    sps->chroma_format_idc = 1;
    sps->separate_colour_plane_flag = 0;
    sps->frame_mbs_only_flag = 1;
    sps->log2_max_frame_num = 4;
    sps->max_frame_num = 16;
    sps->pic_order_cnt_type = 1;
    sps->log2_max_poc_lsb = 4;
    sps->max_poc_lsb = 16;
    sps->delta_pic_order_always_zero_flag = 0;
    sps->offset_for_non_ref_pic = -1;
    sps->offset_for_top_to_bottom_field = 0;
    sps->num_ref_frames_in_pic_order_cnt_cycle = 2;
    sps->offset_for_ref_frame[0] = 2;
    sps->offset_for_ref_frame[1] = 2;

    pps = &store->pps[0];
    pps->valid = 1;
    pps->sps_id = 0;
    pps->bottom_field_pic_order_in_frame_present_flag = 0;
    pps->num_slice_groups_minus1 = 0;
    pps->entropy_coding_mode_flag = 0;
    pps->redundant_pic_cnt_present_flag = 0;
    pps->weighted_pred_flag = 0;
    pps->weighted_bipred_idc = 0;
    pps->num_ref_idx_l0_active_minus1 = 1;
    pps->num_ref_idx_l1_active_minus1 = 1;
}

/* (a) P slice, poc type 1: delta0=-2, num_ref_idx override, l0
 * ref_pic_list_modification (idc 1 -> ue, stop 3), slice_qp_delta=-3 and a
 * dec_ref_pic_marking loop { mmco 6 + ue, mmco 3 + ue ue, mmco 0 } ->
 * unreferenced must be set by mmco 3 without breaking the loop. */
static void test_deep_p_slice_mmco(void)
{
    param_store_t store;
    uint8_t buf[64];
    bitwriter_t bw;
    slice_hdr_t hdr;
    size_t size;

    fill_store(&store, 77);

    bw_init(&bw, buf, sizeof(buf));
    bw_ue(&bw, 0);            /* first_mb_in_slice */
    bw_ue(&bw, 0);            /* slice_type = P */
    bw_ue(&bw, 0);            /* pic_parameter_set_id */
    bw_u(&bw, 5, 4);          /* frame_num (log2_max_frame_num = 4) */
    bw_se(&bw, -2);           /* delta_pic_order_cnt[0] */
    bw_u(&bw, 1, 1);          /* num_ref_idx_active_override_flag */
    bw_ue(&bw, 1);            /* num_ref_idx_l0_active_minus1 */
    bw_ue(&bw, 1);            /* modification_of_pic_nums_idc = 1 */
    bw_ue(&bw, 2);            /* abs_diff_pic_num_minus1 */
    bw_ue(&bw, 3);            /* modification_of_pic_nums_idc = 3 (stop) */
    bw_se(&bw, -3);           /* slice_qp_delta */
    bw_u(&bw, 1, 1);          /* adaptive_ref_pic_marking_mode_flag */
    bw_ue(&bw, 6);            /* mmco 6 */
    bw_ue(&bw, 0);            /* long_term_frame_idx */
    bw_ue(&bw, 3);            /* mmco 3 -> unreferenced */
    bw_ue(&bw, 0);            /* long_term_pic_num */
    bw_ue(&bw, 0);            /* long_term_frame_idx */
    bw_ue(&bw, 0);            /* mmco 0 -> end of loop */
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    size = bw_finish(&bw);

    TEST_ASSERT_EQUAL_INT(0, slice_header_parse(&store, 1, 3, buf, size, &hdr));
    TEST_ASSERT_EQUAL_CHAR('P', hdr.pic_type);
    TEST_ASSERT_EQUAL_UINT32(5, hdr.frame_num);
    TEST_ASSERT_EQUAL_INT32(-2, hdr.delta_poc0);
    TEST_ASSERT_EQUAL_INT32(0, hdr.delta_poc1); /* no bottom flag -> untouched */
    TEST_ASSERT_EQUAL_INT32(-3, hdr.slice_qp_delta);
    TEST_ASSERT_EQUAL_INT(1, hdr.unreferenced);

    param_store_free(&store);
}

/* (b) B slice with pred_weight_table on l0 and l1 (chroma_format_idc = 1),
 * direct_spatial_mv_pred, redundant_pic_cnt, nal_ref_idc == 0 -> no
 * dec_ref_pic_marking, unreferenced stays 0. */
static void test_deep_b_slice_weight_table(void)
{
    param_store_t store;
    uint8_t buf[64];
    bitwriter_t bw;
    slice_hdr_t hdr;
    size_t size;

    fill_store(&store, 77);
    store.pps[0].redundant_pic_cnt_present_flag = 1;
    store.pps[0].weighted_bipred_idc = 1;

    bw_init(&bw, buf, sizeof(buf));
    bw_ue(&bw, 0);            /* first_mb_in_slice */
    bw_ue(&bw, 1);            /* slice_type = B */
    bw_ue(&bw, 0);            /* pic_parameter_set_id */
    bw_u(&bw, 7, 4);          /* frame_num */
    bw_se(&bw, -1);           /* delta_pic_order_cnt[0] */
    bw_ue(&bw, 2);            /* redundant_pic_cnt */
    bw_u(&bw, 1, 1);          /* direct_spatial_mv_pred_flag */
    bw_u(&bw, 0, 1);          /* num_ref_idx_active_override_flag = 0 ->
                               * eff l0 = eff l1 = pps minus1 + 1 = 2 */
    bw_ue(&bw, 3);            /* ref_pic_list_modification l0: stop */
    bw_ue(&bw, 3);            /* ref_pic_list_modification l1: stop */
    bw_se(&bw, 1);            /* slice_qp_delta */
    /* pred_weight_table: l0 and l1, two entries each, chroma present */
    bw_ue(&bw, 2);            /* luma_log2_weight_denom */
    bw_ue(&bw, 1);            /* chroma_log2_weight_denom */
    bw_u(&bw, 1, 1);          /* l0[0] luma_weight_flag */
    bw_se(&bw, 1);            /* luma_weight */
    bw_se(&bw, -2);           /* luma_offset */
    bw_u(&bw, 1, 1);          /* l0[0] chroma_weight_flag */
    bw_se(&bw, 2);            /* chroma_weight[0] */
    bw_se(&bw, 1);            /* chroma_offset[0] */
    bw_se(&bw, -1);           /* chroma_weight[1] */
    bw_se(&bw, 3);            /* chroma_offset[1] */
    bw_u(&bw, 0, 1);          /* l0[1] luma_weight_flag */
    bw_u(&bw, 0, 1);          /* l0[1] chroma_weight_flag */
    bw_u(&bw, 1, 1);          /* l1[0] luma_weight_flag */
    bw_se(&bw, -1);
    bw_se(&bw, 1);
    bw_u(&bw, 1, 1);          /* l1[0] chroma_weight_flag */
    bw_se(&bw, 1);
    bw_se(&bw, 0);
    bw_se(&bw, 1);
    bw_se(&bw, 0);
    bw_u(&bw, 0, 1);          /* l1[1] luma_weight_flag */
    bw_u(&bw, 0, 1);          /* l1[1] chroma_weight_flag */
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    size = bw_finish(&bw);

    /* nal_ref_idc == 0: dec_ref_pic_marking absent */
    TEST_ASSERT_EQUAL_INT(0, slice_header_parse(&store, 1, 0, buf, size, &hdr));
    TEST_ASSERT_EQUAL_CHAR('B', hdr.pic_type);
    TEST_ASSERT_EQUAL_UINT32(7, hdr.frame_num);
    TEST_ASSERT_EQUAL_INT32(-1, hdr.delta_poc0);
    TEST_ASSERT_EQUAL_UINT32(2, hdr.redundant_pic_cnt);
    TEST_ASSERT_EQUAL_INT32(1, hdr.slice_qp_delta);
    TEST_ASSERT_EQUAL_INT(0, hdr.unreferenced);

    param_store_free(&store);
}

/* (c) High-family profile: ref_pic_list_mvc_modification_present_flag is
 * inferred as 1 -> the deep path must refuse the slice with -1. */
static void test_deep_high_profile_rejected(void)
{
    param_store_t store;
    uint8_t buf[32];
    bitwriter_t bw;
    slice_hdr_t hdr;
    size_t size;

    fill_store(&store, 118);  /* Progressive High */

    bw_init(&bw, buf, sizeof(buf));
    bw_ue(&bw, 0);            /* first_mb_in_slice */
    bw_ue(&bw, 0);            /* slice_type = P */
    bw_ue(&bw, 0);            /* pic_parameter_set_id */
    bw_u(&bw, 1, 4);          /* frame_num */
    bw_se(&bw, 0);            /* delta_pic_order_cnt[0] */
    bw_u(&bw, 0, 1);          /* num_ref_idx_active_override_flag */
    bw_ue(&bw, 3);            /* ref_pic_list_modification: stop */
    bw_se(&bw, 0);            /* slice_qp_delta */
    bw_u(&bw, 0, 1);          /* adaptive_ref_pic_marking_mode_flag */
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    size = bw_finish(&bw);

    TEST_ASSERT_EQUAL_INT(-1, slice_header_parse(&store, 1, 3, buf, size, &hdr));

    param_store_free(&store);
}

/* (d) Truncated deep path: the prefix parses, the overrun is hit in the
 * continuation -> -1. */
static void test_deep_truncated(void)
{
    param_store_t store;
    uint8_t buf[64];
    bitwriter_t bw;
    slice_hdr_t hdr;
    size_t full;
    size_t i;

    fill_store(&store, 77);

    bw_init(&bw, buf, sizeof(buf));
    bw_ue(&bw, 0);            /* first_mb_in_slice */
    bw_ue(&bw, 0);            /* slice_type = P */
    bw_ue(&bw, 0);            /* pic_parameter_set_id */
    bw_u(&bw, 2, 4);          /* frame_num */
    bw_se(&bw, -2);           /* delta_pic_order_cnt[0] */
    bw_u(&bw, 0, 1);          /* num_ref_idx_active_override_flag */
    bw_ue(&bw, 3);            /* ref_pic_list_modification: stop */
    bw_se(&bw, 0);            /* slice_qp_delta */
    bw_u(&bw, 1, 1);          /* adaptive_ref_pic_marking_mode_flag */
    bw_ue(&bw, 3);            /* mmco 3 (operands cut off) */
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    full = bw_finish(&bw);

    /* cut inside mmco (keep one byte less than needed for its operands) */
    for (i = 1; i < full; i++) {
        TEST_ASSERT_EQUAL_INT(-1, slice_header_parse(&store, 1, 3, buf, i, &hdr));
    }

    param_store_free(&store);
}

/* (e) Regression: pic_order_cnt_type != 1 keeps the old fast path - the
 * header parses and nothing past poc_lsb / frame_num is consumed. */
static void test_fast_path_unchanged(void)
{
    param_store_t store;
    uint8_t buf[32];
    bitwriter_t bw;
    slice_hdr_t hdr;
    size_t size;

    fill_store(&store, 77);

    /* poc type 0: prefix + poc_lsb, then trailing garbage the fast path
     * must not touch (a deep read of se(-2) would consume it). */
    store.sps[0].pic_order_cnt_type = 0;
    bw_init(&bw, buf, sizeof(buf));
    bw_ue(&bw, 0);            /* first_mb_in_slice */
    bw_ue(&bw, 0);            /* slice_type = P */
    bw_ue(&bw, 0);            /* pic_parameter_set_id */
    bw_u(&bw, 9, 4);          /* frame_num */
    bw_u(&bw, 6, 4);          /* pic_order_cnt_lsb */
    bw_u(&bw, 0x00, 8);       /* "garbage": as se() this would be a parse
                               * error on overrun in the deep path */
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    size = bw_finish(&bw);

    TEST_ASSERT_EQUAL_INT(0, slice_header_parse(&store, 1, 3, buf, size, &hdr));
    TEST_ASSERT_EQUAL_UINT32(9, hdr.frame_num);
    TEST_ASSERT_EQUAL_UINT32(6, hdr.poc_lsb);
    TEST_ASSERT_EQUAL_CHAR('P', hdr.pic_type);
    TEST_ASSERT_EQUAL_INT32(0, hdr.delta_poc0);
    TEST_ASSERT_EQUAL_UINT32(0, hdr.redundant_pic_cnt);
    TEST_ASSERT_EQUAL_INT(0, hdr.unreferenced);

    /* poc type 2: frame_num only, no poc fields at all */
    store.sps[0].pic_order_cnt_type = 2;
    bw_init(&bw, buf, sizeof(buf));
    bw_ue(&bw, 0);
    bw_ue(&bw, 0);            /* slice_type = P */
    bw_ue(&bw, 0);
    bw_u(&bw, 3, 4);          /* frame_num */
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    size = bw_finish(&bw);

    TEST_ASSERT_EQUAL_INT(0, slice_header_parse(&store, 1, 3, buf, size, &hdr));
    TEST_ASSERT_EQUAL_UINT32(3, hdr.frame_num);
    TEST_ASSERT_EQUAL_UINT32(0, hdr.poc_lsb);
    TEST_ASSERT_EQUAL_INT32(0, hdr.slice_qp_delta);

    param_store_free(&store);
}

int main(void)
{
    UnityBegin("test_slice_header.c");
    RUN_TEST(test_deep_p_slice_mmco);
    RUN_TEST(test_deep_b_slice_weight_table);
    RUN_TEST(test_deep_high_profile_rejected);
    RUN_TEST(test_deep_truncated);
    RUN_TEST(test_fast_path_unchanged);
    return UnityEnd();
}
