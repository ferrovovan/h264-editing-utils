#!/usr/bin/env bash
#
# fuzz.sh - mutation fuzz harness for h264-cut.
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
# Usage: fuzz.sh /path/to/h264-cut   (or H264_CUT=/path/to/h264-cut fuzz.sh)
#
# Optional environment: FUZZ_SEED (default 1234, deterministic),
# FUZZ_MUTANTS (mutants per base stream, default 60).
#
# Generates two base streams, mutates them (truncations + bit flips via
# fuzz_mutate.py, fixed seed) and runs every mutant through --list (and a
# subset through --exact).  Allowed exit codes: 0, 2, 3.  Any other code,
# a timeout, a signal or sanitizer output (ASAN/LSAN/UBSAN) on stderr is
# a FAIL.  Details of the first 5 failures are printed; stderr of every
# run is kept in a per-run log inside the (temporary) work dir.

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

# Predictable sanitizer defaults: leaks must fail the run even if the
# environment globally disables them.
unset ASAN_OPTIONS UBSAN_OPTIONS LSAN_OPTIONS

# shellcheck disable=SC1007  # CDPATH= cd — env prefix for the command, not an assignment
TESTS_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MUTPY="$TESTS_DIR/fuzz_mutate.py"
PY=python3

WORK=$(mktemp -d "${TMPDIR:-/tmp}/h264cut-fuzz.XXXXXX") || {
    echo "error: cannot create temp dir" >&2; exit 2;
}
trap 'rm -rf "$WORK"' EXIT

SEED="${FUZZ_SEED:-1234}"
PER_BASE="${FUZZ_MUTANTS:-60}"   # 2 base streams x PER_BASE mutants
EXACT_EVERY=4          # 120/4 = 30 mutants also run through --exact

gen() { # gen NAME FFMPEG_ARGS...
    local name=$1
    shift
    if ! ffmpeg -v error -f lavfi "$@" -an -y "$WORK/$name.264" \
            2>"$WORK/gen.err"; then
        echo "error: cannot generate stream '$name': $(cat "$WORK/gen.err")" >&2
        exit 2
    fi
}

gen b1 -i "testsrc2=size=176x144:rate=25:duration=3" \
    -c:v libx264 -profile:v high -g 12 -bf 2 -x264-params scenecut=0
gen b2 -i "testsrc2=size=176x144:rate=25:duration=2" \
    -c:v libx264 -profile:v baseline -g 10 -bf 0 -x264-params scenecut=0

mkdir -p "$WORK/mut1" "$WORK/mut2" "$WORK/logs"
if ! "$PY" "$MUTPY" "$WORK/b1.264" "$WORK/mut1" "$PER_BASE" "$SEED" >/dev/null \
        || ! "$PY" "$MUTPY" "$WORK/b2.264" "$WORK/mut2" "$PER_BASE" "$SEED" >/dev/null; then
    echo "error: mutation generation failed" >&2
    exit 2
fi

runs=0
fail=0
shown=0
SAN_PAT='AddressSanitizer|LeakSanitizer|runtime error'

# run_case TAG FILE... — one instrumented invocation; argv after FILE is
# the h264-cut command line (the cut output goes to a scratch tmpfile).
run_case() {
    local tag=$1 file=$2
    shift 2
    local err="$WORK/logs/$tag.err" rc
    runs=$((runs + 1))
    timeout 10 "$BIN" "$@" "$file" >/dev/null 2>"$err"
    rc=$?
    case $rc in
        0|2|3)
            if grep -Eq "$SAN_PAT" "$err"; then
                fail=$((fail + 1))
                if [ "$shown" -lt 5 ]; then
                    shown=$((shown + 1))
                    echo "[FAIL] $tag: rc $rc but sanitizer output:"
                    sed -n '1,8p' "$err" | sed 's/^/       /'
                fi
            fi
            ;;
        *)
            fail=$((fail + 1))
            if [ "$shown" -lt 5 ]; then
                shown=$((shown + 1))
                echo "[FAIL] $tag: exit code $rc ($( [ "$rc" -ge 124 ] && echo 'timeout/signal' || echo 'unexpected')), $(basename "$file"):"
                sed -n '1,8p' "$err" | sed 's/^/       /'
            fi
            ;;
    esac
}

n=0
for f in "$WORK"/mut1/fuzz_*.h264 "$WORK"/mut2/fuzz_*.h264; do
    tag=$(basename "$f" .h264)
    run_case "$tag.list" "$f" --list
    if [ $((n % EXACT_EVERY)) -eq 0 ]; then
        run_case "$tag.exact" "$f" --exact 00:00:00.100-00:00:02.000 \
            -o "$WORK/exact_out.264"
    fi
    n=$((n + 1))
done

echo
echo "mutants: $((2 * PER_BASE)), runs: $runs, failures: $fail"
if [ "$fail" -ne 0 ]; then
    echo "fuzz: FAIL"
    exit 1
fi
echo "fuzz: PASS"
exit 0
