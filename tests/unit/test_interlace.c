/*
 * test_interlace.c - index_build() tests for interlaced (PAFF/field) streams:
 * a field pair is ONE picture (spec 8.21.1 + 7.4.1.2.4), the picture POC is
 * the top field's POC, and the pic_order_cnt_type 0 wrap state is tracked
 * per field parity. All streams are fully synthetic Annex B written with
 * bitwriter.h (SPS with frame_mbs_only_flag = 0), MBAFF regression included.
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

static size_t put_nal(uint8_t *stream, size_t pos, const uint8_t *nal, size_t n)
{
    stream[pos++] = 0x00;
    stream[pos++] = 0x00;
    stream[pos++] = 0x01;
    memcpy(stream + pos, nal, n);
    return pos + n;
}

/*
 * Profile 77 SPS 0 with frame_mbs_only_flag = 0 (interlaced-capable, MBAFF
 * allowed) and no VUI. poc_type 0: MaxPicOrderCntLsb = 16 (MAX_POC_LSB);
 * poc_type 1: always_zero=0, offset_for_non_ref_pic=0,
 * offset_for_top_to_bottom_field=off2b, cycle { offset_for_ref_frame[0]=2 }
 * -> a frame picture's TopFieldOrderCnt is 2 * FrameNum.
 */
static size_t build_sps(uint8_t *tmp, int poc_type, int32_t off2b)
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
        bw_se(&bw, 0);     /* offset_for_non_ref_pic */
        bw_se(&bw, off2b); /* offset_for_top_to_bottom_field */
        bw_ue(&bw, 1);     /* num_ref_frames_in_pic_order_cnt_cycle */
        bw_se(&bw, 2);     /* offset_for_ref_frame[0] */
    }
    bw_ue(&bw, 4);         /* num_ref_frames */
    bw_u(&bw, 0, 1);       /* gaps_in_frame_num_value_allowed_flag */
    bw_ue(&bw, 0);         /* pic_width_in_mbs_minus1 */
    bw_ue(&bw, 0);         /* pic_height_in_map_units_minus1 */
    bw_u(&bw, 0, 1);       /* frame_mbs_only_flag = 0 (interlaced) */
    bw_u(&bw, 1, 1);       /* mb_adaptive_frame_field_flag */
    bw_u(&bw, 1, 1);       /* direct_8x8_inference_flag */
    bw_u(&bw, 0, 1);       /* frame_cropping_flag */
    bw_u(&bw, 0, 1);       /* vui_parameters_present_flag */
    bw_u(&bw, 1, 1);       /* rbsp_stop_one_bit */
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    return bw_finish(&bw);
}

/* CAVLC PPS 0 on SPS 0 with a selectable bottom_field_pic_order_in_frame_-
 * present_flag (spec name of h264bitstream's pic_order_present_flag). */
static size_t build_pps(uint8_t *tmp, int pic_order_present)
{
    bitwriter_t bw;

    bw_init(&bw, tmp, 64);
    bw_u8(&bw, 0x68);      /* nal header: ref_idc 3, type 8 */
    bw_ue(&bw, 0);         /* pic_parameter_set_id */
    bw_ue(&bw, 0);         /* seq_parameter_set_id */
    bw_u(&bw, 0, 1);       /* entropy_coding_mode_flag */
    bw_u(&bw, (uint32_t)pic_order_present, 1);
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
    bw_u(&bw, 1, 1);       /* rbsp_stop_one_bit */
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    return bw_finish(&bw);
}

/*
 * IDR slice header as written by our parser's grammar (params.c reads
 * frame_num unconditionally, see pitfall 1): first_mb, slice_type I, pps 0,
 * frame_num u(4), field_pic_flag, bottom_field_flag (only when field),
 * idr_pic_id, pic_order_cnt_lsb u(4). nal_byte is 0x65 (ref_idc 3).
 */
static size_t build_field_idr(uint8_t *tmp, uint32_t first_mb,
                              uint32_t frame_num, uint32_t idr_pic_id,
                              int field, int bottom, uint32_t poc_lsb)
{
    bitwriter_t bw;

    bw_init(&bw, tmp, 64);
    bw_u8(&bw, 0x65);
    bw_ue(&bw, first_mb);
    bw_ue(&bw, 2);         /* slice_type = I */
    bw_ue(&bw, 0);         /* pic_parameter_set_id */
    bw_u(&bw, frame_num, 4);
    bw_u(&bw, (uint32_t)field, 1);
    if (field) {
        bw_u(&bw, (uint32_t)bottom, 1);
    }
    bw_ue(&bw, idr_pic_id);
    bw_u(&bw, poc_lsb, 4);
    bw_u(&bw, 1, 1);       /* rbsp_stop_one_bit */
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    return bw_finish(&bw);
}

