/*
 * test_avcc.c - unit tests for the AVCDecoderConfigurationRecord (14496-15)
 * parse/build and the AVCC <-> Annex B pipeline.
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

#include "unity.h"

#include "avcc.h"

void setUp(void)
{
}

void tearDown(void)
{
}

/*
 * Golden record written by FFmpeg's mov muxer (baseline profile, one SPS
 * and one PPS, lengthSizeMinusOne = 3). The SPS payload contains the
 * emulation_prevention sequence 00 00 03 - it must survive parse/build
 * byte-for-byte (no unescaping).
 */
static const uint8_t golden_base[] = {
    0x01, 0x42, 0xC0, 0x0B, 0xFF, 0xE1,
    0x00, 0x16,
    0x67, 0x42, 0xC0, 0x0B, 0xD9, 0x02, 0xC4, 0xEC,
    0x04, 0x40, 0x00, 0x00, 0x03, 0x00, 0x40, 0x00,
    0x00, 0x0C, 0x83, 0xC5, 0x0A, 0x92,
    0x01,
    0x00, 0x05, 0x68, 0xCB, 0x83, 0xCB, 0x20
};

/*
 * (а) baseline: field-by-field parse of the golden record plus a byte-exact
 * build from a hand-stated config (build == golden).
 */
static void test_parse_build_baseline_golden(void)
{
    avcc_config_t cfg;
    uint8_t *out = NULL;
    size_t out_size = 0;

    TEST_ASSERT_EQUAL_INT(0, avcc_parse(golden_base, sizeof(golden_base),
                                        &cfg));
    TEST_ASSERT_EQUAL_UINT8(0x42, cfg.profile_idc);
    TEST_ASSERT_EQUAL_UINT8(0xC0, cfg.profile_compat);
    TEST_ASSERT_EQUAL_UINT8(0x0B, cfg.level_idc);
    TEST_ASSERT_EQUAL_UINT8(3, cfg.length_size_minus_one);
    TEST_ASSERT_EQUAL_INT(0, cfg.has_ext_fields);
    TEST_ASSERT_EQUAL_INT(1, cfg.sps_count);
    TEST_ASSERT_EQUAL_INT(1, cfg.pps_count);
    TEST_ASSERT_EQUAL_UINT64(22, (uint64_t)cfg.sps_len[0]);
    TEST_ASSERT_EQUAL_UINT64(5, (uint64_t)cfg.pps_len[0]);
    /* NAL header byte kept, emulation bytes NOT removed. */
    TEST_ASSERT_EQUAL_UINT8(0x67, cfg.sps[0][0]);
    TEST_ASSERT_EQUAL_MEMORY(&golden_base[8], cfg.sps[0], 22);
    TEST_ASSERT_EQUAL_MEMORY(&golden_base[33], cfg.pps[0], 5);

    TEST_ASSERT_EQUAL_INT(0, avcc_build(&cfg, &out, &out_size));
    TEST_ASSERT_EQUAL_UINT64(sizeof(golden_base), (uint64_t)out_size);
    TEST_ASSERT_EQUAL_MEMORY(golden_base, out, sizeof(golden_base));
    free(out);
    avcc_config_free(&cfg);

    /* Byte-exact build from a stated config too (payloads are copies). */
    memset(&cfg, 0, sizeof(cfg));
    cfg.profile_idc = 0x42;
    cfg.profile_compat = 0xC0;
    cfg.level_idc = 0x0B;
    cfg.length_size_minus_one = 3;
    cfg.sps[0] = (uint8_t *)&golden_base[8];
    cfg.sps_len[0] = 22;
    cfg.sps_count = 1;
    cfg.pps[0] = (uint8_t *)&golden_base[33];
    cfg.pps_len[0] = 5;
    cfg.pps_count = 1;
    out = NULL;
    TEST_ASSERT_EQUAL_INT(0, avcc_build(&cfg, &out, &out_size));
    TEST_ASSERT_EQUAL_UINT64(sizeof(golden_base), (uint64_t)out_size);
    TEST_ASSERT_EQUAL_MEMORY(golden_base, out, sizeof(golden_base));
    free(out);
    /* the stated config borrowed static payloads: nothing to free */
}

/*
 * High profile with the 2013 extended tail. Non-canonical bit_depth values
 * and one ext SPS: the ext payload must be length-checked but ignored,
 * the chroma/bit_depth fields extracted from the low bits.
 */
