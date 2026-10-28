/*
 * avcc.c - AVCDecoderConfigurationRecord (ISO/IEC 14496-15) parse/build and
 * the length-prefixed ("AVCC") <-> Annex B byte stream pipeline.
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

#include "annexb.h"
#include "avcc.h"

/*
 * Profiles whose records carry the extended tail (chroma/bit depth + ext
 * SPS list), 14496-15 5.3.3.1.2 (2013 revision). 144 is beyond the 2013
 * list; the project treats it as High-family per its own contract, see
 * tests/unit/test_avcc.c.
 */
static int is_high_profile(unsigned profile_idc)
{
    static const unsigned hi[] = {
        44, 83, 86, 100, 110, 118, 122, 128, 134, 135, 138, 139, 144, 244
    };
    size_t i;

    for (i = 0; i < sizeof(hi) / sizeof(hi[0]); i++) {
        if (profile_idc == hi[i])
            return 1;
    }
    return 0;
}

/* One u16-length-prefixed NAL payload: copy [pos, pos+len) into out. */
static uint8_t *copy_nal_entry(const uint8_t *buf, size_t *pos, size_t size,
                               size_t *len_out)
{
    size_t len;
    uint8_t *copy;

    if (*pos + 2 > size)
        return NULL;
    len = ((size_t)buf[*pos] << 8) | buf[*pos + 1];
    *pos += 2;
    if (len == 0 || len > size - *pos)
        return NULL;
    copy = malloc(len);
    if (!copy)
        return NULL;
    memcpy(copy, buf + *pos, len);
    *pos += len;
    *len_out = len;
    return copy;
}

/* One parameter-set list: u8 count (masked: u5 for the SPS list) followed
 * by count u16-length-prefixed NALs. */
static int parse_nal_list(const uint8_t *buf, size_t *pos, size_t size,
                          unsigned count_mask,
                          uint8_t **items, size_t *item_lens, int *count_out)
{
    unsigned count;
    int i;

    if (*pos >= size)
        return -1;
    count = buf[(*pos)++] & count_mask;
    if (count > AVCC_MAX_SETS)
        return -1;
    for (i = 0; i < (int)count; i++) {
        /* Publish the count as we go: avcc_config_free must reclaim the
         * already parsed entries when a later entry fails. */
        *count_out = i;
        items[i] = copy_nal_entry(buf, pos, size, &item_lens[i]);
        if (!items[i])
            return -1;
    }
    *count_out = (int)count;
    return 0;
}

int avcc_parse(const uint8_t *buf, size_t size, avcc_config_t *out)
{
    size_t pos = 0;

    memset(out, 0, sizeof(*out));
    if (!buf || size < 6)
        goto fail;
    if (buf[0] != 1)
        goto fail; /* configurationVersion */
    out->profile_idc = buf[1];
    out->profile_compat = buf[2];
    out->level_idc = buf[3];
    if ((buf[4] & 0xFC) != 0xFC)
        goto fail; /* the 6 reserved bits must read 0b111111 */
    out->length_size_minus_one = (uint8_t)(buf[4] & 0x03);
    pos = 5;
    /* byte 5: top 3 bits reserved ('111' in the spec text), numOfSPS is
     * the low u5. */
    if (parse_nal_list(buf, &pos, size, 0x1F, out->sps, out->sps_len,
                       &out->sps_count) != 0)
        goto fail;
    if (parse_nal_list(buf, &pos, size, 0xFF, out->pps, out->pps_len,
                       &out->pps_count) != 0)
        goto fail;
    if (is_high_profile(out->profile_idc)) {
        unsigned ext_count;
        size_t ext_len;
        int i;

        if (pos + 4 > size)
            goto fail; /* truncated extended tail */
        out->chroma_format_idc = (uint8_t)(buf[pos] & 0x03);
        out->bit_depth_luma_minus8 = (uint8_t)(buf[pos + 1] & 0x07);
        out->bit_depth_chroma_minus8 = (uint8_t)(buf[pos + 2] & 0x07);
        ext_count = buf[pos + 3] & 0x1F;
        pos += 4;
        for (i = 0; i < (int)ext_count; i++) {
            /* Subset SPS payloads are length-checked but not stored. */
            uint8_t *ext = copy_nal_entry(buf, &pos, size, &ext_len);

            if (!ext)
                goto fail;
            free(ext);
        }
        out->has_ext_fields = 1;
    }
    if (pos != size)
        goto fail; /* ext tail forbidden for other profiles / trailing bytes */
    return 0;

fail:
    avcc_config_free(out);
    return -1;
}

