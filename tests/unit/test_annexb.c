/*
 * test_annexb.c - unit tests for source/common/annexb.c (scan, unescape,
 * write round-trip).
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "annexb.h"

void setUp(void)
{
}

void tearDown(void)
{
}

/* 4-byte start code (00 00 00 01) and 3-byte start code (00 00 01). */
static void test_scan_start_codes(void)
{
    static const uint8_t buf[] = {
        0x00, 0x00, 0x00, 0x01,       /* 4-byte start code               */
        0x67, 0x0A,                   /* SPS: ref_idc=3, type=7          */
        0x00, 0x00, 0x01,             /* 3-byte start code               */
        0x68, 0x0B                    /* PPS: ref_idc=3, type=8          */
    };
    nal_list_t list;

    TEST_ASSERT_EQUAL_INT(0, annexb_scan(buf, sizeof(buf), &list));
    TEST_ASSERT_EQUAL_INT(2, (int)list.count);
    if (list.count == 2) {
        TEST_ASSERT_EQUAL_UINT8(7, list.items[0].type);
        TEST_ASSERT_EQUAL_UINT8(3, list.items[0].ref_idc);
        TEST_ASSERT_EQUAL_INT(4, (int)list.items[0].off);   /* byte after 4-byte SC */
        TEST_ASSERT_EQUAL_INT(2, (int)list.items[0].size);  /* 67 0A */
        TEST_ASSERT_EQUAL_UINT8(8, list.items[1].type);
        TEST_ASSERT_EQUAL_INT(9, (int)list.items[1].off);   /* byte after 3-byte SC */
        TEST_ASSERT_EQUAL_INT(2, (int)list.items[1].size);  /* 68 0B */
    }
    annexb_list_free(&list);
    TEST_ASSERT_NULL(list.items);
}

/*
 * leading_zero_8bits: a run of six 0x00 before 01 is one start code,
 * the scanner finds the last 00 00 01 of the run -> exactly one NAL.
 */
static void test_scan_leading_zeros_run(void)
{
    static const uint8_t buf[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,  /* zeros + 3-byte SC */
        0x65, 0x11                                  /* IDR: ref=3, type=5 */
    };
    nal_list_t list;

    TEST_ASSERT_EQUAL_INT(0, annexb_scan(buf, sizeof(buf), &list));
    TEST_ASSERT_EQUAL_INT(1, (int)list.count);
    if (list.count == 1) {
        TEST_ASSERT_EQUAL_UINT8(5, list.items[0].type);
        TEST_ASSERT_EQUAL_UINT8(3, list.items[0].ref_idc);
        TEST_ASSERT_EQUAL_INT(7, (int)list.items[0].off);
        TEST_ASSERT_EQUAL_INT(2, (int)list.items[0].size);
    }
    annexb_list_free(&list);
}

/* zero_byte right after a start code: no NAL unit, no junk in the list. */
static void test_scan_zero_byte(void)
{
    static const uint8_t with_nal[] = {
        0x00, 0x00, 0x01, 0x00,       /* start code + zero_byte: no NAL   */
        0x00, 0x00, 0x00, 0x01,       /* real 4-byte start code           */
        0x41, 0x22                    /* slice: ref=2, type=1             */
    };
    static const uint8_t only_zero_byte[] = { 0x00, 0x00, 0x01, 0x00 };
    nal_list_t list;

    TEST_ASSERT_EQUAL_INT(0, annexb_scan(with_nal, sizeof(with_nal), &list));
    TEST_ASSERT_EQUAL_INT(1, (int)list.count);
    if (list.count == 1) {
        TEST_ASSERT_EQUAL_UINT8(1, list.items[0].type);
        TEST_ASSERT_EQUAL_INT(8, (int)list.items[0].off);
        TEST_ASSERT_EQUAL_INT(2, (int)list.items[0].size);
    }
    annexb_list_free(&list);

    TEST_ASSERT_EQUAL_INT(0, annexb_scan(only_zero_byte, sizeof(only_zero_byte), &list));
    TEST_ASSERT_EQUAL_INT(0, (int)list.count);
    annexb_list_free(&list);
}

