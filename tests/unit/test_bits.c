/*
 * test_bits.c - unit tests for source/common/bits.c (bit reader, ue/se).
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
#include "bits.h"
#include "bitwriter.h"

void setUp(void)
{
}

void tearDown(void)
{
}

/*
 * Golden ue(v) codewords, spec 9.1 (v -> bit string):
 * 0 -> "1", 1 -> "010", 2 -> "011", 3 -> "00100", 6 -> "00111",
 * 100 -> "0000001100101" (100 in [63..126]: 6 leading zeros +
 * suffix 100-63=37=0b100101). NOTE: the often-quoted "00001010101"
 * for 100 is wrong: 4 leading zeros + suffix 0b0101 decode to 20.
 */
static void test_ue_golden(void)
{
    /* {value, bytes (padding bits are 0), byte count} */
    static const struct {
        uint32_t v;
        uint8_t bytes[2];
        size_t size;
    } cases[] = {
        {  0, { 0x80, 0x00 }, 1 },  /* "1"             -> 1000 0000 */
        {  1, { 0x40, 0x00 }, 1 },  /* "010"           -> 0100 0000 */
        {  2, { 0x60, 0x00 }, 1 },  /* "011"           -> 0110 0000 */
        {  3, { 0x20, 0x00 }, 1 },  /* "00100"         -> 0010 0000 */
        {  6, { 0x38, 0x00 }, 1 },  /* "00111"         -> 0011 1000 */
        {100, { 0x03, 0x28 }, 2 }   /* "0000001100101" -> 0000 0011 00101 000 */
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        bits_t b;
        uint32_t got;

        bits_init(&b, cases[i].bytes, cases[i].size);
        got = bits_read_ue(&b);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(cases[i].v, got, "golden ue(v)");
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, bits_error(&b), "golden ue(v) error");
    }
}

/* Concatenated ue(0) ue(1) ue(2) ue(3) ue(6): "1" "010" "011" "00100" "00111" */
static void test_ue_concatenated(void)
{
    static const uint8_t buf[] = {
        0xA6, /* 1 | 010 | 011 | 0      -> ue=0, ue=1, ue=2, ue=3 starts */
        0x43, /* 0 1 00 | 00 111 -> ue=3 ("00100"), ue=6 ("00111") starts */
        0x80  /* 1 | 0000 000           -> ue=6 ends; rest is padding     */
    };
    static const uint32_t expect[] = { 0, 1, 2, 3, 6 };
    bits_t b;
    size_t i;

    bits_init(&b, buf, sizeof(buf));
    for (i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
        TEST_ASSERT_EQUAL_UINT32(expect[i], bits_read_ue(&b));
    }
    TEST_ASSERT_EQUAL_INT(0, bits_error(&b));
    /* only zero padding remains -> next ue must fail, not read OOB */
    TEST_ASSERT_EQUAL_UINT32(0, bits_read_ue(&b));
    TEST_ASSERT_EQUAL_INT(1, bits_error(&b));
}

/*
 * Golden se(v) codewords via the 9.1.1 map
 * (se value <-> codeNum: 0->0, 1->1, -1->2, 2->3, -2->4, 3->5, -3->6).
 */
static void test_se_golden(void)
{
    static const struct {
        int32_t v;
        uint8_t byte;
    } cases[] = {
        {  0, 0x80 },  /* codeNum 0: "1"     */
        {  1, 0x40 },  /* codeNum 1: "010"   */
        { -1, 0x60 },  /* codeNum 2: "011"   */
        {  2, 0x20 },  /* codeNum 3: "00100" */
        { -2, 0x28 },  /* codeNum 4: "00101" */
        {  3, 0x30 },  /* codeNum 5: "00110" */
        { -3, 0x38 }   /* codeNum 6: "00111" */
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        bits_t b;
        int32_t got;

        bits_init(&b, &cases[i].byte, 1);
        got = bits_read_se(&b);
        TEST_ASSERT_EQUAL_INT32_MESSAGE(cases[i].v, got, "golden se(v)");
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, bits_error(&b), "golden se(v) error");
    }
}

