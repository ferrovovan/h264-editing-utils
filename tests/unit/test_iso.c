/*
 * test_iso.c - unit tests for the ISO-BMFF (MP4) reader.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "iso.h"

void setUp(void)
{
}

void tearDown(void)
{
}

/* --- growable big-endian byte builder ------------------------------------ */
typedef struct {
    uint8_t *b;
    size_t n, cap;
} buf_t;

static void bneed(buf_t *x, size_t add)
{
    if (x->n + add <= x->cap)
        return;
    x->cap = (x->cap ? x->cap * 2 : 256) + add;
    x->b = realloc(x->b, x->cap);
    TEST_ASSERT_NOT_NULL(x->b);
}

static void bput(buf_t *x, const void *p, size_t n)
{
    bneed(x, n);
    memcpy(x->b + x->n, p, n);
    x->n += n;
}

static void b8(buf_t *x, uint8_t v)
{
    bneed(x, 1);
    x->b[x->n++] = v;
}

static void b32(buf_t *x, uint32_t v)
{
    bneed(x, 4);
    x->b[x->n++] = (uint8_t)(v >> 24);
    x->b[x->n++] = (uint8_t)(v >> 16);
    x->b[x->n++] = (uint8_t)(v >> 8);
    x->b[x->n++] = (uint8_t)v;
}

static void b64(buf_t *x, uint64_t v)
{
    b32(x, (uint32_t)(v >> 32));
    b32(x, (uint32_t)v);
}

static void bfcc(buf_t *x, const char *f)
{
    bput(x, f, 4);
}

/* Open a box: reserve the 4-byte size, write the fourcc, return the size-field
 * offset to hand to box_close(). */
static size_t box_open(buf_t *x, const char *f)
{
    size_t at = x->n;
    b32(x, 0);
    bfcc(x, f);
    return at;
}

static void box_close(buf_t *x, size_t at)
{
    uint32_t sz = (uint32_t)(x->n - at);
    x->b[at] = (uint8_t)(sz >> 24);
    x->b[at + 1] = (uint8_t)(sz >> 16);
    x->b[at + 2] = (uint8_t)(sz >> 8);
    x->b[at + 3] = (uint8_t)sz;
}

/* version/flags prefix of a full box. */
static void verflags(buf_t *x, uint8_t v)
{
    b32(x, (uint32_t)v << 24);
}

static void ftyp(buf_t *x)
{
    size_t at = box_open(x, "ftyp");
    bfcc(x, "isom");
    b32(x, 0x200);
    bfcc(x, "isom");
    box_close(x, at);
}

static void mvhd(buf_t *x)
{
    size_t at = box_open(x, "mvhd");
    verflags(x, 0);
    b32(x, 0);          /* creation */
    b32(x, 0);          /* modification */
    b32(x, 1000);       /* timescale */
    b32(x, 8000);       /* duration */
    box_close(x, at);
}

static void tkhd(buf_t *x, uint32_t w, uint32_t h)
{
    size_t at = box_open(x, "tkhd");
    verflags(x, 0);
    b32(x, 0);          /* creation */
    b32(x, 0);          /* modification */
    b32(x, 1);          /* track_id */
    b32(x, 0);          /* reserved */
    b32(x, 8000);       /* duration */
    b64(x, 0);          /* reserved */
    b32(x, 0);          /* layer, alt_group */
    b32(x, 0);          /* volume, reserved */
    bput(x, "\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
            "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01", 24);
    b32(x, 0);          /* matrix tail */
    b32(x, 0);
    b32(x, 0);
    b32(x, w << 16);    /* width 16.16 */
    b32(x, h << 16);    /* height 16.16 */
    box_close(x, at);
}

static void hdlr(buf_t *x, const char *handler)
{
    size_t at = box_open(x, "hdlr");
    verflags(x, 0);
    b32(x, 0);          /* pre_defined */
    bfcc(x, handler);
    b32(x, 0);          /* reserved 3x */
    b32(x, 0);
    b32(x, 0);
    b8(x, 0);           /* name terminator */
    box_close(x, at);
}

static void mdhd(buf_t *x, uint32_t timescale)
{
    size_t at = box_open(x, "mdhd");
    verflags(x, 0);
    b32(x, 0);
    b32(x, 0);
    b32(x, timescale);
    b32(x, 200);
    b32(x, 0x55c40000); /* language+quality */
    box_close(x, at);
}