static void test_parse_high_profile_ext_tail(void)
{
    static const uint8_t rec[] = {
        0x01, 0x64, 0x00, 0x1F, 0xFF, 0xE1,
        0x00, 0x04, 0x67, 0x64, 0x00, 0x1F,
        0x01,
        0x00, 0x04, 0x68, 0xEB, 0xE3, 0xCB,
        0xFD,                    /* reserved 111111, chroma_format_idc 01 */
        0xFA,                    /* reserved 11111, bit_depth_luma -8 = 2 */
        0xF8,                    /* reserved 11111, bit_depth_chroma -8 = 0 */
        0x01,                    /* reserved 000, numOfSPSExt = 1 */
        0x00, 0x03, 0x6F, 0x01, 0x02 /* subset SPS (ignored) */
    };
    avcc_config_t cfg;
    uint8_t *out = NULL;
    size_t out_size = 0;

    TEST_ASSERT_EQUAL_INT(0, avcc_parse(rec, sizeof(rec), &cfg));
    TEST_ASSERT_EQUAL_UINT8(100, cfg.profile_idc);
    TEST_ASSERT_EQUAL_INT(1, cfg.has_ext_fields);
    TEST_ASSERT_EQUAL_UINT8(1, cfg.chroma_format_idc);
    TEST_ASSERT_EQUAL_UINT8(2, cfg.bit_depth_luma_minus8);
    TEST_ASSERT_EQUAL_UINT8(0, cfg.bit_depth_chroma_minus8);
    TEST_ASSERT_EQUAL_INT(1, cfg.sps_count);
    TEST_ASSERT_EQUAL_INT(1, cfg.pps_count);

    /* Ext SPS list is rebuilt empty (payloads are not stored). */
    TEST_ASSERT_EQUAL_INT(0, avcc_build(&cfg, &out, &out_size));
    TEST_ASSERT_EQUAL_UINT64(sizeof(rec) - 5, (uint64_t)out_size);
    TEST_ASSERT_EQUAL_MEMORY(rec, out, sizeof(rec) - 6);
    TEST_ASSERT_EQUAL_UINT8(0x00, out[out_size - 1]);
    free(out);
    avcc_config_free(&cfg);

    /* A High record truncated right after the PPS list lacks the mandatory
     * ext tail: syntactically incomplete -> error, no leaked config. */
    memset(&cfg, 0, sizeof(cfg));
    TEST_ASSERT_EQUAL_INT(-1, avcc_parse(rec, 21, &cfg));
    TEST_ASSERT_EQUAL_INT(0, cfg.sps_count);
    TEST_ASSERT_EQUAL_INT(0, cfg.pps_count);
}

/* (в) build -> parse -> build byte-identical, High and baseline. */
static void test_roundtrip_build_parse_build(void)
{
    static const uint8_t sps1[] = { 0x67, 0x64, 0x00, 0x1F, 0xAC, 0xD9 };
    static const uint8_t sps2[] = { 0x67, 0x64, 0x00, 0x1F };
    static const uint8_t pps1[] = { 0x68, 0xEE, 0x3C, 0x80 };
    avcc_config_t cfg;
    uint8_t *b1 = NULL;
    uint8_t *b2 = NULL;
    size_t s1 = 0;
    size_t s2 = 0;
    int pass;

    for (pass = 0; pass < 2; pass++) {
        memset(&cfg, 0, sizeof(cfg));
        cfg.profile_idc = (uint8_t)(pass ? 100 : 66);
        cfg.profile_compat = 0x00;
        cfg.level_idc = 0x1F;
        cfg.length_size_minus_one = 3;
        cfg.chroma_format_idc = 1;
        cfg.bit_depth_luma_minus8 = 2;
        cfg.bit_depth_chroma_minus8 = 3;
        cfg.has_ext_fields = pass;
        cfg.sps[0] = (uint8_t *)sps1;
        cfg.sps_len[0] = sizeof(sps1);
        cfg.sps[1] = (uint8_t *)sps2;
        cfg.sps_len[1] = sizeof(sps2);
        cfg.sps_count = 2;
        cfg.pps[0] = (uint8_t *)pps1;
        cfg.pps_len[0] = sizeof(pps1);
        cfg.pps_count = 1;

        TEST_ASSERT_EQUAL_INT(0, avcc_build(&cfg, &b1, &s1));
        TEST_ASSERT_EQUAL_INT(0, avcc_parse(b1, s1, &cfg));
        TEST_ASSERT_EQUAL_INT(pass, cfg.has_ext_fields);
        if (pass) {
            TEST_ASSERT_EQUAL_UINT8(1, cfg.chroma_format_idc);
            TEST_ASSERT_EQUAL_UINT8(2, cfg.bit_depth_luma_minus8);
            TEST_ASSERT_EQUAL_UINT8(3, cfg.bit_depth_chroma_minus8);
        }
        TEST_ASSERT_EQUAL_INT(0, avcc_build(&cfg, &b2, &s2));
        TEST_ASSERT_EQUAL_UINT64((uint64_t)s1, (uint64_t)s2);
        TEST_ASSERT_EQUAL_MEMORY(b1, b2, s1);
        free(b1);
        free(b2);
        b1 = NULL;
        b2 = NULL;
        avcc_config_free(&cfg);
    }
}

