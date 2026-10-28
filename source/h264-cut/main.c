/*
 * main.c - h264-cut: cut a frame range out of an Annex B H.264 stream
 * without re-encoding.
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

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "index.h"

#define PROG "h264-cut"

#define ERR(...) fprintf(stderr, PROG ": " __VA_ARGS__)

/* Exit codes. */
enum {
    EXIT_OK = 0,
    EXIT_USAGE = 1,
    EXIT_STREAM = 2,
    EXIT_RANGE = 3,
    EXIT_IO = 4
};

typedef struct {
    int list_mode;
    int exact_mode;
    int have_from;
    int have_to;
    long long from;
    long long to;
    long double t0;
    long double t1;
    int have_fps;
    long double fps;
    const char *output;
    const char *input;
} opts_t;

static void usage(FILE *out)
{
    fputs("usage: h264-cut [опции] INPUT.264\n"
          "  -l, --list            список кадров в stdout и выход 0"
          " (не совместимо с -f/-t/-e/-o);\n"
          "                        колонка t — display-время (сек),"
          " суффиксы типа: L — leading\n"
          "                        picture открытой GOP, R — recovery point\n"
          "  -f, --from-frame N    первый сохраняемый кадр (0-based,"
          " порядок декодирования, включительно)\n"
          "  -t, --to-frame M      последний сохраняемый кадр (0-based,"
          " порядок декодирования, включительно; по умолчанию последний)\n"
          "  -e, --exact T0-T1     диапазон по времени отображения,"
          " HH:MM:SS.mmm-HH:MM:SS.mmm (миллисекунды необязательны)\n"
          "      --fps FPS         явный fps (десятичный); таймбаза по"
          " умолчанию: VUI fixed-fps,\n"
          "                        иначе SEI picture timing (clock ts, VFR"
          " — с предупреждением)\n"
          "  -o, --output FILE     файл результата (обязателен для"
          " вырезания)\n"
          "  -h, --help            эта справка\n",
          out);
}

/* Consume one or more decimal digits; NULL on no digits, else pointer past. */
static const char *scan_uint(const char *p, unsigned long *out)
{
    unsigned long v = 0;

    if (*p < '0' || *p > '9')
        return NULL;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (unsigned long)(*p - '0');
        p++;
    }
    *out = v;
    return p;
}

/* Parse "HH:MM:SS[.mmm]" into seconds. 0 on success, -1 on error. */
static int parse_time(const char *s, long double *out)
{
    const char *p;
    unsigned long hh, mm, ss;
    long double frac = 0.0L;
    long double scale;

    p = scan_uint(s, &hh);
    if (!p || hh > 99 || *p != ':')
        return -1;
    p = scan_uint(p + 1, &mm);
    if (!p || mm > 59 || *p != ':')
        return -1;
    p = scan_uint(p + 1, &ss);
    if (!p || ss > 59)
        return -1;
    if (*p == '.') {
        p++;
        if (*p < '0' || *p > '9')
            return -1;
        for (scale = 10.0L; *p >= '0' && *p <= '9'; p++) {
            frac += (long double)(*p - '0') / scale;
            scale *= 10.0L;
        }
    }
    if (*p != '\0')
        return -1;
    *out = (long double)hh * 3600.0L + (long double)mm * 60.0L
         + (long double)ss + frac;
    return 0;
}

/* Parse "T0-T1" as two times split around the first '-'. 0 on success. */
static int parse_range(const char *s, long double *t0, long double *t1)
{
    char buf[64];
    const char *dash = strchr(s, '-');
    size_t len;

    if (!dash)
        return -1;
    len = (size_t)(dash - s);
    if (len == 0 || len >= sizeof(buf))
        return -1;
    memcpy(buf, s, len);
    buf[len] = '\0';
    if (parse_time(buf, t0) != 0)
        return -1;
    if (parse_time(dash + 1, t1) != 0)
        return -1;
    return 0;
}

