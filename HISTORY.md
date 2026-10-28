# HISTORY.md — журнал разработки

Хронология вех проекта. Детали решений — `PLAN.md`, смысл — `SOUL.md`,
правила работы — `AGENTS.md`.

## 2026-10-28 — init

Зародыш репозитория: README с целями (вырезание H.264 с минимальным
перекодированием, миллисекундная точность, структура `source/`, GPLv3+GFDL),
конспект `docs/H.264_explanation_RU.md` (уровни, NAL, GoP, CAVLC/CABAC,
Annex B), лицензии GPLv3 и GFDL-1.3 в `COPYING/`.

## 2026-10-28 — v0.1: h264-cut, копирующий режим (коммиты `c5c6d69..18e07ba`)

Инструмент вырезки диапазона кадров из raw Annex B без перекодирования.

- Каркас: CMake (C11, `-Wall -Wextra` ноль warning'ов), git submodule
  `source/vendor/h264bitstream` (собираются только рабочие файлы upstream —
  `h264_stream.c`, `h264_sei.c`, `h264_nal.c`).
- `source/common/` (библиотека `h264common`): `bits` (MSB-ридер, exp-Golomb),
  `annexb` (скан start code'ов 3/4 байт, unescape emulation prevention,
  запись с нормализацией start code и побайтовой передачей NAL payload),
  `params` (SPS/PPS-состояние через h264bitstream + собственный парсер
  slice header 7.3.3; `frame_num` читается и для IDR — как x264/FFmpeg),
  `index` (NAL→кадры: multi-slice, IDR по idr_pic_id, POC типов 0/2 с
  half-range guard, display-порядок по IDR-сегментам, параметрические NAL
  атрибутируются следующему кадру).
- `h264-cut`: `--list` (кадр→байты), `-f/-t` (порядок декодирования, левая
  граница — к IDR; правая не двигается: порядок декодирования ссылается
  только назад), `--exact T0-T1` (+`--fps`), коды выхода 0/1/2/3/4,
  автодописывание SPS/PPS при их отсутствии в диапазоне.
- Тесты: `tests/run.sh` + `tests/nal_seq.py` — 35 проверок против ffprobe
  (high/baseline, B-кадры, multi-slice), побайтовая сверка NAL-последностей;
  ctest-интеграция. Конспект `docs/slice_header_and_vui_RU.md`.

## 2026-10-28 — этап A: коммиты и документация сборки

Разделение на зелёные по сборке коммиты (каркас → утилита → тесты → доки),
в README добавлены «Сборка» (clone --recursive, cmake, ctest) и план
дальнейших работ. Проверка «свежего клона» — сборка и тесты зелёные.

## 2026-10-28 — фиксирующие решения (SOUL.md / PLAN.md / AGENTS.md)

- Язык — чистый C11, включая тесты (GTest как C++ отклонён).
- Философия — «минимальное перекодирование»: тело вырезки переносится
  байт-в-байт; кадры, чьи зависимости уходят за границу вырезки (стартовый
  неполный GOP, открытые GOP, B-кадры за правым краем), не выбрасываются, а
  перекодируются. Это этап R.
- Бэкенд перекодирования (для R): декодер — libavcodec (только H.264),
  энкодер — напрямую x264; опция `WITH_SMART`.
- Unit-слой — vendored Unity (MIT); интеграционный оракул — ffprobe.

## 2026-10-28 — этап B: точность и детекция (B1–B6)

Без новых runtime-зависимостей; ctest: 8 тестов (6 unit + integration +
fuzz), интеграционных проверок — 62; ASAN+UBSAN на фаззере — чисто.

- **B1** unit-слой: Unity vendored (`tests/unit/vendor/unity`, MIT),
  CMake-GLOB (один `test_*.c` = один ctest), `bitwriter.h` (эталонный
  бит-райтер); suite'ы `bits` (голомб 9.1, края, переполнения) и `annexb`
  (start code, хвостовые нули, unescape golden, round-trip записи).
- **B2** `common/sei.{h,c}`: итератор sei_message (FF-расширения типа/размера,
  чистая остановка на мусоре), payload 6 recovery_point, payload 1
  picture_timing (cnt_type 0, n_90kHz 0 → clock_ts в 1/90000 с);
  14 unit-тестов. Поймана и исправлена ошибка в контрактной формуле часов
  (3600, а не 32400 секунд).
- **B3** POC type 1: deep-путь slice header (delta_pic_order_cnt,
  redundant_pic_cnt, ref_pic_list_modification l0/l1, cabac_init_idc,
  pred_weight_table, dec_ref_pic_marking с mmco→unreferenced; MVC/Extended —
  отказ), алгоритм 8.2.1.1 с размоткой FrameNumOffset и циклическими
  offset_for_ref_frame; `poc_supported` для type 1 больше не падает.
  Golden-тесты `test_poc`/`test_slice_header` (x264 type 1 не пишет —
  интеграция недоступна, зафиксировано).
- **B4** таймбазы `--exact`: `--fps` > fixed-fps (VUI) > SEI clock-ts
  (warning «VFR: время из SEI») > VUI без fixed-флага (x264 всегда пишет
  fixed=0) > отказ; колонка `t` в `--list`; суточная размотка clock-ts.
  Выяснено: x264 не пишет picture-timing SEI — VFR-путь в тестах проверяется
  инжектом SEI.
- **B5** открытые GOP: recovery point — якорь вырезки при
  `recovery_frame_cnt==0 && exact_match && !broken_link` (иначе snap к IDR),
  сегментация display-порядка по IDR/recovery; leading pictures (non-ref с
  POC раньше якоря — штатный отрицательный POC half-range guard'а) — суффикс
  `L` в `--list`, recovery — `R`; в copy-режиме предупреждение о переносе с
  битыми зависимостями (лечится этапом R). Интеграция: x264 `open-gop=1`.
- **B6** hardening: опция `-DENABLE_SANITIZERS=ON`; `tests/fuzz.sh` +
  `tests/fuzz_mutate.py` (детерминированный seed, усечения+флипы, 120
  мутантов/150 прогонов, допустимые коды 0/2/3, таймаут) — ASAN/UBSAN/LSan
  чисто, ~6 с под ASAN; T14 (старт с середины GOP — собирается побайтово,
  `ffmpeg -ss` на raw ES mid-GOP не даёт) и T15 (усечённый хвост).

## 2026-10-28 — этап R: минимальное перекодирование (`--smart`)

Новые зависимости за воротами CMake (`option(WITH_SMART ON)`): x264,
libavcodec, libavutil (pkg-config; нет их — WARNING и сборка без --smart).
ctest: 8 тестов; интеграционных проверок — 89 (было 62); ASAN+UBSAN — чисто.

- **R1** `common/transcode.{h,c}`: AU из NAL-диапазонов кадра (4-байтные
  start codes) → libavcodec-декодер H.264 (pts=dts=dec-индекс, слив
  NULL-пакетом) → фильтр тёплых кадров по pts → x264 (medium, CRF,
  scenecut=0, threads=1, repeat-headers, annexb, i_sps_id и профиль из SPS
  источника) в порядке выдачи декодера; обратный скан за «свежими» SPS/PPS;
  лимиты v1 (интерлейс/кроп/>8 бит/не-4:2:0/без fps) — до тяжёлой работы.
  `sps_state_t` расширен размером кадра, битностью и кроп-полятами.
- **R2** планировщик `--smart`/`--crf` в h264-cut: граница на чистом IDR без
  leading — побайтовый copy (T17 проходит cmp); иначе [a0..next_anchor) —
  перекод с прогревом от якоря, хвост от следующего якоря — копия с препендом
  параметр-сетов источника (лечит конфликт sps_id: x264 пишет i_sps_id
  источника с другим содержимым). Пойманы два бага открытой GOP (ловушки
  12, 13): прогрев только от IDR, копируемый хвост — только от чистого IDR,
  иначе перекод до него насквозь.
- **R3** T16–T20: mid-GOP (150 кадров, первый key, чистый декод, PSNR
  перекодированного GOP 54 dB против display-перемонтированного эталона,
  копия-хвост NAL-в-NAL идентичен источнику), `--exact --smart`, открытая
  GOP (copy предупреждает, smart чинит), лимиты (tff/crop → код 2, copy той
  же границы работает), гейт «собрано без поддержки» → SKIP в сборке без
  WITH_SMART.

## Дальше

B7 (интерлейс) и C (ISO-BMFF / mp4-cut); trailing pictures открытой GOP на
правом крае и x264-лог в stderr при --smart — в `TODO.md`.