/*
 * (г) error paths. A zero SPS count stays valid: 14496-15's grammar allows
 * numOfSequenceParameterSets == 0 (in-band parameter signalling), so we
 * parse it as an empty list instead of rejecting - fixed interpretation.
 */
static void test_error_paths(void)
{
    avcc_config_t cfg;
    uint8_t buf[64];
    uint8_t *out = NULL;
    size_t out_size = 0;

    /* configurationVersion != 1 */
    memcpy(buf, golden_base, sizeof(golden_base));
    buf[0] = 2;
    TEST_ASSERT_EQUAL_INT(-1, avcc_parse(buf, sizeof(golden_base), &cfg));

    /* byte 4 reserved bits != 0b111111 */
    memcpy(buf, golden_base, sizeof(golden_base));
    buf[4] = 0x3F;
    TEST_ASSERT_EQUAL_INT(-1, avcc_parse(buf, sizeof(golden_base), &cfg));

    /* SPS payload truncated inside the record */
    TEST_ASSERT_EQUAL_INT(-1, avcc_parse(golden_base, 20, &cfg));

    /* numOfPPS byte missing (record stops right after the SPS list) */
    TEST_ASSERT_EQUAL_INT(-1, avcc_parse(golden_base, 30, &cfg));

    /* trailing garbage after the PPS list on a non-High profile */
    memcpy(buf, golden_base, sizeof(golden_base));
    TEST_ASSERT_EQUAL_INT(-1, avcc_parse(buf, sizeof(golden_base) + 1,
                                         &cfg));

    /* too-short buffer */
    TEST_ASSERT_EQUAL_INT(-1, avcc_parse(golden_base, 5, &cfg));

    /* zero SPS count: VALID empty list */
    {
        static const uint8_t empty_sps[] = {
            0x01, 0x42, 0xC0, 0x0B, 0xFF, 0xE0,
            0x01,
            0x00, 0x05, 0x68, 0xCB, 0x83, 0xCB, 0x20
        };
        TEST_ASSERT_EQUAL_INT(0, avcc_parse(empty_sps, sizeof(empty_sps),
                                            &cfg));
        TEST_ASSERT_EQUAL_INT(0, cfg.sps_count);
        TEST_ASSERT_EQUAL_INT(1, cfg.pps_count);
        TEST_ASSERT_EQUAL_INT(0, avcc_build(&cfg, &out, &out_size));
        TEST_ASSERT_EQUAL_UINT64(sizeof(empty_sps), (uint64_t)out_size);
        TEST_ASSERT_EQUAL_MEMORY(empty_sps, out, sizeof(empty_sps));
        free(out);
        out = NULL;
        avcc_config_free(&cfg);
    }

    /* build rejects a dangling entry (count > 0 with NULL payload) */
    memset(&cfg, 0, sizeof(cfg));
    cfg.sps_count = 1;
    TEST_ASSERT_EQUAL_INT(-1, avcc_build(&cfg, &out, &out_size));
    TEST_ASSERT_NULL(out);
}

/*
 * (д) pipeline samples: two NALs of different sizes, one carrying the
 * 00 00 03 emulation sequence; Annex B with 4-byte start codes round-trips
 * byte-for-byte through both widths (-n 4 and -n 2).
 */