/* bits_read_u: MSB-first, n = 1/3/4/32. */
static void test_read_u_msb(void)
{
    static const uint8_t one[]   = { 0x80 };              /* 1000 0000 */
    static const uint8_t four[]  = { 0xA0 };              /* 1010 0000 */
    static const uint8_t three[] = { 0xE0 };              /* 1110 0000 */
    static const uint8_t word[]  = { 0x12, 0x34, 0x56, 0x78 };
    bits_t b;

    bits_init(&b, one, sizeof(one));
    TEST_ASSERT_EQUAL_HEX32(1, bits_read_u(&b, 1));
    TEST_ASSERT_EQUAL_HEX32(0, bits_read_u(&b, 1));
    TEST_ASSERT_EQUAL_INT(0, bits_error(&b));

    bits_init(&b, four, sizeof(four));
    TEST_ASSERT_EQUAL_HEX32(0xA, bits_read_u(&b, 4));
    TEST_ASSERT_EQUAL_INT(0, bits_error(&b));

    bits_init(&b, three, sizeof(three));
    TEST_ASSERT_EQUAL_HEX32(7, bits_read_u(&b, 3));

    bits_init(&b, word, sizeof(word));
    TEST_ASSERT_EQUAL_HEX32(0x12345678, bits_read_u(&b, 32));
    TEST_ASSERT_EQUAL_INT(0, bits_error(&b));
}

/* Overrun and bad n: error is set, reads return 0, no UB, error sticks. */
static void test_read_overrun(void)
{
    static const uint8_t one[] = { 0xFF };
    bits_t b;

    bits_init(&b, one, sizeof(one));
    TEST_ASSERT_EQUAL_HEX32(0xFF, bits_read_u(&b, 8));
    TEST_ASSERT_EQUAL_INT(0, bits_error(&b));
    /* buffer exhausted */
    TEST_ASSERT_EQUAL_HEX32(0, bits_read_u(&b, 1));
    TEST_ASSERT_EQUAL_INT(1, bits_error(&b));
    /* continued reading is not UB and error stays set */
    TEST_ASSERT_EQUAL_HEX32(0, bits_read_u(&b, 32));
    TEST_ASSERT_EQUAL_HEX32(0, bits_read_u(&b, 4));
    TEST_ASSERT_EQUAL_UINT32(0, bits_read_ue(&b));
    TEST_ASSERT_EQUAL_INT32(0, bits_read_se(&b));
    TEST_ASSERT_EQUAL_INT(1, bits_error(&b));

    /* invalid n */
    bits_init(&b, one, sizeof(one));
    TEST_ASSERT_EQUAL_HEX32(0, bits_read_u(&b, 0));
    TEST_ASSERT_EQUAL_INT(1, bits_error(&b));
    bits_init(&b, one, sizeof(one));
    TEST_ASSERT_EQUAL_HEX32(0, bits_read_u(&b, 33));
    TEST_ASSERT_EQUAL_INT(1, bits_error(&b));
}

/* ue() with 33+ leading zeros -> error (guard lz > 32). */
static void test_ue_too_many_leading_zeros(void)
{
    static const uint8_t zeros[] = { 0, 0, 0, 0, 0, 0, 0, 0 }; /* 64 zero bits */
    bits_t b;

    bits_init(&b, zeros, sizeof(zeros));
    TEST_ASSERT_EQUAL_UINT32(0, bits_read_ue(&b));
    TEST_ASSERT_EQUAL_INT(1, bits_error(&b));
}

/*
 * Boundary: exactly 32 leading zeros are accepted (lz == 32 does not trip
 * the guard); the value must still fit in 32 bits, so only suffix == 0
 * decodes to 0xFFFFFFFF, suffix > 0 overflows to an error.
 */
