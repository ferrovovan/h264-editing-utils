# AGENTS.md — правила работы над репозиторием

Для людей и субагентов. Сначала прочитай `SOUL.md` (зачем мы это делаем) и
`PLAN.md` (что делаем).

## Сборка и проверка

```bash
git submodule update --init --recursive
cmake -B build && cmake --build build        # -Wall -Wextra, ноль warning'ов обязателен
ctest --test-dir build                       # unit + integration
bash tests/run.sh build/source/h264-cut/h264-cut   # только интеграционный слой
```

Коммит-гит: короткие сообщения в lower case (`h264-cut: ...`, `docs: ...`).
Не пушить и не коммитить без явной просьбы.

## Структура

- `source/common/` — библиотека `h264common` (C11): `bits` (bit reader, exp-Golomb),
  `annexb` (скан/запись Annex B), `params` (SPS/PPS-состояние + slice header),
  `index` (NAL→кадры, POC, display-порядок), `sei` (SEI-итератор и payload'ы).
- `source/h264-cut/` — утилита; вся логика бинаря — в `main.c`, она тонкая,
  мякоть в `common`.
- `source/vendor/h264bitstream/` — git submodule, **read-only**.
- `tests/unit/` — unit-тесты на Unity (vendored в `tests/unit/vendor/unity`,
  один файл `test_<модуль>.c` = один ctest-тест, новых файлов в CMake не требует);
  `tests/run.sh` + `tests/nal_seq.py` — интеграционные тесты, оракул — ffprobe.
- `docs/` — конспекты стандарта (GFDL), `*.md` на русском.

## Соглашения кода

- GPL-шапка в каждом файле исходников (копирайты: "h264-editing-utils contributors").
- Заголовки библиотеки — контракт: публичные API менять можно, но тогда
  синхронно правятся вызывающий код и unit-тесты.
- Никаких новых зависимостей без решения, зафиксированного в `PLAN.md`.
- Вывод утилит на русском, ошибки — в stderr с префиксом `h264-cut: `,
  коды выхода: 0 ок / 1 use-case / 2 поток-или-метаданные / 3 диапазон / 4 I/O.

## Изученные ловушки (не наступать повторно)

1. **`frame_num` присутствует и в IDR-срезах.** Реальные кодеры (x264, FFmpeg)
   и h264bitstream читают его безусловно, вопреки поздним редакциям 7.3.3.
   Мы читаем всегда — см. docs/slice_header_and_vui_RU.md.
2. **Из h264bitstream компилируются только `h264_stream.c`, `h264_sei.c`,
   `h264_nal.c`** (см. upstream Makefile.am); `h264_slice_data.c`/`h264_avcc.c`
   — мусор генератора, standalone не собирается. Slice header парсим сами.
3. Поля h264bitstream со старыми именами: `residual_colour_transform_flag` =
   `separate_colour_plane_flag`, `pps->pic_order_present_flag` =
   `bottom_field_pic_order_in_frame_present_flag`.
4. **POC type 0 — с half-range guard** (как `ff_h264_init_poc`), наивное
   `lsb < prev ⇒ msb += Max` ломает B-порядок декодирования.
5. Параметрические NAL (SPS/PPS/SEI/AUD) атрибутируются **следующему** кадру —
   так диапазон NAL кадра самодостаточен и вырезка уносит свои параметр-сеты.
6. При записи start code нормализуется до 4 байт, хвостовые zero_8bits между
   NAL отбрасываются; NAL-payload — байт-в-байт. Сравнения с оригиналом в
   тестах — на уровне NAL-последовательностей (`tests/nal_seq.py`), не сырых байт.
7. x264: `-x264-params scenecut=0` для детерминированного ключинга в тестах;
   POC type 1 x264 не пишет (тестируется unit-синтетикой).
8. **x264 не пишет picture-timing SEI** (clock timestamp недоступны на его
   потоках — в тестах инжектимся вручную) и **всегда пишет
   `fixed_frame_rate_flag=0`** — поэтому таймбаз у `--exact` четыре:
   `--fps` > fixed-fps > SEI clock-ts > VUI-timing без флага (фолбэк).
9. `ffmpeg -ss` на raw ES не даёт старт с середины GOP (input-`-ss` — пустой
   файл, output-`-ss` выравнивается на IDR). Mid-GOP-поток для тестов
   собирается побайтово: SPS/PPS + кадры из середины GOP по byte_range.
10. POC type 1: отдельной msb-логики нет — её роль играет размотанный
    FrameNumOffset; mmco-unreferenced POC не сдвигает (современный
    `ff_h264_init_poc`). Recovery point — полноценный якорь display-сегмента.
11. Leading picture открытой GOP получает **отрицательный POC** через
    half-range guard type 0 — это штатно, display-бэйл по `poc < 0` делать
    нельзя (бэйлим только на parse_error).
12. **FFmpeg (≥7), стартуя сессию с recovery point, выбрасывает leading-картинку
    сразу за ним** («декодер выдал N-1, ожидалось N»). Тёплый прогрев транскодера
    — всегда от ближайшего IDR, даже если usable_anchor-якорь ближе.
13. **Нельзя резать сплайсом открытую GOP слева от recovery-хвоста:** leading
    picture после recovery ссылаются на кадры ДО recovery, а перекодированный
    фрагмент даёт их с другими frame_num/POC → «mmco: unref short failure».
    Копируемый хвост обязан начинаться с чистого IDR; открытые GOP перекодируются
    насквозь до него.
14. `x264.h` объявляет `NAL_SLICE..NAL_FILLER` — clash с нашим `annexb.h`
    (тот же rename-трюк, что с h264bitstream в params.c). В CMake ≥4
    `PkgConfig::<p>` target появляется только с `IMPORTED_TARGET`.

## Тестовая дисциплина

- Новый алгоритм в `common/` = unit-тест на синтетическом RBSP (бит-райтер
  есть в `tests/unit/bitwriter.h`) + интеграционная проверка против ffprobe.
- Тесты не ослаблять: если ffprobe/эталон не совпадает — ищем баг, а не
  меняем ожидание.
- Изменения вырезки обязаны проходить проверку декодируемости
  (`ffmpeg -v error -i out -f null -`) и побайтовость copy-частей.

## Субагенты

Крупные куски — через субагентов с жёстким контрактом: конкретные файлы,
заголовки-договорённости, способ самопроверки, запрет трогать чужие области.
Интеграция и финальная зелёная сборка — за ведущим.
