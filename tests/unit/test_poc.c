/*
 * test_poc.c - end-to-end tests for PicOrderCnt computation in index_build():
 * pic_order_cnt_type == 1 (spec 8.2.1.2 / 8.2.1.1 of the 2003 edition) on a
 * fully synthetic Annex B stream, plus regression for types 0 and 2. Every
 * byte of SPS/PPS/slice NALs is written with bitwriter.h; the expected POC
 * vectors are derived by hand from the spec (each number commented).
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
#include "index.h"
#include "bitwriter.h"

void setUp(void)
{
}

void tearDown(void)
{
}

#define MAX_FRAME_NUM 16 /* log2_max_frame_num_minus4 = 0 in every SPS here */

static size_t put_nal(uint8_t *stream, size_t pos, const uint8_t *nal, size_t n)
{
    stream[pos++] = 0x00;
    stream[pos++] = 0x00;
    stream[pos++] = 0x01;
    memcpy(stream + pos, nal, n);
    return pos + n;
}

/* Main profile (77), frame_mbs_only, no VUI; poc_type selects the
 * type-specific fields. For poc type 1: always_zero=0,
 * offset_for_non_ref_pic=-1, offset_for_top_to_bottom_field=0,
 * cycle = { offset_for_ref_frame[0]=2, [1]=3 } (sum 5). */
static size_t build_sps(uint8_t *tmp, int poc_type)
{
    bitwriter_t bw;

    bw_init(&bw, tmp, 64);
    bw_u8(&bw, 0x67);      /* nal header: ref_idc 3, type 7 */
    bw_u8(&bw, 77);        /* profile_idc */
    bw_u8(&bw, 0);         /* constraint flags + reserved */
    bw_u8(&bw, 30);        /* level_idc */
    bw_ue(&bw, 0);         /* seq_parameter_set_id */
    bw_ue(&bw, 0);         /* log2_max_frame_num_minus4 -> MaxFrameNum 16 */
    bw_ue(&bw, (uint32_t)poc_type);
    if (poc_type == 0) {
        bw_ue(&bw, 0);     /* log2_max_pic_order_cnt_lsb_minus4 -> 16 */
    } else if (poc_type == 1) {
        bw_u(&bw, 0, 1);   /* delta_pic_order_always_zero_flag */
        bw_se(&bw, -1);    /* offset_for_non_ref_pic */
        bw_se(&bw, 0);     /* offset_for_top_to_bottom_field */
        bw_ue(&bw, 2);     /* num_ref_frames_in_pic_order_cnt_cycle */
        bw_se(&bw, 2);     /* offset_for_ref_frame[0] */
        bw_se(&bw, 3);     /* offset_for_ref_frame[1] */
    }
    bw_ue(&bw, 4);         /* num_ref_frames */
    bw_u(&bw, 0, 1);       /* gaps_in_frame_num_value_allowed_flag */
    bw_ue(&bw, 0);         /* pic_width_in_mbs_minus1 */
    bw_ue(&bw, 0);         /* pic_height_in_map_units_minus1 */
    bw_u(&bw, 1, 1);       /* frame_mbs_only_flag */
    bw_u(&bw, 1, 1);       /* direct_8x8_inference_flag */
    bw_u(&bw, 0, 1);       /* frame_cropping_flag */
    bw_u(&bw, 0, 1);       /* vui_parameters_present_flag */
    bw_u(&bw, 1, 1);       /* rbsp_stop_one_bit */
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    return bw_finish(&bw);
}

/* CAVLC PPS 0, SPS 0, both num_ref_idx_*_active_minus1 = 1, no weighting,
 * no redundant pictures. */
