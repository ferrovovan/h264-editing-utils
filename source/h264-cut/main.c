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
#include "params.h"
#include "transcode.h"

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
    int json_mode;
    int exact_mode;
    int have_from;
    int have_to;
    long long from;
    long long to;
    long double t0;
    long double t1;
    int have_fps;
    long double fps;
    int smart_mode;
    int crf;
    const char *output;
    const char *input;
} opts_t;

static void usage(FILE *out)
{
    fputs("usage: h264-cut [опции] INPUT.264\n"
          "  -l, --list            список кадров; с -f/-t/-e (опц. --smart)"
          " — ПЛАН вырезки\n"
          "                        без записи файла; с -o — ошибка\n"
          "                        колонка t — display-время (сек),"
          " суффиксы типа: L — leading\n"
          "                        picture открытой GOP, R — recovery point\n"
          "      --json            только вместе с --list:"
          " машинночитаемый JSON-объект\n"
          "                        в stdout (текста на русском в"
          " stdout нет); отдельно или с\n"
          "                        вырезанием — ошибка, код 1\n"
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
           "      --smart           минимальное перекодирование стартового"
           " хвоста GOP при старте\n"
           "                        из середины GOP (вместо сдвига границы"
           " назад на IDR; требует\n"
           "                        известную частоту кадров: --fps или VUI"
           " timing)\n"
           "      --crf N           качество x264 для --smart (0..51, по"
           " умолчанию 18)\n"
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
    OPT_LIST, OPT_JSON, OPT_FROM, OPT_TO, OPT_EXACT, OPT_FPS, OPT_SMART,
    OPT_CRF, OPT_OUTPUT, OPT_HELP
};

static const struct opt_def long_opts[] = {
    { "list",       0, OPT_LIST },
    { "json",       0, OPT_JSON },
    { "from-frame", 1, OPT_FROM },
    { "to-frame",   1, OPT_TO },
    { "exact",      1, OPT_EXACT },
    { "fps",        1, OPT_FPS },
    { "smart",      0, OPT_SMART },
    { "crf",        1, OPT_CRF },
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
        case OPT_JSON:
            opt->json_mode = 1;
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
        case OPT_SMART:
            opt->smart_mode = 1;
            opt->crf = 18;
            break;
        case OPT_CRF: {
            char *end;
            long v = strtol(val, &end, 10);

            if (*val == '\0' || *end != '\0' || v < 0 || v > 51) {
                ERR("invalid --crf value: %s (expected 0..51)\n", val);
                return EXIT_USAGE;
            }
            opt->crf = (int)v;
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
    if (opt->json_mode && !opt->list_mode) {
        ERR("--json используется только вместе с --list"
            " (при вырезании JSON недоступен)\n");
        return EXIT_USAGE;
    }
    if (opt->list_mode) {
        /* -l alone lists frames; -l together with -f/-t/-e (optionally
         * --smart) is a DRY-RUN: print the cut plan, write nothing. */
        if (opt->output) {
            ERR("--list ничего не пишет: -o с --list несовместим\n");
            return EXIT_USAGE;
        }
        if (opt->smart_mode && !opt->have_from && !opt->have_to
                && !opt->exact_mode) {
            ERR("--smart вместе с --list требует диапазона (-f/-t/-e)\n");
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

static int plan_mode(const opts_t *opt)
{
    return opt->list_mode
        && (opt->have_from || opt->have_to || opt->exact_mode);
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

/*
 * --list --json: one JSON object on stdout, machine-readable. The same
 * numbers as the text listing: byte_first/byte_last are the byte_range
 * bounds (start of the first NAL .. end of the last one), t duplicates
 * the t column ("- " becomes null), nulls mark unknowns (disp_id, poc).
 * No Russian text goes to stdout; warnings stay on stderr as before.
 */
static int do_list_json(const stream_index_t *idx)
{
    size_t i;
    long double fps;
    int have_fps;

    have_fps = time_base_fps(idx, &fps) == 0;
    printf("{\"stream\":{\"frame_count\":%zu,\"poc_supported\":%s,"
           "\"has_fields\":%s,\"timing\":{\"fps\":",
           idx->frame_count,
           idx->poc_supported ? "true" : "false",
           idx->has_fields ? "true" : "false");
    if (have_fps)
        printf("%.9Lg", fps);
    else
        fputs("null", stdout);
    printf(",\"fixed\":%s,\"clock_timing\":%s}},\"frames\":[",
           idx->fixed_frame_rate ? "true" : "false",
           idx->clock_timing ? "true" : "false");
    for (i = 0; i < idx->frame_count; i++) {
        const frame_t *f = &idx->frames[i];
        char tstr[32];
        char disp[32];
        char poc[32];
        char ts[32];
        size_t byte_begin = idx->nals.items[f->nal_first].off;
        size_t byte_end = idx->nals.items[f->nal_last].off
                        + idx->nals.items[f->nal_last].size;

        frame_time_str(idx, f, tstr, sizeof(tstr));
        if (f->display_index < 0)
            strcpy(disp, "null");
        else
            snprintf(disp, sizeof(disp), "%lld", f->display_index);
        if (!idx->poc_supported || f->parse_error)
            strcpy(poc, "null");
        else
            snprintf(poc, sizeof(poc), "%lld", f->poc);
        if (f->has_clock_ts)
            snprintf(ts, sizeof(ts), "%llu",
                     (unsigned long long)f->clock_ts_90k);
        else
            strcpy(ts, "null");

        printf("%s{\"dec_id\":%zu,\"disp_id\":%s,\"type\":\"%c\","
               "\"idr\":%s,\"frame_num\":%u,\"poc\":%s,\"t\":%s,"
               "\"leading\":%s,\"recovery\":%s,"
               "\"recovery_frame_cnt\":%u,\"clock_ts_90k\":%s,"
               "\"nal_first\":%zu,\"nal_last\":%zu,"
               "\"byte_first\":%zu,\"byte_last\":%zu}",
               i ? "," : "",
               i, disp, f->pic_type,
               f->is_idr ? "true" : "false",
               (unsigned)f->frame_num, poc,
               strcmp(tstr, "-") == 0 ? "null" : tstr,
               f->leading_picture ? "true" : "false",
               f->recovery_point ? "true" : "false",
               (unsigned)f->recovery_frame_cnt, ts,
               f->nal_first, f->nal_last, byte_begin, byte_end);
    }
    fputs("]}\n", stdout);
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

/* Extra facts about an --exact selection, for the dry-run plan report. */
typedef struct {
    int active;        /* selection was --exact */
    int clock_base;    /* times came from SEI picture timing (VFR) */
    long long d_min, d_max;   /* display ranks kept (fps path) */
    long double fps;   /* time base used (0 when clock_base) */
} exact_meta_t;

static int cut_bounds_ex(const stream_index_t *idx, const opts_t *opt,
                         size_t *a0, size_t *b0, exact_meta_t *em);

/* Compute the kept decoding-order index range [a0, b0]. Returns 0, 2 or 3. */
static int cut_bounds(const stream_index_t *idx, const opts_t *opt,
                      size_t *a0, size_t *b0)
{
    return cut_bounds_ex(idx, opt, a0, b0, NULL);
}

static int cut_bounds_ex(const stream_index_t *idx, const opts_t *opt,
                         size_t *a0, size_t *b0, exact_meta_t *em)
{
    if (em)
        memset(em, 0, sizeof(*em));
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
        if (em) {
            em->active = 1;
            em->clock_base = clock_base;
            em->fps = clock_base ? 0.0L : fps;
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
            if (em) {
                em->d_min = d_min;
                em->d_max = d_max;
                em->fps = fps;
            }
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

/* Smallest i > a0 usable as a cut anchor; 0 on success, -1 if none. */
static int find_anchor_right(const stream_index_t *idx, size_t a0, size_t *out)
{
    size_t i;

    for (i = a0 + 1; i < idx->frame_count; i++) {
        if (idx->frames[i].usable_anchor) {
            *out = i;
            return 0;
        }
    }
    return -1;
}

/* a/b reduced by their gcd; a and b nonzero. */
static void reduce_ratio(uint32_t a, uint32_t b, uint32_t *na, uint32_t *nb)
{
    uint32_t x = a, y = b, t;

    while (y != 0) {
        t = x % y;
        x = y;
        y = t;
    }
    *na = a / x;
    *nb = b / x;
}

/* floor(x * scale) computed exactly on the integer path (values are small
 * here: fps * 1000), %Lg long double printf for the fallback. */
static int fps_milli(long double fps, uint32_t *out)
{
    long double scaled = fps * 1000.0L;
    long long n;
    char buf[64];

    snprintf(buf, sizeof(buf), "%.9Lg", scaled);
    if (sscanf(buf, "%lld", &n) == 1 && n > 0 && n <= 1000000000LL) {
        /* The scaled value of any sane --fps is integer at this precision. */
        *out = (uint32_t)n;
        return 0;
    }
    return -1;
}

/*
 * The active SPS of a frame: parameter sets are attributed to the frame they
 * precede (trapping 5), so a set inside the frame's own AU wins, else the
 * last SPS before its first NAL. 0 on success, -1 if no valid state.
 */
static int frame_sps_state(const stream_index_t *idx, size_t fr,
                           sps_state_t *out)
{
    const frame_t *f = &idx->frames[fr];
    size_t n;

    for (n = f->nal_last + 1; n-- > f->nal_first;) {
        const nal_t *nal = &idx->nals.items[n];
        int id;

        if (nal->type != NAL_SPS && nal->type != NAL_SUBSET_SPS)
            continue;
        if (nal->size < 2)
            continue;
        id = idx->buf[nal->off + 1] & 0x1f;
        if (idx->params.sps[id].valid) {
            *out = idx->params.sps[id];
            return 0;
        }
        break;
    }
    {
        size_t at = 0;

        if (find_last_nal_type(idx, NAL_SPS, f->nal_first, &at) != 0)
            return -1;
        if (idx->nals.items[at].size < 2)
            return -1;
        {
            int id = idx->buf[idx->nals.items[at].off + 1] & 0x1f;

            if (!idx->params.sps[id].valid)
                return -1;
            *out = idx->params.sps[id];
        }
    }
    return 0;
}

static int sps_limit_err(const sps_state_t *s, char *msg, size_t cap)
{
    if (msg[0] != '\0')
        return 1;
    if (!s->frame_mbs_only_flag) {
        snprintf(msg, cap, "чересстрочный контент (frame_mbs_only_flag = 0)"
                 " не поддерживается --smart");
        return 1;
    }
    if (s->bit_depth_luma_minus8 > 0 || s->bit_depth_chroma_minus8 > 0) {
        snprintf(msg, cap, "битность больше 8 бит (luma %d, chroma %d)"
                 " не поддерживается --smart",
                 8 + s->bit_depth_luma_minus8, 8 + s->bit_depth_chroma_minus8);
        return 1;
    }
    if (s->chroma_format_idc != 1) {
        snprintf(msg, cap, "не 4:2:0 (chroma_format_idc = %d)"
                 " не поддерживается --smart", s->chroma_format_idc);
        return 1;
    }
    return 0;
}

/*
 * v1 limit checks of the plan (PLAN.md rule 6) before transcoding.  The
 * active SPS of every frame in [feed_first, enc_last] is checked (param
 * sets are attributed to the frame they precede, so re-checking per frame
 * follows mid-stream SPS switches); the first violation reported wins.
 */
static int smart_check_limits(const stream_index_t *idx,
                              size_t feed_first, size_t enc_last)
{
    char msg[256];
    size_t k;

    msg[0] = '\0';

    if (idx->has_fields) {
        snprintf(msg, sizeof(msg), "чересстрочный контент (поля в потоке)"
                 " не поддерживается --smart");
    } else {
        for (k = feed_first; k <= enc_last && msg[0] == '\0'; k++) {
            sps_state_t s;

            if (frame_sps_state(idx, k, &s) != 0)
                continue; /* transcode reports a missing SPS itself */
            sps_limit_err(&s, msg, sizeof(msg));
        }
    }
    if (msg[0] == '\0') {
        for (k = feed_first; k <= enc_last; k++) {
            if (idx->frames[k].parse_error) {
                snprintf(msg, sizeof(msg), "кадр %zu не разобран"
                         " (ошибка разбора slice header) — --smart невозможен",
                         k);
                break;
            }
        }
    }
    if (msg[0] != '\0') {
        ERR("%s\n", msg);
        return EXIT_STREAM;
    }
    return EXIT_OK;
}

/*
 * Append source SPS/PPS valid for frame `fr` to `out` unless its own AU
 * already carries them (they get normalized to 4-byte start codes like
 * everything else). Same rule as the copy mode: look inside the frame's
 * NAL range first, then the last set before it.
 */
static int write_param_sets(FILE *out, const stream_index_t *idx,
                            const uint8_t *buf, const char *outpath,
                            size_t fr)
{
    size_t n, sps_at = 0, pps_at = 0;
    int have_sps = 0, have_pps = 0;

    for (n = idx->frames[fr].nal_first; n <= idx->frames[fr].nal_last; n++) {
        if (idx->nals.items[n].type == NAL_SPS)
            have_sps = 1;
        else if (idx->nals.items[n].type == NAL_PPS)
            have_pps = 1;
    }
    if (!have_sps) {
        if (find_last_nal_type(idx, NAL_SPS,
                               idx->frames[fr].nal_first, &sps_at) != 0) {
            ERR("no SPS available\n");
            return EXIT_STREAM;
        }
        if (annexb_write_nal(out, buf, &idx->nals.items[sps_at]) != 0) {
            ERR("write error: %s\n", outpath);
            return EXIT_IO;
        }
    }
    if (!have_pps) {
        if (find_last_nal_type(idx, NAL_PPS,
                               idx->frames[fr].nal_first, &pps_at) != 0) {
            ERR("no PPS available\n");
            return EXIT_STREAM;
        }
        if (annexb_write_nal(out, buf, &idx->nals.items[pps_at]) != 0) {
            ERR("write error: %s\n", outpath);
            return EXIT_IO;
        }
    }
    return EXIT_OK;
}

/* Forward: the smart planner needs the pure-copy fast path and bounds. */
static int do_cut_copy(const stream_index_t *idx, const uint8_t *buf,
                       const opts_t *opt, size_t a0, size_t b0);

/*
 * --smart: minimal re-encoding of the start-of-GOP tail (PLAN.md rules
 * 1-6). copy_bounds is the already computed [a0, b0]; unlike copy mode the
 * left boundary is NOT snapped: frames [a0 .. next_anchor-1] are
 * re-encoded (decoder warm-up from the anchor feed_first <= a0), the rest
 * of the range is copied byte-for-byte.
 */
/*
 * The --smart segmentation, shared by the executor and the dry-run plan:
 * decode-order window [a0..b0] splits into re-encode [a0..enc_last] and
 * byte-copy [copy_first..b0] (copy_first > b0 means "encode to the end"),
 * with the decoder warmed from feed_first. pure_copy implements rule 5.
 * Returns 0, or -1 when no anchor exists at or before a0.
 */
static int smart_plan(const stream_index_t *idx, size_t a0, size_t b0,
                      size_t *feed_first, size_t *enc_last,
                      size_t *copy_first, int *pure_copy)
{
    size_t n, next_anchor = 0;

    *pure_copy = 0;
    if (find_anchor_left(idx, a0, feed_first) != 0)
        return -1;
    /*
     * A decoder session started at a recovery point drops the leading
     * picture right after it (FFmpeg >= 7 behaviour) and that picture may
     * reference pre-recovery content anyway: warm up from the last IDR at
     * or before a0 when one exists (PLAN rule 4's "previous anchor").
     */
    if (!idx->frames[*feed_first].is_idr) {
        for (n = a0; ; n--) {
            if (idx->frames[n].is_idr) {
                *feed_first = n;
                break;
            }
            if (n == 0)
                break;
        }
    }
    if (find_anchor_right(idx, a0, &next_anchor) != 0)
        next_anchor = idx->frame_count;

    /* Rule 5: a0 is an anchor and the range holds no leading picture up to
     * the next anchor (or the end) - re-encoding would be a lossy no-op,
     * the output must be byte-for-byte the copy-mode one. */
    {
        size_t pure_last = next_anchor <= b0 ? next_anchor - 1 : b0;
        int need_enc = !idx->frames[a0].usable_anchor;

        for (n = a0; !need_enc && n <= pure_last; n++) {
            if (idx->frames[n].leading_picture)
                need_enc = 1;
        }
        if (!need_enc) {
            *pure_copy = 1;
            return 0;
        }
    }

    *enc_last = (next_anchor <= b0) ? next_anchor - 1 : b0;
    *copy_first = next_anchor;

    /*
     * Open GOP: a copy tail may not start at a recovery anchor - the
     * leading pictures after it reference pictures BEFORE that anchor,
     * which the re-encoded fragment supplies under different frame_num/POC
     * (a byte-copy splice breaks them). Extend the re-encode over every
     * such GOP until the copy tail opens on a plain IDR or the range ends.
     */
    while (*copy_first <= b0 && idx->frames[*copy_first].recovery_point) {
        size_t na;

        if (find_anchor_right(idx, *copy_first, &na) != 0)
            na = idx->frame_count;
        *enc_last = (na <= b0) ? na - 1 : b0;
        *copy_first = na;
    }
    return 0;
}

static int do_cut_smart(const stream_index_t *idx, const uint8_t *buf,
                        const opts_t *opt, size_t a0, size_t b0)
{
    size_t feed_first = 0, enc_first, enc_last = 0, copy_first = 0, n;
    int pure_copy = 0;
    uint32_t fps_num = 0, fps_den = 0;
    long double fps;
    char terr[256];
    uint8_t *tbuf = NULL;
    size_t tsize = 0;
    nal_list_t tnals;
    FILE *out;
    int rc;

    tnals.items = NULL;
    tnals.count = 0;

    if (smart_plan(idx, a0, b0, &feed_first, &enc_last, &copy_first,
                   &pure_copy) != 0) {
        ERR("no cut anchor (IDR or recovery point) at or before frame %llu\n",
            (unsigned long long)a0);
        return EXIT_RANGE;
    }
    if (pure_copy)
        return do_cut_copy(idx, buf, opt, a0, b0);

    enc_first = a0;

    rc = smart_check_limits(idx, feed_first, enc_last);
    if (rc != EXIT_OK)
        return rc;

    /* fps: --fps wins, else VUI timing (same base as --exact). */
    if (opt->have_fps) {
        if (fps_milli(opt->fps, &fps_num) != 0) {
            ERR("некорректная --fps для --smart\n");
            return EXIT_STREAM;
        }
        fps_den = 1000;
        reduce_ratio(fps_num, fps_den, &fps_num, &fps_den);
    } else if (time_base_fps(idx, &fps) == 0 && fps > 0.0L
            && idx->num_units_in_tick > 0 && idx->time_scale > 0
            && idx->num_units_in_tick <= 0x7FFFFFFFu) {
        /* fps = time_scale / (2 * num_units_in_tick), exactly. */
        reduce_ratio(idx->time_scale, 2u * idx->num_units_in_tick,
                     &fps_num, &fps_den);
    } else {
        ERR("--smart требует известную частоту кадров:"
            " укажите --fps или VUI timing\n");
        return EXIT_STREAM;
    }

    if (idx->frames[feed_first].recovery_point) {
        fprintf(stderr, PROG ": греется от recovery point (кадр %llu)\n",
                (unsigned long long)feed_first);
    }

    terr[0] = '\0';
    rc = transcode_range(buf, idx, feed_first, enc_first, enc_last, opt->crf,
                         (uint32_t)fps_num, (uint32_t)fps_den,
                         &tbuf, &tsize, &tnals, terr, sizeof(terr));
    if (rc != TRANSCODE_OK) {
        if (terr[0] != '\0')
            ERR("%s\n", terr);
        else
            ERR("перекодирование не удалось\n");
        return EXIT_STREAM;
    }

    if (copy_first <= b0) {
        fprintf(stderr, PROG ": перекодируем кадры [%zu..%zu]"
                " (прогрев от кадра %zu), копируем [%zu..%zu]\n",
                enc_first, enc_last, feed_first, copy_first, b0);
    } else {
        fprintf(stderr, PROG ": перекодируем кадры [%zu..%zu]"
                " (прогрев от кадра %zu)\n",
                enc_first, enc_last, feed_first);
    }

    out = fopen(opt->output, "wb");
    if (!out) {
        ERR("cannot open output file: %s\n", opt->output);
        free(tbuf);
        annexb_list_free(&tnals);
        return EXIT_IO;
    }
    for (n = 0; n < tnals.count; n++) {
        if (annexb_write_nal(out, tbuf, &tnals.items[n]) != 0) {
            ERR("write error: %s\n", opt->output);
            break;
        }
    }
    if (n < tnals.count) {
        rc = EXIT_IO;
    } else if (copy_first <= b0) {
        rc = write_param_sets(out, idx, buf, opt->output, copy_first);
        if (rc == EXIT_OK) {
            size_t nf = idx->frames[copy_first].nal_first;
            size_t nl = idx->frames[b0].nal_last;

            for (n = nf; n <= nl; n++) {
                if (annexb_write_nal(out, buf, &idx->nals.items[n]) != 0) {
                    ERR("write error: %s\n", opt->output);
                    rc = EXIT_IO;
                    break;
                }
            }
        }
    }
    if (fclose(out) != 0 && rc == EXIT_OK) {
        ERR("write error: %s\n", opt->output);
        rc = EXIT_IO;
    }
    free(tbuf);
    annexb_list_free(&tnals);
    return rc;
}

/* Copy mode body for the already resolved [a0, b0] (shared with --smart's
 * pure-copy fast path). */
static int do_cut_copy(const stream_index_t *idx, const uint8_t *buf,
                       const opts_t *opt, size_t a0, size_t b0)
{
    size_t a, b, n;
    size_t sps_at = 0, pps_at = 0;
    int have_sps = 0, have_pps = 0, write_sps = 0, write_pps = 0;
    FILE *out;

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

/* ------------------------------------------------------------------------
 * Dry-run plan (--list combined with -f/-t/-e): explains the boundary
 * decisions without writing anything. The numbers come from the SAME
 * functions the real cut uses (cut_bounds_ex/find_anchor_left/smart_plan),
 * so the plan cannot drift from the action.
 * ---------------------------------------------------------------------- */

enum { TB_NONE = 0, TB_FPS, TB_VUI, TB_CLOCK };

static int plan_time_base(const stream_index_t *idx, const opts_t *opt,
                          long double *fps_out)
{
    long double fps = 0.0L;

    *fps_out = 0.0L;
    if (opt->have_fps) {
        *fps_out = opt->fps;
        return TB_FPS;
    }
    if (time_base_fps(idx, &fps) == 0 && fps > 0.0L) {
        *fps_out = fps;
        return TB_VUI;
    }
    if (idx->clock_timing)
        return TB_CLOCK;
    return TB_NONE;
}

typedef struct {
    size_t a0, b0;                /* decode-order box */
    exact_meta_t em;
    int copy_ok;
    size_t copy_start;
    size_t n_leading;
    int smart_wanted;
    int smart_ok;
    int smart_pure;
    size_t feed_first, enc_last, copy_first;
} plan_t;

static int plan_compute(const stream_index_t *idx, const opts_t *opt,
                        plan_t *pl)
{
    size_t k;
    int rc;

    memset(pl, 0, sizeof(*pl));
    rc = cut_bounds_ex(idx, opt, &pl->a0, &pl->b0, &pl->em);
    if (rc != EXIT_OK)
        return rc;
    pl->copy_ok = find_anchor_left(idx, pl->a0, &pl->copy_start) == 0;
    for (k = pl->copy_start; pl->copy_ok && k <= pl->b0; k++) {
        if (idx->frames[k].leading_picture)
            pl->n_leading++;
    }
    pl->smart_wanted = opt->smart_mode;
    if (pl->smart_wanted && transcode_available()) {
        if (smart_plan(idx, pl->a0, pl->b0, &pl->feed_first, &pl->enc_last,
                       &pl->copy_first, &pl->smart_pure) == 0) {
            /* smart_check_limits prints the reason to stderr itself. */
            pl->smart_ok = smart_check_limits(idx, pl->feed_first,
                                              pl->enc_last) == EXIT_OK;
        }
    }
    return EXIT_OK;
}

/* Edge frames and their fates (code keys for json, Russian for text). */
enum {
    FE_ANCHOR, FE_WARMUP, FE_ENC_FIRST, FE_ENC_LAST, FE_COPY_TAIL,
    FE_BOX_FIRST, FE_LOST_LEFT, FE_LOST_RIGHT, FE_XFIRST, FE_XLAST,
    FE_WRITE_LAST
};

static const char *fe_code(int c)
{
    switch (c) {
    case FE_ANCHOR:     return "copy_anchor_start";
    case FE_WARMUP:     return "smart_warmup_not_written";
    case FE_ENC_FIRST:  return "smart_encode_first";
    case FE_ENC_LAST:   return "smart_encode_last";
    case FE_COPY_TAIL:  return "smart_copy_tail_start";
    case FE_BOX_FIRST:  return "box_first";
    case FE_LOST_LEFT:  return "lost_left_of_box";
    case FE_LOST_RIGHT: return "lost_right_of_box";
    case FE_XFIRST:     return "exact_display_first";
    case FE_XLAST:      return "exact_display_last";
    default:            return "write_last";
    }
}

static const char *fe_russian(int c)
{
    switch (c) {
    case FE_ANCHOR:     return "copy: начало записи (якорь)";
    case FE_WARMUP:     return "smart: прогрев декодера, не пишется";
    case FE_ENC_FIRST:  return "smart: первый перекодированный";
    case FE_ENC_LAST:   return "smart: последний перекодированный";
    case FE_COPY_TAIL:  return "smart: начало побайтовой копии";
    case FE_BOX_FIRST:  return "первый кадр окна декодирования";
    case FE_LOST_LEFT:  return "последний кадр до окна (не сохраняется)";
    case FE_LOST_RIGHT: return "первый кадр после окна (не сохраняется)";
    case FE_XFIRST:     return "первый display-кадр запроса --exact";
    case FE_XLAST:      return "последний display-кадр запроса --exact";
    default:            return "последний сохраняемый";
    }
}

#define PLAN_EDGES_MAX 16

typedef struct {
    size_t dec;
    int codes[4];
    int n_codes;
} plan_edge_t;

static void plan_edge_add(plan_edge_t *ed, size_t *n, size_t dec, int code)
{
    size_t i;
    int j;

    if (dec >= (size_t)-1)
        return;
    for (i = 0; i < *n; i++) {
        if (ed[i].dec == dec) {
            for (j = 0; j < ed[i].n_codes; j++) {
                if (ed[i].codes[j] == code)
                    return;
            }
            if (ed[i].n_codes < 4)
                ed[i].codes[ed[i].n_codes++] = code;
            return;
        }
    }
    if (*n >= PLAN_EDGES_MAX)
        return;
    ed[*n].dec = dec;
    ed[*n].codes[0] = code;
    ed[*n].n_codes = 1;
    (*n)++;
}

static void plan_edges_build(const stream_index_t *idx, const plan_t *pl,
                             plan_edge_t *ed, size_t *n)
{
    size_t i;

    *n = 0;
    if (pl->a0 > 0)
        plan_edge_add(ed, n, pl->a0 - 1, FE_LOST_LEFT);
    plan_edge_add(ed, n, pl->a0, FE_BOX_FIRST);
    if (pl->copy_ok && pl->copy_start < pl->a0)
        plan_edge_add(ed, n, pl->copy_start, FE_ANCHOR);
    if (pl->smart_ok && !pl->smart_pure) {
        if (pl->feed_first < pl->a0)
            plan_edge_add(ed, n, pl->feed_first, FE_WARMUP);
        plan_edge_add(ed, n, pl->a0, FE_ENC_FIRST);
        plan_edge_add(ed, n, pl->enc_last, FE_ENC_LAST);
        if (pl->copy_first <= pl->b0)
            plan_edge_add(ed, n, pl->copy_first, FE_COPY_TAIL);
    }
    if (pl->em.active && !pl->em.clock_base) {
        for (i = 0; i < idx->frame_count; i++) {
            if (idx->frames[i].display_index == pl->em.d_min)
                plan_edge_add(ed, n, i, FE_XFIRST);
            if (idx->frames[i].display_index == pl->em.d_max)
                plan_edge_add(ed, n, i, FE_XLAST);
        }
    }
    plan_edge_add(ed, n, pl->b0, FE_WRITE_LAST);
    if (pl->b0 + 1 < idx->frame_count)
        plan_edge_add(ed, n, pl->b0 + 1, FE_LOST_RIGHT);
}

static void plan_edge_print(const stream_index_t *idx, const plan_edge_t *e)
{
    const frame_t *f = &idx->frames[e->dec];
    char t[32], disp[24], type[8], fate[192];
    int j, used = 0;

    frame_time_str(idx, f, t, sizeof t);
    if (f->display_index < 0)
        strcpy(disp, "-");
    else
        snprintf(disp, sizeof disp, "%lld", f->display_index);
    snprintf(type, sizeof type, "%c%s%s", f->pic_type,
             f->leading_picture ? "L" : "", f->recovery_point ? "R" : "");
    fate[0] = '\0';
    for (j = 0; j < e->n_codes; j++) {
        int w = snprintf(fate + used, sizeof(fate) - (size_t)used, "%s%s",
                         used ? "; " : "", fe_russian(e->codes[j]));
        if (w > 0 && (size_t)w < sizeof(fate) - (size_t)used)
            used += w;
    }
    printf("  %-6zu %-6s %-4s %-8s %s\n", e->dec, disp, type, t, fate);
}

static void plan_text(const stream_index_t *idx, const opts_t *opt,
                      const plan_t *pl)
{
    plan_edge_t ed[PLAN_EDGES_MAX];
    size_t n, i, box_n = pl->b0 - pl->a0 + 1;
    long double fps = 0.0L;
    int tb = plan_time_base(idx, opt, &fps);

    printf("h264-cut: план вырезки (файл не записан)\n");
    printf("  вход: %zu кадров; таймбаза: ", idx->frame_count);
    switch (tb) {
    case TB_FPS:   printf("--fps (%.6Lg)", fps); break;
    case TB_VUI:   printf("VUI (%.6Lg fps)", fps); break;
    case TB_CLOCK: printf("SEI picture timing (VFR)"); break;
    default:       printf("неизвестна"); break;
    }
    printf("\n");

    if (opt->exact_mode) {
        printf("  запрос:  --exact %.3Lf - %.3Lf с", opt->t0, opt->t1);
        if (pl->em.clock_base)
            printf(" (отбор по SEI clock)");
        else
            printf(" → display-ранги [%lld..%lld]",
                   pl->em.d_min, pl->em.d_max);
        printf("\n");
    } else {
        printf("  запрос:  кадры [%zu..%zu] (dec, включительно)\n",
               pl->a0, pl->b0);
    }

    if (pl->em.active && !pl->em.clock_base) {
        size_t extra = 0;

        for (i = pl->a0; i <= pl->b0; i++) {
            long long d = idx->frames[i].display_index;

            if (d >= 0 && (d < pl->em.d_min || d > pl->em.d_max))
                extra++;
        }
        printf("  окно декодирования: [%zu..%zu] (%zu кадров); вне display-"
               "диапазона %zu (нужны для декодирования — политика коробки)\n",
               pl->a0, pl->b0, box_n, extra);
    } else {
        printf("  окно декодирования: [%zu..%zu] (%zu кадров)\n",
               pl->a0, pl->b0, box_n);
    }

    if (!pl->copy_ok) {
        printf("  copy:  нет якоря (IDR/recovery) перед кадром %zu — "
               "вырезка невозможна\n", pl->a0);
    } else if (pl->copy_start == pl->a0) {
        printf("  copy:  граница на якоре — чистая копия [%zu..%zu] (%zu)\n",
               pl->a0, pl->b0, box_n);
    } else {
        printf("  copy:  запись [%zu..%zu] (%zu); +%zu кадров [%zu..%zu] "
               "добавлены для декода (запрошенное окно — их надмножество)\n",
               pl->copy_start, pl->b0, pl->b0 - pl->copy_start + 1,
               pl->a0 - pl->copy_start, pl->copy_start, pl->a0 - 1);
    }
    if (pl->n_leading > 0)
        printf("         leading-кадров с битыми ссылками в копии: %zu"
               " (лечится --smart)\n", pl->n_leading);

    if (pl->smart_wanted) {
        if (!transcode_available())
            printf("  smart: сборка без поддержки перекодирования\n");
        else if (!pl->smart_ok)
            printf("  smart: недоступен для этого диапазона"
                   " (причина — выше в stderr)\n");
        else if (pl->smart_pure)
            printf("  smart: граница на якоре без leading — побайтовая "
                   "копия, идентичная copy\n");
        else if (pl->copy_first > pl->b0)
            printf("  smart: перекод [%zu..%zu] (прогрев с %zu, не "
                   "пишется), запись [%zu..%zu] (%zu)\n",
                   pl->a0, pl->enc_last, pl->feed_first,
                   pl->a0, pl->b0, box_n);
        else
            printf("  smart: перекод [%zu..%zu] (прогрев с %zu, не "
                   "пишется), копия [%zu..%zu]; запись [%zu..%zu] (%zu)\n",
                   pl->a0, pl->enc_last, pl->feed_first,
                   pl->copy_first, pl->b0, pl->a0, pl->b0, box_n);
    } else if (pl->copy_ok && pl->copy_start < pl->a0) {
        printf("  (--smart сохранил бы запрос [%zu..%zu] без сдвига назад)\n",
               pl->a0, pl->b0);
    }

    plan_edges_build(idx, pl, ed, &n);
    {
        size_t p, q;
        for (p = 1; p < n; p++) {
            plan_edge_t key = ed[p];
            for (q = p; q > 0 && ed[q - 1].dec > key.dec; q--)
                ed[q] = ed[q - 1];
            ed[q] = key;
        }
    }
    printf("  краевые кадры:\n");
    printf("  %-6s %-6s %-4s %-8s %s\n", "dec", "disp", "type", "t", "судьба");
    for (i = 0; i < n; i++)
        plan_edge_print(idx, &ed[i]);
}

static void plan_json(const stream_index_t *idx, const opts_t *opt,
                      const plan_t *pl)
{
    plan_edge_t ed[PLAN_EDGES_MAX];
    size_t n, i, box_n = pl->b0 - pl->a0 + 1;
    long double fps = 0.0L;
    int tb = plan_time_base(idx, opt, &fps);

    printf("{\"plan\":1,\"mode\":\"%s\",",
           pl->smart_wanted ? "smart" : "copy");
    printf("\"input\":{\"frame_count\":%zu,\"time_base\":", idx->frame_count);
    switch (tb) {
    case TB_FPS:   fputs("\"fps\"", stdout); break;
    case TB_VUI:   fputs("\"vui\"", stdout); break;
    case TB_CLOCK: fputs("\"sei_clock\"", stdout); break;
    default:       fputs("\"none\"", stdout);
    }
    printf(",\"fps\":");
    if (fps > 0.0L)
        printf("%.9Lg", fps);
    else
        fputs("null", stdout);
    printf("},\"request\":{");
    if (opt->exact_mode) {
        printf("\"kind\":\"exact\",\"t0\":%.9Lg,\"t1\":%.9Lg,"
               "\"clock_base\":%s,\"d_min\":",
               opt->t0, opt->t1, pl->em.clock_base ? "true" : "false");
        if (pl->em.clock_base)
            fputs("null", stdout);
        else
            printf("%lld", pl->em.d_min);
        printf(",\"d_max\":");
        if (pl->em.clock_base)
            fputs("null", stdout);
        else
            printf("%lld", pl->em.d_max);
    } else {
        printf("\"kind\":\"frames\",\"from\":%zu,\"to\":%zu",
               pl->a0, pl->b0);
    }
    printf("},\"box\":{\"first\":%zu,\"last\":%zu,\"count\":%zu},",
           pl->a0, pl->b0, box_n);
    if (pl->copy_ok) {
        printf("\"copy\":{\"ok\":true,\"start\":%zu,\"written\":%zu,"
               "\"added_for_decode\":%zu,\"leading_broken\":%zu},",
               pl->copy_start, pl->b0 - pl->copy_start + 1,
               pl->a0 - pl->copy_start, pl->n_leading);
    } else {
        fputs("\"copy\":{\"ok\":false},", stdout);
    }
    printf("\"smart\":{\"wanted\":%s,\"ok\":%s",
           pl->smart_wanted ? "true" : "false",
           pl->smart_ok ? "true" : "false");
    if (pl->smart_ok && !pl->smart_pure) {
        printf(",\"pure_copy\":false,\"feed_first\":%zu,\"encode_first\":"
               "%zu,\"encode_last\":%zu,\"copy_first\":%zu,\"written\":%zu",
               pl->feed_first, pl->a0, pl->enc_last, pl->copy_first, box_n);
    } else if (pl->smart_ok) {
        fputs(",\"pure_copy\":true", stdout);
    }
    printf("},\"edges\":[");
    plan_edges_build(idx, pl, ed, &n);
    {
        size_t p, q;
        for (p = 1; p < n; p++) {
            plan_edge_t key = ed[p];
            for (q = p; q > 0 && ed[q - 1].dec > key.dec; q--)
                ed[q] = ed[q - 1];
            ed[q] = key;
        }
    }
    for (i = 0; i < n; i++) {
        int j;

        printf("%s{\"dec_id\":%zu,\"fate\":[", i ? "," : "", ed[i].dec);
        for (j = 0; j < ed[i].n_codes; j++)
            printf("%s\"%s\"", j ? "," : "", fe_code(ed[i].codes[j]));
        fputs("]}", stdout);
    }
    fputs("]}\n", stdout);
}

static int do_plan(const stream_index_t *idx, const opts_t *opt)
{
    plan_t pl;
    int rc = plan_compute(idx, opt, &pl);

    if (rc != EXIT_OK)
        return rc;
    if (opt->json_mode)
        plan_json(idx, opt, &pl);
    else
        plan_text(idx, opt, &pl);
    return EXIT_OK;
}

static int do_cut(const stream_index_t *idx, const uint8_t *buf,
                  const opts_t *opt)
{
    size_t a0 = 0, b0 = 0;
    int rc;

    rc = cut_bounds(idx, opt, &a0, &b0);
    if (rc != EXIT_OK)
        return rc;
    if (opt->smart_mode)
        return do_cut_smart(idx, buf, opt, a0, b0);
    return do_cut_copy(idx, buf, opt, a0, b0);
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

    if (opt.smart_mode && !transcode_available()) {
        ERR("собрано без поддержки --smart (нужны x264 и libavcodec)\n");
        rc = EXIT_STREAM;
        goto out;
    }

    if (opt.list_mode && plan_mode(&opt))
        rc = do_plan(&idx, &opt);
    else if (opt.list_mode)
        rc = opt.json_mode ? do_list_json(&idx) : do_list(&idx);
    else
        rc = do_cut(&idx, buf, &opt);

out:
    if (idx_built)
        index_free(&idx);
    free(buf);
    return rc;
}