static int build_nal_list(const uint8_t *const *items, const size_t *item_lens,
                          int count, uint8_t **w)
{
    int i;

    *(*w)++ = (uint8_t)count;
    for (i = 0; i < count; i++) {
        if (!items[i] || item_lens[i] == 0 || item_lens[i] > 0xFFFF)
            return -1;
        *(*w)++ = (uint8_t)(item_lens[i] >> 8);
        *(*w)++ = (uint8_t)(item_lens[i] & 0xFF);
        memcpy(*w, items[i], item_lens[i]);
        *w += item_lens[i];
    }
    return 0;
}

int avcc_build(const avcc_config_t *c, uint8_t **out, size_t *out_size)
{
    /* 6 fixed bytes (incl. the u5 numOfSPS written below) + 1 numOfPPS
     * byte written by build_nal_list for the PPS list. */
    size_t total = 7;
    size_t ext_total = 0;
    int high = is_high_profile(c->profile_idc);
    uint8_t *buf;
    uint8_t *w;
    int i;

    if (!c || !out || !out_size || c->length_size_minus_one > 3
            || c->sps_count < 0 || c->sps_count > AVCC_MAX_SETS
            || c->pps_count < 0 || c->pps_count > AVCC_MAX_SETS)
        return -1;
    for (i = 0; i < c->sps_count; i++) {
        if (!c->sps[i] || c->sps_len[i] == 0 || c->sps_len[i] > 0xFFFF
                || total > SIZE_MAX - 2 - c->sps_len[i])
            return -1;
        total += 2 + c->sps_len[i];
    }
    for (i = 0; i < c->pps_count; i++) {
        if (!c->pps[i] || c->pps_len[i] == 0 || c->pps_len[i] > 0xFFFF
                || total > SIZE_MAX - 2 - c->pps_len[i])
            return -1;
        total += 2 + c->pps_len[i];
    }
    if (high)
        ext_total = 4; /* tail header only: numOfSequenceParameterSetExt = 0 */
    if (total > SIZE_MAX - ext_total)
        return -1;
    buf = malloc(total + ext_total);
    if (!buf)
        return -1;
    w = buf;
    *w++ = 1; /* configurationVersion */
    *w++ = c->profile_idc;
    *w++ = c->profile_compat;
    *w++ = c->level_idc;
    *w++ = (uint8_t)(0xFC | c->length_size_minus_one);
    /* numOfSPS in the low 5 bits; the '111' reserved pattern as written
     * by the common muxers (byte value 0xE1 for the single-SPS record).
     * build_nal_list writes the PPS count only. */
    *w++ = (uint8_t)(0xE0 | (c->sps_count & 0x1F));
    {
        /* The SPS list without its count byte (already written). */
        int i2;

        for (i2 = 0; i2 < c->sps_count; i2++) {
            *w++ = (uint8_t)(c->sps_len[i2] >> 8);
            *w++ = (uint8_t)(c->sps_len[i2] & 0xFF);
            memcpy(w, c->sps[i2], c->sps_len[i2]);
            w += c->sps_len[i2];
        }
    }
    if (build_nal_list((const uint8_t *const *)c->pps, c->pps_len,
                       c->pps_count, &w) != 0) {
        free(buf);
        return -1;
    }
    if (high) {
        *w++ = (uint8_t)(0xFC | (c->chroma_format_idc & 0x03));
        *w++ = (uint8_t)(0xF8 | (c->bit_depth_luma_minus8 & 0x07));
        *w++ = (uint8_t)(0xF8 | (c->bit_depth_chroma_minus8 & 0x07));
        *w++ = 0x00; /* reserved 000 + numOfSequenceParameterSetExt = 0 */
    }
    *out = buf;
    *out_size = (size_t)(w - buf);
    return 0;
}

