# PLAN.md — дорожная карта

Формат: этапы с чекбоксами; решения, зафиксированные участниками, помечены
**[Р]** и не пересматриваются без явного повода.

## Ключевые решения

- **[Р] Язык — чистый C11.** Не C++ — ни в библиотеке, ни в тестах.
- **[Р] Тесты: Unity (чистый C, MIT, vendored)** для unit-слоя + bash/ffprobe
  для интеграционного. GTest отклонён как C++.
- **[Р] Философия вырезки — «минимальное перекодирование»** (см. SOUL.md):
  копируемое — копируется байт-в-байт; кадры, чьи dependencies уходят за
  границу вырезки, — перекодируются, а **не выбрасываются**.
- **[Р] Бэкенд перекодирования:** декод — libavcodec (только Н.264-декодер,
  без libavformat), энкод — напрямую x264 (Annex B-вывод, профиль/уровень/fps
  берём из нашего SPS-парсера). Обе библиотеки — system, LGPL; опция сборки
  `WITH_SMART` (ON, если нашлись). OpenH264 — запасной вариант.
- Интерфейс перекодирования: `h264-cut --smart`, а не отдельная утилита —
  для пользователя это та же вырезка.

## Сделано

- [x] **Этап 0-5 (v0.1, в main):** каркас CMake + submodule h264bitstream;
      Annex B читатель/писатель; индекс кадров (POC 0/2, multi-slice, B-кадры);
      `h264-cut --list / -f/-t / --exact` (copy-режим, closed GOP);
      интеграционные тесты (ctest) против ffprobe; конспекты в `docs/`.
- [x] **Этап A:** коммиты, раздел «Сборка» в README.

## Этап B — точность и детекция (чистый C, без новых зависимостей)

- [x] **B1. Unit-слой на Unity.** `tests/unit/`: unity vendored, CMake
      (GLOB `test_*.c` → один ctest на файл), бит-райтер для синтетики,
      suite'ы: `bits` (голомб/края), `annexb` (start code/unescape/round-trip).
- [x] **B2. `common/sei.{h,c}`.** Итератор SEI-сообщений; payload 6
      recovery_point; payload 1 picture_timing (clock_timestamp cnt_type 0,
      u24/u32; 1/2 — «не поддерживается»); юнит-тесты на синтетике.
- [x] **B3. POC type 1.** `sps_state_t` += поля type 1; deep-путь slice header
      (ref_pic_list_modification, pred_weight_table, dec_ref_pic_marking,
      mmco→unref) включается только при poc_type==1; unit-тесты
      golden-последовательностями. Отклонения (обоснованы FFmpeg): для type 1
      нет отдельной msb-логики (роль msb — размотанный FrameNumOffset);
      mmco-unref на POC не влияет; mmco 5 и интерлейсные ветви — B7.
- [x] **B4. Таймбаза `--exact`:** приоритет `--fps` > fixed-fps (VUI
      `fixed_frame_rate_flag`) > SEI clock-ts (warning «VFR, время из SEI») >
      **VUI-timing без fixed-флага (фолбэк: x264 всегда пишет fixed=0, иначе
      --exact умерал бы на половине реальных потоков)** > отказ; в `--list`
      колонка `t` (секунды).
- [x] **B5. Открытые GOP — детекция (копирующий режим).** Recovery point как
      якорь при `recovery_frame_cnt==0 && exact_match && !broken_link`
      (сообщение «сдвинуто к recovery point»); leading/trailing pictures
      помечены суффиксами `L`/`R` в `--list`; в copy-режиме при leading в
      диапазоне — предупреждение «переносятся с битыми зависимостями,
      --smart — этап R». Display-сегментация: IDR или recovery point.
- [x] **B6. Hardening.** `-DENABLE_SANITIZERS=ON` (ASAN+UBSAN);
      `tests/fuzz.sh` + `tests/fuzz_mutate.py` (усечения/флипы, seed
      детерминирован; коды 0/2/3; таймаут; ctest-лейбл `fuzz`) — чисто;
      T14 (старт с середины GOP) и T15 (усечённый хвост) в run.sh.

Отложено из B: **интерлейс** (поля, POC с offset_for_top_to_bottom_field) —
этап B7 после R.

## Этап R — минимальное перекодирование («хвосты напильником»)

Правила вырезки (v1, зафиксированы):

1. **Тело:** GOP'ы целиком внутри диапазона и все кадры, чьи референсы внутри
   диапазона, — байт-в-байт (текущий путь).
2. **Стартовый хвост:** режется GOP на части: кадры `[enc_first..next_anchor-1]`
   перекодируются, где `enc_first` — левая граница после snap'а, а
   `next_anchor` — следующий usable_anchor (IDR/recovery) после `enc_first`
   (если его нет — правая граница диапазона). Декодер «прогревается» от
   якоря текущего GOP (или от предыдущего якоря, см. п.4), энкодер обязан
   начать с IDR.
3. **Правый край в v1 не трогаем:** окно по порядку декодирования замкнуто
   относительно зависимостей; задержка — trailing pictures открытого GOP,
   чьё отображение в диапазоне, а декодирование за `to` (в TODO.md).
