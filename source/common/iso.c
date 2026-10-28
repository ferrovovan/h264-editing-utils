/*
 * iso.c - read-only ISO-BMFF (MP4) reader for the single-H.264-video subset.
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

#include "iso.h"

/* Absolute upper bound on decoded sample-table size, far above any real file
 * but low enough to reject a crafted stts/stsz count before allocation. */
#define ISO_MAX_SAMPLES (1u << 26)

/* --- big-endian helpers (byte-at-a-time, alignment safe) ------------------ */
static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd64(const uint8_t *p)
{
    return ((uint64_t)rd32(p) << 32) | (uint64_t)rd32(p + 4);
}

typedef struct {
    const uint8_t *buf;
    size_t size;
    char *err;
    size_t errcap;
} iso_r;

static void iso_err(iso_r *r, const char *msg)
{
    if (r->err && r->errcap)
        snprintf(r->err, r->errcap, "%s", msg);
}

typedef struct {
    const uint8_t *content;
    size_t content_len;
    char type[5];
    const uint8_t *next;
    uint8_t version;
} box_t;

static int box_read(iso_r *r, const uint8_t *p, const uint8_t *parent_end,
                    int top_level, box_t *b)
{
    uint64_t total;
    size_t hdr;
    uint32_t sz;

    if ((size_t)(parent_end - p) < 8) {
        iso_err(r, "усечённый заголовок бокса");
        return -1;
    }
    sz = rd32(p);
    memcpy(b->type, p + 4, 4);
    b->type[4] = '\0';
    hdr = 8;
    if (sz == 1) {
        if ((size_t)(parent_end - p) < 16) {
            iso_err(r, "усечённый 64-битный размер бокса");
            return -1;
        }
        total = rd64(p + 8);
        hdr = 16;
        if (total < 16 || p + total > parent_end) {
            iso_err(r, "некорректный 64-битный размер бокса");
            return -1;
        }
    } else if (sz == 0) {
        if (!top_level) {
            iso_err(r, "бокс нулевого размера внутри контейнера");
            return -1;
        }
        total = (uint64_t)(parent_end - p);
    } else {
        total = sz;
        if (total < 8 || p + total > parent_end) {
            iso_err(r, "некорректный размер бокса");
            return -1;
        }
    }
    b->content = p + hdr;
    b->content_len = (size_t)(total - hdr);
    b->next = p + total;
    b->version = b->content_len ? b->content[0] : 0xFF;
    return 0;
}

/* First child with fourcc: 0 = found, -1 = absent, -2 = malformed child. */
static int find_child(iso_r *r, const box_t *parent, const char *fourcc,
                      box_t *out)
{
    const uint8_t *p = parent->content;
    const uint8_t *end = parent->content + parent->content_len;
    while (p < end) {
        box_t b;
        if (box_read(r, p, end, 0, &b) != 0)
            return -2;
        if (memcmp(b.type, fourcc, 4) == 0) {
            *out = b;
            return 0;
        }
        p = b.next;
    }
    return -1;
}

/* mvhd/mdhd prefix: version/flags, creation, modification, timescale,
 * duration. Returns timescale and duration. */
static int parse_mtimes(const box_t *b, uint32_t *timescale, uint64_t *dur)
{
    int v1 = (b->version == 1);
    size_t off;
    if (b->content_len < (v1 ? 32u : 20u))
        return -1;
    off = 4 + (v1 ? 16u : 8u);
    *timescale = rd32(b->content + off);
    off += 4;
    *dur = v1 ? rd64(b->content + off) : rd32(b->content + off);
    return 0;
}

static int parse_tkhd(iso_r *r, const box_t *trak, iso_video_t *v)
{
    box_t tkhd;
    int rc = find_child(r, trak, "tkhd", &tkhd);
    if (rc == -2)
        return -1;
    if (rc < 0 || tkhd.content_len < 4 + 4 + 8) {
        iso_err(r, "нет/битый tkhd");
        return -1;
    }
    /* width/height are the last two 16.16 fields of tkhd. */
    v->width = rd32(tkhd.content + tkhd.content_len - 8) >> 16;
    v->height = rd32(tkhd.content + tkhd.content_len - 4) >> 16;
    return 0;
}

