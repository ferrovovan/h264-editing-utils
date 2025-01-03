## Основа
Делится на 2 уровня:
1. Логический уровень:
   + Это абстракция, которая описывает, как видео выглядит "снаружи":
      Группы кадров (GoP).  
      Взаимосвязь между кадрами (I, P, B).  
      Применение предсказания и сжатия.  
   + Этот уровень представляет то, что мы "видим" при воспроизведении видео: последовательность кадров, которые декодируются и отображаются.
   Это "высший" уровень — результат, который получается после обработки данных.

2. Транспортный уровень:
   + Это способ представления данных для передачи, хранения и обработки.
   + Он делит видео на NAL-юниты, которые содержат кадры или их части, а также метаданные (SPS, PPS и другие).
   + Каждый NAL-юнит имеет заголовок, определяющий его тип, и полезную нагрузку (данные кадра или метаданные).

Переход от транспортного к логическому уровню:
   + На транспортном уровне данные хранятся фрагментами (NAL-юнитами).
   + При декодировании эти фрагменты собираются, создавая логическую структуру (GoP, кадры, макроблоки).


## Логический уровень:
### Group of Pictures
#### Определение
*GoP* (Group of Pictures) — это логическая структура, определяющая, как кадры упорядочены. Cостоит из группы кадров (I, P, B).
#### Пример
*GOP* может выглядеть так:
`I P B B P B B P`
Важно: B-кадры располагаются между I и P-кадрами.  
#### Параметры *GoP*
* Длинна *GoP* часто фиксирована и задаётся в параметрах кодера.  
Например, "каждые 10 кадров должен быть I-кадр". Это влияет на качество и компрессию.

### Типы кадров H.264
* I-кадр (интракадр):  
   + Это самостоятельный кадр. Аналог фотографии.
   + Используется как точка восстановления (например, после потери данных).
   + Макроблоки кодируются исключительно с использованием Intra-кодирования.
* P-кадр (прогнозируемый):  Хранит только изменения с предыдущих кадров (Forward Prediction).
   + Макроблоки кодируются с использованием Inter-кодирования, то есть с предсказанием изменений на основе предыдущих кадров.
   + Каждый макроблок может быть:  
     Intra-кодированным, если предсказание даёт плохие результаты.  
     Inter-кодированным, если предсказание эффективно (экономит пространство).  
* B-кадр (двунаправленный): Использует информацию из предыдущих и будущих кадров.
   + Макроблоки преимущественно кодируются как Inter-кодированные, используя предсказание из предыдущих и последующих кадров.
   + Как и в P-кадрах, макроблок может быть Intra-кодированным, если это выгоднее.  

Примечание: B-кадры могут использоваться только в профилях Main и High, а в Baseline они отсутствуют.

### Макроблоки
* Кадр разбивается на макроблоки (обычно 16x16 пикселей), которые могут делиться на меньшие блоки (например, 8x8 или 4x4).
* Каждый макроблок может быть:  
   + Intra-кодированным (анализ только внутри текущего кадра).
   + Inter-кодированным (использует предсказание с других кадров).  

16x16 пикселей — это стандартный размер, но кодер может разбивать макроблоки на блоки меньших размеров для более точного сжатия (например, 4x4 при сложных сценах).

### Кодирование Макроблоков
* **DCT**: преобразует макроблок из пространственной области (пиксели) в частотную область (коэффициенты).
Высокие частоты (мелкие детали) обычно уменьшаются, так как человеческий глаз менее чувствителен к ним.
* **Квантование**: округляет коэффициенты *DCT*, удаляя менее значимые данные для сжатия.
Задаётся параметром *QP* (Quantization Parameter), который регулирует баланс между качеством и размером.


## Транспортный Уровень
H.264-поток **полностью состоит из последовательности NAL-юнитов**. Он не содержит других типов данных (в базовой комплектации).  
*NAL-юниты* — это контейнеры для доставки данных. Это "обёртка" любых данных.  