static size_t build_pps(uint8_t *tmp)
{
    bitwriter_t bw;

    bw_init(&bw, tmp, 64);
    bw_u8(&bw, 0x68);      /* nal header: ref_idc 3, type 8 */
    bw_ue(&bw, 0);         /* pic_parameter_set_id */
    bw_ue(&bw, 0);         /* seq_parameter_set_id */
    bw_u(&bw, 0, 1);       /* entropy_coding_mode_flag */
    bw_u(&bw, 0, 1);       /* bottom_field_pic_order_in_frame_present_flag */
    bw_ue(&bw, 0);         /* num_slice_groups_minus1 */
    bw_ue(&bw, 1);         /* num_ref_idx_l0_active_minus1 */
    bw_ue(&bw, 1);         /* num_ref_idx_l1_active_minus1 */
    bw_u(&bw, 0, 1);       /* weighted_pred_flag */
    bw_u(&bw, 0, 2);       /* weighted_bipred_idc */
    bw_se(&bw, 0);         /* pic_init_qp_minus26 */
    bw_se(&bw, 0);         /* pic_init_qs_minus26 */
    bw_se(&bw, 0);         /* chroma_qp_index_offset */
    bw_u(&bw, 1, 1);       /* deblocking_filter_control_present_flag */
    bw_u(&bw, 0, 1);       /* constrained_intra_pred_flag */
    bw_u(&bw, 0, 1);       /* redundant_pic_cnt_present_flag */
    bw_u(&bw, 1, 1);       /* rbsp_stop_one_bit (ends more_rbsp_data) */
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    return bw_finish(&bw);
}

/* IDR / I slice; for poc type 1 the deep path is exercised (delta0, qp,
 * dec_ref_pic_marking of an IDR); poc_lsb is used for poc type 0. */
static size_t build_idr(uint8_t *tmp, int poc_type, uint32_t poc_lsb)
{
    bitwriter_t bw;

    bw_init(&bw, tmp, 64);
    bw_u8(&bw, 0x65);      /* nal header: ref_idc 3, type 5 */
    bw_ue(&bw, 0);         /* first_mb_in_slice */
    bw_ue(&bw, 2);         /* slice_type = I */
    bw_ue(&bw, 0);         /* pic_parameter_set_id */
    bw_u(&bw, 0, 4);       /* frame_num */
    bw_ue(&bw, 0);         /* idr_pic_id */
    if (poc_type == 0) {
        bw_u(&bw, poc_lsb, 4);
    } else if (poc_type == 1) {
        bw_se(&bw, 0);     /* delta_pic_order_cnt[0] */
        bw_se(&bw, 0);     /* slice_qp_delta */
        bw_u(&bw, 0, 1);   /* no_output_of_prior_pics_flag */
        bw_u(&bw, 0, 1);   /* long_term_reference_flag */
    }
    bw_u(&bw, 1, 1);       /* rbsp_stop_one_bit */
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    return bw_finish(&bw);
}

/* P slice, nal_ref_idc 3 (byte 0x61). Deep tail (override off, empty l0
 * ref_pic_list_modification, qp, adaptive marking off) for poc type 1;
 * poc type 0 carries poc_lsb and is parsed by the fast path. */
static size_t build_p(uint8_t *tmp, int poc_type, uint32_t frame_num,
                      uint32_t poc_lsb, int32_t delta0)
{
    bitwriter_t bw;

    bw_init(&bw, tmp, 64);
    bw_u8(&bw, 0x61);
    bw_ue(&bw, 0);         /* first_mb_in_slice */
    bw_ue(&bw, 0);         /* slice_type = P */
    bw_ue(&bw, 0);         /* pic_parameter_set_id */
    bw_u(&bw, frame_num, 4);
    if (poc_type == 0) {
        bw_u(&bw, poc_lsb, 4);
    } else if (poc_type == 1) {
        bw_se(&bw, delta0);          /* delta_pic_order_cnt[0] */
        bw_u(&bw, 0, 1);             /* num_ref_idx_active_override_flag */
        bw_ue(&bw, 3);               /* ref_pic_list_modification: stop */
        bw_se(&bw, 0);               /* slice_qp_delta */
        bw_u(&bw, 0, 1);             /* adaptive_ref_pic_marking_mode_flag */
    }
    bw_u(&bw, 1, 1);       /* rbsp_stop_one_bit */
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    return bw_finish(&bw);
}