/* avcC record: lengthSizeMinusOne + 1 (spec reserves value 2 -> 3 bytes). */
static int parse_avcc(iso_r *r, const box_t *avcc, iso_video_t *v)
{
    const uint8_t *c;
    size_t n, i, p;
    uint8_t num_sps;
    if (avcc->content_len < 7) {
        iso_err(r, "обрезанный avcC");
        return -1;
    }
    c = avcc->content;
    n = avcc->content_len;
    if (c[0] != 1) {
        iso_err(r, "неподдерживаемая версия avcC");
        return -1;
    }
    v->nal_len_size = (uint8_t)((c[4] & 3u) + 1u);
    if (v->nal_len_size == 3) {
        iso_err(r, "avcC: длина NAL 3 байта (зарезервировано)");
        return -1;
    }
    num_sps = (uint8_t)(c[5] & 0x1Fu);
    if (num_sps == 0) {
        iso_err(r, "avcC: нет SPS (после резки поток нечем открыть)");
        return -1;
    }
    p = 6;
    for (i = 0; i < num_sps; i++) {
        uint16_t slen;
        if (p + 2 > n) {
            iso_err(r, "avcC: усечённый список SPS");
            return -1;
        }
        slen = (uint16_t)((c[p] << 8) | c[p + 1]);
        p += 2;
        if (p + slen > n) {
            iso_err(r, "avcC: усечённый SPS");
            return -1;
        }
        p += slen;
    }
    v->avcc = c;
    v->avcc_size = n;
    return 0;
}

/* Enter the (single) video sample entry (avc1/avc3) and its avcC. */
static int parse_stsd(iso_r *r, const box_t *stsd, iso_video_t *v)
{
    box_t entry, avcc, pseudo;
    const uint8_t *p, *end;
    uint32_t entry_count;
    int rc;

    if (stsd->content_len < 8) {
        iso_err(r, "обрезанный stsd");
        return -1;
    }
    entry_count = rd32(stsd->content + 4);
    if (entry_count == 0) {
        iso_err(r, "пустой stsd");
        return -1;
    }
    p = stsd->content + 8;
    end = stsd->content + stsd->content_len;
    if (box_read(r, p, end, 0, &entry) != 0)
        return -1;
    if (memcmp(entry.type, "avc1", 4) != 0 && memcmp(entry.type, "avc3", 4) != 0) {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "кодек %.4s не поддерживается (нужен avc1/avc3)", entry.type);
        iso_err(r, msg);
        return -1;
    }
    v->avc1 = entry.content - 8;
    v->avc1_size = (size_t)(entry.next - (entry.content - 8));
    if (entry.content_len < 78) {
        iso_err(r, "обрезанный VisualSampleEntry");
        return -1;
    }
    pseudo.content = entry.content + 78;
    pseudo.content_len = entry.content_len - 78;
    rc = find_child(r, &pseudo, "avcC", &avcc);
    if (rc == -2)
        return -1;
    if (rc < 0) {
        iso_err(r, "нет avcC в описании сэмпла");
        return -1;
    }
    return parse_avcc(r, &avcc, v);
}

/* Total sample count from a run table (stts/ctts: count+delta pairs). */
static int runs_total(iso_r *r, const box_t *box, uint64_t *total)
{
    uint32_t n, i;
    uint64_t sum = 0;
    const uint8_t *c;
    if (box->content_len < 8) {
        iso_err(r, "обрезанная таблица ранов");
        return -1;
    }
    n = rd32(box->content + 4);
    if (n > ISO_MAX_SAMPLES || box->content_len < 8 + (size_t)n * 8) {
        iso_err(r, "неполная/чрезмерная таблица ранов");
        return -1;
    }
    c = box->content + 8;
    for (i = 0; i < n; i++) {
        sum += rd32(c + (size_t)i * 8);
        if (sum > ISO_MAX_SAMPLES) {
            iso_err(r, "слишком много сэмплов");
            return -1;
        }
    }
    *total = sum;
    return 0;
}

/*
 * Expand the sample tables of one stbl into v->samples. Requires stsd already
 * parsed (v already carries avcC/avc1). Fills dts/pts/is_sync/off/size.
 */
