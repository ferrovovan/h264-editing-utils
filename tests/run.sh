#!/usr/bin/env bash
#
# run.sh - integration tests for h264-cut.
#
# Copyright (C) 2026 h264-editing-utils contributors
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
# Usage: run.sh /path/to/h264-cut   (or H264_CUT=/path/to/h264-cut run.sh)

set -u

BIN="${1:-${H264_CUT:-}}"
if [ -z "$BIN" ]; then
    echo "error: no h264-cut binary given (usage: $0 PATH, or set H264_CUT)" >&2
    exit 2
fi
if [ ! -x "$BIN" ]; then
    echo "error: not an executable: $BIN" >&2
    exit 2
fi

TESTS_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
NALPY="$TESTS_DIR/nal_seq.py"
PY=python3

WORK=$(mktemp -d "${TMPDIR:-/tmp}/h264cut-tests.XXXXXX") || {
    echo "error: cannot create temp dir" >&2; exit 2;
}
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0

ok() { printf '[ OK ] %s\n' "$1"; pass=$((pass + 1)); }
ko() { printf '[FAIL] %s\n' "$1"; fail=$((fail + 1)); }

# check_eq NAME EXPECTED ACTUAL
check_eq() {
    if [ "$2" = "$3" ]; then
        ok "$1"
    else
        ko "$1 (expected [$2], got [$3])"
    fi
}

# check_rc NAME EXPECTED_RC COMMAND...
check_rc() {
    local name=$1 want=$2
    shift 2
    "$@" >/dev/null 2>&1
    local got=$?
    if [ "$want" = "$got" ]; then
        ok "$name"
    else
        ko "$name (expected rc $want, got $got)"
    fi
}

list_rows() { tail -n +2 "$1"; }

ff_nb() {
    ffprobe -v error -count_frames -select_streams v:0 \
        -show_entries stream=nb_read_frames -of default=nw=1:nk=1 "$1"
}

# nal payload signature of a file: "<nal_type> <payload_hex>" per NAL
nal_sigs() {
    "$PY" "$NALPY" "$1" | awk '{print $2, $4}'
}

# Expected NAL signature sequence of cut [a..b]: NALs of the source
# [frames[a].nal_first .. frames[b].nal_last] plus the SPS/PPS the tool
# prepends when the first AU does not carry them.
expected_nals() {
    local lst=$1 src=$2 a=$3 b=$4 lo hi
    lo=$(list_rows "$lst" | awk -v i="$a" 'NR==i+1{split($7,r,"-");print r[1]}')
    hi=$(list_rows "$lst" | awk -v i="$b" 'NR==i+1{split($7,r,"-");print r[2]}')
    "$PY" "$NALPY" "$src" | awk -v lo="$lo" -v hi="$hi" '
        $1 < lo && $2 == 7 { last_sps = $2 " " $4 }
        $1 < lo && $2 == 8 { last_pps = $2 " " $4 }
        $1 >= lo && $1 <= hi && $2 == 7 { have_sps = 1 }
        $1 >= lo && $1 <= hi && $2 == 8 { have_pps = 1 }
        $1 >= lo && $1 <= hi { n++; buf[n] = $2 " " $4 }
        END {
            if (!have_sps && last_sps != "") print last_sps
            if (!have_pps && last_pps != "") print last_pps
            for (i = 1; i <= n; i++) print buf[i]
        }'
}

# check_nal_eq NAME SRC_LIST SRC_FILE CUT_FILE A B
check_nal_eq() {
    local name=$1 lst=$2 src=$3 cut=$4 a=$5 b=$6
    expected_nals "$lst" "$src" "$a" "$b" > "$WORK/exp.tmp"
    nal_sigs "$cut" > "$WORK/got.tmp"
    if diff -q "$WORK/exp.tmp" "$WORK/got.tmp" >/dev/null; then
        ok "$name"
    else
        ko "$name (NAL sequence differs: $(diff "$WORK/exp.tmp" "$WORK/got.tmp" | head -5 | tr '\n' ' '))"
    fi
}

# ---------------------------------------------------------------------------
# Stream generation
# ---------------------------------------------------------------------------

gen() { # gen NAME FFMPEG_ARGS...
    local name=$1
    shift
    if ! ffmpeg -v error -f lavfi "$@" -an -y "$WORK/$name.264" \
            2>"$WORK/gen.err"; then
        echo "error: cannot generate stream '$name': $(cat "$WORK/gen.err")" >&2
        exit 2
    fi
}

gen s1 -i "testsrc2=size=176x144:rate=25:duration=4" \
    -c:v libx264 -profile:v high -g 15 -bf 2 -x264-params scenecut=0
gen s2 -i "testsrc2=size=176x144:rate=30000/1001:duration=3" \
    -c:v libx264 -profile:v high -g 10 -bf 3 -x264-params scenecut=0:slices=2
