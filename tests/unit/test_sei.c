/*
 * test_sei.c - unit tests for the SEI iterator and payload parsers.
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

#include <string.h>

#include "unity.h"

#include "sei.h"

void setUp(void)
{
}

void tearDown(void)
{
}

/* RBSP with zero usable bytes: the iterator must simply stop. */
static void test_iter_empty_rbsp(void)
{
    static const uint8_t rbsp[] = { 0x00 }; /* never read: size 0 */
    sei_iter_t it;
    sei_msg_t msg;

    sei_iter_init(&it, rbsp, 0);
    TEST_ASSERT_EQUAL_INT(0, sei_iter_next(&it, &msg));

    /* Stop bit of rbsp_trailing_bits alone: 0x80 would be a payloadType
     * with no payloadSize behind it -> stop, no phantom message. */
    sei_iter_init(&it, rbsp, 1);
    TEST_ASSERT_EQUAL_INT(0, sei_iter_next(&it, &msg));
}

/* payloadType and payloadSize FF-extension per 7.3.2.3.1. */
static void test_iter_ff_extension(void)
{
    /* one message: type = 255 + 0x06 = 261, size 1, payload 0xAB */
    static const uint8_t one[] = {
        0xFF, /* payloadType: 0xFF -> +255 */
        0x06, /* payloadType: 0x06 -> +6, total 261 */
        0x01, /* payloadSize: 1 */
        0xAB  /* payload */
    };
    /* size header = 0xFF 0x08 -> 263, but only 3 payload bytes follow:
     * FF-run parses fine, then the payload does not fit -> clean stop. */
    static const uint8_t truncated[] = {
        0x01, /* payloadType: 1 (picture timing) */
        0xFF, /* payloadSize: 0xFF -> +255 */
        0x08, /* payloadSize: 0x08 -> +8, total 263 */
        0xAA, 0xBB, 0xCC
    };
    /* RBSP of pure 0xFF: type run never terminates cleanly. */
    static const uint8_t ff_only[] = { 0xFF, 0xFF, 0xFF };
    sei_iter_t it;
    sei_msg_t msg;

    sei_iter_init(&it, one, sizeof(one));
    TEST_ASSERT_EQUAL_INT(1, sei_iter_next(&it, &msg));
    TEST_ASSERT_EQUAL_UINT32(261, msg.type);
    TEST_ASSERT_EQUAL_UINT64(1, (uint64_t)msg.size);
    TEST_ASSERT_EQUAL_UINT8(0xAB, msg.payload[0]);
    TEST_ASSERT_EQUAL_INT(0, sei_iter_next(&it, &msg));

    sei_iter_init(&it, truncated, sizeof(truncated));
    TEST_ASSERT_EQUAL_INT(0, sei_iter_next(&it, &msg));

    sei_iter_init(&it, ff_only, sizeof(ff_only));
    TEST_ASSERT_EQUAL_INT(0, sei_iter_next(&it, &msg));
}

/* payloadType 0 (buffering period) must be iterated over, type 6 found. */
static void test_iter_skip_type0_find_type6(void)
{
    static const uint8_t rbsp[] = {
        /* message 1: buffering period, type 0, size 1 */
        0x00, 0x01, 0x0F,
        /* message 2: recovery point, type 6, size 1, payload 0x72 */
        0x06, 0x01, 0x72
    };
    sei_iter_t it;
    sei_msg_t msg;

    sei_iter_init(&it, rbsp, sizeof(rbsp));
    TEST_ASSERT_EQUAL_INT(1, sei_iter_next(&it, &msg));
    TEST_ASSERT_EQUAL_UINT32(SEI_PAYLOAD_BUFFERING_PERIOD, msg.type);
    TEST_ASSERT_EQUAL_UINT64(1, (uint64_t)msg.size);

    TEST_ASSERT_EQUAL_INT(1, sei_iter_next(&it, &msg));
    TEST_ASSERT_EQUAL_UINT32(SEI_PAYLOAD_RECOVERY_POINT, msg.type);
    TEST_ASSERT_EQUAL_UINT64(1, (uint64_t)msg.size);
    TEST_ASSERT_EQUAL_UINT8(0x72, msg.payload[0]);

    TEST_ASSERT_EQUAL_INT(0, sei_iter_next(&it, &msg));
}

/*
 * Trailing bytes after the last message must not spawn a phantom message:
 * a lone 0x80 (rbsp stop bit pattern) has no payloadSize byte; 0x00 0x00
 * decodes to a zero-size payload, treated as garbage -> stop.
 */
