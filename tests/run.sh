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

# shellcheck disable=SC1007  # CDPATH= cd — env prefix for the command, not an assignment
TESTS_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
NALPY="$TESTS_DIR/nal_seq.py"
PY=python3

WORK=$(mktemp -d "${TMPDIR:-/tmp}/h264cut-tests.XXXXXX") || {
    echo "error: cannot create temp dir" >&2; exit 2;
}
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0
skip=0

ok() { printf '[ OK ] %s\n' "$1"; pass=$((pass + 1)); }
ko() { printf '[FAIL] %s\n' "$1"; fail=$((fail + 1)); }
skip_note() { printf '[SKIP] %s\n' "$1"; skip=$((skip + 1)); }

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
    lo=$(list_rows "$lst" | awk -v i="$a" 'NR==i+1{split($8,r,"-");print r[1]}')
    hi=$(list_rows "$lst" | awk -v i="$b" 'NR==i+1{split($8,r,"-");print r[2]}')
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

# --- smart-aware check wrappers (T16..T20) ---------------------------------
# $smart is 0 when the binary was built without --smart support (see the gate
# in the T16 block): every --smart check is then reported as SKIP instead of
# pass/fail, so a WITH_SMART=OFF binary still ends the suite green.
smart=1

sm_eq() { # NAME EXPECTED ACTUAL
    if [ "$smart" = 0 ]; then
        skip_note "$1"
    else
        check_eq "$1" "$2" "$3"
    fi
}

sm_rc() { # NAME EXPECTED_RC COMMAND...
    if [ "$smart" = 0 ]; then
        skip_note "$1"
    else
        check_rc "$1" "$2" "${@:3}"
    fi
}

sm_grep() { # NAME PATTERN FILE
    if [ "$smart" = 0 ]; then
        skip_note "$1"
    elif [ ! -f "$3" ]; then
        ko "$1 (file does not exist: $3)"
    elif grep -q "$2" "$3"; then
        ok "$1"
    else
        ko "$1 (pattern not found; file: $(tr '\n' ';' <"$3" | cut -c1-160))"
    fi
}

# first packet flag of a stream (K for a keyframe packet)
first_pkt_flag() {
    ffprobe -v error -select_streams v:0 -show_entries packet=flags \
        -of csv=p=0 "$1" | head -1 | cut -c1
}