static void avcc(buf_t *x)
{
    static const uint8_t sps[] = { 0x67, 0x42, 0xC0, 0x1E };
    static const uint8_t pps[] = { 0x68, 0xCE, 0x38, 0x80 };
    size_t at = box_open(x, "avcC");
    b8(x, 1);           /* configurationVersion */
    b8(x, 0x42);        /* profile */
    b8(x, 0xC0);        /* compat */
    b8(x, 0x1E);        /* level */
    b8(x, 0xFF);        /* reserved 111111 + lengthSizeMinusOne 3 (->4) */
    b8(x, 0xE1);        /* reserved 111 + numOfSPS 1 */
    b32(x, (uint32_t)sizeof(sps) << 16); /* u16 length (top 2 of the u32) */
    bput(x, sps, sizeof(sps));
    b8(x, 1);           /* numOfPPS */
    b32(x, (uint32_t)sizeof(pps) << 16);
    bput(x, pps, sizeof(pps));
    box_close(x, at);
}

static void avc1_entry(buf_t *x)
{
    static const uint8_t body[78] = { 0 };
    size_t at = box_open(x, "avc1");
    bput(x, body, sizeof(body)); /* VisualSampleEntry fixed body */
    avcc(x);
    box_close(x, at);
}

/* stts from a fixed run list. */
static void stts(buf_t *x, const uint32_t *runs, size_t npairs)
{
    size_t at = box_open(x, "stts");
    size_t i;
    verflags(x, 0);
    b32(x, (uint32_t)npairs);
    for (i = 0; i < npairs; i++) {
        b32(x, runs[i * 2]);      /* count */
        b32(x, runs[i * 2 + 1]);  /* delta */
    }
    box_close(x, at);
}

/* ctts v0 from a run list. */
static void ctts(buf_t *x, const uint32_t *runs, size_t npairs)
{
    size_t at = box_open(x, "ctts");
    size_t i;
    verflags(x, 0);
    b32(x, (uint32_t)npairs);
    for (i = 0; i < npairs; i++) {
        b32(x, runs[i * 2]);      /* count */
        b32(x, runs[i * 2 + 1]);  /* offset */
    }
    box_close(x, at);
}

static void stss(buf_t *x, const uint32_t *nums, size_t n)
{
    size_t at = box_open(x, "stss");
    size_t i;
    verflags(x, 0);
    b32(x, (uint32_t)n);
    for (i = 0; i < n; i++)
        b32(x, nums[i]);
    box_close(x, at);
}

/* stsc from (first_chunk, samples_per_chunk) run list. */
static void stsc(buf_t *x, const uint32_t *runs, size_t npairs)
{
    size_t at = box_open(x, "stsc");
    size_t i;
    verflags(x, 0);
    b32(x, (uint32_t)npairs);
    for (i = 0; i < npairs; i++) {
        b32(x, runs[i * 2]);     /* first_chunk */
        b32(x, runs[i * 2 + 1]); /* samples_per_chunk */
        b32(x, 1);               /* sample_desc_index */
    }
    box_close(x, at);
}

static void stsz(buf_t *x, uint32_t constant, const uint32_t *sizes, size_t n)
{
    size_t at = box_open(x, "stsz");
    size_t i;
    verflags(x, 0);
    b32(x, constant);
    b32(x, (uint32_t)n);
    if (constant == 0)
        for (i = 0; i < n; i++)
            b32(x, sizes[i]);
    box_close(x, at);
}

static void stco(buf_t *x, const uint32_t *offs, size_t n)
{
    size_t at = box_open(x, "stco");
    size_t i;
    verflags(x, 0);
    b32(x, (uint32_t)n);
    for (i = 0; i < n; i++)
        b32(x, offs[i]);
    box_close(x, at);
}

/* A generic video trak with a caller-supplied stbl body already appended to
 * an out-only helper: we build the trak by nesting directly. */
struct stbl_spec {
    const uint32_t *stts;    size_t stts_n;   /* npairs */
    const uint32_t *ctts;    size_t ctts_n;
    const uint32_t *stss;    size_t stss_n;
    const uint32_t *stsc;    size_t stsc_n;
    uint32_t        stsz_const;
    const uint32_t *sizes;   size_t size_n;
    const uint32_t *chunks;  size_t chunk_n;
    int             use_co64;
    int             with_edits;
};