static int build_samples(iso_r *r, const box_t *stbl, iso_video_t *v)
{
    box_t stts, ctts, stss, stsc, stsz, stco, co64;
    int have_ctts, have_stss, have_co64, rc;
    uint64_t n_stts = 0, n_ctts = 0, N;
    size_t n;
    uint32_t i, j, chunk_count, stsz_const, stsz_count;
    const uint8_t *c;
    uint64_t dts = 0;
    size_t si;
    uint32_t *sizes = NULL;
    uint32_t *per_chunk = NULL;

    have_ctts = (find_child(r, stbl, "ctts", &ctts) == 0);
    have_stss = (find_child(r, stbl, "stss", &stss) == 0);
    rc = find_child(r, stbl, "stsz", &stsz);
    if (rc == -2)
        return -1;
    if (rc < 0 || stsz.content_len < 12) {
        iso_err(r, "нет/битый stsz");
        return -1;
    }
    have_co64 = (find_child(r, stbl, "co64", &co64) == 0);
    rc = have_co64 ? 0 : find_child(r, stbl, "stco", &stco);
    if (rc == -2)
        return -1;
    if (rc < 0) {
        iso_err(r, "нет stco/co64");
        return -1;
    }
    if (find_child(r, stbl, "stts", &stts) != 0) {
        iso_err(r, "нет stts");
        return -1;
    }
    if (find_child(r, stbl, "stsc", &stsc) != 0) {
        iso_err(r, "нет stsc");
        return -1;
    }

    if (runs_total(r, &stts, &n_stts) != 0)
        return -1;
    N = n_stts;
    if (N == 0) {
        iso_err(r, "нет сэмплов");
        return -1;
    }
    n = (size_t)N;

    stsz_const = rd32(stsz.content + 4);
    stsz_count = rd32(stsz.content + 8);
    if (stsz_count != (uint32_t)N) {
        iso_err(r, "stsz: count не совпадает со stts");
        return -1;
    }
    if (stsz_const == 0
            && stsz.content_len < 12 + (size_t)N * 4) {
        iso_err(r, "stsz: неполный список размеров");
        return -1;
    }

    if (have_ctts) {
        if (runs_total(r, &ctts, &n_ctts) != 0)
            return -1;
        if (n_ctts != N) {
            iso_err(r, "ctts: count не совпадает со stts");
            return -1;
        }
    }

    sizes = malloc(n * sizeof(*sizes));
    v->samples = calloc(N, sizeof(iso_sample_t));
    if (!sizes || !v->samples)
        goto oom;

    if (stsz_const != 0) {
        for (i = 0; i < n; i++)
            sizes[i] = stsz_const;
    } else {
        c = stsz.content + 12;
        for (i = 0; i < n; i++)
            sizes[i] = rd32(c + (size_t)i * 4);
    }

    /* dts/dts_delta from stts (run: count, delta). */
    si = 0;
    c = stts.content + 8;
    for (i = 0; i < rd32(stts.content + 4); i++) {
        uint32_t cnt = rd32(c + (size_t)i * 8);
        uint32_t delta = rd32(c + (size_t)i * 8 + 4);
        for (j = 0; j < cnt; j++, si++) {
            v->samples[si].dts = dts;
            v->samples[si].dts_delta = delta;
            if (dts > UINT64_MAX - delta) {
                iso_err(r, "переполнение dts");
                goto fail;
            }
            dts += delta;
        }
    }
    v->sample_count = si;

    /* pts = dts + ctts offset (ctts absent -> 0). */
    if (have_ctts) {
        int signed_v1 = (ctts.version == 1);
        si = 0;
        c = ctts.content + 8;
        for (i = 0; i < rd32(ctts.content + 4); i++) {
            uint32_t cnt = rd32(c + (size_t)i * 8);
            int64_t off = signed_v1 ? (int64_t)(int32_t)rd32(c + (size_t)i * 8 + 4)
                                    : (int64_t)rd32(c + (size_t)i * 8 + 4);
            for (j = 0; j < cnt; j++, si++)
                v->samples[si].pts = (int64_t)v->samples[si].dts + off;
        }
    } else {
        for (si = 0; si < n; si++)
            v->samples[si].pts = (int64_t)v->samples[si].dts;
    }

    /* is_sync: all sync unless stss present. */
    for (si = 0; si < n; si++)
        v->samples[si].is_sync = have_stss ? 0 : 1;
    if (have_stss) {
        if (stss.content_len < 8) {
            iso_err(r, "обрезанный stss");
            goto fail;
        }
        c = stss.content + 8;
        for (i = 0; i < rd32(stss.content + 4); i++) {
            uint32_t num = rd32(c + (size_t)i * 4);
            if (num == 0 || num > (uint32_t)N) {
                iso_err(r, "stss: номер сэмпла вне диапазона");
                goto fail;
            }
            v->samples[num - 1].is_sync = 1;
        }
    }

    /* chunk offsets */
    {
        const box_t *co = have_co64 ? &co64 : &stco;
        size_t estride = have_co64 ? 8 : 4;
        uint64_t off;
        if (co->content_len < 8) {
            iso_err(r, "обрезанный stco/co64");
            goto fail;
        }
        chunk_count = rd32(co->content + 4);
        if (co->content_len < 8 + (size_t)chunk_count * estride) {
            iso_err(r, "неполный stco/co64");
            goto fail;
        }
        per_chunk = malloc((chunk_count ? chunk_count : 1) * sizeof(*per_chunk));
        if (!per_chunk)
            goto oom;

        /* Expand stsc into samples-per-chunk. */
        {
            uint32_t nrun = rd32(stsc.content + 4);
            uint32_t covered = 0;
            if (stsc.content_len < 8 + (size_t)nrun * 12) {
                iso_err(r, "неполный stsc");
                goto fail;
            }
            c = stsc.content + 8;
            for (i = 0; i < nrun; i++) {
                uint32_t first = rd32(c + (size_t)i * 12);
                uint32_t spc = rd32(c + (size_t)i * 12 + 4);
                uint32_t next = (i + 1 < nrun) ? rd32(c + (size_t)(i + 1) * 12)
                                               : chunk_count + 1;
                if (first != covered + 1 || first > chunk_count || next <= first) {
                    iso_err(r, "stsc: пропуск/порядок чанков");
                    goto fail;
                }
                for (j = first; j < next && j <= chunk_count; j++)
                    per_chunk[j - 1] = spc;
                covered = next - 1;
            }
            if (covered != chunk_count) {
                iso_err(r, "stsc: не покрыты все чанки");
                goto fail;
            }
        }

        /* Walk chunks, assign offsets/sizes. */
        si = 0;
        for (i = 0; i < chunk_count; i++) {
            c = (have_co64 ? co64.content + 8 : stco.content + 8);
            off = have_co64 ? rd64(c + (size_t)i * 8)
                            : (uint64_t)rd32(c + (size_t)i * 4);
            for (j = 0; j < per_chunk[i]; j++, si++) {
                if (si >= n) {
                    iso_err(r, "stsc: больше сэмплов, чем в stts");
                    goto fail;
                }
                if (off + sizes[si] > r->size) {
                    iso_err(r, "сэмпл выходит за конец файла");
                    goto fail;
                }
                v->samples[si].off = off;
                v->samples[si].size = sizes[si];
                off += sizes[si];
            }
        }
        if (si != n) {
            iso_err(r, "stsc: меньше сэмплов, чем в stts");
            goto fail;
        }
    }

    free(sizes);
    free(per_chunk);
    return 0;

oom:
    iso_err(r, "нет памяти");
fail:
    free(sizes);
    free(per_chunk);
    free(v->samples);
    v->samples = NULL;
    v->sample_count = 0;
    return -1;
}