### Типы NAL-юнитов:
1. NAL-юниты для видео:
   + Несрезанные кадры, срезанные кадры, ключевые кадры и т.д.
2. Метаданные:
   + SPS (Sequence Parameter Set): описывает параметры последовательности, включая разрешение, частоту кадров, профиль, уровень и т.д.
   + PPS (Picture Parameter Set): параметры конкретного кадра или группы кадров (размеры кадра, частоту кадров, информация о макроблоках и т.д.).
   + SEI (Supplemental Enhancement Info) для вспомогательной информации.
3. Структурные данные:
   + Access Unit Delimiter (тип 9): Помогает обозначить границы групп кадров.
   + Конец последовательности или потока (типы 10 и 11).

#### SEI
**SEI** (Supplemental Enhancement Information) — это вспомогательная информация, которая сопровождает основное видео.
Она используется для передачи метаданных, которые могут быть полезны для декодера или приложений.  
Основные категории *SEI*:
   * Технические данные: информация о цветовых пространствах, HDR, аспектных соотношениях, задержках и синхронизации.
   * Субтитры: текстовые или графические данные, встроенные в поток.
   * Дополнительные параметры: настройки для улучшения качества отображения.
*SEI* имеет гибкую структуру и может включать множество сообщений (payload types). Каждое сообщение начинается с идентификатора типа и длины данных.  
Примеры полезных нагрузок:  
   + Buffering period: задержка между декодированием и отображением.
   + Picture timing: временные метки для синхронизации.
   + User data registered: данные, зарегистрированные пользователями (например, субтитры).
   + User data unregistered: пользовательские данные без регистрации.

### Заголовок NAL-юнита
Бывают обычные и расширенные.
* Обычный NAL-юнит используется в минималистичных сценариях (локальное хранение, простая передача). Он состоит из 1 байта заголовка и полезной нагрузки.
* Расширенный NAL-юнит добавляет дополнительные поля (например, старт-коды в *Annex B* или длину в *RTP*).

#### Заголовок обыкновенного NAL-юнита
* **F** = forbidden_zero_bit (1 бит):
   + Всегда должен быть 0.
   + Если он равен 1, это указывает на ошибку или повреждение данных.
* **NRI** = nal_ref_idc (2 бита): Уровень важности NAL-юнита (для декодирования и восстановления)
   + Значения 00, 01, 10, 11 определяют приоритет при декодировании.

* **Type** = nal_unit_type (5 бит):
   + Определяет тип NAL-юнита (например, кадры, метаданные или служебная информация).

Пример обычного NAL-юнита:
| F | NRI | NRI | T | T | T | T | T |
|---|-----|-----|---|---|---|---|---|
| 0 |  0  |  1  | 0 | 0 | 1 | 0 | 1 |

F = 0 (корректный).
NRI = 01 (низкая важность).
Type = 5 (закодированный кадр, например, IDR-кадр).

##### Таблица типов NAL-юнитов
| **Значение** | **Имя**                                | **Описание**                       |
|--------------|----------------------------------------|------------------------------------|
| 0            | Зарезервировано                        | Не используется.                   |
| 1            | Несрезанный кадр                       | Данные обычного кадра.             |
| 2            | Срезанный кадр (часть кадра)           | Первое из нескольких разделений.   |
| 3            | Срезанный кадр (часть кадра)           | Последующее разделение.            |
| 4            | Срезанный кадр (часть кадра)           | Дополнительные разделения.         |
| 5            | Срезанный кадр (ключевой)              | Независимый кадр.                  |
| 6            | SEI (Supplemental Enhancement Info)    | Метаданные (HDR, субтитры и т.д.). |
| 7            | SPS (Sequence Parameter Set)           | Глобальные параметры видео.        |
| 8            | PPS (Picture Parameter Set)            | Параметры групп кадров.            |
| 9            | Access Unit Delimiter                  | Маркер начала новой группы.        |
| 10           | Конец последовательности               | Завершение видеопоследовательности.|
| 11           | Конец битового потока                  | Завершение потока данных.          |
| 12           | Filler Data                            | Заполняющие данные.                |
| 13–23        | Зарезервировано                        | Для будущих версий.                |
| 24-27        | Несинтаксические данные                | Частные или нестандартные.         |
| 28-29        | Срезанные NAL-кадры                    | Используются для больших кадров.   |
| 30-31        | Несинтаксические данные                | Частные или нестандартные.         |