int avcc_to_annexb(const uint8_t *in, size_t in_size, unsigned nal_len,
                   uint8_t **out, size_t *out_size)
{
    static const uint8_t start_code[4] = { 0x00, 0x00, 0x00, 0x01 };
    size_t pos = 0;
    size_t total = 0;
    uint8_t *buf;
    uint8_t *w;

    if (!out || !out_size || nal_len < 2 || nal_len > 4
            || (!in && in_size != 0))
        return -1;
    /* Pass 1: every length field must fit in the buffer, every payload
     * must reach no further than the end of it. */
    while (pos < in_size) {
        size_t len = 0;
        unsigned k;

        if (pos + nal_len > in_size)
            return -1;
        for (k = 0; k < nal_len; k++)
            len = (len << 8) | in[pos + k];
        pos += nal_len;
        if (len > in_size - pos)
            return -1;
        pos += len;
        if (total > SIZE_MAX - 4 - len)
            return -1;
        total += 4 + len;
    }
    buf = malloc(total ? total : 1);
    if (!buf)
        return -1;
    w = buf;
    pos = 0;
    while (pos < in_size) {
        size_t len = 0;
        unsigned k;

        for (k = 0; k < nal_len; k++)
            len = (len << 8) | in[pos + k];
        pos += nal_len;
        memcpy(w, start_code, 4);
        w += 4;
        memcpy(w, in + pos, len);
        w += len;
        pos += len;
    }
    *out = buf;
    *out_size = total;
    return 0;
}

int annexb_to_avcc(const uint8_t *in, size_t in_size, unsigned nal_len,
                   uint8_t **out, size_t *out_size)
{
    nal_list_t nals;
    uint64_t max_len;
    size_t total = 0;
    size_t i;
    uint8_t *buf;
    uint8_t *w;

    if (!out || !out_size || nal_len < 2 || nal_len > 4
            || (!in && in_size != 0))
        return -1;
    if (annexb_scan(in, in_size, &nals) != 0)
        return -1;
    max_len = (uint64_t)1 << (8 * nal_len);
    for (i = 0; i < nals.count; i++) {
        if ((uint64_t)nals.items[i].size >= max_len
                || total > SIZE_MAX - nal_len - nals.items[i].size) {
            annexb_list_free(&nals);
            return -1;
        }
        total += nal_len + nals.items[i].size;
    }
    buf = malloc(total ? total : 1);
    if (!buf) {
        annexb_list_free(&nals);
        return -1;
    }
    w = buf;
    for (i = 0; i < nals.count; i++) {
        const nal_t *nal = &nals.items[i];
        size_t len = nal->size;
        unsigned k;

        for (k = nal_len; k-- > 0;)
            *w++ = (uint8_t)((len >> (8 * k)) & 0xFF);
        memcpy(w, in + nal->off, len);
        w += len;
    }
    annexb_list_free(&nals);
    *out = buf;
    *out_size = total;
    return 0;
}

void avcc_config_free(avcc_config_t *cfg)
{
    int i;

    if (!cfg)
        return;
    for (i = 0; i < cfg->sps_count; i++)
        free(cfg->sps[i]);
    for (i = 0; i < cfg->pps_count; i++)
        free(cfg->pps[i]);
    memset(cfg, 0, sizeof(*cfg));
}
