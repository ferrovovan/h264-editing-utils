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
#include "sei.h"

/* One day in 1/90000 s units (clock_timestamp wraps with this period, D.2). */
#define DAY_90K ((uint64_t)90000 * 24 * 3600)

/*
 * SEI content parsed from the AUs seen so far, waiting for the picture it
 * belongs to (parametric NALs are attributed to the NEXT picture, so pending
 * records are attached when that picture is created - or when the bottom
 * field of an already open picture starts, which is the same picture).
 */
typedef struct {
    int have_recovery;
    sei_recovery_point_t recovery;
    int have_clock;
    uint64_t clock_raw;   /* raw (still wrapped) clock ts of the pending AU */
    uint64_t ts_base;     /* accumulated day unwraps */
    uint64_t prev_raw;    /* raw ts of the previous picture with clock ts */
    int have_prev;
} sei_pending_t;

static void attach_sei_pending(frame_t *f, sei_pending_t *p)
{
    if (p->have_recovery) {
        f->recovery_point = 1;
        f->recovery_frame_cnt = p->recovery.recovery_frame_cnt;
        f->exact_match_flag = p->recovery.exact_match_flag;
        f->broken_link_flag = p->recovery.broken_link_flag;
        if (!f->is_idr) {
            f->usable_anchor =
                p->recovery.recovery_frame_cnt == 0 &&
                p->recovery.exact_match_flag &&
                !p->recovery.broken_link_flag;
        }
        p->have_recovery = 0;
    }
    if (p->have_clock) {
        uint64_t raw = p->clock_raw;
        /* One-sided unwrap: the clock restarts at 0 once a day, so a drop
         * of more than half a day against the previous picture means the
         * day counter advanced. No anchor reset: base keeps accumulating,
         * which is enough for monotonic ts inside every segment. */
        if (p->have_prev && raw < p->prev_raw &&
                p->prev_raw - raw > DAY_90K / 2) {
            p->ts_base += DAY_90K;
        }
        p->prev_raw = raw;
        p->have_prev = 1;
        f->has_clock_ts = 1;
        f->clock_ts_90k = p->ts_base + raw;
        p->have_clock = 0;
    }
}

/* pic_struct_present_flag of the SPS referenced by the most recently seen
 * PPS (the "active" parameter state at SEI parse time); 0 when unavailable. */