/* Non-IDR field/frame slice, pic_order_cnt_type 0 fast path. nal_byte:
 * 0x61 ref P, 0x41 ref I, 0x01 non-ref B. */
static size_t build_field_poc0(uint8_t *tmp, uint8_t nal_byte,
                               uint32_t slice_type, uint32_t first_mb,
                               uint32_t frame_num, int field, int bottom,
                               uint32_t poc_lsb)
{
    bitwriter_t bw;

    bw_init(&bw, tmp, 64);
    bw_u8(&bw, nal_byte);
    bw_ue(&bw, first_mb);
    bw_ue(&bw, slice_type);
    bw_ue(&bw, 0);         /* pic_parameter_set_id */
    bw_u(&bw, frame_num, 4);
    bw_u(&bw, (uint32_t)field, 1);
    if (field) {
        bw_u(&bw, (uint32_t)bottom, 1);
    }
    bw_u(&bw, poc_lsb, 4);
    bw_u(&bw, 1, 1);       /* rbsp_stop_one_bit */
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    return bw_finish(&bw);
}

/* IDR field slice for pic_order_cnt_type 1 (deep path): delta_pic_order_cnt[0],
 * slice_qp_delta and the two IDR dec_ref_pic_marking flags. */
static size_t build_idr1(uint8_t *tmp, uint32_t frame_num,
                         uint32_t idr_pic_id, int field, int bottom,
                         int32_t delta0)
{
    bitwriter_t bw;

    bw_init(&bw, tmp, 64);
    bw_u8(&bw, 0x65);
    bw_ue(&bw, 0);         /* first_mb_in_slice */
    bw_ue(&bw, 2);         /* slice_type = I */
    bw_ue(&bw, 0);         /* pic_parameter_set_id */
    bw_u(&bw, frame_num, 4);
    bw_u(&bw, (uint32_t)field, 1);
    if (field) {
        bw_u(&bw, (uint32_t)bottom, 1);
    }
    bw_ue(&bw, idr_pic_id);
    bw_se(&bw, delta0);
    bw_se(&bw, 0);         /* slice_qp_delta */
    bw_u(&bw, 0, 1);       /* no_output_of_prior_pics_flag */
    bw_u(&bw, 0, 1);       /* long_term_reference_flag */
    bw_u(&bw, 1, 1);       /* rbsp_stop_one_bit */
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    return bw_finish(&bw);
}

/* Reference P field slice for pic_order_cnt_type 1 (deep path, same tail as
 * build_p in test_poc.c: override off, empty l0 list modification, qp, no
 * adaptive marking). delta_pic_order_cnt[1] is NOT written: with
 * field_pic_flag = 1 it is not present even when pic_order_present (7.3.3). */
static size_t build_p1(uint8_t *tmp, uint32_t frame_num, int field,
                       int bottom, int32_t delta0)
{
    bitwriter_t bw;

    bw_init(&bw, tmp, 64);
    bw_u8(&bw, 0x61);
    bw_ue(&bw, 0);         /* first_mb_in_slice */
    bw_ue(&bw, 0);         /* slice_type = P */
    bw_ue(&bw, 0);         /* pic_parameter_set_id */
    bw_u(&bw, frame_num, 4);
    bw_u(&bw, (uint32_t)field, 1);
    if (field) {
        bw_u(&bw, (uint32_t)bottom, 1);
    }
    bw_se(&bw, delta0);
    bw_u(&bw, 0, 1);       /* num_ref_idx_active_override_flag */
    bw_ue(&bw, 3);         /* ref_pic_list_modification: stop */
    bw_se(&bw, 0);         /* slice_qp_delta */
    bw_u(&bw, 0, 1);       /* adaptive_ref_pic_marking_mode_flag */
    bw_u(&bw, 1, 1);       /* rbsp_stop_one_bit */
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    return bw_finish(&bw);
}

