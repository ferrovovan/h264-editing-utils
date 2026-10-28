/*
 * sei.c - SEI message iteration and the payload parsers we consume.
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

#include "bits.h"
#include "sei.h"

void sei_iter_init(sei_iter_t *it, const uint8_t *rbsp, size_t size)
{
    it->rbsp = rbsp;
    it->size = size;
    it->pos = 0;
}

/*
 * sei_message() syntax, spec 7.3.2.3.1:
 *
 *   payloadType  = sum of leading 0xFF bytes * 255 + first non-0xFF byte
 *   payloadSize  = same coding
 *   payloadSize bytes of payload
 *
 * Anything malformed (RBSP exhausted inside an FF-extended header, payload
 * extending past the RBSP, zero-size payload which only ever occurs as
 * trailing/garbage bytes) stops the iteration with 0 instead of reporting a
 * message. In particular the rbsp_trailing_bits stop pattern (0x80...) left
 * over at the end of the RBSP never turns into a phantom message: it is read
 * as a payloadType with no payloadSize byte behind it, or as a header whose
 * payload does not fit, and iteration stops.
 */
int sei_iter_next(sei_iter_t *it, sei_msg_t *out)
{
    const uint8_t *p;
    size_t avail;
    uint64_t type = 0;
    uint64_t size = 0;

    memset(out, 0, sizeof(*out));
    if (!it->rbsp || it->pos > it->size) {
        return 0;
    }
    p = it->rbsp + it->pos;
    avail = it->size - it->pos;

    for (;;) {
        if (avail < 1) {
            return 0;
        }
        type += *p;
        avail--;
        if (type > UINT32_MAX) {
            return 0;
        }
        if (*p++ != 0xFF) {
            break;
        }
    }
    /*
     * size <= 255 * (bytes scanned) + 255, so the accumulator cannot wrap
     * for any buffer that actually exists in memory.
     */
    for (;;) {
        if (avail < 1) {
            return 0;
        }
        size += *p;
        avail--;
        if (*p++ != 0xFF) {
            break;
        }
    }
    if (size == 0 || size > avail) {
        return 0;
    }

    out->type = (uint32_t)type;
    out->payload = p;
    out->size = (size_t)size;
    it->pos = (size_t)(p - it->rbsp) + (size_t)size;
    return 1;
}

/* sei_recovery_point_payload, spec D.1.7 (2003 edition numbering). */
int sei_parse_recovery_point(const sei_msg_t *msg, sei_recovery_point_t *out)
{
    bits_t b;

    memset(out, 0, sizeof(*out));
    bits_init(&b, msg->payload, msg->size);

    out->recovery_frame_cnt = bits_read_ue(&b);
    out->exact_match_flag = (int)bits_read_u(&b, 1);
    out->broken_link_flag = (int)bits_read_u(&b, 1);
    out->changing_slice_group_idc = bits_read_u(&b, 2);
    if (bits_error(&b)) {
        return -1;
    }
    /* Bytes left over in the payload are allowed (profile/reserved bits). */
    return 0;
}

/*
 * sei_picture_timing_payload, old Annex E.1.2 clock_timestamp syntax
 * (the one x264 and ATSC encoders emit; the 2016+ edition moved this
 * payload to D.1.3 and replaced the clock timestamps with SMPTE timecode).
 *
 * We read pic_struct (if present) and only the FIRST clock timestamp -
 * that is all the time base needs. NumClockTS per the task contract would be
 * (pic_struct == 1 || pic_struct == 2) ? 2 : 1 when pic_struct is present,
 * 1 otherwise, but we never iterate past the first entry.
 */
int sei_parse_picture_timing(const sei_msg_t *msg, int pic_struct_present,
                             sei_picture_timing_t *out)
{
    bits_t b;
    uint32_t ts;

    memset(out, 0, sizeof(*out));
    out->pic_struct_present = pic_struct_present ? 1 : 0;
    bits_init(&b, msg->payload, msg->size);

    if (pic_struct_present) {
        out->pic_struct = bits_read_u(&b, 4);
    }
    out->has_clock_timestamp = (int)bits_read_u(&b, 1);
    if (bits_error(&b)) {
        return -1;
    }
    if (!out->has_clock_timestamp) {
        return 0;
    }

    out->cnt_type = bits_read_u(&b, 2);
    out->n_90kHz_flag = (int)bits_read_u(&b, 1);
    ts = bits_read_u(&b, out->n_90kHz_flag ? 32 : 24);
    if (bits_error(&b)) {
        return -1;
    }

    switch (out->cnt_type) {
    case 0: {
        uint32_t seconds = bits_read_u(&b, 6);
        uint32_t minutes = bits_read_u(&b, 6);
        uint32_t hours = bits_read_u(&b, 5);
        if (bits_error(&b)) {
            return -1;
        }
        if (!out->n_90kHz_flag) {
            out->supported = 1;
            out->clock_ts_90k = (uint64_t)ts
                + (uint64_t)hours * 3600u * 90000u
                + (uint64_t)minutes * 60u * 90000u
                + (uint64_t)seconds * 90000u;
        }
        break;
    }
    case 1: {
        /* Offset-only clock: read the first timestamp's syntax fully;
         * the values are not usable as a time base either way. */
        if (!bits_read_u(&b, 1)) {         /* offset_not_present_flag */
            bits_read_u(&b, 6);            /* seconds */
        }
        if (!bits_read_u(&b, 1)) {         /* second_offset_not_present_flag (i == 0) */
            bits_read_u(&b, 6);            /* seconds */
        }
        if (bits_error(&b)) {
            return -1;
        }
        break;
    }
    case 2:
        if (bits_read_u(&b, 2)) {          /* two_clock_timestamps */
            bits_read_u(&b, 6);            /* second_offset */
            bits_read_u(&b, 1);            /* clock_timestamp_flag */
        }
        if (bits_error(&b)) {
            return -1;
        }
        break;
    default: {
        /*
         * cnt_type == 3 (SMPTE-style timecode). The contract only defines
         * cnt_type 0..2; parse the fixed-syntax prefix so a truncated
         * payload still reports -1. time_offset at the tail has SPS-derived
         * length (time_offset_length) which is not known here, so stop
         * before it - supported stays 0 either way.
         */
        uint32_t full_timestamp_flag;
        bits_read_u(&b, 1);                /* nuit_field_based_flag */
        bits_read_u(&b, 5);                /* counting_type */
        full_timestamp_flag = bits_read_u(&b, 1);
        bits_read_u(&b, 1);                /* discontinuity_flag */
        bits_read_u(&b, 1);                /* cnt_dropped_flag */
        bits_read_u(&b, 8);                /* n_frames */
        if (full_timestamp_flag) {
            bits_read_u(&b, 6);            /* seconds_value */
            bits_read_u(&b, 6);            /* minutes_value */
            bits_read_u(&b, 5);            /* hours_value */
        } else {
            if (bits_read_u(&b, 1)) {      /* seconds_flag */
                bits_read_u(&b, 6);
                if (bits_read_u(&b, 1)) {  /* minutes_flag */
                    bits_read_u(&b, 6);
                    if (bits_read_u(&b, 1)) { /* hours_flag */
                        bits_read_u(&b, 5);
                    }
                }
            }
        }
        if (bits_error(&b)) {
            return -1;
        }
        break;
    }
    }
    return 0;
}