/* Parse the single video trak: tkhd dims, mdia timescale, stbl tables. */
static int parse_video_track(iso_r *r, const box_t *trak, iso_video_t *v)
{
    box_t mdia, mdhd, minf, stbl, stsd;
    int rc;

    if (parse_tkhd(r, trak, v) != 0)
        return -1;
    if (find_child(r, trak, "mdia", &mdia) != 0) {
        iso_err(r, "нет mdia");
        return -1;
    }
    if (find_child(r, &mdia, "mdhd", &mdhd) != 0
            || parse_mtimes(&mdhd, &v->timescale, &v->duration) != 0) {
        iso_err(r, "нет/битый mdhd");
        return -1;
    }
    if (find_child(r, &mdia, "minf", &minf) != 0
            || find_child(r, &minf, "stbl", &stbl) != 0) {
        iso_err(r, "нет stbl");
        return -1;
    }
    if (find_child(r, &stbl, "stsd", &stsd) != 0) {
        iso_err(r, "нет stsd");
        return -1;
    }
    if (parse_stsd(r, &stsd, v) != 0)
        return -1;
    if (build_samples(r, &stbl, v) != 0)
        return -1;

    rc = 0;
    return rc;
}

int iso_open(iso_movie_t *m, const uint8_t *buf, size_t size,
             char *err, size_t errcap)
{
    iso_r r;
    box_t moov, moovbox;
    const uint8_t *p, *top_end;
    int video_found = 0;

    memset(m, 0, sizeof(*m));
    m->buf = buf;
    m->size = size;
    r.buf = buf;
    r.size = size;
    r.err = err;
    r.errcap = errcap;
    if (err && errcap)
        err[0] = '\0';

    if (size < 8) {
        iso_err(&r, "файл слишком мал для MP4");
        return -1;
    }
    p = buf;
    top_end = buf + size;
    memset(&moovbox, 0, sizeof(moovbox));
    while (p < top_end) {
        box_t b;
        if (box_read(&r, p, top_end, 1, &b) != 0)
            return -1;
        if (memcmp(b.type, "ftyp", 4) == 0 && !m->ftyp) {
            m->ftyp = p;
            m->ftyp_size = (size_t)(b.next - p);
        } else if (memcmp(b.type, "moov", 4) == 0) {
            if (moovbox.content_len) {
                iso_err(&r, "несколько moov-боксов");
                return -1;
            }
            moovbox = b;
        } else if (memcmp(b.type, "moof", 4) == 0) {
            iso_err(&r, "поддержка фрагментированных MP4 не реализована");
            return -1;
        }
        p = b.next;
    }
    if (!moovbox.content_len) {
        iso_err(&r, "нет moov (это MP4?)");
        return -1;
    }
    if (!m->ftyp) {
        iso_err(&r, "нет ftyp");
        return -1;
    }
    moov = moovbox;

    {
        box_t mvhd;
        if (find_child(&r, &moov, "mvhd", &mvhd) != 0
                || parse_mtimes(&mvhd, &m->movie_timescale, &m->movie_duration) != 0) {
            iso_err(&r, "нет/битый mvhd");
            return -1;
        }
        if (m->movie_timescale == 0) {
            iso_err(&r, "mvhd: нулевой timescale");
            return -1;
        }
    }

    p = moov.content;
    {
        const uint8_t *mend = moov.content + moov.content_len;
        while (p < mend) {
            box_t tk, mdia, hdlr, edts;
            const char *handler;
            int rc;
            if (box_read(&r, p, mend, 0, &tk) != 0)
                return -1;
            p = tk.next;
            if (memcmp(tk.type, "trak", 4) != 0)
                continue;
            if (find_child(&r, &tk, "mdia", &mdia) == -2)
                return -1;
            rc = find_child(&r, &mdia, "hdlr", &hdlr);
            if (rc == -2)
                return -1;
            if (rc < 0 || hdlr.content_len < 12) {
                iso_err(&r, "нет hdlr в дорожке");
                return -1;
            }
            handler = (const char *)hdlr.content + 8; /* verflags + pre_defined */
            if (memcmp(handler, "soun", 4) == 0) {
                m->has_audio = 1;
                iso_err(&r, "аудио-дорожки не поддерживаются (удалите: ffmpeg -an)");
                return -1;
            }
            if (memcmp(handler, "vide", 4) != 0) {
                char msg[96];
                snprintf(msg, sizeof(msg),
                         "дорожка %.4s не поддерживается (нужно видео)", handler);
                iso_err(&r, msg);
                return -1;
            }
            if (video_found) {
                iso_err(&r, "несколько видео-дорожек");
                return -1;
            }
            if (parse_video_track(&r, &tk, &m->video) != 0)
                return -1;
            if (m->video.timescale == 0) {
                iso_err(&r, "mdhd: нулевой timescale");
                goto fail;
            }
            if (find_child(&r, &tk, "edts", &edts) == 0) {
                box_t elst;
                if (find_child(&r, &edts, "elst", &elst) == 0
                        && elst.content_len >= 8 && rd32(elst.content + 4) > 0)
                    m->video.has_edits = 1;
            }
            video_found = 1;
        }
    }
    if (!video_found) {
        iso_err(&r, "нет видео-дорожки");
        return -1;
    }
    return 0;

fail:
    free(m->video.samples);
    memset(m, 0, sizeof(*m));
    return -1;
}

void iso_close(iso_movie_t *m)
{
    free(m->video.samples);
    memset(m, 0, sizeof(*m));
}

int iso_first_sync_at_or_before(const iso_video_t *v, size_t dec_index,
                                size_t *out)
{
    size_t i, lim = dec_index;
    int found = 0;
    if (lim >= v->sample_count)
        lim = v->sample_count ? v->sample_count - 1 : 0;
    for (i = 0; i <= lim; i++) {
        if (v->samples[i].is_sync) {
            *out = i;
            found = 1;
        }
    }
    return found ? 0 : -1;
}

int iso_last_pts_le(const iso_video_t *v, int64_t pts_ticks, size_t *out)
{
    size_t i;
    int found = 0;
    for (i = 0; i < v->sample_count; i++) {
        if (v->samples[i].pts <= pts_ticks) {
            *out = i;
            found = 1;
        }
    }
    return found ? 0 : -1;
}