4. **Leading pictures:** если левая граница — якорь, и в диапазоне есть
   помеченные `L` кадры, они опираются на кадры ДО якоря → перекодируется
   весь GOP `[anchor..next_anchor)`, декодер греется от предыдущего якоря.
5. Если граница уже на якоре и `L` нет — выход `--smart` обязан быть
   **байт-в-байт равен copy-режиму** (тест T17).
6. Ограничения v1 (чистые ошибки, не молчаливая деградация): интерлейс,
   кадрирование (frame_cropping), bit_depth > 8, chroma != 4:2:0,
   parse_error в окне, отсутствие fps (VUI или `--fps`) — отказ с кодом 2.

Декомпозиция (субагенты, подряд):

- [x] **R1. `common/transcode.{h,c}`** (за `WITH_SMART`):
      `transcode_available(err)` и
      `transcode_range(buf, idx, feed_first, enc_first, enc_last, crf,
      fps_num, fps_den, &out_buf, &out_size, &out_nals, err)` —
      сборка AU из NAL-диапазонов кадра (pts=dts=dec-индекс) →
      libavcodec H.264-декодер (extradata in-band) → фильтр dec≥enc_first →
      x264 (preset medium, CRF, annexb, scenecut off, профиль из SPS:
      baseline/main/high, `i_sps_id` из SPS источника, размер из
      pic_width/height_in_mbs) в порядке выдачи декодера (display) →
      конкатенация NAL + `annexb_scan`. Плюс: `sps_state_t` расширяется
      (bit_depth_luma_minus8, pic_width/height_in_mbs_minus1, кроп-поля);
      CMake `option(WITH_SMART ON)` с pkg-config (x264, libavcodec,
      libavutil) и авто-выключением с warning; compile-define
      `H264EU_HAVE_SMART`. Самопроверка — драйвер в /tmp на ffmpeg-потоке.
- [x] **R2. `--smart` в h264-cut:** чистый планировщик сегментов
      (копия/перекод по правилам 1-6), запись: encoded-сегмент, затем
      копия `[next_anchor..b]` с явным препендом SPS/PPS источника перед
      копированным хвостом (возможный конфликт id параметр-сетов
      решается препендом); сообщения в stderr «перекодируем [a..b],
      копируем [c..d]»; `--exact --smart`; README-секция + `--crf` (по
      умолчанию 18); без WITH_SMART — код 2 «собрано без поддержки
      --smart».
- [x] **R3. Тесты T16–T20 (run.sh):**
      T16 старт из середины GOP: 39 кадров, первый key, декод чистый,
      PSNR перекодированного GOP ≥ 30 dB против trim-референса исходника,
      nal_seq: копия-хвост == NAL-диапазон источника байт-в-байт;
      T17 граница на IDR → побайтовое равенство с copy-выходом;
      T18 `--exact --smart` sane; T19 open-GOP с `L` в окне: smart
      перекодирует GOP, декод чище copy-варианта; T20 интерлейс/кроп →
      отказ 2 с внятными текстами. PSNR через ffmpeg lavfi.

Риски и митигации: (a) порядок decode↔display при маппинге кадров — тегируем
пакеты dec-индексом через pts; (b) конфликт sps_id на стыке сегментов —
препенд параметр-сетов источника перед копированным хвостом; (c) несовпадение
профиля x264 с источником — таблица маппинга + фолбэк main, в тестах сверка
профиля через ffprobe.

Приёмка этапа: полный ctest зелёный (включая сборку без WITH_SMART —
`-DWITH_SMART=OFF`, и с санитайзерами); правила 1–5 покрыты тестами;
README/HISTORY обновлены.

## Финальный заход — v1.0

- [ ] **D1.** `common/avcc.{h,c}`: avcC (ISO/IEC 14496-15) parse/build +
      конвейер AVCC↔Annex B; бинари `avcc2annexb`/`annexb2avcc`; unit-тесты
      (golden avcC) + интеграция (round-trip, сверка nal_seq).
- [ ] **D2.** `--list --json` (машинночитаемый вывод, test: json.load).
- [ ] **B7.** Интерлейс: PAFF поле-пары → картинки, display-порядок 8.21,
      вырезка copy-режима целыми картинками; MBAFF = штатные кадры;
      `--smart` интерлейс по-прежнему отказывает (documented).
- [ ] **C.** `common/iso.{h,c}`: обход боксов, avcC, stts/ctts/stss/stsc/
      stsz/stco/co64 → `mp4-cut` v1: одно видео-трек, границы `-f/-t/--exact`,
      пересборка stbl, байтовая идентичность сэмплов, ffprobe-оракул.
- [ ] **R4.** Правый край `--smart --exact`: trailing-картины открытой GOP,
      чьё отображение в диапазоне, а декодирование за `to` — попадают в
      перекодируемое окно (feed_last за `to`).
- [ ] **CI.** Fresh-clone job (`clone --recursive` + сборка + ctest);
      релизные артефакты на тегах (бинарь + CHANGELOG из HISTORY).
- [ ] **v1.0.0**: README (полная картина), HISTORY, тег.

Приёмка: ctest зелёный во всех конфигурациях (обычная, `-Werror`,
`WITH_SMART=OFF`, санитайзеры); новые ограничения — чистые отказы кодом 2
с русскими текстами.