static void test_ue_32_leading_zeros_boundary(void)
{
    static const uint8_t ok[]  = { 0x00, 0x00, 0x00, 0x00,
                                   0x80,
                                   0x00, 0x00, 0x00, 0x00 };
    static const uint8_t bad[] = { 0x00, 0x00, 0x00, 0x00,
                                   0x80,
                                   0x00, 0x00, 0x00, 0x80 };
    bits_t b;

    bits_init(&b, ok, sizeof(ok));
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFF, bits_read_ue(&b));
    TEST_ASSERT_EQUAL_INT(0, bits_error(&b));

    bits_init(&b, bad, sizeof(bad));
    TEST_ASSERT_EQUAL_UINT32(0, bits_read_ue(&b));
    TEST_ASSERT_EQUAL_INT(1, bits_error(&b));
}

/* ue() with a truncated suffix: "00001" + only 3 bits left -> error. */
static void test_ue_truncated_suffix(void)
{
    static const uint8_t buf[] = { 0x08 }; /* 0000 1000: 4 zeros, 1, then "000" */
    bits_t b;

    bits_init(&b, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(0, bits_read_ue(&b));
    TEST_ASSERT_EQUAL_INT(1, bits_error(&b));
}

/* Round-trip bw_u <-> bits_read_u. */
static void test_roundtrip_u(void)
{
    static const struct {
        uint32_t val;
        int n;
    } vals[] = {
        { 0x1u, 1 }, { 0x0u, 1 }, { 0xAu, 4 }, { 0xFu, 4 }, { 0x7u, 3 },
        { 0xA5u, 8 }, { 0x12345678u, 32 }, { 0x80000000u, 32 },
        { 0xFFFFFFFFu, 32 }
    };
    uint8_t buf[64];
    bitwriter_t bw;
    bits_t b;
    size_t size;
    size_t i;

    bw_init(&bw, buf, sizeof(buf));
    for (i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        bw_u(&bw, vals[i].val, vals[i].n);
    }
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    size = bw_finish(&bw);
    TEST_ASSERT_EQUAL_INT(15, (int)size); /* 117 bits -> 15 bytes */

    bits_init(&b, buf, size);
    for (i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(vals[i].val, bits_read_u(&b, vals[i].n),
                                        "round-trip u");
    }
    TEST_ASSERT_EQUAL_INT(0, bits_error(&b));
}

/*
 * Round-trip bw_ue <-> bits_read_ue, including boundaries on the number
 * of leading zeros: lz = 1 (v = 1, 2), lz = 15 (v = 0x7FFF, 0xFFFE),
 * lz = 31 (v = 0x7FFFFFFF, 0xFFFFFFFE).
 */
static void test_roundtrip_ue(void)
{
    static const uint32_t vals[] = {
        0, 1, 2, 3, 6, 100,
        0x7FFFu, 0xFFFEu,          /* leading zeros 15 */
        0x7FFFFFFFu, 0xFFFFFFFEu   /* leading zeros 31 */
    };
    uint8_t buf[64];
    bitwriter_t bw;
    bits_t b;
    size_t size;
    size_t i;

    bw_init(&bw, buf, sizeof(buf));
    for (i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        bw_ue(&bw, vals[i]);
    }
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    size = bw_finish(&bw);

    bits_init(&b, buf, size);
    for (i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(vals[i], bits_read_ue(&b),
                                         "round-trip ue");
    }
    TEST_ASSERT_EQUAL_INT(0, bits_error(&b));
}

/* Round-trip bw_se <-> bits_read_se (moderate values). */
static void test_roundtrip_se(void)
{
    static const int32_t vals[] = { 0, 1, -1, 2, -2, 3, -3, 1000, -1000 };
    uint8_t buf[64];
    bitwriter_t bw;
    bits_t b;
    size_t size;
    size_t i;

    bw_init(&bw, buf, sizeof(buf));
    for (i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        bw_se(&bw, vals[i]);
    }
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    size = bw_finish(&bw);

    bits_init(&b, buf, size);
    for (i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        TEST_ASSERT_EQUAL_INT32_MESSAGE(vals[i], bits_read_se(&b),
                                        "round-trip se");
    }
    TEST_ASSERT_EQUAL_INT(0, bits_error(&b));
}

/* Writer byte patterns must match the reader's goldens. */
static void test_writer_golden_bytes(void)
{
    uint8_t buf[64];
    bitwriter_t bw;

    bw_init(&bw, buf, sizeof(buf));
    bw_ue(&bw, 100); /* "0000001100101" -> 0000 0011 | 00101 000 */
    TEST_ASSERT_EQUAL_INT(2, (int)bw_finish(&bw));
    TEST_ASSERT_EQUAL_UINT8(0x03, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x28, buf[1]);

    bw_init(&bw, buf, sizeof(buf));
    bw_se(&bw, -3); /* codeNum 6: "00111" -> 00111 000 */
    TEST_ASSERT_EQUAL_INT(1, (int)bw_finish(&bw));
    TEST_ASSERT_EQUAL_UINT8(0x38, buf[0]);

    bw_init(&bw, buf, sizeof(buf));
    static const uint8_t byte3c[] = { 0x3C };
    bw_u8(&bw, 0xA5);
    bw_bytes(&bw, byte3c, 1);
    TEST_ASSERT_EQUAL_INT(2, (int)bw_finish(&bw));
    TEST_ASSERT_EQUAL_UINT8(0xA5, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x3C, buf[1]);
}

/* Overflow: error set, nothing is written past cap, bad n rejected. */
static void test_writer_overflow(void)
{
    uint8_t buf[8];
    bitwriter_t bw;
    size_t i;

    for (i = 0; i < sizeof(buf); i++) {
        buf[i] = 0xAA;
    }
    bw_init(&bw, buf, 4); /* cap = 4 bytes, buf[4..7] must stay 0xAA */
    bw_u(&bw, 0xFFFFFFFFu, 32);   /* fills cap exactly */
    TEST_ASSERT_EQUAL_INT(0, bw.error);
    bw_u(&bw, 0xFFFFFFFFu, 32);   /* overflow */
    bw_ue(&bw, 100);              /* dropped after error */
    bw_se(&bw, -2);
    TEST_ASSERT_EQUAL_INT(1, bw.error);
    for (i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT8(0xFF, buf[i]);
    }
    for (i = 4; i < sizeof(buf); i++) {
        TEST_ASSERT_EQUAL_UINT8(0xAA, buf[i]);
    }

    bw_init(&bw, buf, sizeof(buf));
    bw_u(&bw, 1, 0);  /* n < 1 */
    TEST_ASSERT_EQUAL_INT(1, bw.error);
    bw_init(&bw, buf, sizeof(buf));
    bw_u(&bw, 1, 33); /* n > 32 */
    TEST_ASSERT_EQUAL_INT(1, bw.error);
}

int main(void)
{
    UnityBegin("test_bits.c");
    RUN_TEST(test_ue_golden);
    RUN_TEST(test_ue_concatenated);
    RUN_TEST(test_se_golden);
    RUN_TEST(test_read_u_msb);
    RUN_TEST(test_read_overrun);
    RUN_TEST(test_ue_too_many_leading_zeros);
    RUN_TEST(test_ue_32_leading_zeros_boundary);
    RUN_TEST(test_ue_truncated_suffix);
    RUN_TEST(test_roundtrip_u);
    RUN_TEST(test_roundtrip_ue);
    RUN_TEST(test_roundtrip_se);
    RUN_TEST(test_writer_golden_bytes);
    RUN_TEST(test_writer_overflow);
    return UnityEnd();
}
