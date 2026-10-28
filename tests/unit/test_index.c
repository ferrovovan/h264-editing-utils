/*
 * test_index.c - index_build() tests for open GOP detection (recovery point
 * SEI -> usable anchors, leading pictures) and SEI picture timing clock
 * timestamps (day-wrap unwrapping, clock_timing aggregation) on fully
 * synthetic Annex B streams.
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

#define MAX_POC_LSB 16 /* log2_max_pic_order_cnt_lsb_minus4 = 0 in every SPS */
#define DAY90 ((uint64_t)90000 * 24 * 3600)

static size_t put_nal(uint8_t *stream, size_t pos, const uint8_t *nal, size_t n)
{
    stream[pos++] = 0x00;
    stream[pos++] = 0x00;
    stream[pos++] = 0x01;
    memcpy(stream + pos, nal, n);
    return pos + n;
}

/* Main profile (77), frame_mbs_only, no VUI; poc type 0,
 * MaxPicOrderCntLsb = 16 (see MAX_POC_LSB). */
static size_t build_sps(uint8_t *tmp)
{
    bitwriter_t bw;

    bw_init(&bw, tmp, 64);
    bw_u8(&bw, 0x67);      /* nal header: ref_idc 3, type 7 */
    bw_u8(&bw, 77);        /* profile_idc */
    bw_u8(&bw, 0);         /* constraint flags + reserved */
    bw_u8(&bw, 30);        /* level_idc */
    bw_ue(&bw, 0);         /* seq_parameter_set_id */
    bw_ue(&bw, 0);         /* log2_max_frame_num_minus4 -> MaxFrameNum 16 */
    bw_ue(&bw, 0);         /* pic_order_cnt_type = 0 */
    bw_ue(&bw, 0);         /* log2_max_pic_order_cnt_lsb_minus4 -> 16 */
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

/* CAVLC PPS 0 on SPS 0 (same shape as test_poc.c). */
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
    bw_u(&bw, 1, 1);       /* rbsp_stop_one_bit */
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    return bw_finish(&bw);
}

/* IDR slice (nal header 0x65), poc type 0 with the given poc_lsb. */
static size_t build_idr(uint8_t *tmp, uint32_t poc_lsb)
{
    bitwriter_t bw;

    bw_init(&bw, tmp, 64);
    bw_u8(&bw, 0x65);
    bw_ue(&bw, 0);         /* first_mb_in_slice */
    bw_ue(&bw, 2);         /* slice_type = I */
    bw_ue(&bw, 0);         /* pic_parameter_set_id */
    bw_u(&bw, 0, 4);       /* frame_num */
    bw_ue(&bw, 0);         /* idr_pic_id */
    bw_u(&bw, poc_lsb, 4); /* pic_order_cnt_lsb */
    bw_u(&bw, 1, 1);       /* rbsp_stop_one_bit */
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    return bw_finish(&bw);
}

/* Reference P slice (nal header 0x61, nal_ref_idc 3). */
static size_t build_p(uint8_t *tmp, uint32_t frame_num, uint32_t poc_lsb)
{
    bitwriter_t bw;

    bw_init(&bw, tmp, 64);
    bw_u8(&bw, 0x61);
    bw_ue(&bw, 0);         /* first_mb_in_slice */
    bw_ue(&bw, 0);         /* slice_type = P */
    bw_ue(&bw, 0);         /* pic_parameter_set_id */
    bw_u(&bw, frame_num, 4);
    bw_u(&bw, poc_lsb, 4);
    bw_u(&bw, 1, 1);       /* rbsp_stop_one_bit */
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    return bw_finish(&bw);
}

/* Non-reference B slice (nal header 0x01, nal_ref_idc 0). */
static size_t build_b_nonref(uint8_t *tmp, uint32_t frame_num,
                             uint32_t poc_lsb)
{
    bitwriter_t bw;

    bw_init(&bw, tmp, 64);
    bw_u8(&bw, 0x01);
    bw_ue(&bw, 0);         /* first_mb_in_slice */
    bw_ue(&bw, 1);         /* slice_type = B */
    bw_ue(&bw, 0);         /* pic_parameter_set_id */
    bw_u(&bw, frame_num, 4);
    bw_u(&bw, poc_lsb, 4);
    bw_u(&bw, 1, 1);       /* rbsp_stop_one_bit */
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    return bw_finish(&bw);
}