/* Parse a non-negative decimal index. 0 on success, -1 on error. */
static int parse_index_val(const char *s, long long *out)
{
    char *end;
    long long v;

    if (*s == '\0')
        return -1;
    v = strtoll(s, &end, 10);
    if (*end != '\0' || v < 0)
        return -1;
    *out = v;
    return 0;
}

/* Parse "floor(x)" / "ceil(x)" into long long (values are small and finite
 * here, so no math.h / libm dependency is needed). */
static long long ld_floor_ll(long double x)
{
    long long n = (long long)x;

    if ((long double)n > x)
        n--;
    return n;
}

static long long ld_ceil_ll(long double x)
{
    long long n = (long long)x;

    if ((long double)n < x)
        n++;
    return n;
}

struct opt_def {
    const char *name;   /* long name without "--", or short name with '-' */
    int has_arg;
    int id;
};

enum {
    OPT_LIST, OPT_FROM, OPT_TO, OPT_EXACT, OPT_FPS, OPT_OUTPUT, OPT_HELP
};

static const struct opt_def long_opts[] = {
    { "list",       0, OPT_LIST },
    { "from-frame", 1, OPT_FROM },
    { "to-frame",   1, OPT_TO },
    { "exact",      1, OPT_EXACT },
    { "fps",        1, OPT_FPS },
    { "output",     1, OPT_OUTPUT },
    { "help",       0, OPT_HELP }
};

static const struct opt_def short_opts[] = {
    { "-l", 0, OPT_LIST },
    { "-f", 1, OPT_FROM },
    { "-t", 1, OPT_TO },
    { "-e", 1, OPT_EXACT },
    { "-o", 1, OPT_OUTPUT },
    { "-h", 0, OPT_HELP }
};

/* 0 = ok, 1 = usage error (message printed), 2 = help requested. */
static int parse_args(int argc, char **argv, opts_t *opt)
{
    int i;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const struct opt_def *def = NULL;
        const char *val = NULL;
        int arg_taken = 0;

        if (arg[0] == '-' && arg[1] == '-') {
            const char *name = arg + 2;
            const char *eq = strchr(name, '=');
            size_t nlen = eq ? (size_t)(eq - name) : strlen(name);
            size_t k;

            for (k = 0; k < sizeof(long_opts) / sizeof(long_opts[0]); k++) {
                if (strlen(long_opts[k].name) == nlen
                        && strncmp(name, long_opts[k].name, nlen) == 0) {
                    def = &long_opts[k];
                    break;
                }
            }
            if (!def) {
                ERR("unknown option: %s\n", arg);
                return EXIT_USAGE;
            }
            if (eq) {
                if (!def->has_arg) {
                    ERR("option --%s does not take a value\n", def->name);
                    return EXIT_USAGE;
                }
                val = eq + 1;
                arg_taken = 1;
            }
        } else if (arg[0] == '-' && arg[1] != '\0') {
            size_t k;

            for (k = 0; k < sizeof(short_opts) / sizeof(short_opts[0]); k++) {
                if (strcmp(arg, short_opts[k].name) == 0) {
                    def = &short_opts[k];
                    break;
                }
            }
            if (!def) {
                ERR("unknown option: %s\n", arg);
                return EXIT_USAGE;
            }
        } else {
            if (opt->input) {
                ERR("unexpected argument: %s\n", arg);
                return EXIT_USAGE;
            }
            opt->input = arg;
            continue;
        }

        if (def->has_arg && !arg_taken) {
            if (i + 1 >= argc) {
                ERR("option %s requires a value\n", arg);
                return EXIT_USAGE;
            }
            val = argv[++i];
        }

        switch (def->id) {
        case OPT_HELP:
            usage(stdout);
            return 2;
        case OPT_LIST:
            opt->list_mode = 1;
            break;
        case OPT_FROM:
            if (parse_index_val(val, &opt->from) != 0) {
                ERR("invalid --from-frame value: %s\n", val);
                return EXIT_USAGE;
            }
            opt->have_from = 1;
            break;
        case OPT_TO:
            if (parse_index_val(val, &opt->to) != 0) {
                ERR("invalid --to-frame value: %s\n", val);
                return EXIT_USAGE;
            }
            opt->have_to = 1;
            break;
        case OPT_EXACT:
            if (parse_range(val, &opt->t0, &opt->t1) != 0) {
                ERR("invalid --exact range: %s"
                    " (expected HH:MM:SS.mmm-HH:MM:SS.mmm)\n", val);
                return EXIT_USAGE;
            }
            opt->exact_mode = 1;
            break;
        case OPT_FPS: {
            char *end;
            long double fps = strtold(val, &end);

            if (*val == '\0' || *end != '\0' || !(fps > 0.0L)) {
                ERR("invalid --fps value: %s\n", val);
                return EXIT_USAGE;
            }
            opt->fps = fps;
            opt->have_fps = 1;
            break;
        }
        case OPT_OUTPUT:
            opt->output = val;
            break;
        default:
            return EXIT_USAGE;
        }
    }

    if (!opt->input) {
        ERR("no input file specified\n");
        return EXIT_USAGE;
    }
    if (opt->list_mode) {
        if (opt->have_from || opt->have_to || opt->exact_mode || opt->output) {
            ERR("--list is not compatible with -f/-t/-e/-o\n");
            return EXIT_USAGE;
        }
    } else {
        if (!opt->output) {
            ERR("--output is required for cutting\n");
            return EXIT_USAGE;
        }
        if (opt->exact_mode && (opt->have_from || opt->have_to)) {
            ERR("--exact is not compatible with -f/-t\n");
            return EXIT_USAGE;
        }
    }
    return EXIT_OK;
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

