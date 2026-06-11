# Flow Weaving — Implementation & Research Log

> Документ обновлён: 2026-06-10  
> Ветка: `flow_weaving_infill`  
> Последний коммит: `186d9ea857`

---

## 1. Концепция

**Flow Weaving** — infill-паттерн для FDM, создающий **межслойное механическое зацепление**
за счёт одновременной синусоидальной модуляции в двух осях:

- **XY ось**: модуляция ширины экструзии — чередование широких и узких сегментов
- **Z ось**: модуляция высоты — нозл двигается синусоидально вверх-вниз

Соседние слои используют противоположную фазу (0 vs π): где слой N широкий+высокий,
слой N+1 узкий+низкий. Широкие сегменты одного слоя **обхватывают** узкие соседнего —
эффект типа "ласточкин хвост".

### Теоретическое улучшение прочности

По литературе (Coffigniez et al. 2021, Kubalak et al. 2019, Luo et al. 2020):
- Z-interlocking: **+15–40%** прочности на разрыв по Z
- Модуляция ширины: **+5–10%** от увеличения площади контакта
- Комбинированно: ожидаемое **+20–50%** улучшения межслойного сцепления

---

## 2. Архитектура (текущее состояние)

### Двухфазная модуляция

```
Phase 1 — Fill Generation (FillFlowWeaving.cpp)
────────────────────────────────────────────────
• Генерирует базовые рекрилинейные линии (100% density)
• Делит каждую линию на sub-segments (~period/8)
• Модулирует ШИРИНУ per sub-segment:
    width = base_w × (1 + amplitude × sin(2π×pos/period + phase))
• safe_zone gating: точки вне fw_safe_expolygons → nominal width

Phase 2 — G-code Generation (GCode.cpp)  ← ТЕКУЩАЯ РЕАЛИЗАЦИЯ
────────────────────────────────────────
• Для каждой точки пути вычисляет Z:
    z = nominal_z + effective_amplitude × sin(phase_idx)
• Применяет fade envelope (FlowWeavingFadeEnvelope)
• Применяет XY-aware Z clamp (FlowWeavingZClamp)
• Выдаёт G1 X Y Z E (3-axis move)
```

### Параметры конфигурации

| Параметр | Ключ | Default | Описание |
|----------|------|---------|----------|
| XY амплитуда | `flow_weaving_xy_amplitude` | 15% | Модуляция ширины в % |
| Z амплитуда | `flow_weaving_z_amplitude` | 500% | Модуляция Z в % от layer height |
| Период | `flow_weaving_period` | 0.4mm | Длина волны (= диаметр нозла) |
| Z fade слои | `flow_weaving_z_fade_layers` | 3 | Слоёв для затухания у границ |
| Z tolerance | `flow_weaving_z_flow_tolerance` | 1 | Tolerance для Z clamp |
| Overlap degree | `flow_weaving_overlap_degree` | 0.2 | Асимметрия вниз (давление на предыдущий слой) |

---

## 3. Система безопасности (четырёхуровневая)

### Уровень 1 — Safe Zone (geometry-based)

Вычисляется в `Fill.cpp` при `Layer::make_fills()`:

```cpp
// Пересечение fill_no_overlap_expolygons всех слоёв в Z-диапазоне модуляции
fw_safe_expolygons = current_layer.fill_no_overlap_expolygons;
for (Layer* adj : all_layers_in_range(z - z_deflection, z + z_deflection)) {
    fw_safe_expolygons = intersection(fw_safe_expolygons, adj.fill_no_overlap_expolygons);
}
```

Результат: 2D-зона где нозл **гарантированно** внутри стен на КАЖДОМ посещаемом Z-уровне.

Хранится в `FillBase::fw_safe_expolygons` — поле добавлено нами в базовый класс Fill.

**Использование в XY (FillFlowWeaving.cpp):**
```cpp
Point midpt = sub_segment_midpoint;
bool inside_safe = any(ep.contains(midpt) for ep in fw_safe_expolygons);
taper = inside_safe ? 1.0 : 0.0;
// width = base_w × (1 + xy_amplitude × taper × sin(θ))
```

### Уровень 2 — Z Clamp (FlowWeavingZClamp)

XY-aware проверка: находится ли точка внутри shell-полигонов — если нет, Z зажимается
к nominal. Реализован в `FlowWeavingZClamp.hpp`.

### Уровень 3 — Fade Envelope (FlowWeavingFadeEnvelope)

Per-point walk по соседним слоям для вычисления fade-коэффициента:

```
n_above = слоёв выше где точка в stInternal (до первого stInternalSolid/shell)
n_below = слоёв ниже аналогично
layers_from_edge = min(n_above, n_below)
t = min(layers_from_edge, fade_n) / fade_n
fade = t² × (3 - 2t)  // smoothstep
effective_amplitude = amplitude × fade
```

Корректно обрабатывает наклонные поверхности (разный fade для разных XY-точек слоя)
и мостовые полы (stInternalBridge = стоп).

### Уровень 4 — Hard Clamp

`z ≥ 0.05mm`, `z ≤ ceiling_z`, `z ≥ floor_z`. Абсолютный страховочный net.

---

## 4. Файловая структура

```
src/libslic3r/
├── Fill/
│   ├── FillBase.hpp                — добавлено поле fw_safe_expolygons
│   ├── Fill.cpp                    — вычисление fw_safe_expolygons в make_fills()
│   └── FlowWeaving/
│       ├── CONCEPT.md              — концепция и референсы
│       ├── IMPLEMENTATION.md       — этот файл
│       ├── README.md               — быстрый справочник
│       ├── FillFlowWeaving.hpp     — декларация класса
│       ├── FillFlowWeaving.cpp     — XY модуляция + safe zone gating
│       ├── FlowWeavingZModulator.hpp   — Z синусоида + overlap_degree
│       ├── FlowWeavingFadeEnvelope.hpp — per-point fade compute
│       ├── FlowWeavingZClamp.hpp       — XY-aware Z clamping
│       ├── FlowWeavingContext.hpp      — debug diagnostics struct
│       └── FlowWeavingGCodeState.hpp   — aggregator для GCode.hpp
├── GCode.hpp                       — включает FlowWeavingGCodeState.hpp
├── GCode.cpp                       — Z-mod runtime (строки ~7596–7732)
└── PrintConfig.hpp/cpp             — все FW параметры
```

### Регистрация в сборке

Все 7 `.hpp` файлов `FlowWeaving/` прописаны в
`src/libslic3r/CMakeLists.txt` (строки 154–160).

### Регистрация типа Fill

`FillBase.cpp` → `Fill::new_from_type()`:
```cpp
case ipFlowWeaving: return new FillFlowWeaving();
```

---

## 5. Исследование сегодня (2026-06-10)

### 5.1 Code Style Audit — соответствие архитектуре Fill/

Изучены все fill-типы и паттерны:

**Прецедент субдиректории:**
- `Fill/Lightning/` — 6 файлов, самостоятельная технология → прецедент оправдан
- `Fill/FlowWeaving/` аналогично → субдиректория принята ✅

**Исправлено:**
- `CMakeLists.txt`: добавлены 3 недостающих `.hpp` + новый `FlowWeavingGCodeState.hpp`
- `GCode.hpp`: заменены 4 отдельных FW include на единый агрегатор

**Паттерн агрегирующего хедера** (по аналогии с `GCode/AdaptivePAProcessor.hpp`):
```cpp
// FlowWeavingGCodeState.hpp — единственный include в GCode.hpp
#include "FlowWeavingContext.hpp"
#include "FlowWeavingZModulator.hpp"
#include "FlowWeavingFadeEnvelope.hpp"
#include "FlowWeavingZClamp.hpp"
```

### 5.2 Анализ FillBase.hpp — что уже есть в базовом классе

| Поле Fill | Доступно в fill_surface_extrusion() |
|-----------|-------------------------------------|
| `layer_id` | ✅ |
| `z` | ✅ (print_z текущего слоя) |
| `spacing`, `overlap`, `angle` | ✅ |
| `bounding_box` | ✅ |
| `print_config` | ✅ → **все параметры принтера** |
| `print_object_config` | ✅ |
| `no_overlap_expolygons` | ✅ (текущий слой) |
| `fw_safe_expolygons` | ✅ (наше поле) |

`FillParams` дополнительно:
- `flow` → width, height, nozzle, mm3_per_mm
- `config` → `PrintRegionConfig*` → все FW параметры
- `layer_height`
- `extrusion_role`

**Вывод**: `FillFlowWeaving::fill_surface_extrusion()` имеет доступ
ко ВСЕМ данным, необходимым для вычисления Z-модуляции.

### 5.3 Обнаружение ZAA-механизма (ContourZ.cpp)

Найден существующий паттерн **Z Anti-Aliasing (ZAA)** в `ContourZ.cpp`:

```cpp
// ContourZ.cpp — записывает Z в точки polyline:
path.polyline = move(polyline_with_3d_z_offsets);
path.z_contoured = true;  // flag на ExtrusionPath

// GCode.cpp строка 7619 — уже обрабатывает это:
if (path.z_contoured) {
    coordf_t z_diff = unscale_(line.b.z());  // читает z из Point3
    double z = m_nominal_z + z_diff;
    m_writer.extrude_to_xyz(Vec3d(x, y, z), dE, ...);
}
```

`FillFlowWeaving` уже создаёт `Point3(sub_b, 0)` с `z = 0`.
Нужно заменить `0` на вычисленное Z-смещение и поставить `z_contoured = true`.

**Конвенция**: `scale_(z_offset_mm)` → `point.z()`, `unscale_(point.z())` в GCode.

### 5.4 Анализ оверинжениринга в GCode.cpp

Сравнение текущей реализации с тем, что уже есть в Fill:

| Компонент в GCode.cpp | Дублирует | Может быть заменён |
|-----------------------|-----------|-------------------|
| `FlowWeavingZModulator` | Логику `accumulated` из FillFlowWeaving | `point.z()` + ZAA path |
| `FlowWeavingFadeEnvelope` | `fw_safe_expolygons` inside_safe check | Тест `fw_safe_expolygons` в Fill |
| `FlowWeavingZClamp` | `!inside_safe → z=0` | Тривиальный `if` в Fill |
| `FlowWeavingContext` | (debug only) | Inline в FillFlowWeaving |

**Заключение**: текущая GCode-реализация (строки 7596–7732, ~80 строк) является
оверинжинирингом. Весь этот код можно переместить в `FillFlowWeaving::fill_surface_extrusion()`,
используя стандартный ZAA путь GCode.cpp.

---

## 6. Запланированный рефакторинг (следующий этап)

### Цель

Переместить всю Z-логику из GCode.cpp в `FillFlowWeaving::fill_surface_extrusion()`.
GCode.cpp обрабатывает через стандартный ZAA-путь (`path.z_contoured`).

### Изменения в FillFlowWeaving::fill_surface_extrusion()

```cpp
// После вычисления inside_safe и taper (уже есть для XY):
double z_phase = M_PI * (fw_phase_idx % 2);  // совпадает с XY-фазой
double z_amp_pct = params.config->flow_weaving_z_amplitude.value;
double z_amp = (z_amp_pct / 100.0) * base_h;

// Overlap degree: асимметрия вниз
double overlap_deg = params.config->flow_weaving_overlap_degree.value;
double pos_t = std::sin(2.0*M_PI*pos/fw_period + z_phase);
double z_sym = pos_t >= 0 ? pos_t : pos_t * (1.0 + overlap_deg);

// Z fade = тот же taper что и для XY (из fw_safe_expolygons)
double z_offset = z_amp * taper * z_sym;

// Clamp к слою
z_offset = std::clamp(z_offset, -base_h, base_h);

// Запись в точку
Point3 pt3(sub_b, scale_(z_offset));  // z в scaled units
ExtrusionPath path(...);
path.polyline.points = { Point3(sub_a, ...), pt3 };
path.z_contoured = true;  // ← GCode.cpp автоматически использует ZAA-путь
```

### Что удаляется

- `FlowWeavingZModulator.hpp` — весь файл
- `FlowWeavingFadeEnvelope.hpp` — весь файл
- `FlowWeavingZClamp.hpp` — весь файл
- `FlowWeavingContext.hpp` — весь файл
- `FlowWeavingGCodeState.hpp` — агрегатор больше не нужен
- GCode.cpp строки 7639–7732 — ~80 строк FW-логики
- `m_fw_z_mod`, `m_fw_fade` — члены класса GCode

### Что остаётся в GCode.cpp

Практически ничего FW-специфичного. Arc fitting guard:
```cpp
// БЫЛО: || fw_z_active
// СТАЛО: уже есть path.z_contoured в условии — ничего добавлять не нужно
```

**Итог**: 80 строк GCode.cpp → 15 строк в FillFlowWeaving, через стандартный ZAA-путь.

### Проверки перед реализацией

1. **Кросс-путевая фаза**: каждый `ExtrusionMultiPath` = одна infill-линия.
   Фаза привязана к XY-позиции вдоль линии, не к глобальной дистанции → OK.

2. **Z-масштаб**: использовать ту же конвенцию что ZAA:
   - запись: `scale_(z_offset_mm)` → `Point3.z()`
   - чтение: `unscale_(line.b.z())` в GCode.cpp

3. **Arc fitting guard**: уже содержит `path.z_contoured` → автоматически работает.

4. **overlap_degree**: переезжает вместе с Z-вычислением.