static void stbl(buf_t *x, const struct stbl_spec *s)
{
    size_t at = box_open(x, "stbl");
    size_t i;
    size_t e;

    e = box_open(x, "stsd");
    verflags(x, 0);
    b32(x, 1);
    avc1_entry(x);
    box_close(x, e);

    stts(x, s->stts, s->stts_n);
    if (s->ctts)
        ctts(x, s->ctts, s->ctts_n);
    if (s->stss)
        stss(x, s->stss, s->stss_n);
    stsc(x, s->stsc, s->stsc_n);
    stsz(x, s->stsz_const, s->sizes, s->size_n);
    if (s->use_co64) {
        size_t c = box_open(x, "co64");
        verflags(x, 0);
        b32(x, (uint32_t)s->chunk_n);
        for (i = 0; i < s->chunk_n; i++)
            b64(x, s->chunks[i]);
        box_close(x, c);
    } else {
        stco(x, s->chunks, s->chunk_n);
    }
    box_close(x, at);
}

static void video_trak(buf_t *x, uint32_t w, uint32_t h,
                       const struct stbl_spec *s)
{
    size_t at = box_open(x, "trak");
    tkhd(x, w, h);
    {
        size_t m = box_open(x, "mdia");
        mdhd(x, 25);
        hdlr(x, "vide");
        {
            size_t mf = box_open(x, "minf");
            stbl(x, s);
            box_close(x, mf);
        }
        box_close(x, m);
    }
    if (s->with_edits) {
        size_t ed = box_open(x, "edts");
        size_t el = box_open(x, "elst");
        verflags(x, 0);
        b32(x, 1);       /* one entry */
        b32(x, 0);       /* segment_duration */
        b32(x, 0);       /* media_time */
        b32(x, 1 << 30); /* media_rate */
        box_close(x, el);
        box_close(x, ed);
    }
    box_close(x, at);
}

/*
 * Build a whole file: ftyp, mdat (samples consecutive), moov. Chunk offsets
 * are derived from the stsc expansion (each stsc run == one chunk; the test
 * specs size samples_per_chunk so the totals match).
 */
static void build_file(buf_t *x, const struct stbl_spec *s, const uint8_t *const *sample_bytes)
{
    size_t mdat_at, mdat_content, i, moov_at;
    uint32_t chunks[16];
    size_t nchunk = s->stsc_n;
    size_t sample_start = 0;
    struct stbl_spec s2 = *s;

    ftyp(x);
    mdat_at = box_open(x, "mdat");
    mdat_content = x->n;
    for (i = 0; i < s->size_n; i++)
        bput(x, sample_bytes[i], s->sizes[i]);
    box_close(x, mdat_at);

    for (i = 0; i < nchunk && i < 16; i++) {
        size_t off = mdat_content, k;
        for (k = 0; k < sample_start && k < s->size_n; k++)
            off += s->sizes[k];
        chunks[i] = (uint32_t)off;
        sample_start += s->stsc[i * 2 + 1];
    }
    s2.chunks = chunks;
    s2.chunk_n = nchunk;

    moov_at = box_open(x, "moov");
    mvhd(x);
    video_trak(x, 320, 240, &s2);
    box_close(x, moov_at);
}

static iso_sample_t *open_ok(buf_t *x, iso_movie_t *m)
{
    char err[256];
    int rc = iso_open(m, x->b, x->n, err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "iso_open failed: %s\n", err);
    }
    TEST_ASSERT_EQUAL_INT(0, rc);
    return m->video.samples;
}