/* VUI-derived constant fps following the --exact time-base priority
 * (fixed flag first, then SEI clock ts, then VUI without the flag);
 * returns 0 when an fps value was produced. */
static int time_base_fps(const stream_index_t *idx, long double *fps)
{
    if (idx->timing_valid && idx->num_units_in_tick > 0) {
        if (idx->fixed_frame_rate || !idx->clock_timing) {
            *fps = (long double)idx->time_scale
                 / (2.0L * (long double)idx->num_units_in_tick);
            if (*fps > 0.0L) {
                return 0;
            }
        }
    }
    return -1;
}

/* Display time of a frame, seconds as "%.3f", "-" when no time base. */
static void frame_time_str(const stream_index_t *idx, const frame_t *f,
                           char *out, size_t n)
{
    long double fps;

    if (time_base_fps(idx, &fps) == 0 && f->display_index >= 0) {
        snprintf(out, n, "%.3Lf", (long double)f->display_index / fps);
        return;
    }
    if (idx->clock_timing) {
        snprintf(out, n, "%.3Lf",
                 (long double)f->clock_ts_90k / 90000.0L);
        return;
    }
    strcpy(out, "-");
}

static int do_list(const stream_index_t *idx)
{
    size_t i;

    fputs("# dec_id disp_id type idr frame_num poc   t        "
          "nal_range   byte_range\n",
          stdout);
    for (i = 0; i < idx->frame_count; i++) {
        const frame_t *f = &idx->frames[i];
        char disp[32], poc[32], nalr[64], tstr[32], type[8];
        size_t byte_begin = idx->nals.items[f->nal_first].off;
        size_t byte_end = idx->nals.items[f->nal_last].off
                        + idx->nals.items[f->nal_last].size;

        if (f->display_index < 0)
            strcpy(disp, "-");
        else
            snprintf(disp, sizeof(disp), "%lld", f->display_index);
        if (!idx->poc_supported || f->parse_error)
            strcpy(poc, "-");
        else
            snprintf(poc, sizeof(poc), "%lld", f->poc);
        snprintf(nalr, sizeof(nalr), "%zu-%zu", f->nal_first, f->nal_last);
        frame_time_str(idx, f, tstr, sizeof(tstr));
        snprintf(type, sizeof(type), "%c%s%s", f->pic_type,
                 f->leading_picture ? "L" : "",
                 f->recovery_point ? "R" : "");

        printf("%-6zu %-7s %-4s %-3d %-9u %-6s %-8s %-12s %zu-%zu\n",
               i, disp, type, f->is_idr ? 1 : 0,
               (unsigned)f->frame_num, poc, tstr, nalr, byte_begin, byte_end);
    }
    return EXIT_OK;
}