static int active_pic_struct_present(const stream_index_t *idx, int last_pps_id)
{
    const pps_state_t *pps;

    if (last_pps_id < 0) {
        return 0;
    }
    pps = &idx->params.pps[last_pps_id];
    if (!pps->valid || pps->sps_id < 0 || pps->sps_id >= 32) {
        return 0;
    }
    return idx->params.sps[pps->sps_id].pic_struct_present_flag ? 1 : 0;
}

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
 * Sort picture indices inside each anchor segment (segments start at an IDR
 * or at a recovery point SEI) by (poc, decoding order) and assign
 * display_index. All display indexes are set to -1 when POC is not usable
 * for every picture. Negative POCs are legitimate: leading pictures of an
 * open GOP after an IDR sort in front of their anchor. A field pair is one
 * picture (one frame_t), so 8.21 display order needs no extra handling
 * here: the picture carries the POC of its top field.
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
        if (idx->frames[i].parse_error) {
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
        if (i == idx->frame_count ||
                (i > 0 && (idx->frames[i].is_idr ||
                           idx->frames[i].recovery_point))) {
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
    /* State of the currently open FIELD (an access unit is one field or one
     * frame; a field pair is two AUs but ONE picture / frame_t). */
    int have_cur = 0;
    size_t cur = 0;
    uint8_t cur_ref_idc = 0;
    uint32_t cur_frame_num = 0;
    uint32_t cur_pps_id = 0;
    uint32_t cur_idr_pic_id = 0;
    int cur_field = 0;
    int cur_bottom = 0;
    int cur_is_idr = 0;
    int cur_have_bottom = 0;
    /* POC type 0 state (spec 8.21.1): msb/lsb of the previous picture are
     * tracked separately per field parity (slot = bottom_field_flag; frame
     * pictures, incl. MBAFF, always use slot 0). */
    long long prev_msb[2];
    long long prev_lsb[2];
    long long prev_frame_num = 0;
    long long prev_fn_offset = 0;
    int last_sps_id = -1;
    int last_pps_id = -1;
    sei_pending_t pending;
    int have_anchor = 0;
    long long anchor_poc = 0;
    /*
     * First NAL not yet attributed to any picture. Parametric NALs (SPS, PPS,
     * SEI, AUD, filler, ...) that precede a slice join the picture that the
     * slice starts, so a picture's NAL range is self-contained: cutting from
     * an IDR always carries the parameter sets that belong to it. A field
     * pair belongs to one picture, so its range spans the slices of BOTH
     * fields (plus the parametric NALs in front of either of them).
     */
    size_t nal_start = 0;

    memset(idx, 0, sizeof(*idx));
    memset(&pending, 0, sizeof(pending));
    prev_msb[0] = prev_msb[1] = 0;
    prev_lsb[0] = prev_lsb[1] = 0;
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
        int same_field;
        int new_pic;
        const pps_state_t *pps;
        const sps_state_t *sps;
        long long poc = -1;
        frame_t *f;

        if (nal->type == NAL_SPS || nal->type == NAL_PPS) {
            bits_t b;
            uint32_t id;

            params_feed_nal(&idx->params, buf, nal);
            /* Read the parameter set id: the SPS id to detect parameter
             * changes / VUI timing, the PPS id to know the active SPS for
             * SEI picture timing parsing. */
            rbsp = annexb_rbsp(buf, nal, &rbsp_size);
            if (!rbsp) {
                goto fail;
            }
            bits_init(&b, rbsp + 1, rbsp_size - 1);
            if (nal->type == NAL_SPS) {
                bits_read_u(&b, 8); /* profile_idc */
                bits_read_u(&b, 8); /* constraint flags */
                bits_read_u(&b, 8); /* level_idc */
            }
            id = bits_read_ue(&b);
            if (!bits_error(&b)) {
                if (nal->type == NAL_PPS) {
                    if (id < 256) {
                        last_pps_id = (int)id;
                    }
                } else if (id < 32) {
                    if ((int)id != last_sps_id) {
                        last_sps_id = (int)id;
                        prev_msb[0] = prev_msb[1] = 0;
                        prev_lsb[0] = prev_lsb[1] = 0;
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

        if (nal->type == NAL_SEI) {
            sei_iter_t it;
            sei_msg_t msg;

            rbsp = annexb_rbsp(buf, nal, &rbsp_size);
            if (!rbsp) {
                goto fail;
            }
            /* RBSP after the nal header byte; malformed messages simply
             * end the iteration (sei_iter_next never fails hard). */
            sei_iter_init(&it, rbsp + 1, rbsp_size > 0 ? rbsp_size - 1 : 0);
            while (sei_iter_next(&it, &msg)) {
                if (msg.type == SEI_PAYLOAD_RECOVERY_POINT) {
                    sei_recovery_point_t rp;
                    if (sei_parse_recovery_point(&msg, &rp) == 0) {
                        /* Several recovery points in one AU: last one wins
                         * ("any"), they cannot legitimately coexist. */
                        pending.recovery = rp;
                        pending.have_recovery = 1;
                    }
                } else if (msg.type == SEI_PAYLOAD_PICTURE_TIMING) {
                    sei_picture_timing_t pt;
                    if (!pending.have_clock &&
                            sei_parse_picture_timing(
                                &msg,
                                active_pic_struct_present(idx, last_pps_id),
                                &pt) == 0 &&
                            pt.supported) {
                        /* First supported clock timestamp of the AU. */
                        pending.clock_raw = pt.clock_ts_90k;
                        pending.have_clock = 1;
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
        if (slice_header_parse(&idx->params, nal->type, nal->ref_idc,
                               rbsp + 1, rbsp_size - 1, &hdr) != 0) {
            free(rbsp);
            rbsp = NULL;
        }

        if (!rbsp) {
            /* Unparsable slice NAL: its own picture, next slice starts
             * fresh. Parity POC state is unknown from here on, so both POC
             * tracking states are left as they are (the picture is marked
             * parse_error and display order is dropped wholesale). */
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
            f->usable_anchor = f->is_idr;
            attach_sei_pending(f, &pending);
            have_cur = 0;
            nal_start = i + 1;
            continue;
        }
        free(rbsp);

        /*
         * AU boundaries (spec 7.4.1.2.4). same_field: another slice of the
         * currently open field (an IDR AU is recognised by its idr_pic_id;
         * a frame AU by the frame_num/pps/ref/field-tuple with a non-zero
         * first_mb_in_slice). Otherwise the slice opens a new field, which
         * however CONTINUES the current picture when it is the bottom field
         * of a field pair (spec 8.21.1: a field pair is one picture, one
         * unit of display and of cutting): bottom, current field is the top
         * one, frame_num / pps_id / nal_ref_idc match and the picture has no
         * bottom field yet. IDR field pairs pair by the very same rule (both
         * NALs are type 5 with the same idr_pic_id - the second one must not
         * open a new IDR segment). An orphan top field (stream cut before
         * its bottom) is a picture of its own, which is fine.
         */
        same_field = 0;
        new_pic = 1;
        if (have_cur) {
            if (nal->type == NAL_IDR) {
                same_field = cur_is_idr &&
                             hdr.idr_pic_id == cur_idr_pic_id &&
                             hdr.field_pic_flag == cur_field &&
                             hdr.bottom_field_flag == cur_bottom;
            } else if (!cur_is_idr) {
                same_field = hdr.first_mb_in_slice != 0 &&
                             hdr.frame_num == cur_frame_num &&
                             hdr.field_pic_flag == cur_field &&
                             hdr.bottom_field_flag == cur_bottom &&
                             nal->ref_idc == cur_ref_idc &&
                             hdr.pps_id == cur_pps_id;
            }
            if (same_field) {
                new_pic = 0;
            } else if (hdr.field_pic_flag && hdr.bottom_field_flag &&
                       cur_field && !cur_bottom && !cur_have_bottom &&
                       hdr.frame_num == cur_frame_num &&
                       hdr.pps_id == cur_pps_id &&
                       nal->ref_idc == cur_ref_idc &&
                       (nal->type == NAL_IDR) == cur_is_idr &&
                       (!cur_is_idr || hdr.idr_pic_id == cur_idr_pic_id)) {
                new_pic = 0;
            }
        }

        if (new_pic) {
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
            f->usable_anchor = (nal->type == NAL_IDR);
            attach_sei_pending(f, &pending);
            cur_have_bottom = hdr.field_pic_flag && hdr.bottom_field_flag;
        } else {
            frames[cur].nal_last = i;
            f = &frames[cur];
            if (!same_field) {
                /* The bottom field of this picture: its own AU boundary can
                 * carry SEI/parametric NALs (they join the picture's NAL
                 * range below), so attach anything pending to this picture. */
                cur_have_bottom = 1;
                attach_sei_pending(f, &pending);
            }
        }
        nal_start = i + 1;

        if (hdr.field_pic_flag) {
            idx->has_fields = 1;
        }

        cur_ref_idc = nal->ref_idc;
        cur_frame_num = hdr.frame_num;
        cur_pps_id = hdr.pps_id;
        cur_idr_pic_id = hdr.idr_pic_id;
        cur_field = hdr.field_pic_flag;
        cur_bottom = hdr.bottom_field_flag;
        cur_is_idr = (nal->type == NAL_IDR);
        have_cur = 1;

        /* POC state advances once per field (access unit), but only the
         * first field of a picture (its top field, or the surviving field of
         * a broken pair) writes frame_t.poc: a picture is identified by the
         * TopFieldOrderCnt of spec 8.21.1. */
        if (!same_field) {
            pps = &idx->params.pps[hdr.pps_id];
            sps = &idx->params.sps[pps->sps_id];
            if (sps->pic_order_cnt_type == 0) {
                /* spec 8.2.1 + 8.21.1 (pic_order_cnt_type == 0), as
                 * implemented by FFmpeg ff_h264_init_poc: the msb moves only
                 * on a wrap of at least half the lsb range, which is what
                 * keeps B-frame decode order (6,2,4,12,...) on a monotonic
                 * POC. With fields the whole wrap rule runs independently on
                 * each field parity (8.21.1 keeps a separate
                 * prevPicOrderCnt{Msb,Lsb} per field type); IDR resets both
                 * slots. Progressive streams only ever touch slot 0, so this
                 * is bit-for-bit the old behaviour there.
                 * Simplification (documented, B7): for a bottom field of a
                 * field pair 8.21.1 additionally takes the msb from its own
                 * top field instead of the parity wrap rule. We do not store
                 * bottom POCs at all (a picture is its TopFieldOrderCnt), so
                 * that clause can only shift the bottom parity slot, which
                 * afterwards is read by orphan bottom fields only. */
                long long max_lsb = sps->max_poc_lsb;
                long long lsb = (long long)hdr.poc_lsb;
                int parity = hdr.bottom_field_flag ? 1 : 0;
                long long msb;
                if (nal->type == NAL_IDR) {
                    msb = 0;
                } else if (lsb < prev_lsb[parity] &&
                           prev_lsb[parity] - lsb >= max_lsb / 2) {
                    msb = prev_msb[parity] + max_lsb;
                } else if (lsb > prev_lsb[parity] &&
                           lsb - prev_lsb[parity] > max_lsb / 2) {
                    msb = prev_msb[parity] - max_lsb;
                } else {
                    msb = prev_msb[parity];
                }
                poc = msb + lsb;
                prev_msb[parity] = msb;
                prev_lsb[parity] = lsb;
                if (nal->type == NAL_IDR) {
                    prev_msb[0] = prev_msb[1] = 0;
                    prev_lsb[0] = prev_lsb[1] = 0;
                }
            } else if (sps->pic_order_cnt_type == 2) {
                /* spec 8.2.1 (pic_order_cnt_type == 2) + 8.21.1: with fields
                 * PicOrderCnt = 2 * FrameNumOffset - (nal_ref_idc == 0 ? 1
                 * : 0) is the TopFieldOrderCnt of the pair (8.21.1 adds +1
                 * for the bottom field, which a picture does not store); the
                 * two fields carry the same frame_num and nal_ref_idc, so
                 * running this per field is idempotent for the pair.
                 * FrameNumOffset unwraps the modular frame_num at each wrap
                 * and at IDR. */
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
            } else if (sps->pic_order_cnt_type == 1) {
                /* spec 8.2.1.2 (8.2.1.1 in the 2003 edition) + 8.21.1,
                 * cross-checked against FFmpeg ff_h264_init_poc. The POC of
                 * the picture is the TopFieldOrderCnt: equations 8-23..8-26
                 * run on the frame_num / nal_ref_idc / delta_pic_order_cnt[0]
                 * of the field pair, which both fields carry identically (the
                 * deep slice path reads delta_pic_order_cnt[1] only for
                 * frames, per 7.3.3), so computing this once per field is
                 * idempotent and no POC1 state is needed for interlacing.
                 * offset_for_top_to_bottom_field affects the BottomField-
                 * OrderCnt (top + 1 + offset) exclusively, so it cannot
                 * change a picture POC and stays unused here. The mmco-5
                 * FrameNumOffset reset is out of scope. POC type 1 carries no
                 * PicOrderCntMsb/PicOrderCntLsb state: the POC is derived
                 * absolutely from the unwrapped FrameNumOffset (8-6), which
                 * plays the role of the msb; a picture unreferenced via
                 * dec_ref_pic_marking keeps the POC of its nal_ref_idc (8-9
                 * uses nal_ref_idc only). */
                long long fn = (long long)hdr.frame_num;
                long long fn_offset = prev_fn_offset;
                long long abs_fn, expected = 0;
                long long cyc_len = sps->num_ref_frames_in_pic_order_cnt_cycle;
                long long k;
                if (nal->type == NAL_IDR) {
                    fn_offset = 0;
                } else if (prev_frame_num > fn) {
                    fn_offset += sps->max_frame_num;
                }
                abs_fn = (cyc_len != 0) ? fn_offset + fn : 0;
                if (nal->ref_idc == 0 && abs_fn > 0) {
                    abs_fn--;
                }
                if (abs_fn > 0) {
                    long long delta_per_cycle = 0;
                    for (k = 0; k < cyc_len; k++) {
                        delta_per_cycle += sps->offset_for_ref_frame[k];
                    }
                    expected = ((abs_fn - 1) / cyc_len) * delta_per_cycle;
                    for (k = 0; k <= (abs_fn - 1) % cyc_len; k++) {
                        expected += sps->offset_for_ref_frame[k];
                    }
                }
                if (nal->ref_idc == 0) {
                    expected += sps->offset_for_non_ref_pic;
                }
                poc = expected + hdr.delta_poc0;
                if (nal->type == NAL_IDR) {
                    poc = 0;
                }
                /* IDR resets the whole tracking state (prev_frame_num is 0
                 * after an IDR by conformance; FFmpeg resets it explicitly). */
                prev_fn_offset = (nal->type == NAL_IDR) ? 0 : fn_offset;
                prev_frame_num = (nal->type == NAL_IDR) ? 0 : fn;
            } else {
                idx->poc_supported = 0;
                poc = -1;
            }
        }
        if (new_pic) {
            f->poc = poc;

            /*
             * Open GOP bookkeeping (B5). Leading picture: a non-ref AU after
             * an anchor (IDR or recovery point) whose POC is below the
             * anchor's - it decodes with references from before the anchor,
             * so a cut starting at the anchor carries it with broken
             * dependencies. The check uses the anchor before this picture;
             * then this picture becomes the new anchor if it is one.
             */
            if (!f->is_idr && nal->ref_idc == 0 && have_anchor &&
                    idx->poc_supported && poc < anchor_poc) {
                f->leading_picture = 1;
            }
            if (f->is_idr || f->recovery_point) {
                have_anchor = 1;
                anchor_poc = poc;
            }
        }
    }

    idx->clock_timing = frame_count > 0;
    for (i = 0; i < frame_count; i++) {
        if (!frames[i].has_clock_ts) {
            idx->clock_timing = 0;
            break;
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