static void test_avcc_annexb_pipeline(void)
{
    static const uint8_t annexb[] = {
        0x00, 0x00, 0x00, 0x01,
        0x67, 0x42, 0xC0, 0x0B, 0x00, 0x00, 0x03, 0x00, 0xF8,
        0x00, 0x00, 0x00, 0x01,
        0x65, 0xAB, 0xCD
    };
    /* 4-byte widths of the expected length-prefixed forms. */
    static const uint8_t avcc4[] = {
        0x00, 0x00, 0x00, 0x09,
        0x67, 0x42, 0xC0, 0x0B, 0x00, 0x00, 0x03, 0x00, 0xF8,
        0x00, 0x00, 0x00, 0x03,
        0x65, 0xAB, 0xCD
    };
    static const uint8_t avcc2[] = {
        0x00, 0x09,
        0x67, 0x42, 0xC0, 0x0B, 0x00, 0x00, 0x03, 0x00, 0xF8,
        0x00, 0x03,
        0x65, 0xAB, 0xCD
    };
    uint8_t *out = NULL;
    uint8_t *back = NULL;
    size_t out_size = 0;
    size_t back_size = 0;

    /* annexb -> avcc (4-byte widths) equals the hand-stated record */
    TEST_ASSERT_EQUAL_INT(0, annexb_to_avcc(annexb, sizeof(annexb), 4,
                                            &out, &out_size));
    TEST_ASSERT_EQUAL_UINT64(sizeof(avcc4), (uint64_t)out_size);
    TEST_ASSERT_EQUAL_MEMORY(avcc4, out, sizeof(avcc4));
    free(out);

    /* ... and back: payload-by-payload equal, i.e. byte-identical to the
     * (already normalized) input. */
    TEST_ASSERT_EQUAL_INT(0, avcc_to_annexb(avcc4, sizeof(avcc4), 4,
                                            &back, &back_size));
    TEST_ASSERT_EQUAL_UINT64(sizeof(annexb), (uint64_t)back_size);
    TEST_ASSERT_EQUAL_MEMORY(annexb, back, sizeof(annexb));
    free(back);

    /* -n 2 */
    out = NULL;
    back = NULL;
    TEST_ASSERT_EQUAL_INT(0, annexb_to_avcc(annexb, sizeof(annexb), 2,
                                            &out, &out_size));
    TEST_ASSERT_EQUAL_UINT64(sizeof(avcc2), (uint64_t)out_size);
    TEST_ASSERT_EQUAL_MEMORY(avcc2, out, sizeof(avcc2));
    TEST_ASSERT_EQUAL_INT(0, avcc_to_annexb(out, out_size, 2,
                                            &back, &back_size));
    TEST_ASSERT_EQUAL_UINT64(sizeof(annexb), (uint64_t)back_size);
    TEST_ASSERT_EQUAL_MEMORY(annexb, back, sizeof(annexb));
    free(out);
    free(back);

    /* empty stream round-trips as empty */
    out = NULL;
    TEST_ASSERT_EQUAL_INT(0, annexb_to_avcc(annexb, 0, 4, &out, &out_size));
    TEST_ASSERT_EQUAL_UINT64(0, (uint64_t)out_size);
    free(out);

    /* truncated final NAL: length reaches past the end */
    {
        static const uint8_t trunc[] = { 0x00, 0x00, 0x00, 0x05, 0x41, 0xAA };
        out = NULL;
        TEST_ASSERT_EQUAL_INT(-1, avcc_to_annexb(trunc, sizeof(trunc), 4,
                                                 &out, &out_size));
        TEST_ASSERT_NULL(out);
    }

    /* bad widths are refused by both directions */
    TEST_ASSERT_EQUAL_INT(-1, avcc_to_annexb(avcc4, sizeof(avcc4), 1,
                                             &out, &out_size));
    TEST_ASSERT_EQUAL_INT(-1, avcc_to_annexb(avcc4, sizeof(avcc4), 5,
                                             &out, &out_size));
    TEST_ASSERT_EQUAL_INT(-1, annexb_to_avcc(annexb, sizeof(annexb), 1,
                                             &out, &out_size));
    TEST_ASSERT_EQUAL_INT(-1, annexb_to_avcc(annexb, sizeof(annexb), 5,
                                             &out, &out_size));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_build_baseline_golden);
    RUN_TEST(test_parse_high_profile_ext_tail);
    RUN_TEST(test_roundtrip_build_parse_build);
    RUN_TEST(test_error_paths);
    RUN_TEST(test_avcc_annexb_pipeline);
    return UNITY_END();
}