/* Largest i <= a0 usable as a cut anchor (IDR or recovery point).
 * 0 on success, -1 if none. */
static int find_anchor_left(const stream_index_t *idx, size_t a0, size_t *out)
{
    size_t i = a0;

    for (;;) {
        if (idx->frames[i].usable_anchor) {
            *out = i;
            return 0;
        }
        if (i == 0)
            return -1;
        i--;
    }
}

/* Last NAL of the given type strictly before index `before`. 0 on success. */
static int find_last_nal_type(const stream_index_t *idx, uint8_t type,
                              size_t before, size_t *out)
{
    size_t i = before;

    while (i > 0) {
        i--;
        if (idx->nals.items[i].type == type) {
            *out = i;
            return 0;
        }
    }
    return -1;
}

/* Compute the kept decoding-order index range [a0, b0]. Returns 0, 2 or 3. */
static int cut_bounds(const stream_index_t *idx, const opts_t *opt,
                      size_t *a0, size_t *b0)
{
    if (opt->exact_mode) {
        long double fps = 0.0L;
        int clock_base = 0;
        long long d_min, d_max;
        size_t i;
        size_t lo = 0, hi = 0;
        int found = 0;

        if (!idx->poc_supported) {
            ERR("display order unavailable"
                " (unsupported pic_order_cnt_type?)\n");
            return EXIT_STREAM;
        }
        for (i = 0; i < idx->frame_count; i++) {
            if (idx->frames[i].display_index < 0) {
                ERR("display order unavailable"
                    " (unsupported pic_order_cnt_type?)\n");
                return EXIT_STREAM;
            }
        }
        /* Time base priority (see time_base_fps): --fps > fixed-fps VUI >
         * SEI clock ts > VUI timing without the fixed flag (x264 leaves
         * the flag at 0). */
        if (opt->have_fps) {
            fps = opt->fps;
        } else if (time_base_fps(idx, &fps) == 0) {
            /* VUI constant fps. */
        } else if (idx->clock_timing) {
            clock_base = 1;
            ERR("внимание: display-время взято из SEI picture timing (VFR)\n");
        }
        if (!clock_base && !(fps > 0.0L)) {
            ERR("no time base (no fixed-fps VUI timing, no SEI clock ts);"
                " specify --fps\n");
            return EXIT_STREAM;
        }

        if (clock_base) {
            /* Frame d occupies [ts(d), ts(d) + dur(d)) in seconds, dur from
             * the next display-order frame (the last one repeats the
             * previous duration); kept on any intersection with [t0, t1]. */
            size_t n = idx->frame_count;
            size_t *pos = malloc(n * sizeof(*pos));
            long double prev_dur = 0.0L;
            long double d;

            if (!pos) {
                ERR("out of memory\n");
                return EXIT_STREAM;
            }
            for (i = 0; i < n; i++) {
                pos[idx->frames[i].display_index] = i;
            }
            for (d = 0; d < (long double)n; d++) {
                const frame_t *fr = &idx->frames[pos[(size_t)d]];
                long double start = (long double)fr->clock_ts_90k / 90000.0L;
                long double dur = 0.0L;

                if ((size_t)d + 1 < n) {
                    uint64_t nt = idx->frames[pos[(size_t)d + 1]].clock_ts_90k;

                    if (nt > fr->clock_ts_90k) {
                        dur = (long double)(nt - fr->clock_ts_90k) / 90000.0L;
                    }
                } else {
                    dur = prev_dur;
                }
                if (start < opt->t1 + 1e-9L &&
                        start + dur > opt->t0 - 1e-9L) {
                    size_t fi = pos[(size_t)d];

                    if (!found) {
                        lo = hi = fi;
                        found = 1;
                    } else {
                        if (fi < lo)
                            lo = fi;
                        if (fi > hi)
                            hi = fi;
                    }
                }
                prev_dur = dur;
            }
            free(pos);
        } else {
            /* Frame d (display_index) is kept iff d/fps < t1 && (d+1)/fps > t0,
             * i.e. d in [floor(t0*fps), ceil(t1*fps) - 1]. The epsilons keep
             * exact frame boundaries inclusive despite fp rounding. */
            d_min = ld_floor_ll(opt->t0 * fps + 1e-9L);
            d_max = ld_ceil_ll(opt->t1 * fps - 1e-9L) - 1;
            if (d_min < 0)
                d_min = 0;
            for (i = 0; i < idx->frame_count; i++) {
                long long disp = idx->frames[i].display_index;

                if (disp >= d_min && disp <= d_max) {
                    if (!found) {
                        lo = hi = i;
                        found = 1;
                    } else {
                        if (i < lo)
                            lo = i;
                        if (i > hi)
                            hi = i;
                    }
                }
            }
        }
        if (!found) {
            ERR("requested range is empty\n");
            return EXIT_RANGE;
        }
        *a0 = lo;
        *b0 = hi;
        return EXIT_OK;
    }

    {
        long long a = opt->have_from ? opt->from : 0;
        long long b = opt->have_to ? opt->to
                                   : (long long)idx->frame_count - 1;

        if (a > b) {
            ERR("from-frame (%lld) is greater than to-frame (%lld)\n", a, b);
            return EXIT_RANGE;
        }
        if (a >= (long long)idx->frame_count) {
            ERR("from-frame (%lld) is beyond the end of the stream (%zu frames)\n",
                a, idx->frame_count);
            return EXIT_RANGE;
        }
        if (b >= (long long)idx->frame_count)
            b = (long long)idx->frame_count - 1;
        *a0 = (size_t)a;
        *b0 = (size_t)b;
    }
    return EXIT_OK;
}

