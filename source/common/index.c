/*
 * index.c - map NAL units of an Annex B stream onto pictures (frames).
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

#include "bits.h"
#include "index.h"

static frame_t *push_frame(frame_t **frames, size_t *count, size_t *cap)
{
    frame_t *nf;

    if (*count == *cap) {
        size_t ncap = *cap ? *cap * 2 : 64;
        nf = realloc(*frames, ncap * sizeof(*nf));
        if (!nf) {
            return NULL;
        }
        *frames = nf;
        *cap = ncap;
    }
    nf = &(*frames)[*count];
    memset(nf, 0, sizeof(*nf));
    (*count)++;
    return nf;
}

/*
 * Sort frame indices inside each IDR segment by (poc, decoding order) and
 * assign display_index. All display indexes are set to -1 when POC is not
 * usable for every frame.
 */
static void compute_display(stream_index_t *idx)
{
    size_t i;
    size_t base = 0;
    size_t seg_start = 0;
    size_t *order;

    for (i = 0; i < idx->frame_count; i++) {
        idx->frames[i].display_index = -1;
    }
    if (!idx->poc_supported) {
        return;
    }
    for (i = 0; i < idx->frame_count; i++) {
        if (idx->frames[i].poc < 0) {
            return;
        }
    }
    if (idx->frame_count == 0) {
        return;
    }
    order = malloc(idx->frame_count * sizeof(*order));
    if (!order) {
        return;
    }
    for (i = 0; i <= idx->frame_count; i++) {
        if (i == idx->frame_count || (i > 0 && idx->frames[i].is_idr)) {
            size_t n = i - seg_start;
            size_t j, k;
            for (j = 0; j < n; j++) {
                size_t v = seg_start + j;
                long long p = idx->frames[v].poc;
                order[j] = v;
                k = j;
                while (k > 0 && idx->frames[order[k - 1]].poc > p) {
                    order[k] = order[k - 1];
                    k--;
                }
                order[k] = v;
            }
            for (j = 0; j < n; j++) {
                idx->frames[order[j]].display_index = (long long)(base + j);
            }
            base += n;
            seg_start = i;
        }
    }
    free(order);
}

