/*
 * iso.h - read-only ISO Base Media File Format (ISO/IEC 14496-12, MP4) reader
 * for the single-H.264-video-track subset used by mp4-cut.
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

#ifndef H264EU_ISO_H
#define H264EU_ISO_H

#include <stdint.h>
#include <stddef.h>

/* One media sample (one H.264 access unit) of the video track. */
typedef struct {
    uint64_t off;        /* absolute file offset of the sample bytes, i.e. of
                          * the first NAL length prefix (no start codes) */
    uint32_t size;       /* sample size in bytes, length prefixes included */
    uint64_t dts;        /* decoding time, cumulative, in track timescale */
    int64_t pts;         /* dts + ctts offset (0 when no ctts), track timescale */
    uint32_t dts_delta;  /* sample duration from stts */
    int is_sync;         /* membership in stss; 1 for every sample when the
                          * track has no stss box (all samples are sync) */
} iso_sample_t;

/* The single supported video track: non-fragmented avc1/avc3. */
typedef struct {
    uint32_t timescale;              /* mdhd */
    uint64_t duration;               /* mdhd (0 when unknown) */
    uint32_t width, height;          /* tkhd, whole pixels (16.16 >> 16) */
    uint8_t nal_len_size;            /* 1/2/3/4, from avcC lengthSizeMinusOne + 1 */
    const uint8_t *avcc;             /* avcC box payload (without box header), in buf */
    size_t avcc_size;
    const uint8_t *avc1;             /* the WHOLE avc1/avc3 sample entry box, header
                                      * included - the writer reuses it verbatim */
    size_t avc1_size;
    iso_sample_t *samples;           /* decoding order; owned, freed by iso_close() */
    size_t sample_count;
    int has_edits;                   /* a non-empty elst was seen (writer drops it) */
} iso_video_t;

typedef struct {
    const uint8_t *buf;              /* whole file, caller-owned */
    size_t size;
    uint32_t movie_timescale;        /* mvhd */
    uint64_t movie_duration;
    iso_video_t video;
    int has_audio;                   /* a 'soun' track was found (diagnostic) */
    const uint8_t *ftyp;             /* the whole ftyp box, header included */
    size_t ftyp_size;
} iso_movie_t;

/*
 * Parse the movie. Every rejected file gets a human-readable Russian message
 * in err (errcap bytes, always NUL-terminated). v1 subset: non-fragmented
 * (moof/mvex), unencrypted (sinf/schm/pssh/...), exactly one 'vide' track
 * whose first sample entry is avc1/avc3 with a parsable avcC (numOfSPS != 0);
 * no audio/subtitle tracks. All sample tables (stts/ctts/stss/stsc/stsz/
 * stco|co64) are cross-checked for consistency, every sample offset must lie
 * inside buf. Returns 0 on success, -1 on any rejection or parse failure.
 */
int iso_open(iso_movie_t *m, const uint8_t *buf, size_t size,
             char *err, size_t errcap);
void iso_close(iso_movie_t *m);

/* Largest sample index <= dec_index with is_sync; 0 on success, -1 if none. */
int iso_first_sync_at_or_before(const iso_video_t *v, size_t dec_index,
                                size_t *out);

/* Largest sample index whose pts <= pts_ticks (pts is NOT monotonic in
 * decoding order; this is a full scan picking the largest index).
 * 0 on success, -1 if no sample qualifies. */
int iso_last_pts_le(const iso_video_t *v, int64_t pts_ticks, size_t *out);

#endif /* H264EU_ISO_H */