/* B slice for the poc type 0/2 fast path: prefix (+ poc_lsb for type 0;
 * the extra field is plain padding for type 2) and a stop bit. */
static size_t build_b_fast(uint8_t *tmp, int poc_type, uint32_t frame_num,
                           uint32_t poc_lsb)
{
    bitwriter_t bw;

    bw_init(&bw, tmp, 64);
    bw_u8(&bw, 0x01);
    bw_ue(&bw, 0);
    bw_ue(&bw, 1);         /* slice_type = B */
    bw_ue(&bw, 0);
    bw_u(&bw, frame_num, 4);
    if (poc_type == 0) {
        bw_u(&bw, poc_lsb, 4);
    }
    bw_u(&bw, 1, 1);       /* rbsp_stop_one_bit */
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    return bw_finish(&bw);
}

/* B slice, nal_ref_idc 0 (byte 0x01), deep tail: direct_spatial_mv_pred,
 * override off, empty l0/l1 ref_pic_list_modification, qp; no
 * dec_ref_pic_marking (nal_ref_idc == 0). */
static size_t build_b(uint8_t *tmp, uint32_t frame_num, int32_t delta0)
{
    bitwriter_t bw;

    bw_init(&bw, tmp, 64);
    bw_u8(&bw, 0x01);
    bw_ue(&bw, 0);
    bw_ue(&bw, 1);         /* slice_type = B */
    bw_ue(&bw, 0);
    bw_u(&bw, frame_num, 4);
    bw_se(&bw, delta0);
    bw_u(&bw, 1, 1);       /* direct_spatial_mv_pred_flag */
    bw_u(&bw, 0, 1);       /* num_ref_idx_active_override_flag */
    bw_ue(&bw, 3);         /* ref_pic_list_modification l0: stop */
    bw_ue(&bw, 3);         /* ref_pic_list_modification l1: stop */
    bw_se(&bw, 0);         /* slice_qp_delta */
    bw_u(&bw, 1, 1);       /* rbsp_stop_one_bit */
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    return bw_finish(&bw);
}

/* B slice, nal_ref_idc 1 (byte 0x21), with dec_ref_pic_marking
 * { mmco 3, long_term_pic_num 0, long_term_frame_idx 0, mmco 0 } ->
 * unreferenced picture in the sense of decoded reference marking. */
static size_t build_b_unreferenced(uint8_t *tmp, uint32_t frame_num)
{
    bitwriter_t bw;

    bw_init(&bw, tmp, 64);
    bw_u8(&bw, 0x21);
    bw_ue(&bw, 0);
    bw_ue(&bw, 1);         /* slice_type = B */
    bw_ue(&bw, 0);
    bw_u(&bw, frame_num, 4);
    bw_se(&bw, 0);         /* delta_pic_order_cnt[0] */
    bw_u(&bw, 1, 1);       /* direct_spatial_mv_pred_flag */
    bw_u(&bw, 0, 1);       /* num_ref_idx_active_override_flag */
    bw_ue(&bw, 3);         /* l0 stop */
    bw_ue(&bw, 3);         /* l1 stop */
    bw_se(&bw, 0);         /* slice_qp_delta */
    bw_u(&bw, 1, 1);       /* adaptive_ref_pic_marking_mode_flag */
    bw_ue(&bw, 3);         /* mmco 3 */
    bw_ue(&bw, 0);         /* long_term_pic_num */
    bw_ue(&bw, 0);         /* long_term_frame_idx */
    bw_ue(&bw, 0);         /* mmco 0 */
    bw_u(&bw, 1, 1);       /* rbsp_stop_one_bit */
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    return bw_finish(&bw);
}

static void assert_ok_frames(const stream_index_t *idx, size_t n)
{
    size_t i;

    TEST_ASSERT_EQUAL_INT((int)n, (int)idx->frame_count);
    TEST_ASSERT_EQUAL_INT(1, idx->poc_supported);
    for (i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, idx->frames[i].parse_error,
                                      "unexpected parse error");
        TEST_ASSERT_TRUE_MESSAGE(idx->frames[i].poc != -1, "poc unknown");
        TEST_ASSERT_TRUE_MESSAGE(idx->frames[i].display_index != -1,
                                 "display_index unknown");
    }
}

