/*
 * transcode.c - minimal re-encoding of GOP fragments for --smart cuts.
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

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "index.h"
#include "params.h"
#include "transcode.h"

/* snprintf wrapper with truncation; err is always NUL-terminated. */
static void t_err(char *err, size_t errcap, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 3, 4)))
#endif
    ;

static void t_err(char *err, size_t errcap, const char *fmt, ...)
{
    va_list ap;

    if (!err || errcap == 0) {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(err, errcap, fmt, ap);
    va_end(ap);
    err[errcap - 1] = '\0';
}

static void zero_out(uint8_t **out_buf, size_t *out_size, nal_list_t *out_nals)
{
    if (out_buf) {
        *out_buf = NULL;
    }
    if (out_size) {
        *out_size = 0;
    }
    if (out_nals) {
        out_nals->items = NULL;
        out_nals->count = 0;
    }
}

#ifndef H264EU_HAVE_SMART

int transcode_available(void)
{
    return 0;
}

int transcode_range(const uint8_t *buf, const stream_index_t *idx,
                    size_t feed_first, size_t enc_first, size_t enc_last,
                    int crf, uint32_t fps_num, uint32_t fps_den,
                    uint8_t **out_buf, size_t *out_size,
                    nal_list_t *out_nals,
                    char *err, size_t errcap)
{
    (void)buf;
    (void)idx;
    (void)feed_first;
    (void)enc_first;
    (void)enc_last;
    (void)crf;
    (void)fps_num;
    (void)fps_den;
    zero_out(out_buf, out_size, out_nals);
    t_err(err, errcap,
          "собрано без поддержки --smart (нужны x264 и libavcodec)");
    return TRANSCODE_UNSUPPORTED;
}

#else /* H264EU_HAVE_SMART */

#include <errno.h>
#include <limits.h>

#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>

/*
 * x264.h declares the very same enumerators (NAL_SLICE..NAL_FILLER) as our
 * annexb.h. They appear only inside its enum declaration, so rename them
 * away while including and restore ours right after (annexb.h is untouchable).
 */
#define NAL_SLICE     x264eu_NAL_SLICE_
#define NAL_SLICE_DPA x264eu_NAL_SLICE_DPA_
#define NAL_SLICE_DPB x264eu_NAL_SLICE_DPB_
#define NAL_SLICE_DPC x264eu_NAL_SLICE_DPC_
#define NAL_SEI       x264eu_NAL_SEI_
#define NAL_SPS       x264eu_NAL_SPS_
#define NAL_PPS       x264eu_NAL_PPS_
#define NAL_AUD       x264eu_NAL_AUD_
#define NAL_FILLER    x264eu_NAL_FILLER_
#include <x264.h>
#undef NAL_SLICE
#undef NAL_SLICE_DPA
#undef NAL_SLICE_DPB
#undef NAL_SLICE_DPC
#undef NAL_SEI
#undef NAL_SPS
#undef NAL_PPS
#undef NAL_AUD
#undef NAL_FILLER

/*
 * h264_stream.h defines its own nal_t which clashes with ours (annexb.h).
 * Same rename trick as params.c: neither header may be edited.
 */
#define nal_t h264bitstream_nal_t_
#include <h264_stream.h>
#undef nal_t

/* Growable byte buffer; every growth is overflow-checked. */
typedef struct {
    uint8_t *data;
    size_t size, cap;
} obuf_t;

static int obuf_append(obuf_t *o, const void *src, size_t n)
{
    size_t need, ncap;
    uint8_t *p;

    if (n == 0) {
        return 0;
    }
    if (n > SIZE_MAX - o->size) {
        return -1;
    }
    need = o->size + n;
    if (need > o->cap) {
        ncap = o->cap ? o->cap : 4096;
        while (ncap < need) {
            if (ncap > SIZE_MAX / 2) {
                ncap = need;
                break;
            }
            ncap *= 2;
        }
        p = realloc(o->data, ncap);
        if (!p) {
            return -1;
        }
        o->data = p;
        o->cap = ncap;
    }
    memcpy(o->data + o->size, src, n);
    o->size += n;
    return 0;
}

/* x264 profile string for a source profile_idc; NULL = do not apply. */
static const char *profile_name(int profile_idc)
{
    switch (profile_idc) {
    case 66:
        return "baseline";
    case 77:
    case 88:
        return "main";
    case 100:
    case 110:
    case 122:
    case 244:
    case 44:
    case 83:
    case 86:
    case 118:
    case 128:
    case 134:
    case 138:
    case 139:
    case 144:
        return "high";
    default:
        return NULL;
    }
}

/* Mutable state of the decode->filter->encode loop. */
typedef struct {
    x264_t *x4;
    x264_picture_t *pic;   /* reused input picture, allocated once */
    obuf_t *out;           /* encoded Annex B output */
    size_t enc_first;      /* pts filter threshold (frame index) */
    size_t warmup;         /* enc_first - feed_first, nopts fallback skip */
    int width, height;
    int nopts;             /* decoder timestamps unavailable -> fallback */
    size_t decoded_total;  /* pictures the decoder produced so far */
    size_t fed;            /* pictures pushed into x264 */
    int failed;            /* an error was recorded in err */
    char *err;
    size_t errcap;
} tx_ctx_t;

/* Copy one decoded frame into the x264 input picture and encode it. */
static void tx_handle_frame(tx_ctx_t *cx, AVFrame *f)
{
    x264_nal_t *nals = NULL;
    x264_picture_t pic_out;
    int want, nnal, n, pl, y;

    if (f->pts == AV_NOPTS_VALUE) {
        cx->nopts = 1;
    }
    if (!cx->nopts) {
        want = (f->pts >= (int64_t)cx->enc_first);
    } else {
        /*
         * Fallback: the decoder gave no pts. Output is in display order and
         * the warm-up pictures ([feed_first, enc_first), fewer pts than
         * enc_first) are displayed first, so skipping the first
         * enc_first - feed_first outputs is equivalent to the pts filter.
         */
        want = (cx->decoded_total >= cx->warmup);
    }
    cx->decoded_total++;
    if (!want) {
        return;
    }
    if (f->format != AV_PIX_FMT_YUV420P) {
        t_err(cx->err, cx->errcap,
              "декодер выдал формат пикселей %d, ожидался YUV420P", (int)f->format);
        cx->failed = 1;
        return;
    }
    for (pl = 0; pl < 3; pl++) {
        int pw = pl ? cx->width / 2 : cx->width;
        int ph = pl ? cx->height / 2 : cx->height;
        const uint8_t *s = f->data[pl];
        uint8_t *d = cx->pic->img.plane[pl];
        int sl = f->linesize[pl];
        int dl = cx->pic->img.i_stride[pl];

        if (!s || sl < pw || dl < pw) {
            t_err(cx->err, cx->errcap,
                  "кадр декодера: плоскость %d недоступна или строка короче %d", pl, pw);
            cx->failed = 1;
            return;
        }
        for (y = 0; y < ph; y++) {
            memcpy(d + (size_t)y * (size_t)dl, s + (size_t)y * (size_t)sl, (size_t)pw);
        }
    }
    cx->pic->i_pts = (int64_t)cx->fed;
    /* The very first coded picture is a forced IDR: the fragment must be
     * independently decodable even when the cut starts mid-GOP. */
    cx->pic->i_type = cx->fed == 0 ? X264_TYPE_IDR : X264_TYPE_AUTO;

    n = x264_encoder_encode(cx->x4, &nals, &nnal, cx->pic, &pic_out);
    if (n < 0) {
        t_err(cx->err, cx->errcap, "x264: ошибка кодирования кадра %zu", cx->fed);
        cx->failed = 1;
        return;
    }
    for (y = 0; y < nnal; y++) {
        if (obuf_append(cx->out, nals[y].p_payload, (size_t)nals[y].i_payload) != 0) {
            t_err(cx->err, cx->errcap, "x264: не хватило памяти под вывод");
            cx->failed = 1;
            return;
        }
    }
    cx->fed++;
}

/* Drain all frames currently produced by the decoder. */
static void tx_drain(tx_ctx_t *cx, AVCodecContext *dec, AVFrame *frm)
{
    int r;

    while ((r = avcodec_receive_frame(dec, frm)) == 0) {
        tx_handle_frame(cx, frm);
        av_frame_unref(frm);
        if (cx->failed) {
            return;
        }
    }
    if (r != AVERROR(EAGAIN) && r != AVERROR_EOF) {
        t_err(cx->err, cx->errcap, "libavcodec: ошибка приёма кадра (%d)", r);
        cx->failed = 1;
    }
}

int transcode_available(void)
{
    return 1;
}

int transcode_range(const uint8_t *buf, const stream_index_t *idx,
                    size_t feed_first, size_t enc_first, size_t enc_last,
                    int crf, uint32_t fps_num, uint32_t fps_den,
                    uint8_t **out_buf, size_t *out_size,
                    nal_list_t *out_nals,
                    char *err, size_t errcap)
{
    h264_stream_t *hs = NULL;
    uint8_t *tmp = NULL;
    const nal_list_t *nals = NULL;
    sps_state_t st;
    const char *prof;
    AVCodecContext *dec = NULL;
    AVFrame *frm = NULL;
    AVPacket *pkt = NULL;
    x264_t *x4 = NULL;
    x264_param_t xp;
    x264_picture_t pic;
    int pic_alloced = 0;
    obuf_t au;
    obuf_t out;
    tx_ctx_t cx;
    size_t au_last, n, i;
    long sps_idx = -1, pps_idx = -1;
    int au_has_sps = 0, au_has_pps = 0;
    int sps_id, width, height;
    int ret = TRANSCODE_ERROR;
    static const uint8_t sc4[4] = { 0, 0, 0, 1 };

    memset(&au, 0, sizeof(au));
    memset(&out, 0, sizeof(out));
    memset(&pic, 0, sizeof(pic));
    zero_out(out_buf, out_size, out_nals);

    if (!buf || !idx || !out_buf || !out_size || !out_nals) {
        t_err(err, errcap, "внутренняя ошибка: пустые аргументы transcode_range");
        return TRANSCODE_ERROR;
    }

    /* --- contract validation (cheap checks before any heavy work) --- */

    if (feed_first > enc_first || enc_first > enc_last || enc_last >= idx->frame_count) {
        t_err(err, errcap,
              "недопустимый диапазон кадров [%zu..%zu] (feed_first=%zu, кадров %zu)",
              enc_first, enc_last, feed_first, idx->frame_count);
        return TRANSCODE_ERROR;
    }
    if (fps_num == 0 || fps_den == 0) {
        t_err(err, errcap, "частота кадров не задана (%u/%u)", fps_num, fps_den);
        return TRANSCODE_UNSUPPORTED;
    }
    if (idx->has_fields) {
        t_err(err, errcap, "чересстрочный контент не поддерживается (--smart)");
        return TRANSCODE_UNSUPPORTED;
    }
    for (n = feed_first; n <= enc_last; n++) {
        if (idx->frames[n].parse_error) {
            t_err(err, errcap,
                  "кадр %zu не разобран (ошибка разбора slice header) — --smart невозможен", n);
            return TRANSCODE_UNSUPPORTED;
        }
    }

    nals = &idx->nals;
    au_last = idx->frames[feed_first].nal_last;

    /* Backward (well, forward-to-the-AU) scan: the last SPS and the last PPS
     * at or before the feed_first AU. NALs of the AU itself win naturally,
     * because parameter sets are attributed to the frame they precede. */
    for (n = 0; n <= au_last; n++) {
        if (nals->items[n].type == NAL_SPS) {
            sps_idx = (long)n;
            au_has_sps = (n >= idx->frames[feed_first].nal_first);
        } else if (nals->items[n].type == NAL_PPS) {
            pps_idx = (long)n;
            au_has_pps = (n >= idx->frames[feed_first].nal_first);
        }
    }
    if (sps_idx < 0) {
        t_err(err, errcap, "SPS не найден перед кадром %zu", feed_first);
        return TRANSCODE_UNSUPPORTED;
    }
    if (pps_idx < 0) {
        t_err(err, errcap, "PPS не найден перед кадром %zu", feed_first);
        return TRANSCODE_UNSUPPORTED;
    }

    /* Parse the chosen SPS to learn its id (and fields if the store lacks it). */
    hs = h264_new();
    if (!hs) {
        t_err(err, errcap, "не удалось создать объект разбора SPS");
        goto fail;
    }
    if (nals->items[sps_idx].size == 0 ||
        nals->items[sps_idx].off + nals->items[sps_idx].size > idx->size) {
        t_err(err, errcap, "битый SPS в потоке");
        goto fail;
    }
    tmp = malloc(nals->items[sps_idx].size);
    if (!tmp) {
        t_err(err, errcap, "не хватило памяти (%zu байт)", nals->items[sps_idx].size);
        goto fail;
    }
    memcpy(tmp, buf + nals->items[sps_idx].off, nals->items[sps_idx].size);
    if (read_nal_unit(hs, tmp, (int)nals->items[sps_idx].size) < 0) {
        t_err(err, errcap, "не удалось разобрать SPS");
        goto fail;
    }
    free(tmp);
    tmp = NULL;
    sps_id = hs->sps->seq_parameter_set_id;
    if (sps_id < 0 || sps_id >= 32) {
        t_err(err, errcap, "недопустимый seq_parameter_set_id %d", sps_id);
        goto fail;
    }

    memset(&st, 0, sizeof(st));
    if (idx->params.sps[sps_id].valid) {
        st = idx->params.sps[sps_id];
    } else {
        /* Rare: the store has no state for this id - take it from our parse. */
        st.profile_idc = hs->sps->profile_idc;
        st.chroma_format_idc = hs->sps->chroma_format_idc;
        st.frame_mbs_only_flag = hs->sps->frame_mbs_only_flag;
        st.bit_depth_luma_minus8 = hs->sps->bit_depth_luma_minus8;
        st.bit_depth_chroma_minus8 = hs->sps->bit_depth_chroma_minus8;
        st.frame_cropping_flag = hs->sps->frame_cropping_flag;
        st.pic_width_in_mbs_minus1 = hs->sps->pic_width_in_mbs_minus1;
        st.pic_height_in_map_units_minus1 = hs->sps->pic_height_in_map_units_minus1;
    }

    /* v1 limits of the contract. */
    if (!st.frame_mbs_only_flag) {
        t_err(err, errcap, "frame_mbs_only_flag = 0 (полеовой кодирование) не поддерживается");
        ret = TRANSCODE_UNSUPPORTED;
        goto fail;
    }
    if (st.frame_cropping_flag) {
        t_err(err, errcap, "обрезка кадра (frame_cropping_flag) не поддерживается (--smart)");
        ret = TRANSCODE_UNSUPPORTED;
        goto fail;
    }
    if (st.bit_depth_luma_minus8 > 0 || st.bit_depth_chroma_minus8 > 0) {
        t_err(err, errcap, "битность больше 8 (%d+%d) не поддерживается (--smart)",
              8 + st.bit_depth_luma_minus8, 8 + st.bit_depth_chroma_minus8);
        ret = TRANSCODE_UNSUPPORTED;
        goto fail;
    }
    if (st.chroma_format_idc != 1) {
        t_err(err, errcap, "не 4:2:0 (chroma_format_idc = %d)", st.chroma_format_idc);
        ret = TRANSCODE_UNSUPPORTED;
        goto fail;
    }
    width = (st.pic_width_in_mbs_minus1 + 1) * 16;
    height = (st.pic_height_in_map_units_minus1 + 1) * 16 *
             (2 - st.frame_mbs_only_flag);
    if (width <= 0 || height <= 0 || width > INT_MAX / 2 || height > INT_MAX / 2) {
        t_err(err, errcap, "недопустимый размер кадра %dx%d", width, height);
        goto fail;
    }

    /* --- x264 encoder --- */
    if (x264_param_default_preset(&xp, "medium", NULL) != 0) {
        t_err(err, errcap, "x264: неизвестный пресет medium");
        goto fail;
    }
    xp.rc.i_rc_method = X264_RC_CRF;
    xp.rc.f_rf_constant = (float)crf;
    xp.i_fps_num = fps_num;
    xp.i_fps_den = fps_den;
    xp.i_scenecut_threshold = 0;
    xp.i_threads = 1;
    xp.rc.i_lookahead = 0;
    xp.b_repeat_headers = 1;
    xp.b_annexb = 1;
    xp.i_sps_id = sps_id;
    xp.i_width = width;
    xp.i_height = height;
    xp.i_csp = X264_CSP_I420;
    xp.b_opencl = 0;
    /* Our own Russian diagnostics are the only stderr output (AGENTS.md);
     * silence x264's internal [info]/[warning] chatter, real failures still
     * surface via x264_encoder_open()/encode() return codes handled below. */
    xp.i_log_level = X264_LOG_ERROR;
    prof = profile_name(st.profile_idc);
    if (prof && x264_param_apply_profile(&xp, prof) < 0) {
        t_err(err, errcap, "x264: не удалось применить профиль %s (idc %d)",
              prof, st.profile_idc);
        goto fail;
    }
    x4 = x264_encoder_open(&xp);
    if (!x4) {
        t_err(err, errcap, "x264: x264_encoder_open не удался (%dx%d)", width, height);
        goto fail;
    }
    if (x264_picture_alloc(&pic, X264_CSP_I420, width, height) != 0) {
        t_err(err, errcap, "x264: x264_picture_alloc(%dx%d) не удался", width, height);
        goto fail;
    }
    pic_alloced = 1;

    /* --- libavcodec decoder --- */
    {
        const AVCodec *cd = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!cd) {
            t_err(err, errcap, "libavcodec: декодер H.264 не найден");
            goto fail;
        }
        dec = avcodec_alloc_context3(cd);
        if (!dec) {
            t_err(err, errcap, "libavcodec: avcodec_alloc_context3 не удался");
            goto fail;
        }
        dec->thread_count = 1;
        if (avcodec_open2(dec, cd, NULL) < 0) {
            t_err(err, errcap, "libavcodec: avcodec_open2 не удался");
            goto fail;
        }
    }
    frm = av_frame_alloc();
    pkt = av_packet_alloc();
    if (!frm || !pkt) {
        t_err(err, errcap, "libavcodec: не удалось выделить кадр/пакет");
        goto fail;
    }

    memset(&cx, 0, sizeof(cx));
    cx.x4 = x4;
    cx.pic = &pic;
    cx.out = &out;
    cx.enc_first = enc_first;
    cx.warmup = enc_first - feed_first;
    cx.width = width;
    cx.height = height;
    cx.err = err;
    cx.errcap = errcap;

    /* Feed pictures [feed_first, enc_last] in decoding order,
     * pts = dts = decoding index. */
    for (i = feed_first; i <= enc_last; i++) {
        const frame_t *fr = &idx->frames[i];
        int r;

        au.size = 0;
        if (i == feed_first) {
            if (!au_has_sps &&
                (obuf_append(&au, sc4, 4) != 0 ||
                 obuf_append(&au, buf + nals->items[sps_idx].off,
                             nals->items[sps_idx].size) != 0)) {
                t_err(err, errcap, "не хватило памяти под SPS");
                goto fail;
            }
            if (!au_has_pps &&
                (obuf_append(&au, sc4, 4) != 0 ||
                 obuf_append(&au, buf + nals->items[pps_idx].off,
                             nals->items[pps_idx].size) != 0)) {
                t_err(err, errcap, "не хватило памяти под PPS");
                goto fail;
            }
        }
        for (n = fr->nal_first; n <= fr->nal_last; n++) {
            if (obuf_append(&au, sc4, 4) != 0 ||
                obuf_append(&au, buf + nals->items[n].off, nals->items[n].size) != 0) {
                t_err(err, errcap, "не хватило памяти под access unit");
                goto fail;
            }
        }
        if (au.size > (size_t)INT_MAX) {
            t_err(err, errcap, "access unit слишком большой (%zu байт)", au.size);
            goto fail;
        }
        pkt->data = au.data;
        pkt->size = (int)au.size;
        pkt->pts = (int64_t)i;
        pkt->dts = (int64_t)i;
        pkt->duration = 0;
        r = avcodec_send_packet(dec, pkt);
        if (r == AVERROR(EAGAIN)) {
            tx_drain(&cx, dec, frm);
            if (cx.failed) {
                goto fail;
            }
            r = avcodec_send_packet(dec, pkt);
        }
        if (r < 0) {
            t_err(err, errcap, "libavcodec: avcodec_send_packet не удался для кадра %zu (%d)", i, r);
            goto fail;
        }
        tx_drain(&cx, dec, frm);
        if (cx.failed) {
            goto fail;
        }
    }

    /* Flush the decoder (NULL packet). */
    {
        int r = avcodec_send_packet(dec, NULL);
        if (r < 0 && r != AVERROR_EOF) {
            t_err(err, errcap, "libavcodec: слив декодера не удался (%d)", r);
            goto fail;
        }
        tx_drain(&cx, dec, frm);
        if (cx.failed) {
            goto fail;
        }
    }

    if (cx.fed != enc_last - enc_first + 1) {
        t_err(err, errcap,
              "декодер выдал %zu картинок в диапазон, ожидалось %zu",
              cx.fed, enc_last - enc_first + 1);
        goto fail;
    }

    /* Drain x264 with a NULL picture until it reports end. */
    {
        int guard = 0;
        for (;;) {
            x264_nal_t *xn = NULL;
            x264_picture_t xo;
            int xn_cnt = 0;
            int k;
            int r = x264_encoder_encode(x4, &xn, &xn_cnt, NULL, &xo);

            if (r < 0) {
                t_err(err, errcap, "x264: ошибка при сливе кодировщика");
                goto fail;
            }
            if (r == 0) {
                break;
            }
            for (k = 0; k < xn_cnt; k++) {
                if (obuf_append(&out, xn[k].p_payload, (size_t)xn[k].i_payload) != 0) {
                    t_err(err, errcap, "x264: не хватило памяти под вывод слива");
                    goto fail;
                }
            }
            if (++guard > (int)(enc_last - enc_first + 1) + 16) {
                t_err(err, errcap, "x264: слив кодировщика не завершается");
                goto fail;
            }
        }
    }

    if (out.size == 0) {
        t_err(err, errcap, "кодирование дало пустой вывод");
        ret = TRANSCODE_UNSUPPORTED;
        goto fail;
    }

    *out_buf = out.data;
    *out_size = out.size;
    out.data = NULL;
    if (annexb_scan(*out_buf, *out_size, out_nals) != 0) {
        t_err(err, errcap, "результат перекодирования не проходит annexb_scan");
        free(*out_buf);
        zero_out(out_buf, out_size, out_nals);
        goto fail;
    }
    ret = TRANSCODE_OK;

fail:
    free(au.data);
    free(out.data);
    free(tmp);
    if (pic_alloced) {
        x264_picture_clean(&pic);
    }
    if (x4) {
        x264_encoder_close(x4);
    }
    if (frm) {
        av_frame_free(&frm);
    }
    if (pkt) {
        av_packet_free(&pkt);
    }
    if (dec) {
        avcodec_free_context(&dec);
    }
    if (hs) {
        h264_free(hs);
    }
    if (ret != TRANSCODE_OK) {
        zero_out(out_buf, out_size, out_nals);
    }
    return ret;
}

#endif /* H264EU_HAVE_SMART */