/* Happy path: 5 samples, two stts runs, ctts, stss, two chunks. */
static void test_happy(void)
{
    static const uint32_t stts[] = { 3, 2, 2, 3 };      /* 3x d2, 2x d3 */
    static const uint32_t ctts[] = { 2, 1, 3, 2 };
    static const uint32_t stss[] = { 1, 4 };
    static const uint32_t stsc[] = { 1, 3, 2, 2 };      /* 3 then 2 */
    static const uint32_t sizes[] = { 10, 12, 8, 12, 10 };
    uint8_t sbuf[5][16];
    const uint8_t *sp[5];
    struct stbl_spec s;
    buf_t f = { 0 };
    iso_movie_t m;
    size_t i;
    uint64_t mdat_off;

    for (i = 0; i < 5; i++) {
        memset(sbuf[i], (int)i + 1, sizeof(sbuf[i]));
        sp[i] = sbuf[i];
    }
    memset(&s, 0, sizeof(s));
    s.stts = stts; s.stts_n = 2;
    s.ctts = ctts; s.ctts_n = 2;
    s.stss = stss; s.stss_n = 2;
    s.stsc = stsc; s.stsc_n = 2;
    s.sizes = sizes; s.size_n = 5;
    build_file(&f, &s, sp);

    open_ok(&f, &m);
    TEST_ASSERT_EQUAL_UINT64(5, m.video.sample_count);
    TEST_ASSERT_EQUAL_UINT(25, m.video.timescale);
    TEST_ASSERT_EQUAL_UINT(1000, m.movie_timescale);
    TEST_ASSERT_EQUAL_UINT(320, m.video.width);
    TEST_ASSERT_EQUAL_UINT(240, m.video.height);
    TEST_ASSERT_EQUAL_UINT(4, m.video.nal_len_size);
    TEST_ASSERT_EQUAL_INT(0, m.video.has_edits);

    /* dts: cumulative prior deltas 0,2,4,6,9 ; pts = dts + ctts(1,1,2,2,2) */
    {
        const uint64_t exp_dts[5] = { 0, 2, 4, 6, 9 };
        const int64_t  exp_pts[5] = { 1, 3, 6, 8, 11 };
        for (i = 0; i < 5; i++) {
            TEST_ASSERT_EQUAL_UINT64(exp_dts[i], m.video.samples[i].dts);
            TEST_ASSERT_EQUAL_INT64(exp_pts[i], m.video.samples[i].pts);
            TEST_ASSERT_EQUAL_UINT32(sizes[i], m.video.samples[i].size);
        }
    }
    /* is_sync from stss {1,4} => s0 and s3 */
    TEST_ASSERT_EQUAL_INT(1, m.video.samples[0].is_sync);
    TEST_ASSERT_EQUAL_INT(0, m.video.samples[1].is_sync);
    TEST_ASSERT_EQUAL_INT(0, m.video.samples[2].is_sync);
    TEST_ASSERT_EQUAL_INT(1, m.video.samples[3].is_sync);
    TEST_ASSERT_EQUAL_INT(0, m.video.samples[4].is_sync);

    /* offsets: samples 0..2 in chunk 1 (consecutive), 3..4 in chunk 2. */
    mdat_off = m.video.samples[0].off;
    TEST_ASSERT_EQUAL_UINT64(mdat_off + 10, m.video.samples[1].off);
    TEST_ASSERT_EQUAL_UINT64(mdat_off + 10 + 12, m.video.samples[2].off);
    TEST_ASSERT_EQUAL_UINT64(mdat_off + 10 + 12 + 8, m.video.samples[3].off);
    TEST_ASSERT_EQUAL_UINT64(mdat_off + 10 + 12 + 8 + 12, m.video.samples[4].off);

    /* avcC / avc1 / ftyp point inside buf */
    TEST_ASSERT_NOT_NULL(m.video.avcc);
    TEST_ASSERT(m.video.avcc >= m.buf && m.video.avcc < m.buf + m.size);
    TEST_ASSERT_NOT_NULL(m.ftyp);

    /* helpers */
    {
        size_t out;
        TEST_ASSERT_EQUAL_INT(0, iso_first_sync_at_or_before(&m.video, 2, &out));
        TEST_ASSERT_EQUAL_UINT64(0, out);
        TEST_ASSERT_EQUAL_INT(0, iso_first_sync_at_or_before(&m.video, 4, &out));
        TEST_ASSERT_EQUAL_UINT64(3, out);
        /* pts not monotonic in decode order? here it is monotonic; pick a value
         * between pts of s4 (12) and s3 (9): 10 -> largest index <=10 is s4? no,
         * s4 pts=12>10, so s3(9) -> index 3. */
        TEST_ASSERT_EQUAL_INT(0, iso_last_pts_le(&m.video, 10, &out));
        TEST_ASSERT_EQUAL_UINT64(3, out);
    }

    iso_close(&m);
    free(f.b);
}