static int do_cut(const stream_index_t *idx, const uint8_t *buf,
                  const opts_t *opt)
{
    size_t a0 = 0, b0 = 0, a, b, n;
    size_t sps_at = 0, pps_at = 0;
    int have_sps = 0, have_pps = 0, write_sps = 0, write_pps = 0;
    FILE *out;
    int rc;

    rc = cut_bounds(idx, opt, &a0, &b0);
    if (rc != EXIT_OK)
        return rc;

    /*
     * Snap the left boundary back to the nearest usable anchor at or
     * before a0 (closed GOP: an IDR; open GOP: a recovery point with
     * recovery_frame_cnt == 0 && exact_match && !broken_link).
     * Justification: in decoding order a picture may only reference
     * pictures that PRECEDE it in decoding order, so the segment
     * [anchor .. b] is self-contained and no right shift is ever needed.
     */
    if (find_anchor_left(idx, a0, &a) != 0) {
        ERR("no cut anchor (IDR or recovery point) at or before frame %llu\n",
            (unsigned long long)a0);
        return EXIT_RANGE;
    }
    if (a != a0) {
        if (idx->frames[a].recovery_point) {
            fprintf(stderr, PROG ": левая граница сдвинута к recovery point"
                    " (кадр %llu)\n", (unsigned long long)a);
        } else {
            fprintf(stderr, PROG ": note: left boundary moved from frame %llu"
                    " to IDR frame %llu\n",
                    (unsigned long long)a0, (unsigned long long)a);
        }
    }
    b = b0;

    /* Leading pictures of open GOPs inside the copied range keep
     * references that lie before their recovery anchor: in copy mode they
     * decode with broken dependencies. */
    {
        size_t k;
        size_t nlead = 0;

        for (k = a; k <= b; k++) {
            if (idx->frames[k].leading_picture) {
                nlead++;
            }
        }
        if (nlead > 0) {
            ERR("внимание: %zu leading-кадров открытых GOP внутри диапазона"
                " переносятся с битыми зависимостями"
                " (перекодирование — --smart, этап R)\n", nlead);
        }
    }

    /*
     * The cut must open with parameter sets valid for frame a. The index
     * attributes parametric NALs to the following frame, so it is enough
     * to look inside the first frame's NAL range (its AU header comes
     * before its slices).
     */
    for (n = idx->frames[a].nal_first; n <= idx->frames[a].nal_last; n++) {
        if (idx->nals.items[n].type == NAL_SPS)
            have_sps = 1;
        else if (idx->nals.items[n].type == NAL_PPS)
            have_pps = 1;
    }
    if (!have_sps) {
        if (find_last_nal_type(idx, NAL_SPS,
                               idx->frames[a].nal_first, &sps_at) != 0) {
            ERR("no SPS available\n");
            return EXIT_STREAM;
        }
        write_sps = 1;
    }
    if (!have_pps) {
        if (find_last_nal_type(idx, NAL_PPS,
                               idx->frames[a].nal_first, &pps_at) != 0) {
            ERR("no PPS available\n");
            return EXIT_STREAM;
        }
        write_pps = 1;
    }

    out = fopen(opt->output, "wb");
    if (!out) {
        ERR("cannot open output file: %s\n", opt->output);
        return EXIT_IO;
    }
    if (write_sps
            && annexb_write_nal(out, buf, &idx->nals.items[sps_at]) != 0) {
        ERR("write error: %s\n", opt->output);
        fclose(out);
        return EXIT_IO;
    }
    if (write_pps
            && annexb_write_nal(out, buf, &idx->nals.items[pps_at]) != 0) {
        ERR("write error: %s\n", opt->output);
        fclose(out);
        return EXIT_IO;
    }
    for (n = idx->frames[a].nal_first; n <= idx->frames[b].nal_last; n++) {
        if (annexb_write_nal(out, buf, &idx->nals.items[n]) != 0) {
            ERR("write error: %s\n", opt->output);
            fclose(out);
            return EXIT_IO;
        }
    }
    if (fclose(out) != 0) {
        ERR("write error: %s\n", opt->output);
        return EXIT_IO;
    }
    return EXIT_OK;
}

int main(int argc, char **argv)
{
    opts_t opt;
    stream_index_t idx;
    uint8_t *buf = NULL;
    size_t size = 0;
    int idx_built = 0;
    int pr;
    int rc = EXIT_USAGE;

    memset(&opt, 0, sizeof(opt));
    memset(&idx, 0, sizeof(idx));

    pr = parse_args(argc, argv, &opt);
    if (pr == 2)
        return EXIT_OK;
    if (pr != EXIT_OK)
        return EXIT_USAGE;

    buf = read_whole_file(opt.input, &size);
    if (!buf) {
        ERR("cannot read input file: %s\n", opt.input);
        rc = EXIT_STREAM;
        goto out;
    }
    if (index_build(&idx, buf, size) != 0) {
        ERR("cannot parse stream: %s\n", opt.input);
        rc = EXIT_STREAM;
        goto out;
    }
    idx_built = 1;
    if (idx.frame_count == 0) {
        ERR("no frames\n");
        rc = EXIT_STREAM;
        goto out;
    }

    if (opt.list_mode)
        rc = do_list(&idx);
    else
        rc = do_cut(&idx, buf, &opt);

out:
    if (idx_built)
        index_free(&idx);
    free(buf);
    return rc;
}