static void test_iter_no_phantom_from_trailing(void)
{
    static const uint8_t stop_bit[] = {
        0x06, 0x01, 0x72, /* recovery point */
        0x80              /* rbsp_trailing_bits stop pattern */
    };
    static const uint8_t zeros[] = {
        0x06, 0x01, 0x72, /* recovery point */
        0x00, 0x00        /* trailing zero_8bits-like garbage */
    };
    sei_iter_t it;
    sei_msg_t msg;

    sei_iter_init(&it, stop_bit, sizeof(stop_bit));
    TEST_ASSERT_EQUAL_INT(1, sei_iter_next(&it, &msg));
    TEST_ASSERT_EQUAL_UINT32(SEI_PAYLOAD_RECOVERY_POINT, msg.type);
    TEST_ASSERT_EQUAL_INT(0, sei_iter_next(&it, &msg));

    sei_iter_init(&it, zeros, sizeof(zeros));
    TEST_ASSERT_EQUAL_INT(1, sei_iter_next(&it, &msg));
    TEST_ASSERT_EQUAL_INT(0, sei_iter_next(&it, &msg));
}

/*
 * Golden recovery_point payload (spec D.1.7, 2003 numbering):
 * recovery_frame_cnt=2 -> ue(v) "011", exact_match=1, broken_link=0,
 * changing_slice_group_idc=0b01:
 *
 *   011 1 0 01 (+pad 0) = 0b01110010 = 0x72
 */
static void test_recovery_point_golden(void)
{
    static const uint8_t payload[] = {
        0x72 /* bits: ue(2)="011" exact=1 broken=0 idc=0b01 pad=0 */
    };
    sei_msg_t msg;
    sei_recovery_point_t rp;

    memset(&msg, 0, sizeof(msg));
    msg.type = SEI_PAYLOAD_RECOVERY_POINT;
    msg.payload = payload;
    msg.size = sizeof(payload);

    TEST_ASSERT_EQUAL_INT(0, sei_parse_recovery_point(&msg, &rp));
    TEST_ASSERT_EQUAL_UINT32(2, rp.recovery_frame_cnt);
    TEST_ASSERT_EQUAL_INT(1, rp.exact_match_flag);
    TEST_ASSERT_EQUAL_INT(0, rp.broken_link_flag);
    TEST_ASSERT_EQUAL_UINT32(0x01, rp.changing_slice_group_idc);
}

/* 0x06 = "00000110": ue prefix consumes 6 bits then runs out of payload. */
static void test_recovery_point_truncated(void)
{
    static const uint8_t payload[] = { 0x06 };
    sei_msg_t msg;
    sei_recovery_point_t rp;

    memset(&msg, 0, sizeof(msg));
    msg.payload = payload;
    msg.size = sizeof(payload);
    TEST_ASSERT_EQUAL_INT(-1, sei_parse_recovery_point(&msg, &rp));
}

/*
 * picture_timing, pic_struct_present = 0 (so no pic_struct field):
 *
 *   clock_timestamp_flag = 1
 *   cnt_type             = 00, n_90kHz_flag = 0
 *   clock_timestamp      = 0x123456 (u24)
 *   seconds = 10 (001010), minutes = 5 (000101), hours = 1 (00001)
 *
 * bits: 1 00 0 0001 0010 0011 0100 0101 0110 001010 000101 00001 (pad 0)
 */
static void test_picture_timing_cnt0_basic(void)
{
    static const uint8_t payload[] = {
        0x81, /* flag=1 cnt=00 n90=0 ts[23:20]=0001 */
        0x23, /* ts[19:12]  */
        0x45, /* ts[11:4]   */
        0x62, /* ts[3:0]=0110 seconds[5:2]=0010 */
        0x85, /* seconds[1:0]=10 minutes=000101 */
        0x08  /* hours=00001 pad */
    };
    sei_msg_t msg;
    sei_picture_timing_t pt;
    uint64_t expected;

    memset(&msg, 0, sizeof(msg));
    msg.type = SEI_PAYLOAD_PICTURE_TIMING;
    msg.payload = payload;
    msg.size = sizeof(payload);

    TEST_ASSERT_EQUAL_INT(0, sei_parse_picture_timing(&msg, 0, &pt));
    TEST_ASSERT_EQUAL_INT(0, pt.pic_struct_present);
    TEST_ASSERT_EQUAL_INT(1, pt.has_clock_timestamp);
    TEST_ASSERT_EQUAL_UINT32(0, pt.cnt_type);
    TEST_ASSERT_EQUAL_INT(0, pt.n_90kHz_flag);
    TEST_ASSERT_EQUAL_INT(1, pt.supported);

    /* ClockTS, semantics D.2: ts24 + hours*3600*90000 + minutes*60*90000
     *                              + seconds*90000 */
    expected = 0x123456ULL
             + (uint64_t)1 * 3600u * 90000u
             + (uint64_t)5 * 60u * 90000u
             + (uint64_t)10 * 90000u;
    TEST_ASSERT_EQUAL_UINT64(expected, pt.clock_ts_90k);
}