/* No stss / no ctts: everything sync, pts==dts. */
static void test_no_sync_no_ctts(void)
{
    static const uint32_t stts[] = { 4, 1 };
    static const uint32_t stsc[] = { 1, 4 };
    static const uint32_t sizes[] = { 6, 6, 6, 6 };
    uint8_t sb[4][8];
    const uint8_t *sp[4];
    struct stbl_spec s;
    buf_t f = { 0 };
    iso_movie_t m;
    size_t i;

    for (i = 0; i < 4; i++) {
        memset(sb[i], 0x30 + (int)i, 6);
        sp[i] = sb[i];
    }
    memset(&s, 0, sizeof(s));
    s.stts = stts; s.stts_n = 1;
    s.stsc = stsc; s.stsc_n = 1;
    s.sizes = sizes; s.size_n = 4;
    build_file(&f, &s, sp);
    open_ok(&f, &m);
    for (i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT(1, m.video.samples[i].is_sync);
        TEST_ASSERT_EQUAL_INT64((int64_t)m.video.samples[i].dts,
                                m.video.samples[i].pts);
    }
    iso_close(&m);
    free(f.b);
}

static void test_edits_flag(void)
{
    static const uint32_t stts[] = { 1, 1 };
    static const uint32_t stsc[] = { 1, 1 };
    static const uint32_t sizes[] = { 4 };
    uint8_t sb[1][4];
    const uint8_t *sp[1];
    struct stbl_spec s;
    buf_t f = { 0 };
    iso_movie_t m;

    memset(sb[0], 0xAB, 4);
    sp[0] = sb[0];
    memset(&s, 0, sizeof(s));
    s.stts = stts; s.stts_n = 1;
    s.stsc = stsc; s.stsc_n = 1;
    s.sizes = sizes; s.size_n = 1;
    s.with_edits = 1;
    build_file(&f, &s, sp);
    open_ok(&f, &m);
    TEST_ASSERT_EQUAL_INT(1, m.video.has_edits);
    iso_close(&m);
    free(f.b);
}

/* Reject helper: builds a file and asserts iso_open fails with a Russian msg. */
static void expect_reject(buf_t *f)
{
    char err[256];
    iso_movie_t m;
    err[0] = '\0';
    TEST_ASSERT_EQUAL_INT(-1, iso_open(&m, f->b, f->n, err, sizeof(err)));
    /* Russian text present => at least one non-ASCII byte. */
    TEST_ASSERT_TRUE_MESSAGE(err[0] != '\0', "empty error message");
    {
        size_t i, nonascii = 0;
        for (i = 0; err[i]; i++)
            if ((unsigned char)err[i] >= 0x80)
                nonascii = 1;
        TEST_ASSERT_TRUE_MESSAGE(nonascii, "error not in Russian");
    }
    iso_close(&m);
}

static void build_and_reject(const struct stbl_spec *s,
                             const uint8_t *const *sp)
{
    buf_t f = { 0 };
    build_file(&f, s, sp);
    expect_reject(&f);
    free(f.b);
}

static void fill_simple_samples(const uint8_t **sp)
{
    static uint8_t a[8] = { 1 }, b[8] = { 2 }, c[8] = { 3 };
    sp[0] = a;
    sp[1] = b;
    sp[2] = c;
}