# decode verdict: clean (ffmpeg -v error, empty stderr) or dirty:<first errors>
dec_verdict() {
    if [ ! -f "$1" ]; then
        echo "missing (cut failed)"
    elif ffmpeg -v error -i "$1" -f null - 2>"$WORK/dv.err"; then
        if [ -s "$WORK/dv.err" ]; then
            echo "dirty: $(tr '\n' ';' <"$WORK/dv.err" | cut -c1-140)"
        else
            echo clean
        fi
    else
        echo "dirty: rc!=0 $(tr '\n' ';' <"$WORK/dv.err" | cut -c1-140)"
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
    $4 != 1 { split($8, r, "-"); if (r[2] - r[1] + 1 < 2) bad++ }
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
# T11: open GOP (x264 open-gop): recovery point SEI as cut anchor,
#      leading pictures marked in --list (type suffixes R / L).
#      scenecut=0 -> deterministic keying (AGENTS pitfall 7): IDR at 0,
#      recovery point at the keyint=50 boundary (frame 50, the scene change
#      of the concat), its leading picture right behind in decode order.
# ---------------------------------------------------------------------------

if ! ffmpeg -v error -f lavfi -i "testsrc=size=176x144:rate=25:duration=2" \
        -f lavfi -i "smptebars=size=176x144:rate=25:duration=2" \
        -filter_complex "[0:v]format=yuv420p[a0];[1:v]format=yuv420p[a1];[a0][a1]concat=n=2:v=1[a]" \
        -map "[a]" -c:v libx264 -g 50 -x264-params open-gop=1:scenecut=0 \
        -an -y "$WORK/s6.264" 2>"$WORK/gen.err"; then
    echo "error: cannot generate open-gop stream: $(cat "$WORK/gen.err")" >&2
    exit 2
fi
S6="$WORK/s6.264"
"$BIN" --list "$S6" > "$WORK/s6.lst" || { echo "fatal: --list failed on s6" >&2; exit 2; }

n_r=$(list_rows "$WORK/s6.lst" | awk '$3 ~ /R/' | wc -l)
n_l=$(list_rows "$WORK/s6.lst" | awk '$3 ~ /L/' | wc -l)
if [ "$n_r" -gt 0 ]; then
    ok "T11 open GOP: recovery point(s) in --list ($n_r rows with type suffix R)"
else
    ko "T11 open GOP: no recovery point (type suffix R) in --list — x264 wrote no usable recovery SEI or detection is broken"
fi
if [ "$n_l" -gt 0 ]; then
    ok "T11 open GOP: leading picture(s) in --list ($n_l rows with type suffix L)"
else
    ko "T11 open GOP: no leading picture (type suffix L) in --list — open-gop leading detection broken"
fi

if [ "$n_r" -gt 0 ]; then
    ridx=$(list_rows "$WORK/s6.lst" | awk '$3 ~ /R/ {print $1; exit}')
    bend=$((ridx + 40))
    "$BIN" -f $((ridx + 1)) -t "$bend" -o "$WORK/cut_open.264" "$S6" \
        2>"$WORK/cut_open.err"
    if grep -q "левая граница сдвинута к recovery point (кадр $ridx)" \
            "$WORK/cut_open.err"; then
        ok "T11 left boundary snaps to recovery frame $ridx, not the earlier IDR"
    else
        ko "T11 left boundary snaps to recovery frame $ridx (stderr: $(tr '\n' ';' < "$WORK/cut_open.err"))"
    fi
    if grep -q "leading-кадров открытых GOP внутри диапазона" "$WORK/cut_open.err"; then
        ok "T11 stderr warns about leading pictures carried with broken dependencies"
    else
        ko "T11 stderr warns about leading pictures in the cut range"
    fi
    check_nal_eq "T11 NAL sequence of the recovery-anchored cut [$ridx..$bend]" \
        "$WORK/s6.lst" "$S6" "$WORK/cut_open.264" "$ridx" "$bend"
    if ffmpeg -v error -i "$WORK/cut_open.264" -f null - 2>"$WORK/dec_open.err"; then
        if [ -s "$WORK/dec_open.err" ]; then
            ko "T11 recovery-anchored cut decodes without stderr"
        else
            ok "T11 recovery-anchored cut decodes without stderr"
        fi
    else
        ko "T11 recovery-anchored cut decodes without stderr"
    fi
fi

# ---------------------------------------------------------------------------
# T12: SEI picture timing (cnt_type 0 clock timestamps) as --exact time base.
#      x264 0.165 writes no picture timing clock timestamps at all
#      (-x264-params pic-timing=1 yields not a single SEI NAL beyond the
#      x264 user_data one), so the test injects one picture_timing SEI per
#      AU into the no-B-frames stream s3 (decode order == display order),
#      raw ts = frame_index * 3600 ticks (25 fps, 1/90000 s units).
#      x264's VUI has fixed_frame_rate_flag = 0, so the clock path is the
#      one actually exercised.
# ---------------------------------------------------------------------------

if ! "$PY" - "$S3" "$WORK/s5.264" <<'PYEOF'
import re, sys
src, dst = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()
starts = [m.start() for m in re.finditer(b'\x00\x00\x01', data)]
def esc(p):
    out, z = bytearray(), 0
    for b in p:
        if z >= 2 and b <= 3:
            out.append(3); z = 0
        out.append(b); z = z + 1 if b == 0 else 0
    return bytes(out)
def sei(i):  # payloadType 1, clock ts: flag=1, cnt_type=00, n_90kHz=0, u24
    bits = "1" + "00" + "0" + format(i * 3600, "024b") + "0" * 17
    bits += "1"
    bits += "0" * (-len(bits) % 8)
    payload = bytes(int(bits[k:k+8], 2) for k in range(0, len(bits), 8))
    return b"\x00\x00\x00\x01\x06\x01" + bytes([len(payload)]) + esc(payload + b"\x80")
out, n = bytearray(), 0
for k, st in enumerate(starts):
    end = starts[k + 1] if k + 1 < len(starts) else len(data)
    if (data[st + 3] & 0x1f) in (1, 5) and (data[st + 4] & 0x80):  # first_mb_in_slice == 0
        out += sei(n); n += 1
    out += data[st:end]
if n == 0:
    sys.exit("no AUs found for SEI injection")
open(dst, "wb").write(out)
PYEOF
then
    echo "error: cannot inject picture timing SEIs into s3" >&2
    exit 2
fi
S5="$WORK/s5.264"
"$BIN" --list "$S5" > "$WORK/s5.lst" || { echo "fatal: --list failed on s5" >&2; exit 2; }

no_t=$(list_rows "$WORK/s5.lst" | awk '$7 == "-"' | wc -l)
check_eq "T12 --list: t column present for every frame (clock base)" "0" "$no_t"
t_first=$(list_rows "$WORK/s5.lst" | awk 'NR==1 {print $7}')
t_second=$(list_rows "$WORK/s5.lst" | awk 'NR==2 {print $7}')
check_eq "T12 t of the first frame" "0.000" "$t_first"
check_eq "T12 t of the second frame" "0.040" "$t_second"

check_rc "T12 --exact via clock base exits 0" 0 \
    "$BIN" --exact 00:00:00.500-00:00:02.500 -o "$WORK/cut_clk.264" "$S5"
"$BIN" --exact 00:00:00.500-00:00:02.500 -o /dev/null "$S5" 2>"$WORK/cut_clk.err"
if grep -q "display-время взято из SEI picture timing (VFR)" "$WORK/cut_clk.err"; then
    ok "T12 stderr notes that display time comes from SEI picture timing"
else
    ko "T12 stderr notes that display time comes from SEI picture timing"
fi
check_rc "T12 --exact --fps exits 0" 0 \
    "$BIN" --exact 00:00:00.500-00:00:02.500 --fps 25 \
    -o "$WORK/cut_clkfps.264" "$S5"
if cmp -s "$WORK/cut_clk.264" "$WORK/cut_clkfps.264"; then
    ok "T12 clock window equals the --fps window (byte-identical cuts)"
else
    ko "T12 clock window equals the --fps window (byte-identical cuts)"
fi
if ffmpeg -v error -i "$WORK/cut_clk.264" -f null - 2>"$WORK/dec_clk.err" \
        && [ ! -s "$WORK/dec_clk.err" ]; then
    ok "T12 clock-based cut decodes without stderr"
else
    ko "T12 clock-based cut decodes without stderr"
fi

# ---------------------------------------------------------------------------
# T13: --list column layout regression after adding the t column:
#      dec_id disp_id type idr frame_num poc t nal_range byte_range.
# ---------------------------------------------------------------------------

check_eq "T13 --list header layout" \
    "# dec_id disp_id type idr frame_num poc t nal_range byte_range" \
    "$(head -1 "$WORK/s1.lst" | awk '{$1=$1};1')"
bad_cols=$(for l in s1 s2 s3 s6; do list_rows "$WORK/$l.lst"; done | awk '
    NF != 9 { bad++; next }
    $8 !~ /^[0-9]+-[0-9]+$/ { bad++; next }
    $9 !~ /^[0-9]+-[0-9]+$/ { bad++ }
    END { print bad + 0 }')
check_eq "T13 all rows have 9 columns, nal_range at \$8, byte_range at \$9" \
    "0" "$bad_cols"
t_dash=$(for l in s1 s2 s3; do list_rows "$WORK/$l.lst"; done | awk '$7 == "-"' | wc -l)
check_eq "T13 VUI streams (x264 fixed flag unset): t falls back to VUI fps, never '-'" \
    "0" "$t_dash"

# ---------------------------------------------------------------------------
# T14: stream starting in the middle of a GOP (no usable anchor at its head).
#      `ffmpeg -ss 1.3 -i s1 -c copy -f h264` cannot produce such a stream
#      from raw ES: as an input option -ss makes this ffmpeg seek past the
#      timestamps of a raw H.264 stream and write an EMPTY file; as an
#      output option it snaps the start to the IDR at 1.2 s. So the mid-GOP
#      start is built deterministically from bytes: the leading SPS/PPS
#      prefix of s1 plus everything from the byte_range start of the frame
#      at decode index 7 (a P frame inside the first GOP, g=15).
# ---------------------------------------------------------------------------

mid_off=$(list_rows "$WORK/s1.lst" | awk '$1 == 7 {split($9, r, "-"); print r[1]}')
if ! "$PY" - "$S1" "$WORK/mid.264" "$mid_off" <<'PYEOF'
import re, sys
src, dst, off = sys.argv[1], sys.argv[2], int(sys.argv[3])
data = open(src, "rb").read()
starts = [m.end() for m in re.finditer(b"\x00{2,3}\x01", data)]
out = bytearray()
for k, h in enumerate(starts):
    end = starts[k + 1] if k + 1 < len(starts) else len(data)
    if (data[h] & 0x1f) in (7, 8):  # leading parameter sets
        out += b"\x00\x00\x00\x01" + data[h:end]
    else:
        break
out += data[off - 3:]  # 3-byte start code before the mid-GOP slice NAL
open(dst, "wb").write(out)
PYEOF
then
    echo "error: cannot build mid-GOP stream" >&2
    exit 2
fi
MID="$WORK/mid.264"
"$BIN" --list "$MID" > "$WORK/mid.lst" 2>"$WORK/mid.err"
check_eq "T14 --list on mid-GOP stream exits 0" "0" "$?"
mid_rows=$(list_rows "$WORK/mid.lst" | wc -l)
if [ "$mid_rows" -gt 0 ]; then
    ok "T14 mid-GOP stream indexed ($mid_rows frames; frames before the first IDR may be invalid)"
else
    ko "T14 mid-GOP stream indexed (no frames in --list)"
fi
"$BIN" -f 0 -o "$WORK/mid_f0.264" "$MID" 2>"$WORK/mid_f0.err"
check_eq "T14 cut from frame 0 exits 3 (no anchor at or before it)" "3" "$?"
if grep -q "no cut anchor" "$WORK/mid_f0.err"; then
    ok "T14 stderr reports 'no cut anchor' for the mid-GOP start"
else
    ko "T14 stderr reports 'no cut anchor' (stderr: $(tr '\n' ';' < "$WORK/mid_f0.err"))"
fi
ridx=$(list_rows "$WORK/mid.lst" | awk '$4 == 1 {print $1; exit}')
if [ -n "$ridx" ]; then
    ok "T14 mid-GOP stream has its first IDR at decode index $ridx"
    "$BIN" -f "$ridx" -o "$WORK/mid_idr.264" "$MID" 2>/dev/null
    check_eq "T14 cut from the first listed IDR exits 0" "0" "$?"
    if ffmpeg -v error -i "$WORK/mid_idr.264" -f null - 2>"$WORK/dec_mid.err" \
            && [ ! -s "$WORK/dec_mid.err" ]; then
        ok "T14 mid-GOP cut from the IDR decodes without stderr"
    else
        ko "T14 mid-GOP cut from the IDR decodes without stderr ($(tr '\n' ';' < "$WORK/dec_mid.err"))"
    fi
else
    ko "T14 mid-GOP stream has its first IDR at decode index (none found in --list)"
fi

# ---------------------------------------------------------------------------
# T15: truncated tail (60% of s1): the last AU is cut mid-NAL. --list must
#      still index the stream and a full-range cut must still work; the
#      last frame may come out as parse_error '?'.
# ---------------------------------------------------------------------------

sz=$(stat -c %s "$S1")
head -c $((sz * 60 / 100)) "$S1" > "$WORK/trunc.264"
"$BIN" --list "$WORK/trunc.264" > "$WORK/trunc.lst" 2>"$WORK/trunc.err"
check_eq "T15 --list on tail-truncated stream exits 0" "0" "$?"
trunc_rows=$(list_rows "$WORK/trunc.lst" | wc -l)
if [ "$trunc_rows" -gt 0 ]; then
    ok "T15 truncated stream indexed ($trunc_rows frames)"
else
    ko "T15 truncated stream indexed (no frames in --list)"
fi
"$BIN" -f 0 -t $((trunc_rows - 1)) -o "$WORK/trunc_cut.264" "$WORK/trunc.264" \
    2>/dev/null
check_eq "T15 full-range cut of the truncated stream exits 0" "0" "$?"

# ---------------------------------------------------------------------------
# --smart gate. Probe a tiny cut on s3: if stderr says the binary was built
# without smart support (WITH_SMART=OFF, e.g. no libavcodec-dev on the CI
# runner), every T16..T20 check is counted SKIP and the suite stays green.
# Any other probe failure leaves smart enabled so the real tests report it.
# ---------------------------------------------------------------------------

"$BIN" --smart -f 1 -o "$WORK/smart_gate.264" "$S3" 2>"$WORK/smart_gate.err"
if grep -q "собрано без поддержки --smart" "$WORK/smart_gate.err"; then
    smart=0
    echo "note: binary built without --smart support, T16..T20 skipped" >&2
fi

# ---------------------------------------------------------------------------
# T16..T20 streams (PLAN rule set for --smart). Deterministic keying via
# scenecut=0 (pitfall 7); x264 always writes VUI timing with fixed=0
# (pitfall 8), so --exact/--smart get their fps from the VUI.
#   s7:  200 frames, g=40 -> IDR at dec 0,40,80,120,160 (closed GOP)
#   s8:  same with open-gop=1 -> recovery points + leading pictures
#   s9:  tff=1 (interlaced),  s10: cropped 318x238 — --smart v1 refusals
# ---------------------------------------------------------------------------

gen s7 -i "testsrc=size=320x240:rate=25:duration=8" \
    -c:v libx264 -preset medium -profile:v high -pix_fmt yuv420p -g 40 \
    -x264-params scenecut=0
gen s8 -i "testsrc=size=320x240:rate=25:duration=8" \
    -c:v libx264 -preset medium -profile:v high -pix_fmt yuv420p -g 40 \
    -x264-params scenecut=0:open-gop=1
gen s9 -i "testsrc=size=320x240:rate=25:duration=2" \
    -c:v libx264 -preset medium -profile:v high -pix_fmt yuv420p -g 40 \
    -x264-params scenecut=0:tff=1
gen s10 -i "testsrc=size=320x240:rate=25:duration=2" \
    -c:v libx264 -preset medium -profile:v high -pix_fmt yuv420p -g 40 \
    -vf crop=318:238 -x264-params scenecut=0

S7="$WORK/s7.264"
S8="$WORK/s8.264"
S9="$WORK/s9.264"
S10="$WORK/s10.264"

for s in s7 s8 s9 s10; do
    "$BIN" --list "$WORK/$s.264" > "$WORK/$s.lst" \
        || { echo "fatal: --list failed on $s" >&2; exit 2; }
done

idrs7=$(list_rows "$WORK/s7.lst" | awk '$4 == 1 {printf "%s ", $1}')
sm_eq "T16 reference stream: 200 frames, IDR at dec 0,40,80,120,160" \
    "0 40 80 120 160 " "$idrs7"

# ---------------------------------------------------------------------------
# T16: --smart start from the middle of a GOP (-f 50, IDR at 40 and 80):
#      the fragment [50..79] is re-encoded (forced IDR start), [80..199]
#      must be copied byte-for-byte.
# ---------------------------------------------------------------------------

if [ "$smart" = 1 ]; then
    "$BIN" --smart -f 50 -o "$WORK/smart_f50.264" "$S7" 2>"$WORK/smart_f50.err"
    rc16=$?
    n16=$(ff_nb "$WORK/smart_f50.264")
    k16=$(first_pkt_flag "$WORK/smart_f50.264")
    dec16=$(dec_verdict "$WORK/smart_f50.264")
else
    rc16=""; n16=""; k16=""; dec16=""
fi
sm_eq "T16 --smart -f 50 exit code" "0" "$rc16"
sm_eq "T16 mid-GOP smart cut decodes to 150 frames" "150" "$n16"
sm_eq "T16 mid-GOP smart cut starts with a key frame" "K" "$k16"
sm_eq "T16 mid-GOP smart cut decodes without stderr" "clean" "$dec16"

# PSNR of the re-encoded GOP against a select-trim reference of the same
# pictures: --list maps dec [50..79] to their display ranks; ffmpeg emits
# frames in presentation order, so eq(n,<disp_id>) picks exactly them.
if [ "$smart" = 1 ]; then
    sel16=$(list_rows "$WORK/s7.lst" | awk '$1 >= 50 && $1 <= 79 {print $2}' \
            | sort -n | awk '{printf "%s eq(n\\,%s)", (NR > 1 ? "+" : ""), $1}')
    if ffmpeg -v error -i "$S7" -vf "select='$sel16'" -fps_mode passthrough \
            -f rawvideo -pix_fmt yuv420p -y "$WORK/smart_ref.yuv" \
            && ffmpeg -v error -i "$WORK/smart_f50.264" -frames:v 30 \
            -f rawvideo -pix_fmt yuv420p -y "$WORK/smart_frag.yuv"; then
        psnr16=$(ffmpeg -hide_banner -s 320x240 -i "$WORK/smart_frag.yuv" \
                 -s 320x240 -i "$WORK/smart_ref.yuv" -lavfi psnr -f null - 2>&1 \
                 | grep -oE 'average:[0-9.]+' | head -1 | cut -d: -f2)
    else
        psnr16=""
    fi
    if [ -n "$psnr16" ] && awk -v p="$psnr16" 'BEGIN { exit !(p >= 30) }'; then
        psnr16_ok=ge30
    else
        psnr16_ok="${psnr16:-none}"
    fi
else
    psnr16_ok=""
fi
sm_eq "T16 re-encoded GOP average PSNR vs source >= 30 dB" "ge30" "$psnr16_ok"

# Byte-for-byte copy tail: the last N NAL signatures of the output must be
# the source NALs of frames [80..199] (N = their count); the re-encoded head
# and any prepended parameter sets sit before them.
if [ "$smart" = 1 ]; then
    lo16=$(list_rows "$WORK/s7.lst" | awk '$1 == 80 {split($9, r, "-"); print r[1]}')
    hi16=$(list_rows "$WORK/s7.lst" | awk 'END {split($9, r, "-"); print r[2]}')
    "$PY" "$NALPY" "$S7" | awk -v lo="$lo16" -v hi="$hi16" \
        '$3 >= lo && $3 <= hi {print $2, $4}' > "$WORK/tail_exp.tmp"
    nal_sigs "$WORK/smart_f50.264" > "$WORK/tail_all.tmp"
    tail -n "$(wc -l <"$WORK/tail_exp.tmp")" "$WORK/tail_all.tmp" \
        > "$WORK/tail_got.tmp"
    if diff -q "$WORK/tail_exp.tmp" "$WORK/tail_got.tmp" >/dev/null; then
        tail16=identical
    else
        tail16="differs: $(diff "$WORK/tail_exp.tmp" "$WORK/tail_got.tmp" | head -3 | tr '\n' ' ')"
    fi
else
    tail16=""
fi
sm_eq "T16 copied tail NAL-exact vs source frames [80..199]" \
    "identical" "$tail16"

# ---------------------------------------------------------------------------
# T17: boundary already on an IDR, no leading pictures (closed GOP): the
#      --smart output must be byte-for-byte the copy-mode one (PLAN rule 5).
# ---------------------------------------------------------------------------

if [ "$smart" = 1 ]; then
    "$BIN" --smart -f 40 -o "$WORK/smart_f40.264" "$S7" 2>/dev/null
    rc17=$?
    "$BIN" -f 40 -o "$WORK/copy_f40.264" "$S7" 2>/dev/null
    if [ "$rc17" = 0 ] && cmp -s "$WORK/smart_f40.264" "$WORK/copy_f40.264"; then
        eq17=identical
    else
        eq17="differs (smart rc $rc17, sizes $(stat -c %s "$WORK/smart_f40.264" 2>/dev/null) vs $(stat -c %s "$WORK/copy_f40.264" 2>/dev/null))"
    fi
else
    rc17=""; eq17=""
fi
sm_eq "T17 --smart -f 40 (IDR boundary) exit code" "0" "$rc17"
sm_eq "T17 --smart output byte-identical to copy output" "identical" "$eq17"

# ---------------------------------------------------------------------------
# T18: --exact --smart: display-time window 2s..5s (25 fps -> frames
#      50..124-ish); sane frame count, key start, clean decode.
# ---------------------------------------------------------------------------

if [ "$smart" = 1 ]; then
    "$BIN" --smart -e 00:00:02.000-00:00:05.000 -o "$WORK/smart_exact.264" \
        "$S7" 2>"$WORK/smart_exact.err"
    rc18=$?
    n18=$(ff_nb "$WORK/smart_exact.264")
    k18=$(first_pkt_flag "$WORK/smart_exact.264")
    dec18=$(dec_verdict "$WORK/smart_exact.264")
    if [ -n "$n18" ] && [ "$n18" -ge 70 ] && [ "$n18" -le 80 ]; then
        n18_ok=in-range
    else
        n18_ok="${n18:-none}"
    fi
else
    rc18=""; k18=""; dec18=""; n18_ok=""
fi
sm_eq "T18 --exact --smart exit code" "0" "$rc18"
sm_eq "T18 --exact --smart frame count within 70..80" "in-range" "$n18_ok"
sm_eq "T18 --exact --smart starts with a key frame" "K" "$k18"
sm_eq "T18 --exact --smart decodes without stderr" "clean" "$dec18"

# ---------------------------------------------------------------------------
# T19: open GOP (s8): --smart from anchor+1 with leading pictures (L) inside
#      the re-encode window — warm-up must repair their dependencies.  The
#      copy mode of the same boundary only warns about them.
# ---------------------------------------------------------------------------

if [ "$smart" = 1 ]; then
    anc19=$(list_rows "$WORK/s8.lst" | awk '$3 ~ /R/ {print $1; exit}')
    l19=$(list_rows "$WORK/s8.lst" \
          | awk -v a="${anc19:--1}" '$3 ~ /L/ && $1 > a + 0' | wc -l)
    if [ -n "$anc19" ] && [ "$l19" -gt 0 ]; then
        prem19=yes
    else
        prem19="no (anchor=${anc19:-none}, leading after it=$l19)"
    fi
else
    anc19=""; l19=""; prem19=""
fi
sm_eq "T19 open-gop stream: recovery anchor with leading pictures after it" \
    "yes" "$prem19"

if [ "$smart" = 1 ] && [ -n "$anc19" ]; then
    b19=$((anc19 + 1))
    "$BIN" --smart -f "$b19" -o "$WORK/smart_open.264" "$S8" \
        2>"$WORK/smart_open.err"
    rc19=$?
    dec19=$(dec_verdict "$WORK/smart_open.264")
    "$BIN" -f "$b19" -o "$WORK/copy_open.264" "$S8" 2>"$WORK/copy_open.err"
else
    rc19=""; dec19=""
fi
sm_eq "T19 --smart -f (anchor+1) exit code" "0" "$rc19"
sm_grep "T19 --smart stderr announces the re-encode window" \
    "перекодируем" "$WORK/smart_open.err"
sm_eq "T19 open-GOP smart cut decodes without stderr" "clean" "$dec19"
sm_grep "T19 copy mode of the same boundary warns about leading pictures" \
    "leading-кадров" "$WORK/copy_open.err"

# ---------------------------------------------------------------------------
# T20: --smart v1 refusals (PLAN rule 6): interlaced and cropped streams
#      must fail with exit 2 and a literate stderr note, while copy mode of
#      the very same boundary keeps working (the limit is smart-specific).
# ---------------------------------------------------------------------------

if [ "$smart" = 1 ]; then
    "$BIN" --smart -f 45 -o "$WORK/smart_tff.264" "$S9" 2>"$WORK/smart_tff.err"
    rc20t=$?
    "$BIN" --smart -f 45 -o "$WORK/smart_crop.264" "$S10" 2>"$WORK/smart_crop.err"
    rc20c=$?
else
    rc20t=""; rc20c=""
fi
sm_eq "T20 --smart refuses the interlaced stream (exit 2)" "2" "$rc20t"
sm_grep "T20 interlaced refusal names frame_mbs_only_flag" \
    "frame_mbs_only_flag = 0" "$WORK/smart_tff.err"
sm_rc "T20 copy mode works on the interlaced stream (-f 45)" 0 \
    "$BIN" -f 45 -o "$WORK/copy_tff.264" "$S9"
sm_eq "T20 --smart refuses the cropped stream (exit 2)" "2" "$rc20c"
sm_grep "T20 crop refusal names frame_cropping_flag" \
    "frame_cropping_flag" "$WORK/smart_crop.err"
sm_rc "T20 copy mode works on the cropped stream (-f 45)" 0 \
    "$BIN" -f 45 -o "$WORK/copy_crop.264" "$S10"

# ---------------------------------------------------------------------------
# --smart smoke: --crf bounds and option mixing (usage errors are checked
# unconditionally: parse-time refusals do not need smart support).
# ---------------------------------------------------------------------------

sm_rc "smoke --smart --crf 51 -f 45 succeeds" 0 \
    "$BIN" --smart --crf 51 -f 45 -o "$WORK/smart_crf51.264" "$S7"
check_rc "smoke --crf 52 is refused (exit 1)" 1 \
    "$BIN" --smart --crf 52 -f 45 -o "$WORK/x.264" "$S7"
check_rc "smoke --smart --list is refused (exit 1)" 1 \
    "$BIN" --smart --list "$S7"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

echo
echo "passed: $pass, skipped: $skip, failed: $fail"
if [ "$fail" -ne 0 ]; then
    exit 1
fi
exit 0