gen s3 -i "testsrc2=size=176x144:rate=25:duration=4" \
    -c:v libx264 -profile:v baseline -g 12 -bf 0

S1="$WORK/s1.264"
S2="$WORK/s2.264"
S3="$WORK/s3.264"

"$BIN" --list "$S1" > "$WORK/s1.lst" || { echo "fatal: --list failed on s1" >&2; exit 2; }
"$BIN" --list "$S2" > "$WORK/s2.lst" || { echo "fatal: --list failed on s2" >&2; exit 2; }
"$BIN" --list "$S3" > "$WORK/s3.lst" || { echo "fatal: --list failed on s3" >&2; exit 2; }

# ---------------------------------------------------------------------------
# T1: high profile, 25 fps, g=15, bf=2 - index vs ffprobe
# ---------------------------------------------------------------------------

n_list=$(list_rows "$WORK/s1.lst" | wc -l)
n_ff=$(ff_nb "$S1")
check_eq "T1 frame count --list vs ffprobe" "100" "$n_list"
check_eq "T1 frame count ffprobe nb_read_frames" "$n_list" "$n_ff"

# IDR frames: set of display ranks must match ffprobe key_frame=1 positions
# (ffprobe lists frames in display order, so its line number is the rank).
idr_list=$(list_rows "$WORK/s1.lst" | awk '$4 == 1 {print $2}' | sort -n | tr '\n' ' ')
idr_ff=$(ffprobe -v error -select_streams v:0 -show_entries frame=key_frame \
         -of csv=p=0 "$S1" | tr -d '\r' | awk -F',' '{if ($1 + 0 == 1) print NR - 1}' | sort -n | tr '\n' ' ')
check_eq "T1 IDR display ranks match ffprobe key frames" "$idr_ff" "$idr_list"

# Type sequence in display order: --list sorted by disp_id vs ffprobe order
# (same multiset check follows implicitly from full sequence equality).
types_dec_disp=$(list_rows "$WORK/s1.lst" | sort -k2,2n | awk '{printf "%s", $3}')
types_ff=$(ffprobe -v error -select_streams v:0 -show_entries frame=pict_type \
           -of csv=p=0 "$S1" | tr -d ',\r\n')
check_eq "T1 pict_type sequence in display order" "$types_ff" "$types_dec_disp"

# Multiset of types in decode order must equal ffprobe's multiset too.
mset_list=$(list_rows "$WORK/s1.lst" | awk '{print $3}' | sort | tr -d '\n')
mset_ff=$(printf '%s' "$types_ff" | fold -w1 | sort | tr -d '\n')
check_eq "T1 pict_type multiset (decode vs display)" "$mset_ff" "$mset_list"

# ---------------------------------------------------------------------------
# T2: multi-slice (slices=2), bf=3, g=10, 30000/1001 - frame grouping
# ---------------------------------------------------------------------------