5. **gcode_comments debug**: если нужны FW debug комментарии — добавить опциональный
   кастомный комментарий в ExtrusionPath или убрать совсем.

---

## 7. История реализации

### Этапы разработки

| Дата | Что сделано |
|------|-------------|
| 2026-06-07/08 | Прототип: XY модуляция, базовая Z-модуляция в GCode.cpp |
| 2026-06-08 | Benchy тест: обнаружены проблемы на границах (deck, hull) |
| 2026-06-08 | Safe zone через fw_safe_expolygons в Fill.cpp |
| 2026-06-09 | XY-aware Z clamp (FlowWeavingZClamp), endpoint taper |
| 2026-06-09 | Анализ G-code: отладка и верификация Z-модуляции |
| 2026-06-09 | Dead code cleanup |
| 2026-06-10 | Code style audit → CMakeLists fix, GCode.hpp агрегатор |
| 2026-06-10 | FadeEnvelope (per-point XY fade) |
| 2026-06-10 | overlap_degree параметр |
| 2026-06-10 | Анализ FillBase + ZAA → обнаружен оверинжениринг GCode |
| **TODO** | Рефакторинг: Z-логика → FillFlowWeaving + ZAA path |
| **TODO** | GUI слайдер для overlap_degree |
| **TODO** | Физические тесты прочности |

### Ключевые баги и решения

**Проблема**: Z-модуляция выходит за пределы стен на Benchy hull  
**Причина**: Стены Benchy наклонные — на Z-уровне выше wall уже, но safe zone это не учитывала  
**Решение**: `fw_safe_expolygons` = intersection across ALL layers in Z-range

**Проблема**: Нозл бьёт deck Benchy (промежуточная сплошная поверхность)  
**Причина**: Z clamp считал от верха модели, не от ближайшего shell  
**Решение**: FlowWeavingZClamp — XY-aware walk до ближайшего shell по соседним слоям

**Проблема**: Резкие Z-скачки на границах infill  
**Причина**: Нет плавного затухания на первых/последних слоях infill  
**Решение**: FlowWeavingFadeEnvelope — per-point XY walk, smoothstep(n/fade_n)

**Проблема**: combine_infill ломает phase alternation  
**Причина**: `layer_id % 2` при combine=2 — все infill-слои нечётные → одна фаза  
**Решение**: `combine_step = path.height / layer_height`, `fw_phase_idx = layer_id / combine_step`

---

## 8. Референсы

### Академические

- Coffigniez et al. (2021) — Non-planar FDM toolpaths for improved interlayer bonding
- Kubalak et al. (2019) — Multi-axis FDM for mechanical interlock
- Luo et al. (2020) — Sinusoidal nozzle path for improved Z-strength

### Кодовая база OrcaSlicer

| Файл | Роль | Чему учит |
|------|------|-----------|
| `ContourZ.cpp` | ZAA (Z Anti-Aliasing) | Как хранить Z в `Point3.z()` и использовать `z_contoured` |
| `Fill/Lightning/` | Субдиректория fill | Прецедент для нашей `Fill/FlowWeaving/` |
| `GCode/AdaptivePAProcessor.hpp` | Агрегирующий хедер | Паттерн для `FlowWeavingGCodeState.hpp` |
| `Fill/FillBase.hpp` | Базовый класс | Все поля доступные любому Fill |
| `Fill/Fill.cpp` | `make_fills()` | Как подготовить данные (fw_safe_expolygons) |
| `Fill/FillRectilinear.hpp` | Рекрилинейный fill | Базовый паттерн для наследования |

### Важные сигнатуры

```cpp
// Базовый класс
class Fill {
    size_t layer_id;           // индекс слоя
    coordf_t z;                // print_z
    const PrintConfig* print_config;
    const PrintObjectConfig* print_object_config;
    ExPolygons no_overlap_expolygons;  // текущий слой
    ExPolygons fw_safe_expolygons;     // наш: пересечение по Z-диапазону

    virtual void fill_surface_extrusion(
        const Surface*, const FillParams&, ExtrusionEntitiesPtr&);
};

// Params при вызове
struct FillParams {
    Flow flow;                     // width, height, mm3_per_mm
    const PrintRegionConfig* config; // все FW параметры
    coordf_t layer_height;
    ExtrusionRole extrusion_role;
};

// ZAA-путь в GCode.cpp
if (path.z_contoured) {
    coordf_t z_diff = unscale_(line.b.z()); // читает из Point3
    m_writer.extrude_to_xyz(Vec3d(x, y, m_nominal_z + z_diff), dE, ...);
}
```