/*
 * Trailing zero_8bits between NALs are stripped from size; the last NAL
 * runs to EOF. Bytes: NAL1 payload 41 42, five 0x00, next start code.
 */
static void test_scan_trailing_zeros_stripped(void)
{
    static const uint8_t buf[] = {
        0x00, 0x00, 0x01,             /* start code                      */
        0x41, 0x42,                   /* NAL1 payload (ref=2, type=1)    */
        0x00, 0x00, 0x00, 0x00, 0x00, /* zero_8bits padding              */
        0x00, 0x00, 0x01,             /* start code (shares the zeros)   */
        0x42, 0x99                    /* NAL2 to EOF (ref=2, type=2)     */
    };
    nal_list_t list;

    TEST_ASSERT_EQUAL_INT(0, annexb_scan(buf, sizeof(buf), &list));
    TEST_ASSERT_EQUAL_INT(2, (int)list.count);
    if (list.count == 2) {
        TEST_ASSERT_EQUAL_INT(3, (int)list.items[0].off);
        TEST_ASSERT_EQUAL_INT(2, (int)list.items[0].size);  /* 41 42, no zeros */
        TEST_ASSERT_EQUAL_INT(13, (int)list.items[1].off);
        TEST_ASSERT_EQUAL_INT(2, (int)list.items[1].size);  /* last NAL to EOF */
    }
    annexb_list_free(&list);
}

/* nal_unit_type 0 in the header byte: the NAL is not recorded. */
static void test_scan_type_zero_skipped(void)
{
    static const uint8_t buf[] = {
        0x00, 0x00, 0x01,             /* start code                      */
        0x60, 0x00, 0x11,             /* header type=0 -> skipped entirely */
        0x00, 0x00, 0x01,             /* start code                      */
        0x41, 0x05                    /* slice: type=1, recorded          */
    };
    nal_list_t list;

    TEST_ASSERT_EQUAL_INT(0, annexb_scan(buf, sizeof(buf), &list));
    TEST_ASSERT_EQUAL_INT(1, (int)list.count);
    if (list.count == 1) {
        TEST_ASSERT_EQUAL_UINT8(1, list.items[0].type);
        TEST_ASSERT_EQUAL_INT(9, (int)list.items[0].off);
    }
    annexb_list_free(&list);
}

/*
 * annexb_rbsp golden unescape (spec 7.4.1): 00 00 03 followed by 0x00..0x03
 * loses the 0x03; 00 01 03 00 keeps it (no 00 00 pair), 00 00 03 04 keeps
 * it (next byte > 0x03). The NAL header byte is out[0].
 */
static void test_rbsp_unescape_golden(void)
{
    static const uint8_t buf[] = {
        0x00, 0x00, 0x01, 0x67,       /* SPS start code + header byte    */
        0x00, 0x00, 0x03, 0x02,       /* 03 removed -> 00 00 02          */
        0x00, 0x01, 0x03, 0x00,       /* kept: no 00 00 pair before 03   */
        0x00, 0x00, 0x03, 0x04,       /* kept: next byte 04 > 03         */
        0x00, 0x00, 0x03, 0x01,       /* 03 removed -> 00 00 01          */
        0x80                          /* stop bit, padding                */
    };
    static const uint8_t expect[] = {
        0x67,
        0x00, 0x00, 0x02,
        0x00, 0x01, 0x03, 0x00,
        0x00, 0x00, 0x03, 0x04,
        0x00, 0x00, 0x01,
        0x80
    };
    nal_list_t list;
    uint8_t *rbsp;
    size_t rbsp_size = 0;

    TEST_ASSERT_EQUAL_INT(0, annexb_scan(buf, sizeof(buf), &list));
    TEST_ASSERT_EQUAL_INT(1, (int)list.count);
    if (list.count != 1) {
        annexb_list_free(&list);
        return;
    }
    TEST_ASSERT_EQUAL_INT(18, (int)list.items[0].size); /* header + 17 EBSP */
    rbsp = annexb_rbsp(buf, &list.items[0], &rbsp_size);
    TEST_ASSERT_NOT_NULL(rbsp);
    if (rbsp) {
        TEST_ASSERT_EQUAL_INT((int)sizeof(expect), (int)rbsp_size);
        TEST_ASSERT_EQUAL_UINT8(0x67, rbsp[0]); /* NAL header byte is out[0] */
        TEST_ASSERT_EQUAL_MEMORY(expect, rbsp, rbsp_size);
        free(rbsp);
    }
    annexb_list_free(&list);
}

