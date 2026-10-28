/*
 * main.c - avcc2annexb: convert a length-prefixed ("AVCC") H.264 elementary
 * stream back to Annex B, optionally emitting the avcC parameter sets first.
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

#define PROG "avcc2annexb"

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
    fputs("usage: avcc2annexb [опции] INPUT.avcc OUTPUT.264\n"
          "  -n N       размер поля длины NAL во входе, байт"
          " (2..4, по умолчанию 4)\n"
          "  -c AVCFILE AVCDecoderConfigurationRecord (avcC): SPS/PPS из"
          " него\n"
          "             записываются в начало выхода (4-байтные старт-коды)"
          " перед ES\n"
          "  -h, --help эта справка\n"
          "\n"
          "ES с length-prefixed NAL (big-endian) -> ES Annex B; NAL"
          " переносятся\n"
          "побайтово, перед каждым — старт-код 00 00 00 01.\n",
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
    const char *cfg_path = NULL;
    unsigned nal_len = 4;
    uint8_t *in = NULL;
    uint8_t *out = NULL;
    uint8_t *cfg_buf = NULL;
    avcc_config_t cfg;
    size_t in_size = 0;
    size_t out_size = 0;
    size_t cfg_size = 0;
    int np = 0;
    int have_cfg = 0;
    int argi;
    int rc = EXIT_OK;
    FILE *f;

    memset(&cfg, 0, sizeof(cfg));
    for (argi = 1; argi < argc; argi++) {
        const char *arg = argv[argi];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(stdout);
            return EXIT_OK;
        }
        if (strcmp(arg, "-n") == 0 || strcmp(arg, "-c") == 0) {
            int is_n = arg[1] == 'n';
            char *end;

            if (argi + 1 >= argc) {
                ERR("option %s requires a value\n", arg);
                return EXIT_USAGE;
            }
            arg = argv[++argi];
            if (is_n) {
                long v = strtol(arg, &end, 10);

                if (*arg == '\0' || *end != '\0' || v < 2 || v > 4) {
                    ERR("invalid -n value: %s (expected 2..4)\n", arg);
                    return EXIT_USAGE;
                }
                nal_len = (unsigned)v;
            } else {
                cfg_path = arg;
            }
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
        ERR("expected INPUT.avcc and OUTPUT.264\n");
        usage(stderr);
        return EXIT_USAGE;
    }

    if (cfg_path) {
        cfg_buf = read_whole_file(cfg_path, &cfg_size);
        if (!cfg_buf) {
            ERR("cannot read config file: %s\n", cfg_path);
            return EXIT_STREAM;
        }
        if (avcc_parse(cfg_buf, cfg_size, &cfg) != 0) {
            ERR("cannot parse avcC record: %s\n", cfg_path);
            free(cfg_buf);
            return EXIT_STREAM;
        }
        have_cfg = 1;
    }

    in = read_whole_file(positional[0], &in_size);
    if (!in) {
        ERR("cannot read input file: %s\n", positional[0]);
        rc = EXIT_STREAM;
        goto out;
    }
    if (avcc_to_annexb(in, in_size, nal_len, &out, &out_size) != 0) {
        ERR("cannot convert stream: %s\n", positional[0]);
        rc = EXIT_STREAM;
        goto out;
    }
    f = fopen(positional[1], "wb");
    if (!f) {
        ERR("cannot open output file: %s\n", positional[1]);
        rc = EXIT_IO;
        goto out;
    }
    if (have_cfg) {
        static const uint8_t start_code[4] = { 0x00, 0x00, 0x00, 0x01 };
        int k;

        for (k = 0; k < cfg.sps_count && rc == EXIT_OK; k++) {
            if (fwrite(start_code, 1, 4, f) != 4
                    || fwrite(cfg.sps[k], 1, cfg.sps_len[k], f)
                            != cfg.sps_len[k])
                rc = EXIT_IO;
        }
        for (k = 0; k < cfg.pps_count && rc == EXIT_OK; k++) {
            if (fwrite(start_code, 1, 4, f) != 4
                    || fwrite(cfg.pps[k], 1, cfg.pps_len[k], f)
                            != cfg.pps_len[k])
                rc = EXIT_IO;
        }
        if (rc != EXIT_OK)
            ERR("write error: %s\n", positional[1]);
    }
    if (rc == EXIT_OK && out_size != 0
            && fwrite(out, 1, out_size, f) != out_size) {
        ERR("write error: %s\n", positional[1]);
        rc = EXIT_IO;
    }
    if (fclose(f) != 0 && rc == EXIT_OK) {
        ERR("write error: %s\n", positional[1]);
        rc = EXIT_IO;
    }

out:
    if (have_cfg)
        avcc_config_free(&cfg);
    free(cfg_buf);
    free(in);
    free(out);
    return rc;
}
