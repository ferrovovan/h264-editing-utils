#!/usr/bin/env python3
#
# Copyright (C) 2026 h264-editing-utils contributors
#
# This file is part of h264-editing-utils.
#
# h264-editing-utils is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# h264-editing-utils is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with h264-editing-utils.  If not, see <https://www.gnu.org/licenses/>.
#
"""Print the NAL units of a raw Annex B H.264 stream, one per line:

    <nal_index> <nal_unit_type> <byte_offset> <payload_hex>

payload_hex is the NAL payload exactly as stored in the file: emulation
prevention three_bytes included, start code excluded, trailing zero_8bits
stripped.  Start codes are found as a maximal run of zeros (length >= 2)
followed by 0x01, so the 3-byte and the 4-byte form each count as ONE
separator (the leading zero_byte of the 4-byte form belongs to the
separator, not to the payload).  This matches the NAL indexing used by
`h264-cut --list` (nal_range / byte_range columns).
"""

import sys

START_CODE = b"\x00\x00\x01"


def scan_nals(data):
    nals = []
    index = 0
    pos = 0
    size = len(data)
    while True:
        p = data.find(START_CODE, pos)
        if p < 0:
            break
        hdr = p + 3
        if hdr >= size or data[hdr] == 0x00:
            # zero padding after the prefix: not a NAL start, keep scanning
            pos = hdr if hdr < size else size
            continue
        nal_type = data[hdr] & 0x1F
        if nal_type == 0:
            pos = hdr + 1
            continue
        q = data.find(START_CODE, hdr + 1)
        end = size if q < 0 else q
        while end > hdr + 1 and data[end - 1] == 0x00:
            end -= 1
        nals.append((index, nal_type, hdr, data[hdr:end].hex()))
        index += 1
        pos = end if end > hdr + 1 else hdr + 1
    return nals


def main():
    if len(sys.argv) != 2:
        sys.stderr.write("usage: nal_seq.py FILE.264\n")
        return 2
    try:
        with open(sys.argv[1], "rb") as f:
            data = f.read()
    except OSError as e:
        sys.stderr.write("nal_seq.py: %s\n" % e)
        return 2
    out = sys.stdout
    for index, nal_type, off, payload in scan_nals(data):
        out.write("%d %d %d %s\n" % (index, nal_type, off, payload))
    return 0


if __name__ == "__main__":
    sys.exit(main())