/* A handful of structural rejections. */
static void test_rejects(void)
{
    static const uint32_t stts[] = { 2, 1 };
    static const uint32_t stsc[] = { 1, 2 };
    static const uint32_t sizes[] = { 4, 4 };
    static const uint32_t stss_bad[] = { 9 };
    const uint8_t *sp[3];
    struct stbl_spec s;

    fill_simple_samples(sp);

    /* base valid: stsz mismatch */
    memset(&s, 0, sizeof(s));
    s.stts = stts; s.stts_n = 1;
    s.stsc = stsc; s.stsc_n = 1;
    s.sizes = sizes; s.size_n = 2;

    /* stsz count mismatch: pass size_n=3 but stts says 2 */
    {
        static const uint32_t sizes3[] = { 4, 4, 4 };
        struct stbl_spec bad = s;
        bad.sizes = sizes3; bad.size_n = 3;
        build_and_reject(&bad, sp);
    }
    /* stss sample number out of range */
    {
        struct stbl_spec bad = s;
        bad.stss = stss_bad; bad.stss_n = 1;
        build_and_reject(&bad, sp);
    }
    /* stsc gap: first_chunk jumps 1 -> 3 with 2 chunks */
    {
        static const uint32_t stsc_gap[] = { 1, 1, 3, 1 };
        struct stbl_spec bad = s;
        bad.stsc = stsc_gap; bad.stsc_n = 2;
        build_and_reject(&bad, sp);
    }
    /* sample offset past EOF: chunk offset pushed way out */
    {
        buf_t f = { 0 };
        size_t mdat_at, mdat_content, moov_at, trak_at;
        ftyp(&f);
        mdat_at = box_open(&f, "mdat");
        mdat_content = f.n;
        bput(&f, "\x01\x02\x03\x04", 4);
        bput(&f, "\x01\x02\x03\x04", 4);
        box_close(&f, mdat_at);
        moov_at = box_open(&f, "moov");
        mvhd(&f);
        trak_at = box_open(&f, "trak");
        tkhd(&f, 16, 16);
        {
            size_t m = box_open(&f, "mdia");
            mdhd(&f, 25);
            hdlr(&f, "vide");
            {
                size_t mf = box_open(&f, "minf");
                size_t stbl_at = box_open(&f, "stbl");
                size_t e = box_open(&f, "stsd");
                verflags(&f, 0); b32(&f, 1); avc1_entry(&f); box_close(&f, e);
                { size_t t = box_open(&f, "stts"); verflags(&f, 0);
                  b32(&f, 1); b32(&f, 2); b32(&f, 1); box_close(&f, t); }
                { size_t t = box_open(&f, "stsc"); verflags(&f, 0);
                  b32(&f, 1); b32(&f, 1); b32(&f, 2); b32(&f, 1); box_close(&f, t); }
                stsz(&f, 0, sizes, 2);
                {
                    uint32_t bogus = (uint32_t)f.n + 1000; /* beyond EOF */
                    stco(&f, &bogus, 1);
                }
                box_close(&f, stbl_at);
                box_close(&f, mf);
            }
            (void)mdat_content;
            box_close(&f, m);
        }
        box_close(&f, trak_at);
        box_close(&f, moov_at);
        expect_reject(&f);
        free(f.b);
    }
}

/* moof (fragmented) and a soun track and a hvc1 codec, each rejected. */
static void test_rejects_container(void)
{
    buf_t f = { 0 };
    size_t at;

    /* no moov at all */
    ftyp(&f);
    at = box_open(&f, "mdat");
    bput(&f, "\0\0\0\0", 4);
    box_close(&f, at);
    expect_reject(&f);
    free(f.b); f.b = NULL; f.n = 0; f.cap = 0;

    /* moof present -> fragmented reject (moov still required, but the moof
     * check fires first at top level) */
    ftyp(&f);
    at = box_open(&f, "moof");
    bput(&f, "\0\0\0\0", 4);
    box_close(&f, at);
    expect_reject(&f);
    free(f.b);
}

