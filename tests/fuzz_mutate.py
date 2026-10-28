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
"""Generate deterministic mutants of a raw Annex B H.264 stream.

    fuzz_mutate.py SRC.264 OUT_DIR COUNT [SEED]

Two mutation modes, alternating (even mutant = truncation at a
pseudorandom length, odd mutant = 1..3 bit flips in 1..3 distinct random
bytes).  SEED (default 0) makes the whole run reproducible.  Files are
written as OUT_DIR/fuzz_%04d.h264 and their paths are printed to stdout,
one per line.  Standard library only.
"""

import os
import random
import sys


def truncate(data, rng):
    # Half the mutants cut inside the first tenth of the stream so that
    # early GOPs and header regions are covered, the rest is uniform.
    if rng.random() < 0.5:
        return data[:rng.randrange(1, max(2, len(data) // 10))]
    return data[:rng.randrange(1, len(data))]


def flip_bits(data, rng):
    out = bytearray(data)
    for pos in rng.sample(range(len(out)), rng.randint(1, 3)):
        out[pos] ^= 1 << rng.randrange(8)
    return bytes(out)


def main(argv):
    if len(argv) not in (4, 5):
        sys.stderr.write(__doc__.splitlines()[0] + "\n")
        return 2
    src, outdir, count, seed = argv[1], argv[2], int(argv[3]), int(argv[4]) if len(argv) == 5 else 0

    with open(src, "rb") as f:
        data = f.read()
    if len(data) < 2:
        sys.stderr.write("fuzz_mutate.py: input too short: %s\n" % src)
        return 2

    os.makedirs(outdir, exist_ok=True)
    rng = random.Random(seed)
    for i in range(count):
        mutated = truncate(data, rng) if i % 2 == 0 else flip_bits(data, rng)
        path = os.path.join(outdir, "fuzz_%04d.h264" % i)
        with open(path, "wb") as f:
            f.write(mutated)
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
