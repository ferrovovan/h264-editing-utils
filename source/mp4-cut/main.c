/*
 * mp4-cut - losslessly cut a single H.264 video track out of an MP4 file.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iso.h"

#define PROG "mp4-cut"
#define ERR(...) fprintf(stderr, PROG ": " __VA_ARGS__)

enum {
    EXIT_OK = 0,
    EXIT_USAGE = 1,
    EXIT_STREAM = 2,
    EXIT_RANGE = 3,
    EXIT_IO = 4
};

typedef struct {
    int list_mode;
    int have_from;
    int have_to;
    int exact_mode;
    long long from;
    long long to;
    long double t0;
    long double t1;
    const char *output;
    const char *input;
} opts_t;

static void usage(FILE *out)
{
    fputs("usage: mp4-cut [опции] INPUT.mp4\n"
          "  -l, --list            список сэмплов видео-дорожки; с -f/-t/-e"
          " — план вырезки\n"
          "                        без записи файла; с -o — ошибка\n"
          "  -f, --from-frame N    первый сохраняемый сэмпл (0-based, порядок\n"
          "                        декодирования; при попадании в середину GOP\n"
          "                        граница сдвигается назад на sync-сэмпл)\n"
          "  -t, --to-frame M      последний сохраняемый сэмпл (по умолчанию\n"
          "                        последний)\n"
          "  -e, --exact T0-T1     диапазон по времени отображения,\n"
          "                        HH:MM:SS.mmm-HH:MM:SS.mmm\n"
          "  -o, --output FILE     файл результата (обязателен для вырезания)\n"
          "  -h, --help            эта справка\n"
          "\n"
          "  v1: один видео-трек avc1/avc3, без аудио/субтитров и без\n"
          "  фрагментирования; payload'ы сэмплов копируются побайтно.\n",
          out);
}

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

static int parse_time(const char *s, long double *out)
{
    const char *p;
    unsigned long hh, mm, ss;
    long double frac = 0.0L, scale = 1.0L;

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
        while (*p >= '0' && *p <= '9') {
            scale *= 10.0L;
            frac = frac * 10.0L + (long double)(*p - '0');
            p++;
        }
        frac /= scale;
    }
    if (*p != '\0')
        return -1;
    *out = (long double)(hh * 3600 + mm * 60) + (long double)ss + frac;
    return 0;
}

static int parse_range(const char *s, long double *t0, long double *t1)
{
    char buf[64];
    const char *dash = strrchr(s, '-');
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
    return (*t1 > *t0) ? 0 : -1;
}

enum {
    OPT_LIST, OPT_FROM, OPT_TO, OPT_EXACT, OPT_OUTPUT, OPT_HELP
};

struct opt_def {
    const char *name;
    int has_arg;
    int id;
};

static const struct opt_def long_opts[] = {
    { "list",       0, OPT_LIST },
    { "from-frame", 1, OPT_FROM },
    { "to-frame",   1, OPT_TO },
    { "exact",      1, OPT_EXACT },
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

static int parse_ll(const char *v, long long *out)
{
    char *end;
    long long n;

    if (!v || *v == '\0')
        return -1;
    n = strtoll(v, &end, 10);
    if (*end != '\0')
        return -1;
    *out = n;
    return 0;
}

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
            if (parse_ll(val, &opt->from) != 0) {
                ERR("invalid --from-frame value: %s\n", val);
                return EXIT_USAGE;
            }
            opt->have_from = 1;
            break;
        case OPT_TO:
            if (parse_ll(val, &opt->to) != 0) {
                ERR("invalid --to-frame value: %s\n", val);
                return EXIT_USAGE;
            }
            opt->have_to = 1;
            break;
        case OPT_EXACT:
            if (parse_range(val, &opt->t0, &opt->t1) != 0) {
                ERR("invalid --exact value: %s\n", val);
                return EXIT_USAGE;
            }
            opt->exact_mode = 1;
            break;
        case OPT_OUTPUT:
            opt->output = val;
            break;
        default:
            return EXIT_USAGE;
        }
    }
    return EXIT_OK;
}

static uint8_t *read_file(const char *path, size_t *out_size)
{
    FILE *f;
    long sz;
    uint8_t *buf;
    size_t got;

    f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        return NULL;
    }
    *out_size = (size_t)sz;
    return buf;
}

static void print_list(const iso_movie_t *m)
{
    const iso_video_t *v = &m->video;
    size_t i;

    fputs("# dec_id pts_t    dts_t    key size\n", stdout);
    for (i = 0; i < v->sample_count; i++) {
        double pts = v->timescale ? (double)v->samples[i].pts / v->timescale : 0.0;
        double dts = v->timescale ? (double)v->samples[i].dts / v->timescale : 0.0;
        printf("%-7zu %-9.3f %-9.3f %-4c %zu\n",
               i, pts, dts, v->samples[i].is_sync ? 'K' : '.',
               (size_t)v->samples[i].size);
    }
}

/* Dry-run: same numbers as the real cut (bounds + snap), prints a plan. */
static void print_plan(const iso_movie_t *m, int exact, long long raw_from,
                       int have_from, size_t sel_from, size_t a, size_t b)
{
    const iso_video_t *v = &m->video;
    size_t i, n_sync = 0;
    double ts = v->timescale ? (double)v->timescale : 1.0;

    for (i = 0; i < v->sample_count; i++) {
        if (v->samples[i].is_sync)
            n_sync++;
    }
    fputs("mp4-cut: план вырезки (файл не записан)\n", stdout);
    printf("  дорожка: %zu сэмплов; timescale %u; sync-сэмплов: %zu\n",
           v->sample_count, v->timescale, n_sync);
    if (exact)
        printf("  запрос:  --exact: по pts сэмпл [%zu..%zu], snap назад на "
               "sync → %zu\n", sel_from, b, a);
    else if (have_from && (size_t)raw_from != a)
        printf("  запрос:  -f %lld: snap назад на sync → %zu (добавлено "
               "%zu сэмплов [%zu..%zu] для декода)\n",
               raw_from, a, (size_t)raw_from - a, a, (size_t)raw_from - 1);
    else
        printf("  запрос:  граница уже на sync (сдвиг не нужен)\n");
    printf("  запишет: сэмплы [%zu..%zu] (%zu)\n", a, b, b - a + 1);
    fputs("  краевые сэмплы:\n", stdout);
    fputs("  idx     pts_t     dts_t     key fate\n", stdout);
    {
        size_t lo = a > 3 ? a - 3 : 0;
        size_t hi = b + 1 < v->sample_count ? b + 1 : v->sample_count - 1;

        for (i = lo; i <= hi; i++) {
            char fbuf[160];
            size_t used = 0;

            fbuf[0] = '\0';
            if (!exact && have_from && i == (size_t)raw_from && a != i)
                used += (size_t)snprintf(fbuf + used, sizeof fbuf - used,
                        "запрошенный -f (середина GOP, не пишется один)");
            if (i == a) {
                used += (size_t)snprintf(fbuf + used, sizeof fbuf - used,
                        "%sпервый сохраняемый (sync)", used ? "; " : "");
            }
            if (i == b) {
                used += (size_t)snprintf(fbuf + used, sizeof fbuf - used,
                        "%sпоследний сохраняемый", used ? "; " : "");
            }
            if (used == 0 && i < a) {
                used += (size_t)snprintf(fbuf + used, sizeof fbuf - used,
                        "не пишется (до sync-начала)");
            }
            if (used == 0 && i > b) {
                used += (size_t)snprintf(fbuf + used, sizeof fbuf - used,
                        "после конца диапазона");
            }
            if (used == 0)
                continue;
            printf("%-8zu %-10.3f %-10.3f %-4c %s\n", i,
                   (double)v->samples[i].pts / ts,
                   (double)v->samples[i].dts / ts,
                   v->samples[i].is_sync ? 'K' : '.', fbuf);
        }
    }
}