/* Insert emulation_prevention_three_byte after two zero bytes (RBSP->EBSP). */
static size_t ebsp(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t i, o = 0;
    int zeros = 0;

    for (i = 0; i < n; i++) {
        if (zeros >= 2 && in[i] <= 3) {
            out[o++] = 3;
            zeros = 0;
        }
        out[o++] = in[i];
        zeros = in[i] == 0 ? zeros + 1 : 0;
    }
    return o;
}

/* sei_recovery_point (payloadType 6) SEI NAL: 06 06 01 <payload> 80.
 * The caller's payload bits are completed with rbsp_stop_one_bit inside
 * the last payload byte (zero padding), then rbsp_trailing_bits (0x80)
 * follows - exactly what x264 writes for recovery_point (06 06 01 c4 80). */
static size_t build_sei_recovery(uint8_t *nal, uint32_t recovery_frame_cnt,
                                 int exact_match_flag, int broken_link_flag)
{
    bitwriter_t bw;
    uint8_t esc_buf[32];
    size_t pay_len, esc_len;

    bw_init(&bw, nal + 3, 16); /* payload straight after header/type/size */
    bw_ue(&bw, recovery_frame_cnt);
    bw_u(&bw, (uint32_t)exact_match_flag, 1);
    bw_u(&bw, (uint32_t)broken_link_flag, 1);
    bw_u(&bw, 0, 2);           /* changing_slice_group_idc */
    bw_u(&bw, 1, 1);           /* rbsp_stop_one_bit (inside the last byte) */
    pay_len = bw_finish(&bw);
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    nal[3 + pay_len] = 0x80;   /* rbsp_trailing_bits */
    esc_len = ebsp(nal + 3, pay_len + 1, esc_buf);
    memmove(nal + 3, esc_buf, esc_len);
    nal[0] = 0x06;             /* nal header */
    nal[1] = 0x06;             /* payloadType: recovery_point */
    nal[2] = (uint8_t)pay_len; /* payloadSize */
    return 3 + esc_len;
}

/* sei_picture_timing (payloadType 1) SEI NAL, classic clock_timestamp
 * variant: clock_timestamp_flag=1, cnt_type=0, n_90kHz_flag=0,
 * time (24-bit 90kHz part) + seconds/minutes/hours. pic_struct absent
 * (the synthetic SPS has no VUI). */
static size_t build_sei_picture_timing(uint8_t *nal, uint32_t ts24,
                                       uint32_t seconds, uint32_t minutes,
                                       uint32_t hours)
{
    bitwriter_t bw;
    uint8_t esc_buf[32];
    size_t pay_len, esc_len;

    bw_init(&bw, nal + 3, 16);
    bw_u(&bw, 1, 1);   /* clock_timestamp_flag */
    bw_u(&bw, 0, 2);   /* cnt_type = 0 */
    bw_u(&bw, 0, 1);   /* n_90kHz_flag = 0 */
    bw_u(&bw, ts24, 24);
    bw_u(&bw, seconds, 6);
    bw_u(&bw, minutes, 6);
    bw_u(&bw, hours, 5);
    bw_u(&bw, 1, 1);   /* rbsp_stop_one_bit (inside the last byte) */
    pay_len = bw_finish(&bw);
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    nal[3 + pay_len] = 0x80;
    esc_len = ebsp(nal + 3, pay_len + 1, esc_buf);
    memmove(nal + 3, esc_buf, esc_len);
    nal[0] = 0x06;
    nal[1] = 0x01;             /* payloadType: picture_timing */
    nal[2] = (uint8_t)pay_len;
    return 3 + esc_len;
}

/*
 * (a) Recovery point SEI attached to frames. Layout:
 *   SPS PPS SEI(cnt 0, exact 1, broken 0) IDR   -> frame 0: recovery point
 *                                              on an IDR; usable anchor.
 *   SEI(cnt 3) P(fn 1, lsb 4)                 -> frame 1: recovery point
 *                                              not usable to cut from.
 */
