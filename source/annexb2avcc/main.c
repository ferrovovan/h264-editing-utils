/*
 * main.c - annexb2avcc: convert an Annex B H.264 elementary stream to the
 * length-prefixed ("AVCC") form used inside MP4-style containers.
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

    /* ftello()/off_t are POSIX; -std=c11 alone would hide them. */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avcc.h"

#define PROG "annexb2avcc"

#define ERR(...) fprintf(stderr, PROG ": " __VA_ARGS__)

/* Exit codes (as in h264-cut). */
enum {
    EXIT_OK = 0,
    EXIT_USAGE = 1,
    EXIT_STREAM = 2,
    EXIT_IO = 4
};

static void usage(FILE *out)
{
    fputs("usage: annexb2avcc [опции] INPUT.264 OUTPUT.avcc\n"
          "  -n N       размер поля длины NAL в выходе, байт"
          " (2..4, по умолчанию 4)\n"
          "  -h, --help эта справка\n"
          "\n"
          "ES Annex B -> ES с length-prefixed NAL (big-endian)."
          " Все NAL, включая\n"
          "SPS/PPS, переносятся побайтово: в MP4 параметр-сеты живут"
          " в avcC, здесь\n"
          "это raw-конвейер без контейнера.\n",
          out);
}

static uint8_t *read_whole_file(const char *path, size_t *out_size)
{
    FILE *f;
    uint8_t *buf;
    off_t fsize;
    size_t got;

    f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    fsize = ftello(f);
    if (fsize < 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)fsize + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        free(buf);
        fclose(f);
        return NULL;
    }
    got = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    if (got != (size_t)fsize) {
        free(buf);
        return NULL;
    }
    buf[(size_t)fsize] = '\0';
    *out_size = (size_t)fsize;
    return buf;
}

int main(int argc, char **argv)
{
    const char *positional[2] = { NULL, NULL };
    unsigned nal_len = 4;
    uint8_t *in = NULL;
    uint8_t *out = NULL;
    size_t in_size = 0;
    size_t out_size = 0;
    int np = 0;
    int i;
    int rc = EXIT_OK;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(stdout);
            return EXIT_OK;
        }
        if (strcmp(arg, "-n") == 0) {
            char *end;
            long v;

            if (i + 1 >= argc) {
                ERR("option -n requires a value\n");
                return EXIT_USAGE;
            }
            v = strtol(argv[++i], &end, 10);
            if (*argv[i] == '\0' || *end != '\0' || v < 2 || v > 4) {
                ERR("invalid -n value: %s (expected 2..4)\n", argv[i]);
                return EXIT_USAGE;
            }
            nal_len = (unsigned)v;
            continue;
        }
        if (arg[0] == '-' && arg[1] != '\0') {
            ERR("unknown option: %s\n", arg);
            return EXIT_USAGE;
        }
        if (np >= 2) {
            ERR("unexpected argument: %s\n", arg);
            return EXIT_USAGE;
        }
        positional[np++] = arg;
    }
    if (np != 2) {
        ERR("expected INPUT.264 and OUTPUT.avcc\n");
        usage(stderr);
        return EXIT_USAGE;
    }

    in = read_whole_file(positional[0], &in_size);
    if (!in) {
        ERR("cannot read input file: %s\n", positional[0]);
        return EXIT_STREAM;
    }
    if (annexb_to_avcc(in, in_size, nal_len, &out, &out_size) != 0) {
        ERR("cannot convert stream: %s\n", positional[0]);
        free(in);
        return EXIT_STREAM;
    }
    {
        FILE *f = fopen(positional[1], "wb");

        if (!f) {
            ERR("cannot open output file: %s\n", positional[1]);
            rc = EXIT_IO;
        } else if (out_size != 0
                && fwrite(out, 1, out_size, f) != out_size) {
            ERR("write error: %s\n", positional[1]);
            rc = EXIT_IO;
        } else if (fclose(f) != 0) {
            ERR("write error: %s\n", positional[1]);
            rc = EXIT_IO;
        }
    }
    free(in);
    free(out);
    return rc;
}