int main(int argc, char **argv)
{
    opts_t opt;
    uint8_t *buf;
    size_t size = 0;
    iso_movie_t m;
    char err[256];
    size_t a, b, sel_from = 0;
    long long raw_from = 0;
    int rc;

    memset(&opt, 0, sizeof(opt));
    rc = parse_args(argc, argv, &opt);
    if (rc == 2)
        return EXIT_OK;
    if (rc != EXIT_OK)
        return rc;
    if (!opt.input) {
        ERR("no input file\n");
        usage(stderr);
        return EXIT_USAGE;
    }
    if (opt.exact_mode && (opt.have_from || opt.have_to)) {
        ERR("--exact несовместим с -f/-t\n");
        return EXIT_USAGE;
    }

    buf = read_file(opt.input, &size);
    if (!buf) {
        ERR("не могу прочитать: %s\n", opt.input);
        return EXIT_IO;
    }
    if (iso_open(&m, buf, size, err, sizeof(err)) != 0) {
        ERR("%s\n", err);
        free(buf);
        return EXIT_STREAM;
    }

    if (opt.list_mode && !opt.have_from && !opt.have_to && !opt.exact_mode) {
        if (opt.output) {
            ERR("--list ничего не пишет: -o с --list несовместим\n");
            iso_close(&m);
            free(buf);
            return EXIT_USAGE;
        }
        print_list(&m);
        iso_close(&m);
        free(buf);
        return EXIT_OK;
    }
    if (opt.list_mode && opt.output) {
        ERR("план ничего не пишет: -o с --list несовместим\n");
        iso_close(&m);
        free(buf);
        return EXIT_USAGE;
    }
    if (!opt.output && !opt.list_mode) {
        ERR("нужен -o OUTPUT (или --list)\n");
        iso_close(&m);
        free(buf);
        return EXIT_USAGE;
    }

    a = 0;
    b = m.video.sample_count - 1;
    sel_from = 0;

    if (opt.exact_mode) {
        int64_t t0t, t1t;
        size_t i;
        int found = 0;

        if (!m.video.timescale) {
            ERR("нет таймбазы дорожки для --exact\n");
            iso_close(&m);
            free(buf);
            return EXIT_RANGE;
        }
        t0t = (int64_t)(opt.t0 * (long double)m.video.timescale + 0.5L);
        t1t = (int64_t)(opt.t1 * (long double)m.video.timescale + 0.5L);
        sel_from = 0; /* set below on first hit */
        for (i = 0; i < m.video.sample_count; i++) {
            if (m.video.samples[i].pts >= t0t) {
                a = i;
                sel_from = i;
                found = 1;
                break;
            }
        }
        if (!found || iso_last_pts_le(&m.video, t1t, &b) != 0 || b < a) {
            ERR("--exact: нет сэмплов в диапазоне\n");
            iso_close(&m);
            free(buf);
            return EXIT_RANGE;
        }
    } else {
        if (opt.have_from) {
            if (opt.from < 0 || (unsigned long long)opt.from >= m.video.sample_count) {
                ERR("--from-frame вне диапазона [0..%zu]\n",
                    m.video.sample_count - 1);
                iso_close(&m);
                free(buf);
                return EXIT_RANGE;
            }
            a = (size_t)opt.from;
        }
        if (opt.have_to) {
            if (opt.to < 0 || (unsigned long long)opt.to >= m.video.sample_count) {
                ERR("--to-frame вне диапазона [0..%zu]\n",
                    m.video.sample_count - 1);
                iso_close(&m);
                free(buf);
                return EXIT_RANGE;
            }
            b = (size_t)opt.to;
        }
        if (b < a) {
            ERR("пустой диапазон [-f %zu > -t %zu]\n", a, b);
            iso_close(&m);
            free(buf);
            return EXIT_RANGE;
        }
    }

    raw_from = (long long)a;
    /* Snap the left edge back to a sync (IDR) sample so the cut is decodable. */
    if (iso_first_sync_at_or_before(&m.video, a, &a) != 0) {
        ERR("нет sync-сэмпла перед позицией %zu\n", a);
        iso_close(&m);
        free(buf);
        return EXIT_RANGE;
    }

    if (opt.list_mode) {
        print_plan(&m, opt.exact_mode, raw_from, opt.have_from, sel_from,
                   a, b);
        iso_close(&m);
        free(buf);
        return EXIT_OK;
    }

    {
        uint8_t *out = NULL;
        size_t out_size = 0;
        FILE *fo;

        if (iso_write_cut(&m, a, b, &out, &out_size, err, sizeof(err)) != 0) {
            ERR("%s\n", err);
            iso_close(&m);
            free(buf);
            return EXIT_STREAM;
        }
        fo = fopen(opt.output, "wb");
        if (!fo) {
            ERR("не могу записать: %s\n", opt.output);
            free(out);
            iso_close(&m);
            free(buf);
            return EXIT_IO;
        }
        if (fwrite(out, 1, out_size, fo) != out_size) {
            ERR("ошибка записи: %s\n", opt.output);
            fclose(fo);
            free(out);
            iso_close(&m);
            free(buf);
            return EXIT_IO;
        }
        fclose(fo);
        fprintf(stderr, PROG ": вырезаны сэмплы [%zu..%zu] → %s\n",
                a, b, opt.output);
        free(out);
    }

    iso_close(&m);
    free(buf);
    return EXIT_OK;
}