/*
 * poc_type == 1 end-to-end. SPS: MaxFrameNum 16, cycle {2, 3} (sum 5),
 * offset_for_non_ref_pic = -1. Decode order: IDR, P1, P2, B1, B2, P3, P4,
 * B3(unreferenced via mmco 3). Display order is P1 B2 B1 P2 P3 P4 B3, i.e.
 * the classic P B B P GOP (poc type 1 mandates a monotonically increasing
 * frame_num in *decode* order, so the B pictures carry frame_num 3, 4 and
 * pull their POC back below P2's with delta_pic_order_cnt).
 *
 * PicOrderCnt per 8.2.1.2 (fn=frame_num, FNO=FrameNumOffset):
 *  IDR  fn 0, ref!=0: FNO=0, absFrameNum=0 -> expected=0, POC=0 (IDR).
 *  P1   fn 1, ref!=0: FNO=0, abs=1 -> cyc=(1-1)/2=0, idx=0 ->
 *                     expected=offset[0]=2,                       POC=2.
 *  P2   fn 2, ref!=0: FNO=0, abs=2 -> cyc=0, idx=1 ->
 *                     expected=offset[0]+offset[1]=5,             POC=5.
 *  B1   fn 3, ref==0, delta0=0: FNO=0, abs=3-1=2 -> expected=5,
 *                     +offset_for_non_ref_pic=-1 -> 5-1,          POC=4.
 *  B2   fn 4, ref==0, delta0=-3: FNO=0, abs=4-1=3 -> cyc=(3-1)/2=1, idx=0 ->
 *                     expected=5+2=7, -1, +delta0=-3 -> 7-1-3,    POC=3.
 *  P3   fn 5, ref!=0: FNO=0, abs=5 -> cyc=(5-1)/2=2, idx=0 ->
 *                     expected=2*5+2=12,                          POC=12.
 *  P4   fn 0, ref!=0: wrap, prevFrameNum=5>0 -> FNO=0+16=16,
 *       abs=16 -> cyc=(16-1)/2=7, idx=1 ->
 *       expected=7*5+2+3=40,                                      POC=40.
 *  B3   fn 1, ref!=0 (nal_ref_idc 1 despite mmco 3): FNO=16,
 *       abs=16+1=17 -> cyc=8, idx=0 -> expected=8*5+2=42.
 *       Per spec 8-9 the unreferenced picture keeps the POC derived from
 *       nal_ref_idc (the slice-header flag is set, POC is not affected).
 * POC vector (decode order): 0, 2, 5, 4, 3, 12, 40, 42.
 * Sorted (0,2,3,4,5,12,40,42) -> display_index:
 *  IDR->0, P1->1, P2->4, B1->3, B2->2, P3->5, P4->6, B3->7.
 */
