/*
 * transcode.h - minimal re-encoding of GOP fragments for --smart cuts.
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

#ifndef H264EU_TRANSCODE_H
#define H264EU_TRANSCODE_H

#include <stddef.h>
#include <stdint.h>

#include "annexb.h"
#include "index.h"

/* Return codes of transcode_range(). */
enum {
    TRANSCODE_OK          =  0,
    TRANSCODE_ERROR       = -1, /* decode/encode/allocation failure, err set */
    TRANSCODE_UNSUPPORTED = -2  /* stream or build violates the v1 limits,
                                 * err set; maps to h264-cut exit code 2 */
};

/*
 * 1 when this build links x264 + libavcodec (WITH_SMART, H264EU_HAVE_SMART),
 * 0 otherwise. When 0, transcode_range() fails with TRANSCODE_UNSUPPORTED.
 */
int transcode_available(void);

/*
 * Re-encode pictures [enc_first, enc_last] (inclusive; indices into
 * idx->frames, i.e. DECODING order) as a self-contained Annex B fragment.
 *
 * feed_first (<= enc_first) is a warm-up anchor: an IDR/usable recovery the
 * caller guarantees decodes cleanly on its own. Every picture of
 * [feed_first, enc_last] is assembled into an access unit from its NAL range
 * in `buf` (4-byte start codes, byte-for-byte payload) and fed to a libavcodec
 * H.264 decoder in decoding order with pts = dts = frame index. Decoded
 * output arrives in DISPLAY order; pictures with pts >= enc_first (the warm-up
 * pictures all have smaller pts, display order is monotonic at the output) are
 * pushed through x264 (preset medium, CRF `crf`, fps fps_num/fps_den,
 * scenecut off, threads 1, forced leading IDR, Annex B output with in-band
 * SPS/PPS and i_sps_id from the source SPS) and concatenated verbatim.
 *
 * The SPS/PPS whose bytes are prepended to the decoder feed (when the feed
 * range does not carry them) and whose id/size/profile drive x264 are the
 * last SPS (and following PPS) at or before the feed_first AU in `buf`;
 * the feed_first AU's own leading parameter sets take precedence.
 *
 * v1 limits, all TRANSCODE_UNSUPPORTED: build without smart support,
 * interlaced content (idx->has_fields or !frame_mbs_only_flag),
 * frame_cropping_flag, bit_depth > 8, chroma_format_idc != 1 (4:2:0 only),
 * fps_num == 0 || fps_den == 0, a parse_error picture inside
 * [feed_first, enc_last], no SPS found at or before the feed range, or an
 * empty output.
 *
 * On success *out_buf (malloc'd, *out_size bytes; caller frees with free())
 * holds the fragment and *out_nals its annexb_scan() view (caller frees with
 * annexb_list_free()). On failure the outputs are left zeroed and a
 * human-readable Russian message goes to err (errcap bytes, always
 * NUL-terminated).
 */
int transcode_range(const uint8_t *buf, const stream_index_t *idx,
                    size_t feed_first, size_t enc_first, size_t enc_last,
                    int crf, uint32_t fps_num, uint32_t fps_den,
                    uint8_t **out_buf, size_t *out_size,
                    nal_list_t *out_nals,
                    char *err, size_t errcap);

#endif /* H264EU_TRANSCODE_H */