bad_group=$(list_rows "$WORK/s2.lst" | awk '
    $4 != 1 { split($7, r, "-"); if (r[2] - r[1] + 1 < 2) bad++ }
    END { print bad + 0 }')
check_eq "T2 every non-IDR frame has >= 2 slice NALs" "0" "$bad_group"

non_idr=$(list_rows "$WORK/s2.lst" | awk '$4 != 1' | wc -l)
if [ "$non_idr" -gt 0 ]; then
    ok "T2 multi-slice stream has non-IDR frames ($non_idr)"
else
    ko "T2 multi-slice stream has non-IDR frames"
fi

n_ff2=$(ff_nb "$S2")
n_list2=$(list_rows "$WORK/s2.lst" | wc -l)
check_eq "T2 frame count --list vs ffprobe" "$n_ff2" "$n_list2"

# ---------------------------------------------------------------------------
# T3: byte-exact NAL payloads of a cut
# ---------------------------------------------------------------------------

# 15 is an IDR (g=15), so no left-boundary shift; 30 is an IDR too.
if "$BIN" -f 15 -t 40 -o "$WORK/cut_a.264" "$S1" 2>"$WORK/cut_a.err"; then
    rc=0
else
    rc=$?
fi
check_eq "T3 cut -f 15 -t 40 exit code" "0" "$rc"
if [ -s "$WORK/cut_a.err" ]; then
    ko "T3 aligned cut produces no stderr note"
else
    ok "T3 aligned cut produces no stderr note"
fi
check_nal_eq "T3 NAL payload byte-exactness of cut [15..40]" \
    "$WORK/s1.lst" "$S1" "$WORK/cut_a.264" 15 40

"$BIN" -f 30 -t 60 -o "$WORK/cut_b.264" "$S1" 2>/dev/null
check_nal_eq "T3 NAL payload byte-exactness of cut [30..60]" \
    "$WORK/s1.lst" "$S1" "$WORK/cut_b.264" 30 60

# ---------------------------------------------------------------------------
# T4: prefix cut [0..30]
# ---------------------------------------------------------------------------

"$BIN" -f 0 -t 30 -o "$WORK/cut_pre.264" "$S1" 2>/dev/null
check_nal_eq "T4 NAL sequence of prefix cut [0..30]" \
    "$WORK/s1.lst" "$S1" "$WORK/cut_pre.264" 0 30

# ---------------------------------------------------------------------------
# T5: left boundary snapped to IDR
# ---------------------------------------------------------------------------

"$BIN" -f 20 -t 40 -o "$WORK/cut_idr.264" "$S1" 2>"$WORK/cut_idr.err"
if grep -q "left boundary moved from frame 20 to IDR frame 15" "$WORK/cut_idr.err"; then
    ok "T5 stderr notes the left-boundary IDR shift"
else
    ko "T5 stderr notes the left-boundary IDR shift"
fi
first_key=$(ffprobe -v error -select_streams v:0 -show_entries frame=key_frame \
            -of csv=p=0 "$WORK/cut_idr.264" | head -1 | tr -d ',\r')
check_eq "T5 cut starts with a key frame" "1" "$first_key"

# ---------------------------------------------------------------------------
# T6: exact frame count of an aligned cut
# ---------------------------------------------------------------------------

"$BIN" -f 15 -t 60 -o "$WORK/cut_c.264" "$S1" 2>/dev/null
n_cut=$(ff_nb "$WORK/cut_c.264")
check_eq "T6 cut [15..60] decodes to 46 frames" "46" "$n_cut"

# ---------------------------------------------------------------------------
# T7: --exact via VUI timing == via explicit --fps
# ---------------------------------------------------------------------------

check_rc "T7 --exact exit code" 0 "$BIN" --exact 00:00:00.500-00:00:02.500 \
    -o "$WORK/cut_e1.264" "$S1"
check_rc "T7 --exact --fps exit code" 0 "$BIN" --exact 00:00:00.500-00:00:02.500 \
    --fps 25 -o "$WORK/cut_e2.264" "$S1"
if cmp -s "$WORK/cut_e1.264" "$WORK/cut_e2.264"; then
    ok "T7 VUI timing and explicit --fps produce identical output"
else
    ko "T7 VUI timing and explicit --fps produce identical output"
fi
n_exact=$(ff_nb "$WORK/cut_e1.264")
if [ "${n_exact:-0}" -gt 0 ]; then
    ok "T7 --exact cut decodes to $n_exact frames"
else
    ko "T7 --exact cut decodes to a nonzero frame count"
fi

# ---------------------------------------------------------------------------
# T8: every cut decodes cleanly
# ---------------------------------------------------------------------------

for f in "$WORK"/cut_*.264; do
    if ffmpeg -v error -i "$f" -f null - 2>"$WORK/dec.err"; then
        if [ -s "$WORK/dec.err" ]; then
            ko "T8 $(basename "$f") decodes without stderr"
        else
            ok "T8 $(basename "$f") decodes without stderr"
        fi
    else
        ko "T8 $(basename "$f") decodes without stderr"
    fi
done

# ---------------------------------------------------------------------------
# T9: exit codes
# ---------------------------------------------------------------------------

check_rc "T9 --help exits 0" 0 "$BIN" --help
check_rc "T9 no arguments exits 1" 1 "$BIN"
check_rc "T9 --list on missing file exits 2" 2 "$BIN" --list "$WORK/no-such-file.264"
printf 'this is definitely not an h264 bitstream' > "$WORK/junk.264"
check_rc "T9 --list on garbage exits 2" 2 "$BIN" --list "$WORK/junk.264"
check_rc "T9 from > to exits 3" 3 "$BIN" -f 5 -t 3 -o "$WORK/x.264" "$S1"
check_rc "T9 from beyond end exits 3" 3 "$BIN" -f 100000 -o "$WORK/x.264" "$S1"

# ---------------------------------------------------------------------------
# T10: baseline, no B frames: display order == decode order
# ---------------------------------------------------------------------------

n_list3=$(list_rows "$WORK/s3.lst" | wc -l)
if [ "$n_list3" -gt 0 ]; then
    ok "T10 baseline stream indexed ($n_list3 frames)"
else
    ko "T10 baseline stream indexed"
fi
mismatch=$(list_rows "$WORK/s3.lst" | awk '$1 != $2' | wc -l)
check_eq "T10 display index equals decode index everywhere" "0" "$mismatch"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

echo
echo "passed: $pass, failed: $fail"
if [ "$fail" -ne 0 ]; then
    exit 1
fi
exit 0