static void test_poc_type1_end_to_end(void)
{
    uint8_t stream[512];
    uint8_t nal[64];
    stream_index_t idx;
    size_t pos = 0, n;
    static const long long poc[] = { 0, 2, 5, 4, 3, 12, 40, 42 };
    static const long long disp[] = { 0, 1, 4, 3, 2, 5, 6, 7 };
    static const char types[] = { 'I', 'P', 'P', 'B', 'B', 'P', 'P', 'B' };
    size_t i;

    n = build_sps(nal, 1);
    pos = put_nal(stream, pos, nal, n);
    n = build_pps(nal);
    pos = put_nal(stream, pos, nal, n);
    n = build_idr(nal, 1, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_p(nal, 1, 1, 0, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_p(nal, 1, 2, 0, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_b(nal, 3, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_b(nal, 4, -3);
    pos = put_nal(stream, pos, nal, n);
    n = build_p(nal, 1, 5, 0, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_p(nal, 1, 0, 0, 0); /* frame_num wraps through MaxFrameNum */
    pos = put_nal(stream, pos, nal, n);
    n = build_b_unreferenced(nal, 1);
    pos = put_nal(stream, pos, nal, n);

    TEST_ASSERT_EQUAL_INT(0, index_build(&idx, stream, pos));
    assert_ok_frames(&idx, 8);
    for (i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_INT64(poc[i], idx.frames[i].poc);
        TEST_ASSERT_EQUAL_INT64(disp[i], idx.frames[i].display_index);
        TEST_ASSERT_EQUAL_CHAR(types[i], idx.frames[i].pic_type);
        TEST_ASSERT_EQUAL_INT_MESSAGE(i == 0, idx.frames[i].is_idr,
                                      "only frame 0 is IDR");
    }
    TEST_ASSERT_EQUAL_UINT32(0, idx.frames[0].frame_num);
    TEST_ASSERT_EQUAL_UINT32(5, idx.frames[5].frame_num);
    TEST_ASSERT_EQUAL_UINT32(0, idx.frames[6].frame_num); /* wrapped */
    index_free(&idx);
}

/* Regression: poc_type == 0 keeps its vector (and display reordering)
 * untouched by the B3 changes. SPS: MaxPicOrderCntLsb = 16.
 *  IDR lsb 0 -> POC 0; P lsb 4 -> 4; B lsb 2 -> 2; P lsb 8 -> 8.
 * Display: 0,2,4,8 -> di: IDR->0, P->2, B->1, P->3. */
static void test_poc_type0_regression(void)
{
    uint8_t stream[256];
    uint8_t nal[64];
    stream_index_t idx;
    size_t pos = 0, n;
    static const long long poc[] = { 0, 4, 2, 8 };
    static const long long disp[] = { 0, 2, 1, 3 };
    size_t i;

    n = build_sps(nal, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_pps(nal);
    pos = put_nal(stream, pos, nal, n);
    n = build_idr(nal, 0, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_p(nal, 0, 1, 4, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_b_fast(nal, 0, 2, 2);
    pos = put_nal(stream, pos, nal, n);
    n = build_p(nal, 0, 3, 8, 0);
    pos = put_nal(stream, pos, nal, n);

    TEST_ASSERT_EQUAL_INT(0, index_build(&idx, stream, pos));
    assert_ok_frames(&idx, 4);
    for (i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT64(poc[i], idx.frames[i].poc);
        TEST_ASSERT_EQUAL_INT64(disp[i], idx.frames[i].display_index);
    }
    index_free(&idx);
}

/* Regression: poc_type == 2 keeps POC = 2*FrameNumOffset+2*fn - (ref==0).
 *  IDR fn0 -> 0; P fn1 -> 2; B fn2 (ref 0) -> 3; P fn3 -> 6. */
static void test_poc_type2_regression(void)
{
    uint8_t stream[256];
    uint8_t nal[64];
    stream_index_t idx;
    size_t pos = 0, n;
    static const long long poc[] = { 0, 2, 3, 6 };
    static const long long disp[] = { 0, 1, 2, 3 };
    size_t i;

    n = build_sps(nal, 2);
    pos = put_nal(stream, pos, nal, n);
    n = build_pps(nal);
    pos = put_nal(stream, pos, nal, n);
    n = build_idr(nal, 2, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_p(nal, 2, 1, 0, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_b_fast(nal, 2, 2, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_p(nal, 2, 3, 0, 0);
    pos = put_nal(stream, pos, nal, n);

    TEST_ASSERT_EQUAL_INT(0, index_build(&idx, stream, pos));
    assert_ok_frames(&idx, 4);
    for (i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT64(poc[i], idx.frames[i].poc);
        TEST_ASSERT_EQUAL_INT64(disp[i], idx.frames[i].display_index);
    }
    index_free(&idx);
}

int main(void)
{
    UnityBegin("test_poc.c");
    RUN_TEST(test_poc_type1_end_to_end);
    RUN_TEST(test_poc_type0_regression);
    RUN_TEST(test_poc_type2_regression);
    return UnityEnd();
}