static void test_recovery_point_anchor(void)
{
    uint8_t stream[512];
    uint8_t nal[64];
    stream_index_t idx;
    size_t pos = 0, n;

    n = build_sps(nal);
    pos = put_nal(stream, pos, nal, n);
    n = build_pps(nal);
    pos = put_nal(stream, pos, nal, n);
    n = build_sei_recovery(nal, 0, 1, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_idr(nal, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_sei_recovery(nal, 3, 1, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_p(nal, 1, 4);
    pos = put_nal(stream, pos, nal, n);

    TEST_ASSERT_EQUAL_INT(0, index_build(&idx, stream, pos));
    TEST_ASSERT_EQUAL_INT(2, (int)idx.frame_count);

    /* Frame 0: SEI attached to the IDR: recovery point and usable anchor
     * (the is_idr rule wins; the cnt-0 rule holds as well). */
    TEST_ASSERT_EQUAL_INT(1, idx.frames[0].is_idr);
    TEST_ASSERT_EQUAL_INT(1, idx.frames[0].recovery_point);
    TEST_ASSERT_EQUAL_UINT32(0, idx.frames[0].recovery_frame_cnt);
    TEST_ASSERT_EQUAL_INT(1, idx.frames[0].exact_match_flag);
    TEST_ASSERT_EQUAL_INT(0, idx.frames[0].broken_link_flag);
    TEST_ASSERT_EQUAL_INT(1, idx.frames[0].usable_anchor);

    /* Frame 1: recovery_frame_cnt != 0 -> not usable as a cut anchor. */
    TEST_ASSERT_EQUAL_INT(0, idx.frames[1].is_idr);
    TEST_ASSERT_EQUAL_INT(1, idx.frames[1].recovery_point);
    TEST_ASSERT_EQUAL_UINT32(3, idx.frames[1].recovery_frame_cnt);
    TEST_ASSERT_EQUAL_INT(0, idx.frames[1].usable_anchor);
    TEST_ASSERT_EQUAL_INT(0, idx.frames[1].leading_picture);
    TEST_ASSERT_EQUAL_INT64(0, idx.frames[0].poc);
    TEST_ASSERT_EQUAL_INT64(4, idx.frames[1].poc);
    index_free(&idx);
}

/*
 * (b) Leading picture after an IDR. SPS MaxPicOrderCntLsb = 16. Layout:
 *   IDR(lsb 0)          -> POC 0, anchor.
 *   B ref_idc 0 lsb 15  -> half-range guard: 15 - 16 -> POC -1 < 0 and
 *                          POC < anchor -> leading picture.
 *   P  ref_idc 3 lsb 1  -> wraps forward: msb -16 + 16, POC 1 -> not
 *                          leading; display order (-1, 0, 1) proves that
 *                          negative POCs keep the display order computable.
 */
static void test_leading_picture_after_idr(void)
{
    uint8_t stream[256];
    uint8_t nal[64];
    stream_index_t idx;
    size_t pos = 0, n;

    n = build_sps(nal);
    pos = put_nal(stream, pos, nal, n);
    n = build_pps(nal);
    pos = put_nal(stream, pos, nal, n);
    n = build_idr(nal, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_b_nonref(nal, 1, MAX_POC_LSB - 1);
    pos = put_nal(stream, pos, nal, n);
    n = build_p(nal, 2, 1);
    pos = put_nal(stream, pos, nal, n);

    TEST_ASSERT_EQUAL_INT(0, index_build(&idx, stream, pos));
    TEST_ASSERT_EQUAL_INT(3, (int)idx.frame_count);
    TEST_ASSERT_EQUAL_INT(1, idx.poc_supported);

    TEST_ASSERT_EQUAL_INT64(0, idx.frames[0].poc);
    TEST_ASSERT_EQUAL_INT(0, idx.frames[0].leading_picture);
    TEST_ASSERT_EQUAL_INT(1, idx.frames[0].usable_anchor);

    TEST_ASSERT_TRUE_MESSAGE(idx.frames[1].poc < 0,
                             "type 0 half-range guard must yield a negative POC");
    TEST_ASSERT_EQUAL_INT64(-1, idx.frames[1].poc); /* 15 - 16 */
    TEST_ASSERT_EQUAL_INT(1, idx.frames[1].leading_picture);

    TEST_ASSERT_EQUAL_INT64(1, idx.frames[2].poc);
    TEST_ASSERT_EQUAL_INT(0, idx.frames[2].leading_picture);

    /* Display order inside the IDR segment: -1, 0, 1. */
    TEST_ASSERT_EQUAL_INT64(1, idx.frames[0].display_index);
    TEST_ASSERT_EQUAL_INT64(0, idx.frames[1].display_index);
    TEST_ASSERT_EQUAL_INT64(2, idx.frames[2].display_index);
    index_free(&idx);
}

/*
 * (c) Day wrap of raw clock timestamps. IDR carries no SEI; the two
 * following pictures carry cnt_type 0 clock timestamps DAY90 - 1000 and
 * 500 (the clock restarted at 0). The unwrap adds one day to the base, so
 * the absolute clock_ts stay monotonic across the day boundary.
 */
static void test_clock_ts_day_wrap(void)
{
    uint8_t stream[256];
    uint8_t nal[64];
    stream_index_t idx;
    size_t pos = 0, n;

    n = build_sps(nal);
    pos = put_nal(stream, pos, nal, n);
    n = build_pps(nal);
    pos = put_nal(stream, pos, nal, n);
    n = build_idr(nal, 0);
    pos = put_nal(stream, pos, nal, n);
    /* h 23 m 59 s 59: 86399 * 90000 + 89000 = DAY90 - 1000. */
    n = build_sei_picture_timing(nal, 89000, 59, 59, 23);
    pos = put_nal(stream, pos, nal, n);
    n = build_p(nal, 1, 2);
    pos = put_nal(stream, pos, nal, n);
    /* h 0 m 0 s 0, ts24 500: the clock restarted past midnight. */
    n = build_sei_picture_timing(nal, 500, 0, 0, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_p(nal, 2, 4);
    pos = put_nal(stream, pos, nal, n);

    TEST_ASSERT_EQUAL_INT(0, index_build(&idx, stream, pos));
    TEST_ASSERT_EQUAL_INT(3, (int)idx.frame_count);

    TEST_ASSERT_EQUAL_INT(0, idx.frames[0].has_clock_ts);
    TEST_ASSERT_EQUAL_INT(1, idx.frames[1].has_clock_ts);
    TEST_ASSERT_EQUAL_INT(1, idx.frames[2].has_clock_ts);
    TEST_ASSERT_EQUAL_UINT64(DAY90 - 1000, idx.frames[1].clock_ts_90k);
    TEST_ASSERT_EQUAL_UINT64(DAY90 + 500, idx.frames[2].clock_ts_90k);
    TEST_ASSERT_TRUE(idx.frames[2].clock_ts_90k > idx.frames[1].clock_ts_90k);
    /* Not every frame has a timestamp. */
    TEST_ASSERT_EQUAL_INT(0, idx.clock_timing);
    index_free(&idx);
}

/*
 * (d) clock_timing = 1 only when every frame has a clock timestamp: same
 * stream as (c) with an extra picture_timing SEI in front of the IDR.
 */
static void test_clock_timing_all_frames(void)
{
    uint8_t stream[256];
    uint8_t nal[64];
    stream_index_t idx;
    size_t pos = 0, n;

    n = build_sps(nal);
    pos = put_nal(stream, pos, nal, n);
    n = build_pps(nal);
    pos = put_nal(stream, pos, nal, n);
    n = build_sei_picture_timing(nal, 100, 0, 0, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_idr(nal, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_sei_picture_timing(nal, 200, 0, 0, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_p(nal, 1, 2);
    pos = put_nal(stream, pos, nal, n);
    n = build_sei_picture_timing(nal, 300, 0, 0, 0);
    pos = put_nal(stream, pos, nal, n);
    n = build_p(nal, 2, 4);
    pos = put_nal(stream, pos, nal, n);

    TEST_ASSERT_EQUAL_INT(0, index_build(&idx, stream, pos));
    TEST_ASSERT_EQUAL_INT(3, (int)idx.frame_count);
    TEST_ASSERT_EQUAL_INT(1, idx.frames[0].has_clock_ts);
    TEST_ASSERT_EQUAL_INT(1, idx.clock_timing);
    TEST_ASSERT_EQUAL_UINT64(100, idx.frames[0].clock_ts_90k);
    TEST_ASSERT_EQUAL_UINT64(200, idx.frames[1].clock_ts_90k);
    TEST_ASSERT_EQUAL_UINT64(300, idx.frames[2].clock_ts_90k);
    index_free(&idx);
}

int main(void)
{
    UnityBegin("test_index.c");
    RUN_TEST(test_recovery_point_anchor);
    RUN_TEST(test_leading_picture_after_idr);
    RUN_TEST(test_clock_ts_day_wrap);
    RUN_TEST(test_clock_timing_all_frames);
    return UnityEnd();
}