/*
 * pic_struct present: pic_struct=3 (bits 0011) -> clock_timestamp_flag=1,
 * cnt_type=0, n90=0, ts=1, seconds=minutes=hours=0.
 * NumClockTS is 1 here, but we only ever parse the first timestamp anyway.
 */
static void test_picture_timing_pic_struct_present(void)
{
    static const uint8_t payload[] = {
        0x38, /* pic_struct=0011 flag=1 cnt=00 n90=0 */
        0x00, /* ts[23:16] */
        0x00, /* ts[15:8]  */
        0x01, /* ts[7:0]   */
        0x00, /* seconds=000000 minutes[5:4]=00 */
        0x00, /* minutes[3:0]=0000 hours[5:2]=00 */
        0x00  /* hours[1:0]=00 pad */
    };
    sei_msg_t msg;
    sei_picture_timing_t pt;

    memset(&msg, 0, sizeof(msg));
    msg.payload = payload;
    msg.size = sizeof(payload);

    TEST_ASSERT_EQUAL_INT(0, sei_parse_picture_timing(&msg, 1, &pt));
    TEST_ASSERT_EQUAL_UINT32(3, pt.pic_struct);
    TEST_ASSERT_EQUAL_INT(1, pt.pic_struct_present);
    TEST_ASSERT_EQUAL_INT(1, pt.has_clock_timestamp);
    TEST_ASSERT_EQUAL_INT(1, pt.supported);
    TEST_ASSERT_EQUAL_UINT64(1, pt.clock_ts_90k);
}

/* pic_struct=1 (top field) -> still parses cleanly, ts=10. */
static void test_picture_timing_pic_struct_1(void)
{
    static const uint8_t payload[] = {
        0x18, /* pic_struct=0001 flag=1 cnt=00 n90=0 */
        0x00, /* ts[23:16] */
        0x00, /* ts[15:8]  */
        0x0A, /* ts[7:0]=10 */
        0x00, /* zeros for seconds/minutes/hours */
        0x00,
        0x00
    };
    sei_msg_t msg;
    sei_picture_timing_t pt;

    memset(&msg, 0, sizeof(msg));
    msg.payload = payload;
    msg.size = sizeof(payload);

    TEST_ASSERT_EQUAL_INT(0, sei_parse_picture_timing(&msg, 1, &pt));
    TEST_ASSERT_EQUAL_UINT32(1, pt.pic_struct);
    TEST_ASSERT_EQUAL_INT(1, pt.has_clock_timestamp);
    TEST_ASSERT_EQUAL_UINT32(0, pt.cnt_type);
    TEST_ASSERT_EQUAL_INT(1, pt.supported);
    TEST_ASSERT_EQUAL_UINT64(10, pt.clock_ts_90k);
}

/* first clock_timestamp_flag = 0 -> no timestamp, clean 0 return. */
static void test_picture_timing_flag0(void)
{
    static const uint8_t payload[] = {
        0x00 /* clock_timestamp_flag=0, rest irrelevant */
    };
    sei_msg_t msg;
    sei_picture_timing_t pt;

    memset(&msg, 0, sizeof(msg));
    msg.payload = payload;
    msg.size = sizeof(payload);

    TEST_ASSERT_EQUAL_INT(0, sei_parse_picture_timing(&msg, 0, &pt));
    TEST_ASSERT_EQUAL_INT(0, pt.has_clock_timestamp);
    TEST_ASSERT_EQUAL_INT(0, pt.supported);
}

/*
 * cnt_type=1 (offset only): ts=0x112233, offset_not_present=1,
 * second_offset_not_present=1 -> parses to the end of the first timestamp,
 * supported stays 0.
 */