/* 0x03 as the last byte of the NAL has no next byte -> kept. */
static void test_rbsp_emulation_three_at_end(void)
{
    static const uint8_t buf[] = {
        0x00, 0x00, 0x01, 0x67,       /* start code + SPS header         */
        0x00, 0x00, 0x03              /* 03 at the very end: no next byte */
    };
    static const uint8_t expect[] = { 0x67, 0x00, 0x00, 0x03 };
    nal_list_t list;
    uint8_t *rbsp;
    size_t rbsp_size = 0;

    TEST_ASSERT_EQUAL_INT(0, annexb_scan(buf, sizeof(buf), &list));
    TEST_ASSERT_EQUAL_INT(1, (int)list.count);
    if (list.count != 1) {
        annexb_list_free(&list);
        return;
    }
    rbsp = annexb_rbsp(buf, &list.items[0], &rbsp_size);
    TEST_ASSERT_NOT_NULL(rbsp);
    if (rbsp) {
        TEST_ASSERT_EQUAL_INT((int)sizeof(expect), (int)rbsp_size);
        TEST_ASSERT_EQUAL_MEMORY(expect, rbsp, rbsp_size);
        free(rbsp);
    }
    annexb_list_free(&list);
}

/*
 * annexb_write_nal round-trip: the written stream is a 4-byte start code
 * plus the NAL payload byte-for-byte (emulation bytes included as stored).
 */
static void test_write_nal_roundtrip(void)
{
    static const uint8_t buf[] = {
        0x00, 0x00, 0x01,             /* start code                      */
        0x67, 0xAA, 0xBB, 0xCC,       /* NAL1                            */
        0x00, 0x00, 0x01,             /* start code                      */
        0x65, 0x00, 0x00, 0x03, 0x00, /* NAL2 with emulation bytes kept  */
        0x44                          /* ... and a non-zero tail         */
    };
    static const uint8_t expect[] = {
        0x00, 0x00, 0x00, 0x01,
        0x67, 0xAA, 0xBB, 0xCC,
        0x00, 0x00, 0x00, 0x01,
        0x65, 0x00, 0x00, 0x03, 0x00, 0x44
    };
    uint8_t out[64];
    nal_list_t list;
    FILE *f;
    size_t got;

    TEST_ASSERT_EQUAL_INT(0, annexb_scan(buf, sizeof(buf), &list));
    TEST_ASSERT_EQUAL_INT(2, (int)list.count);
    if (list.count != 2) {
        annexb_list_free(&list);
        return;
    }
    f = tmpfile();
    TEST_ASSERT_NOT_NULL(f);
    if (!f) {
        annexb_list_free(&list);
        return;
    }
    TEST_ASSERT_EQUAL_INT(0, annexb_write_nal(f, buf, &list.items[0]));
    TEST_ASSERT_EQUAL_INT(0, annexb_write_nal(f, buf, &list.items[1]));
    rewind(f);
    got = fread(out, 1, sizeof(out), f);
    fclose(f);
    TEST_ASSERT_EQUAL_INT((int)sizeof(expect), (int)got);
    TEST_ASSERT_EQUAL_MEMORY(expect, out, sizeof(expect));
    annexb_list_free(&list);
}

int main(void)
{
    UnityBegin("test_annexb.c");
    RUN_TEST(test_scan_start_codes);
    RUN_TEST(test_scan_leading_zeros_run);
    RUN_TEST(test_scan_zero_byte);
    RUN_TEST(test_scan_trailing_zeros_stripped);
    RUN_TEST(test_scan_type_zero_skipped);
    RUN_TEST(test_rbsp_unescape_golden);
    RUN_TEST(test_rbsp_emulation_three_at_end);
    RUN_TEST(test_write_nal_roundtrip);
    return UnityEnd();
}