*Примечание*:  
1. Типы *NAL-юнитов* с 24 по 31 определены в стандарте, но в контексте основной спецификации они считаются "несинтаксическими" или зарезервированными.
Это означает, что стандарт допускает их использование для пользовательских нужд или расширений, но конкретной интерпретации для них в базовой версии *H.264* не предусмотрено.
2. Примерно соответствует разделу 7.3.1 оригинальной спецификации.

#### Расширенный формат заголовка
В расширенных случаях NAL-юнит может содержать дополнительные данные, такие как индикаторы длины, маркеры или специфические заголовки. Это может быть полезно в:
  + **RTP (Real-Time Protocol)**: для передачи видео по сети.
  + **Annex B**: формат хранения NAL-юнитов в потоках H.264.
Пример **Annex B**:
`[Start Code] [NAL Header] [RBSP Data]`
* Start Code: 3 байта (0x000001) или 4 байта (0x00000001), указывающие начало NAL-юнита.
* NAL Header: заголовок (1 байт).
* RBSP (Raw Byte Sequence Payload) Data: полезная нагрузка (например, закодированный макроблок).

#### Тело NAL-юнита (RBSP)
**RBSP** (*Raw Byte Sequence Payload* — полезная последовательность байтов):
Это общее название содержания данных.    
*RBSP* может включать специальные байты "emulation prevention bytes" (0x03), которые предотвращают конфликт с старт-кодами.  

Размер RBSP определяется контейнером или форматом передачи:
- **Annex B**:  
  - Используются старт-коды `0x000001` или `0x00000001` для обозначения начала NAL-юнита.  
  - Применяется для потоковой передачи или форматов вроде MPEG-TS.
- **RTP** (передача видео по сети):  
  - Длина указывается явно в заголовке RTP-пакета.
- **Контейнеры** (например, MP4/MKV):  
  - Заголовок контейнера содержит информацию о длине NAL-юнита.



## Энтропийное кодирование
После кодирования на транспортном уровне происходит Энтропийное кодирование на уровне битов (CABAC/CAVLC):
* **CAVLC** (Context-Adaptive Variable Length Coding): использует таблицы сжатия, чтобы заменить часто встречающиеся числа короткими кодами, а редкие — длинными. Простой алгоритм.
* **CABAC** (Context-Adaptive Binary Arithmetic Coding): использует вероятности появления данных для ещё более эффективного сжатия. более сложный алгоритм.

*CAVLC* проще и быстрее, поэтому используется в Baseline.  
*CABAC* эффективнее, но сложнее, поэтому доступен только в Main и High.

## Дополнительно
### H.264 профили и уровни:
* Профили (Baseline, Main, High) определяют, какие функции кодирования доступны.
   * Baseline: упрощённое кодирование (без B-кадров, без CABAC).
   * Main: добавлены B-кадры, CABAC.
   * High: ещё больше оптимизаций для качества.
* Уровни (Levels) ограничивают параметры видео (например, разрешение и битрейт), чтобы поддерживать совместимость с устройствами.

### Annex B
**Annex B** (*буквально: Приложение Б*) — это **дополнение к стандарту H.264**,
описывающее формат потока, в котором используются старт-коды для маркировки начала каждого NAL-юнита.
Этот формат часто используется для потоковой передачи или в .ts-файлах (MPEG Transport Stream).  
Его можно назвать "диалектом", так как он предлагает конкретный способ представления данных H.264 в потоке.  
Данный формат часто используется для потоковой передачи из-за простоты синхронизации с помощью старт-кодов.

### Документация
Копию стандарта можно получить на сайте [ITU](http://www.itu.int/rec/T-REC-H.264/e).