static void test_picture_timing_cnt1_unsupported(void)
{
    static const uint8_t payload[] = {
        0xA1, /* flag=1 cnt=01 n90=0 ts[23:20]=0001 */
        0x12, /* ts[19:12] */
        0x23, /* ts[11:4]  */
        0x3C  /* ts[3:0]=0011 offset_not_present=1 second_offset_not_present=1 pad */
    };
    sei_msg_t msg;
    sei_picture_timing_t pt;

    memset(&msg, 0, sizeof(msg));
    msg.payload = payload;
    msg.size = sizeof(payload);

    TEST_ASSERT_EQUAL_INT(0, sei_parse_picture_timing(&msg, 0, &pt));
    TEST_ASSERT_EQUAL_INT(1, pt.has_clock_timestamp);
    TEST_ASSERT_EQUAL_UINT32(1, pt.cnt_type);
    TEST_ASSERT_EQUAL_INT(0, pt.supported);
}

/* cnt_type=2: ts=1, two_clock_timestamps=0 -> supported 0, return 0. */
static void test_picture_timing_cnt2_unsupported(void)
{
    static const uint8_t payload[] = {
        0xC0, /* flag=1 cnt=10 n90=0 ts[23:20]=0000 */
        0x00, /* ts[19:12] */
        0x00, /* ts[11:4]  */
        0x10  /* ts[3:0]=0001 two_clock_timestamps=00 pad */
    };
    sei_msg_t msg;
    sei_picture_timing_t pt;

    memset(&msg, 0, sizeof(msg));
    msg.payload = payload;
    msg.size = sizeof(payload);

    TEST_ASSERT_EQUAL_INT(0, sei_parse_picture_timing(&msg, 0, &pt));
    TEST_ASSERT_EQUAL_UINT32(2, pt.cnt_type);
    TEST_ASSERT_EQUAL_INT(0, pt.supported);
}

/* cnt_type=0 with n_90kHz_flag=1: clock_timestamp is u32, not supported. */
static void test_picture_timing_n90_unsupported(void)
{
    static const uint8_t payload[] = {
        0x90, /* flag=1 cnt=00 n90=1 ts[31:28]=0000 */
        0x00, /* ts[27:20] */
        0x00, /* ts[19:12] */
        0x00, /* ts[11:4]  */
        0x50, /* ts[3:0]=0101 seconds[5:2]=00 */
        0x00, /* seconds[1:0]=00 minutes=000000... */
        0x00  /* ...hours=00000 pad */
    };
    sei_msg_t msg;
    sei_picture_timing_t pt;

    memset(&msg, 0, sizeof(msg));
    msg.payload = payload;
    msg.size = sizeof(payload);

    TEST_ASSERT_EQUAL_INT(0, sei_parse_picture_timing(&msg, 0, &pt));
    TEST_ASSERT_EQUAL_UINT32(0, pt.cnt_type);
    TEST_ASSERT_EQUAL_INT(1, pt.n_90kHz_flag);
    TEST_ASSERT_EQUAL_INT(0, pt.supported);
}

/* clock_timestamp cut off mid-u24 -> syntactic break, -1. */
static void test_picture_timing_truncated(void)
{
    static const uint8_t payload[] = {
        0x81, /* flag=1 cnt=00 n90=0 ts[23:20] */
        0x23  /* ts[19:12] -- buffer ends here, 12 bits of ts missing */
    };
    sei_msg_t msg;
    sei_picture_timing_t pt;

    memset(&msg, 0, sizeof(msg));
    msg.payload = payload;
    msg.size = sizeof(payload);

    TEST_ASSERT_EQUAL_INT(-1, sei_parse_picture_timing(&msg, 0, &pt));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_iter_empty_rbsp);
    RUN_TEST(test_iter_ff_extension);
    RUN_TEST(test_iter_skip_type0_find_type6);
    RUN_TEST(test_iter_no_phantom_from_trailing);
    RUN_TEST(test_recovery_point_golden);
    RUN_TEST(test_recovery_point_truncated);
    RUN_TEST(test_picture_timing_cnt0_basic);
    RUN_TEST(test_picture_timing_pic_struct_present);
    RUN_TEST(test_picture_timing_pic_struct_1);
    RUN_TEST(test_picture_timing_flag0);
    RUN_TEST(test_picture_timing_cnt1_unsupported);
    RUN_TEST(test_picture_timing_cnt2_unsupported);
    RUN_TEST(test_picture_timing_n90_unsupported);
    RUN_TEST(test_picture_timing_truncated);
    return UNITY_END();
}