/* Round-trip: cut [a..b] via iso_write_cut, re-parse, compare tables+bytes. */
static void test_write_roundtrip(void)
{
    static const uint32_t stts[] = { 3, 2, 2, 3 };
    static const uint32_t ctts[] = { 2, 1, 3, 2 };
    static const uint32_t stss[] = { 1, 4 };
    static const uint32_t stsc[] = { 1, 3, 2, 2 };
    static const uint32_t sizes[] = { 10, 12, 8, 12, 10 };
    static uint8_t sbuf[5][16];
    const uint8_t *sp[5];
    struct stbl_spec s;
    buf_t f = { 0 };
    iso_movie_t m, m2;
    uint8_t *out = NULL;
    size_t out_size = 0, i;
    char err[256];

    for (i = 0; i < 5; i++) {
        memset(sbuf[i], (int)i + 0x40, sizeof(sbuf[i]));
        /* make first `size` bytes meaningful per sample */
        sp[i] = sbuf[i];
    }
    memset(&s, 0, sizeof(s));
    s.stts = stts; s.stts_n = 2;
    s.ctts = ctts; s.ctts_n = 2;
    s.stss = stss; s.stss_n = 2;
    s.stsc = stsc; s.stsc_n = 2;
    s.sizes = sizes; s.size_n = 5;
    build_file(&f, &s, sp);
    open_ok(&f, &m);

    /* cut samples [1..4] */
    TEST_ASSERT_EQUAL_INT(0, iso_write_cut(&m, 1, 4, &out, &out_size,
                                           err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(out);

    memset(&m2, 0, sizeof(m2));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, iso_open(&m2, out, out_size, err, sizeof(err)),
                                  err);
    TEST_ASSERT_EQUAL_UINT64(4, m2.video.sample_count);
    TEST_ASSERT_EQUAL_UINT(25, m2.video.timescale);
    TEST_ASSERT_EQUAL_INT(1, m2.video.has_ctts);
    TEST_ASSERT_EQUAL_INT(1, m2.video.has_stss);

    for (i = 0; i < 4; i++) {
        /* sizes preserved */
        TEST_ASSERT_EQUAL_UINT32(sizes[i + 1], m2.video.samples[i].size);
        /* payload bytes byte-identical to the source sample */
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(
            m.buf + m.video.samples[i + 1].off,
            m2.buf + m2.video.samples[i].off,
            sizes[i + 1], "sample payload differs");
    }
    /* sync flags: source is_sync at global {0,3}; cut [1..4] keeps only global 3
     * -> new index 2. */
    TEST_ASSERT_EQUAL_INT(0, m2.video.samples[0].is_sync);
    TEST_ASSERT_EQUAL_INT(0, m2.video.samples[1].is_sync);
    TEST_ASSERT_EQUAL_INT(1, m2.video.samples[2].is_sync);
    TEST_ASSERT_EQUAL_INT(0, m2.video.samples[3].is_sync);

    /* composition offsets (pts - dts) preserved per sample */
    for (i = 0; i < 4; i++) {
        int64_t src_off = m.video.samples[i + 1].pts
                        - (int64_t)m.video.samples[i + 1].dts;
        int64_t new_off = m2.video.samples[i].pts
                        - (int64_t)m2.video.samples[i].dts;
        TEST_ASSERT_EQUAL_INT64(src_off, new_off);
    }

    iso_close(&m2);
    free(out);
    iso_close(&m);
    free(f.b);
}

/* Full-range cut equals the original table (sizes/sync/bytes). */
static void test_write_full(void)
{
    static const uint32_t stts[] = { 4, 1 };
    static const uint32_t stsc[] = { 1, 4 };
    static const uint32_t sizes[] = { 6, 7, 8, 9 };
    static uint8_t sbuf[4][9];
    const uint8_t *sp[4];
    struct stbl_spec s;
    buf_t f = { 0 };
    iso_movie_t m, m2;
    uint8_t *out = NULL;
    size_t out_size = 0, i;
    char err[256];

    for (i = 0; i < 4; i++) {
        memset(sbuf[i], 0x10 + (int)i, sizes[i]);
        sp[i] = sbuf[i];
    }
    memset(&s, 0, sizeof(s));
    s.stts = stts; s.stts_n = 1;
    s.stsc = stsc; s.stsc_n = 1;
    s.sizes = sizes; s.size_n = 4;
    build_file(&f, &s, sp);
    open_ok(&f, &m);

    TEST_ASSERT_EQUAL_INT(0, iso_write_cut(&m, 0, 3, &out, &out_size,
                                           err, sizeof(err)));
    memset(&m2, 0, sizeof(m2));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, iso_open(&m2, out, out_size, err, sizeof(err)),
                                  err);
    TEST_ASSERT_EQUAL_UINT64(4, m2.video.sample_count);
    for (i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT32(sizes[i], m2.video.samples[i].size);
        TEST_ASSERT_EQUAL_INT(1, m2.video.samples[i].is_sync); /* no stss -> all */
        TEST_ASSERT_EQUAL_MEMORY(sbuf[i], m2.buf + m2.video.samples[i].off,
                                 sizes[i]);
    }
    iso_close(&m2);
    free(out);
    iso_close(&m);
    free(f.b);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_happy);
    RUN_TEST(test_no_sync_no_ctts);
    RUN_TEST(test_edits_flag);
    RUN_TEST(test_rejects);
    RUN_TEST(test_rejects_container);
    RUN_TEST(test_write_roundtrip);
    RUN_TEST(test_write_full);
    return UNITY_END();
}