/*
 * (a) Field pairs are one picture. IDR field pair (its top field written as
 * two slices) + two P field pairs, PPS with pic_order_present = 1 so the
 * fields carry distinct poc_lsb (bottom = top + 1 in decode order):
 *   IDR  top lsb 0, bottom lsb 1  -> picture 0, POC 0 (top field's)
 *   P1   top lsb 2, bottom lsb 3  -> picture 1, POC 2
 *   P2   top lsb 4, bottom lsb 5  -> picture 2, POC 4
 * Six field AUs, three pictures; the second IDR field must NOT open a new
 * IDR segment (same idr_pic_id), so display indexes stay 0,1,2 and only
 * picture 0 is IDR. Picture 0's NAL range spans SPS, PPS and the three IDR
 * field slices.
 */
static void test_field_pair_is_one_picture(void)
{
    uint8_t stream[512];
    uint8_t nal[64];
    stream_index_t idx;
    size_t pos = 0, n;

    n = build_sps(nal, 0, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_pps(nal, 1);
    pos = put_nal(stream, pos, nal, n);
    n = build_field_idr(nal, 0, 0, 0, 1, 0, 0); /* top, first slice */
    pos = put_nal(stream, pos, nal, n);
    n = build_field_idr(nal, 1, 0, 0, 1, 0, 0); /* top, second slice */
    pos = put_nal(stream, pos, nal, n);
    n = build_field_idr(nal, 0, 0, 0, 1, 1, 1); /* bottom */
    pos = put_nal(stream, pos, nal, n);
    n = build_field_poc0(nal, 0x61, 0, 0, 1, 1, 0, 2);
    pos = put_nal(stream, pos, nal, n);
    n = build_field_poc0(nal, 0x61, 0, 0, 1, 1, 1, 3);
    pos = put_nal(stream, pos, nal, n);
    n = build_field_poc0(nal, 0x61, 0, 0, 2, 1, 0, 4);
    pos = put_nal(stream, pos, nal, n);
    n = build_field_poc0(nal, 0x61, 0, 0, 2, 1, 1, 5);
    pos = put_nal(stream, pos, nal, n);

    TEST_ASSERT_EQUAL_INT(0, index_build(&idx, stream, pos));
    TEST_ASSERT_EQUAL_INT(3, (int)idx.frame_count);
    TEST_ASSERT_EQUAL_INT(1, idx.has_fields);
    TEST_ASSERT_EQUAL_INT(1, idx.poc_supported);

    TEST_ASSERT_EQUAL_INT(1, idx.frames[0].is_idr);
    TEST_ASSERT_EQUAL_INT(0, idx.frames[1].is_idr);
    TEST_ASSERT_EQUAL_INT(0, idx.frames[2].is_idr);
    TEST_ASSERT_EQUAL_INT64(0, idx.frames[0].poc);
    TEST_ASSERT_EQUAL_INT64(2, idx.frames[1].poc);
    TEST_ASSERT_EQUAL_INT64(4, idx.frames[2].poc);
    TEST_ASSERT_EQUAL_INT64(0, idx.frames[0].display_index);
    TEST_ASSERT_EQUAL_INT64(1, idx.frames[1].display_index);
    TEST_ASSERT_EQUAL_INT64(2, idx.frames[2].display_index);

    /* NAL ranges cover the slices of BOTH fields (and the parametric NALs
     * in front of the pair): SPS PPS IDR-top IDR-top2 IDR-bot | P1t P1b |
     * P2t P2b. */
    TEST_ASSERT_EQUAL_INT(0, (int)idx.frames[0].nal_first);
    TEST_ASSERT_EQUAL_INT(4, (int)idx.frames[0].nal_last);
    TEST_ASSERT_EQUAL_INT(5, (int)idx.frames[1].nal_first);
    TEST_ASSERT_EQUAL_INT(6, (int)idx.frames[1].nal_last);
    TEST_ASSERT_EQUAL_INT(7, (int)idx.frames[2].nal_first);
    TEST_ASSERT_EQUAL_INT(8, (int)idx.frames[2].nal_last);
    index_free(&idx);
}

/*
 * (b) Fields of several pictures in B (decode) order: the display order is
 * the POC order of the PICTURES (top field POC), not of the fields.
 *   IDR  top 0 / bot 1   -> POC 0, anchor
 *   B    top 5 / bot 6   -> POC 5 (non-ref, above the anchor: not leading)
 *   P1   top 2 / bot 3   -> POC 2
 *   P2   top 8 / bot 9   -> POC 8
 * Sorted POCs 0,2,5,8 -> display_index 0,2,1,3 in decode order.
 */
static void test_field_display_order_b_pattern(void)
{
    uint8_t stream[512];
    uint8_t nal[64];
    stream_index_t idx;
    size_t pos = 0, n;
    static const long long poc[] = { 0, 5, 2, 8 };
    static const long long disp[] = { 0, 2, 1, 3 };
    size_t i;

    n = build_sps(nal, 0, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_pps(nal, 1);
    pos = put_nal(stream, pos, nal, n);
    n = build_field_idr(nal, 0, 0, 0, 1, 0, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_field_idr(nal, 0, 0, 0, 1, 1, 1);
    pos = put_nal(stream, pos, nal, n);
    n = build_field_poc0(nal, 0x01, 1, 0, 1, 1, 0, 5); /* B top, non-ref */
    pos = put_nal(stream, pos, nal, n);
    n = build_field_poc0(nal, 0x01, 1, 0, 1, 1, 1, 6); /* B bottom */
    pos = put_nal(stream, pos, nal, n);
    n = build_field_poc0(nal, 0x61, 0, 0, 2, 1, 0, 2); /* P1 top */
    pos = put_nal(stream, pos, nal, n);
    n = build_field_poc0(nal, 0x61, 0, 0, 2, 1, 1, 3); /* P1 bottom */
    pos = put_nal(stream, pos, nal, n);
    n = build_field_poc0(nal, 0x61, 0, 0, 3, 1, 0, 8); /* P2 top */
    pos = put_nal(stream, pos, nal, n);
    n = build_field_poc0(nal, 0x61, 0, 0, 3, 1, 1, 9); /* P2 bottom */
    pos = put_nal(stream, pos, nal, n);

    TEST_ASSERT_EQUAL_INT(0, index_build(&idx, stream, pos));
    TEST_ASSERT_EQUAL_INT(4, (int)idx.frame_count);
    TEST_ASSERT_EQUAL_INT(1, idx.has_fields);
    for (i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, idx.frames[i].parse_error,
                                      "unexpected parse error");
        TEST_ASSERT_EQUAL_INT64(poc[i], idx.frames[i].poc);
        TEST_ASSERT_EQUAL_INT64(disp[i], idx.frames[i].display_index);
    }
    /* The B picture sits above the anchor's POC: not a leading picture. */
    TEST_ASSERT_EQUAL_INT(0, idx.frames[1].leading_picture);
    index_free(&idx);
}

/*
 * (c) pic_order_cnt_type 1 with fields: the picture POC is the TopField-
 * OrderCnt, offset_for_top_to_bottom_field (= 2 here) belongs to the bottom
 * field's POC only (8.21.1: BottomFieldOrderCnt = TopFieldOrderCnt + 1 +
 * offset) and must never leak into the picture POC. The bottom field of the
 * second picture carries a bogus delta_pic_order_cnt[0] = +9 to prove that
 * only the TOP field's header defines the picture POC.
 * SPS: cycle {2}, offset_for_non_ref_pic 0 -> TopFieldOrderCnt = 2*FrameNum.
 *   IDR pair fn 0 -> POC 0; P1 pair fn 1 -> 2; P2 pair fn 2 -> 4.
 */
static void test_poc1_field_picture_poc_is_top(void)
{
    uint8_t stream[512];
    uint8_t nal[64];
    stream_index_t idx;
    size_t pos = 0, n;

    n = build_sps(nal, 1, 2);
    pos = put_nal(stream, pos, nal, n);
    n = build_pps(nal, 1);
    pos = put_nal(stream, pos, nal, n);
    n = build_idr1(nal, 0, 0, 1, 0, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_idr1(nal, 0, 0, 1, 1, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_p1(nal, 1, 1, 0, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_p1(nal, 1, 1, 1, 9); /* bottom: offset + bogus delta, ignored */
    pos = put_nal(stream, pos, nal, n);
    n = build_p1(nal, 2, 1, 0, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_p1(nal, 2, 1, 1, 0);
    pos = put_nal(stream, pos, nal, n);

    TEST_ASSERT_EQUAL_INT(0, index_build(&idx, stream, pos));
    TEST_ASSERT_EQUAL_INT(3, (int)idx.frame_count);
    TEST_ASSERT_EQUAL_INT(1, idx.poc_supported);
    TEST_ASSERT_EQUAL_INT(1, idx.has_fields);
    TEST_ASSERT_EQUAL_INT64(0, idx.frames[0].poc);
    /* Not 2 + 1 + 2 = 5 (bottom POC) and not 2 + 9 (bottom's delta). */
    TEST_ASSERT_EQUAL_INT64(2, idx.frames[1].poc);
    TEST_ASSERT_EQUAL_INT64(4, idx.frames[2].poc);
    TEST_ASSERT_EQUAL_INT64(0, idx.frames[0].display_index);
    TEST_ASSERT_EQUAL_INT64(1, idx.frames[1].display_index);
    TEST_ASSERT_EQUAL_INT64(2, idx.frames[2].display_index);
    index_free(&idx);
}

/*
 * (d) Orphan fields (truncated pairs) are pictures of their own and parse
 * cleanly: an IDR top field, a lone P top field, a lone B bottom field.
 * Nothing here opens a second IDR segment, display order is 0,1,2.
 */
static void test_orphan_fields(void)
{
    uint8_t stream[512];
    uint8_t nal[64];
    stream_index_t idx;
    size_t pos = 0, n;

    n = build_sps(nal, 0, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_pps(nal, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_field_idr(nal, 0, 0, 0, 1, 0, 0); /* IDR top, bottom missing */
    pos = put_nal(stream, pos, nal, n);
    n = build_field_poc0(nal, 0x61, 0, 0, 1, 1, 0, 2); /* lone top */
    pos = put_nal(stream, pos, nal, n);
    n = build_field_poc0(nal, 0x01, 1, 0, 2, 1, 1, 3); /* lone bottom */
    pos = put_nal(stream, pos, nal, n);

    TEST_ASSERT_EQUAL_INT(0, index_build(&idx, stream, pos));
    TEST_ASSERT_EQUAL_INT(3, (int)idx.frame_count);
    TEST_ASSERT_EQUAL_INT(1, idx.has_fields);
    TEST_ASSERT_EQUAL_INT64(0, idx.frames[0].poc);
    TEST_ASSERT_EQUAL_INT64(2, idx.frames[1].poc);
    TEST_ASSERT_EQUAL_INT64(3, idx.frames[2].poc);
    TEST_ASSERT_EQUAL_INT64(0, idx.frames[0].display_index);
    TEST_ASSERT_EQUAL_INT64(1, idx.frames[1].display_index);
    TEST_ASSERT_EQUAL_INT64(2, idx.frames[2].display_index);
    TEST_ASSERT_EQUAL_INT(1, idx.frames[0].is_idr);
    TEST_ASSERT_EQUAL_INT(0, idx.frames[2].leading_picture);
    index_free(&idx);
}

/*
 * (e1) POC type 0 wrap (half-range guard) with fields: MaxPicOrderCntLsb 16,
 * pairs (l sb top/bot) 0/1 (IDR), 4/5, 6/7, 14/15, 0/1 (forward wrap), 2/3.
 * Top parity: 14 -> 0 is a drop of 14 >= 8 -> msb += 16; bottom parity wraps
 * on its own state as well (15 -> 1). Picture POCs must stay monotonic:
 * 0, 4, 6, 14, 16, 18 (a single shared state could only reach 16 by luck,
 * see e2 for the case that tells the two apart).
 */
static void test_field_poc_wrap(void)
{
    uint8_t stream[512];
    uint8_t nal[64];
    stream_index_t idx;
    size_t pos = 0, n;
    static const long long poc[] = { 0, 4, 6, 14, 16, 18 };
    static const uint32_t fn[] = { 0, 1, 2, 3, 4, 5 };
    static const uint32_t lsb_top[] = { 0, 4, 6, 14, 0, 2 };
    size_t i;

    n = build_sps(nal, 0, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_pps(nal, 1);
    pos = put_nal(stream, pos, nal, n);
    for (i = 0; i < 6; i++) {
        if (i == 0) {
            n = build_field_idr(nal, 0, fn[i], 0, 1, 0, lsb_top[i]);
            pos = put_nal(stream, pos, nal, n);
            n = build_field_idr(nal, 0, fn[i], 0, 1, 1, lsb_top[i] + 1);
        } else {
            n = build_field_poc0(nal, 0x61, 0, 0, fn[i], 1, 0, lsb_top[i]);
            pos = put_nal(stream, pos, nal, n);
            n = build_field_poc0(nal, 0x61, 0, 0, fn[i], 1, 1,
                                 lsb_top[i] + 1);
        }
        pos = put_nal(stream, pos, nal, n);
    }

    TEST_ASSERT_EQUAL_INT(0, index_build(&idx, stream, pos));
    TEST_ASSERT_EQUAL_INT(6, (int)idx.frame_count);
    for (i = 0; i < 6; i++) {
        TEST_ASSERT_EQUAL_INT64(poc[i], idx.frames[i].poc);
        TEST_ASSERT_EQUAL_INT64((long long)i, idx.frames[i].display_index);
    }
    index_free(&idx);
}

/*
 * (e2) The wrap state is per field parity, not shared. MaxPicOrderCntLsb 16,
 * pairs 0/1 (IDR), 6/7, 8/9; then a LONE BOTTOM field with poc_lsb 1: its
 * parity's previous lsb is 9, so 9 - 1 = 8 >= 16/2 wraps the msb and the
 * field lands at POC 17. With a single shared state the previous lsb would
 * be 8 (the last top field): 8 - 1 = 7 < 8, no wrap, POC 1 - and the orphan
 * would sort in front of the whole stream.
 */
static void test_field_poc_wrap_per_parity(void)
{
    uint8_t stream[512];
    uint8_t nal[64];
    stream_index_t idx;
    size_t pos = 0, n;

    n = build_sps(nal, 0, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_pps(nal, 1);
    pos = put_nal(stream, pos, nal, n);
    n = build_field_idr(nal, 0, 0, 0, 1, 0, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_field_idr(nal, 0, 0, 0, 1, 1, 1);
    pos = put_nal(stream, pos, nal, n);
    n = build_field_poc0(nal, 0x61, 0, 0, 1, 1, 0, 6);
    pos = put_nal(stream, pos, nal, n);
    n = build_field_poc0(nal, 0x61, 0, 0, 1, 1, 1, 7);
    pos = put_nal(stream, pos, nal, n);
    n = build_field_poc0(nal, 0x61, 0, 0, 2, 1, 0, 8);
    pos = put_nal(stream, pos, nal, n);
    n = build_field_poc0(nal, 0x61, 0, 0, 2, 1, 1, 9);
    pos = put_nal(stream, pos, nal, n);
    n = build_field_poc0(nal, 0x61, 0, 0, 3, 1, 1, 1); /* lone bottom */
    pos = put_nal(stream, pos, nal, n);

    TEST_ASSERT_EQUAL_INT(0, index_build(&idx, stream, pos));
    TEST_ASSERT_EQUAL_INT(4, (int)idx.frame_count);
    TEST_ASSERT_EQUAL_INT64(0, idx.frames[0].poc);
    TEST_ASSERT_EQUAL_INT64(6, idx.frames[1].poc);
    TEST_ASSERT_EQUAL_INT64(8, idx.frames[2].poc);
    TEST_ASSERT_EQUAL_INT64(17, idx.frames[3].poc);
    TEST_ASSERT_EQUAL_INT64(0, idx.frames[0].display_index);
    TEST_ASSERT_EQUAL_INT64(1, idx.frames[1].display_index);
    TEST_ASSERT_EQUAL_INT64(2, idx.frames[2].display_index);
    TEST_ASSERT_EQUAL_INT64(3, idx.frames[3].display_index);
    index_free(&idx);
}

/*
 * (f) MBAFF regression: frame_mbs_only_flag = 0 but every slice is a whole
 * frame (field_pic_flag = 0) -> pictures are 1:1 coded pictures, exactly as
 * in a progressive stream (IDR, P with two slices, non-ref B, P; POC type 0).
 */
static void test_mbff_regression(void)
{
    uint8_t stream[512];
    uint8_t nal[64];
    stream_index_t idx;
    size_t pos = 0, n;
    static const long long poc[] = { 0, 4, 2, 8 };
    static const long long disp[] = { 0, 2, 1, 3 };
    size_t i;

    n = build_sps(nal, 0, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_pps(nal, 1);
    pos = put_nal(stream, pos, nal, n);
    n = build_field_idr(nal, 0, 0, 0, 0, 0, 0);        /* IDR frame */
    pos = put_nal(stream, pos, nal, n);
    n = build_field_poc0(nal, 0x61, 0, 0, 1, 0, 0, 4); /* P slice 1 */
    pos = put_nal(stream, pos, nal, n);
    n = build_field_poc0(nal, 0x61, 0, 1, 1, 0, 0, 4); /* P slice 2 */
    pos = put_nal(stream, pos, nal, n);
    n = build_field_poc0(nal, 0x01, 1, 0, 2, 0, 0, 2); /* non-ref B */
    pos = put_nal(stream, pos, nal, n);
    n = build_field_poc0(nal, 0x61, 0, 0, 3, 0, 0, 8);
    pos = put_nal(stream, pos, nal, n);

    TEST_ASSERT_EQUAL_INT(0, index_build(&idx, stream, pos));
    TEST_ASSERT_EQUAL_INT(4, (int)idx.frame_count);
    TEST_ASSERT_EQUAL_INT(0, idx.has_fields);
    TEST_ASSERT_EQUAL_INT(1, idx.poc_supported);
    for (i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, idx.frames[i].parse_error,
                                      "unexpected parse error");
        TEST_ASSERT_EQUAL_INT64(poc[i], idx.frames[i].poc);
        TEST_ASSERT_EQUAL_INT64(disp[i], idx.frames[i].display_index);
    }
    /* The two P slices stayed in one picture (SPS PPS IDR P P B P). */
    TEST_ASSERT_EQUAL_INT(3, (int)idx.frames[1].nal_first);
    TEST_ASSERT_EQUAL_INT(4, (int)idx.frames[1].nal_last);
    index_free(&idx);
}

/*
 * (g) An IDR field pair followed by a normal P field pair: the second field
 * of the IDR (NAL type 5, same idr_pic_id) must not open a second IDR
 * segment - display indexes are contiguous across the whole stream, and the
 * IDR picture's NAL range ends at its bottom field.
 */
static void test_idr_field_pair_single_segment(void)
{
    uint8_t stream[512];
    uint8_t nal[64];
    stream_index_t idx;
    size_t pos = 0, n;

    n = build_sps(nal, 0, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_pps(nal, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_field_idr(nal, 0, 0, 7, 1, 0, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_field_idr(nal, 0, 0, 7, 1, 1, 1);
    pos = put_nal(stream, pos, nal, n);
    n = build_field_poc0(nal, 0x61, 0, 0, 1, 1, 0, 2);
    pos = put_nal(stream, pos, nal, n);
    n = build_field_poc0(nal, 0x61, 0, 0, 1, 1, 1, 3);
    pos = put_nal(stream, pos, nal, n);
    /* A second IDR (different idr_pic_id) does open a new segment. */
    n = build_field_idr(nal, 0, 0, 8, 1, 0, 0);
    pos = put_nal(stream, pos, nal, n);

    TEST_ASSERT_EQUAL_INT(0, index_build(&idx, stream, pos));
    TEST_ASSERT_EQUAL_INT(3, (int)idx.frame_count);
    TEST_ASSERT_EQUAL_INT(1, idx.frames[0].is_idr);
    TEST_ASSERT_EQUAL_INT(0, idx.frames[1].is_idr);
    TEST_ASSERT_EQUAL_INT(1, idx.frames[2].is_idr);
    TEST_ASSERT_EQUAL_INT64(0, idx.frames[0].display_index);
    TEST_ASSERT_EQUAL_INT64(1, idx.frames[1].display_index);
    /* New IDR -> new segment, its display index restarts after the segment. */
    TEST_ASSERT_EQUAL_INT64(2, idx.frames[2].display_index);
    index_free(&idx);
}

int main(void)
{
    UnityBegin("test_interlace.c");
    RUN_TEST(test_field_pair_is_one_picture);
    RUN_TEST(test_field_display_order_b_pattern);
    RUN_TEST(test_poc1_field_picture_poc_is_top);
    RUN_TEST(test_orphan_fields);
    RUN_TEST(test_field_poc_wrap);
    RUN_TEST(test_field_poc_wrap_per_parity);
    RUN_TEST(test_mbff_regression);
    RUN_TEST(test_idr_field_pair_single_segment);
    return UnityEnd();
}