int index_build(stream_index_t *idx, const uint8_t *buf, size_t size)
{
    frame_t *frames = NULL;
    size_t frame_count = 0;
    size_t frame_cap = 0;
    size_t i;
    int have_cur = 0;
    size_t cur = 0;
    uint8_t cur_ref_idc = 0;
    uint32_t cur_frame_num = 0;
    uint32_t cur_pps_id = 0;
    int cur_field = 0;
    int cur_bottom = 0;
    int have_prev_idr = 0;
    uint32_t prev_idr_pic_id = 0;
    long long prev_msb = 0;
    long long prev_lsb = 0;
    long long prev_frame_num = 0;
    long long prev_fn_offset = 0;
    int last_sps_id = -1;
    /*
     * First NAL not yet attributed to any frame. Parametric NALs (SPS, PPS,
     * SEI, AUD, filler, ...) that precede a slice join the frame that the
     * slice starts, so a frame's NAL range is self-contained: cutting from
     * an IDR always carries the parameter sets that belong to it.
     */
    size_t nal_start = 0;

    memset(idx, 0, sizeof(*idx));
    idx->buf = buf;
    idx->size = size;
    idx->poc_supported = 1;

    if (annexb_scan(buf, size, &idx->nals) != 0) {
        return -1;
    }
    if (param_store_init(&idx->params) != 0) {
        annexb_list_free(&idx->nals);
        return -1;
    }

    for (i = 0; i < idx->nals.count; i++) {
        const nal_t *nal = &idx->nals.items[i];
        size_t rbsp_size = 0;
        uint8_t *rbsp;
        slice_hdr_t hdr;
        int new_frame;
        const pps_state_t *pps;
        const sps_state_t *sps;
        long long poc = -1;
        frame_t *f;

        if (nal->type == NAL_SPS || nal->type == NAL_PPS) {
            params_feed_nal(&idx->params, buf, nal);
            if (nal->type != NAL_SPS) {
                continue;
            }
            /* Retrieve the SPS id to detect parameter changes / VUI timing. */
            rbsp = annexb_rbsp(buf, nal, &rbsp_size);
            if (!rbsp) {
                goto fail;
            }
            {
                bits_t b;
                uint32_t id;
                bits_init(&b, rbsp + 1, rbsp_size - 1);
                bits_read_u(&b, 8); /* profile_idc */
                bits_read_u(&b, 8); /* constraint flags */
                bits_read_u(&b, 8); /* level_idc */
                id = bits_read_ue(&b);
                if (!bits_error(&b) && id < 32) {
                    if ((int)id != last_sps_id) {
                        last_sps_id = (int)id;
                        prev_msb = 0;
                        prev_lsb = 0;
                        prev_frame_num = 0;
                        prev_fn_offset = 0;
                    }
                    sps = &idx->params.sps[id];
                    if (!idx->timing_valid && sps->valid && sps->has_timing &&
                        sps->num_units_in_tick > 0) {
                        idx->time_scale = sps->time_scale;
                        idx->num_units_in_tick = sps->num_units_in_tick;
                        idx->fixed_frame_rate = sps->fixed_frame_rate_flag;
                        idx->timing_valid = 1;
                    }
                }
            }
            free(rbsp);
            continue;
        }

        if (nal->type < NAL_SLICE || nal->type > NAL_IDR) {
            continue;
        }

        rbsp = annexb_rbsp(buf, nal, &rbsp_size);
        if (!rbsp) {
            goto fail;
        }
        if (slice_header_parse(&idx->params, nal->type, rbsp + 1,
                               rbsp_size - 1, &hdr) != 0) {
            free(rbsp);
            rbsp = NULL;
        }

        if (!rbsp) {
            /* Unparsable slice NAL: its own frame, next slice starts fresh. */
            f = push_frame(&frames, &frame_count, &frame_cap);
            if (!f) {
                goto fail;
            }
            f->nal_first = nal_start;
            f->nal_last = i;
            f->is_idr = (nal->type == NAL_IDR);
            f->pic_type = '?';
            f->frame_num = 0;
            f->poc = -1;
            f->parse_error = 1;
            have_cur = 0;
            nal_start = i + 1;
            continue;
        }
        free(rbsp);

        if (!have_cur) {
            new_frame = 1;
        } else if (nal->type == NAL_IDR) {
            new_frame = !have_prev_idr || hdr.idr_pic_id != prev_idr_pic_id;
        } else {
            new_frame = hdr.first_mb_in_slice == 0 ||
                        hdr.frame_num != cur_frame_num ||
                        hdr.field_pic_flag != cur_field ||
                        hdr.bottom_field_flag != cur_bottom ||
                        nal->ref_idc != cur_ref_idc ||
                        hdr.pps_id != cur_pps_id;
        }

        if (new_frame) {
            f = push_frame(&frames, &frame_count, &frame_cap);
            if (!f) {
                goto fail;
            }
            cur = frame_count - 1;
            f->nal_first = nal_start;
            f->nal_last = i;
            f->is_idr = (nal->type == NAL_IDR);
            f->pic_type = hdr.pic_type;
            f->frame_num = hdr.frame_num;

            have_cur = 1;
            cur_ref_idc = nal->ref_idc;
            cur_frame_num = hdr.frame_num;
            cur_pps_id = hdr.pps_id;
            cur_field = hdr.field_pic_flag;
            cur_bottom = hdr.bottom_field_flag;
        } else {
            frames[cur].nal_last = i;
            f = &frames[cur];
        }
        nal_start = i + 1;

        if (hdr.field_pic_flag) {
            idx->has_fields = 1;
        }
        if (nal->type == NAL_IDR) {
            have_prev_idr = 1;
            prev_idr_pic_id = hdr.idr_pic_id;
        }

        if (new_frame) {
            pps = &idx->params.pps[hdr.pps_id];
            sps = &idx->params.sps[pps->sps_id];
            if (sps->pic_order_cnt_type == 0) {
                /* spec 8.2.1 (pic_order_cnt_type == 0), as implemented by
                 * FFmpeg ff_h264_init_poc: the msb moves only on a wrap of
                 * at least half the lsb range, which is what keeps B-frame
                 * decode order (6,2,4,12,...) on a monotonic POC. */
                long long max_lsb = sps->max_poc_lsb;
                long long lsb = (long long)hdr.poc_lsb;
                long long msb;
                if (nal->type == NAL_IDR) {
                    msb = 0;
                } else if (lsb < prev_lsb && prev_lsb - lsb >= max_lsb / 2) {
                    msb = prev_msb + max_lsb;
                } else if (lsb > prev_lsb && lsb - prev_lsb > max_lsb / 2) {
                    msb = prev_msb - max_lsb;
                } else {
                    msb = prev_msb;
                }
                poc = msb + lsb;
                prev_msb = msb;
                prev_lsb = lsb;
                if (nal->type == NAL_IDR) {
                    prev_msb = 0;
                    prev_lsb = 0;
                }
            } else if (sps->pic_order_cnt_type == 2) {
                /* spec 8.2.1 (pic_order_cnt_type == 2):
                 * PicOrderCnt = 2 * FrameNumOffset - (nal_ref_idc == 0 ? 1 : 0),
                 * FrameNumOffset unwrapping the modular frame_num at each
                 * wrap and at IDR. */
                long long fn = (long long)hdr.frame_num;
                long long fn_offset;
                if (nal->type == NAL_IDR) {
                    prev_frame_num = 0;
                    prev_fn_offset = 0;
                }
                fn_offset = prev_fn_offset;
                if (fn < prev_frame_num) {
                    fn_offset += sps->max_frame_num;
                }
                poc = 2 * (fn_offset + fn) - (nal->ref_idc != 0 ? 0 : 1);
                prev_fn_offset = fn_offset;
                prev_frame_num = fn;
            } else {
                idx->poc_supported = 0;
                poc = -1;
            }
            f->poc = poc;
        }
    }

    idx->frames = frames;
    idx->frame_count = frame_count;
    compute_display(idx);
    return 0;

fail:
    free(frames);
    param_store_free(&idx->params);
    annexb_list_free(&idx->nals);
    memset(idx, 0, sizeof(*idx));
    return -1;
}

void index_free(stream_index_t *idx)
{
    annexb_list_free(&idx->nals);
    param_store_free(&idx->params);
    free(idx->frames);
    memset(idx, 0, sizeof(*idx));
}
