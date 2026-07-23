# FlowWeaving — Technical Implementation

## 1. File Structure

All FlowWeaving logic is self-contained in `src/libslic3r/Fill/FlowWeaving/`:

```
Fill/FlowWeaving/
├── README.md                  — overview and usage
├── CONCEPT.md                 — research, hypotheses, future plans
├── IMPLEMENTATION.md          — this file
├── FillFlowWeaving.hpp        — class declaration
├── FillFlowWeaving.cpp        — main algorithm
├── FlowWeavingZModulator.hpp  — wave function abstraction (header-only)
├── FlowWeavingContext.hpp     — parameter context
├── FlowWeavingFadeEnvelope.hpp — wall-taper logic
└── FlowWeavingZClamp.hpp      — Z safety clamp helpers
```

**Changes outside this directory** (all minimal and justified):

| File | Change | Reason |
|---|---|---|
| `Fill/FillBase.hpp` | + `infill_layers_above` field | top-taper data propagation |
| `Fill/Fill.cpp` | + counter in `make_fills()` | populate `infill_layers_above` |
| `libslic3r/PrintConfig.hpp` | + 7 parameters declared | config system |
| `libslic3r/PrintConfig.cpp` | + 7 parameter definitions | label/tooltip/range/default |
| `libslic3r/Preset.cpp` | + 7 keys in `s_Preset_print_options` | preset save/load |
| `GUI/Tab.cpp` | + 7 `append_single_option_line` | UI panel |
| `GUI/ConfigManipulation.cpp` | + in 2 toggle loops | show/hide when FlowWeaving selected |
| `GUI/GUI_Factories.cpp` | + in `PART_CATEGORY_SETTINGS` | modifier object access |

---

## 2. Core Algorithm: FillFlowWeaving::fill_surface_extrusion()

### 2.1 Entry and Setup

```
fill_surface_extrusion(surface, params, out)
  1. Extract parameters from params.config:
     - z_amp_frac = flow_weaving_z_amplitude / 100
     - xy_amp_frac = flow_weaving_xy_amplitude / 100
     - xy_path_amp_mm = flow_weaving_xy_path_amplitude
     - period_mm = flow_weaving_period
     - phase_offset = flow_weaving_phase_offset
     - z_overlap_pct = flow_weaving_z_overlap
     - top_taper_n = flow_weaving_top_taper_layers

  2. Call parent FillRectilinear::fill_surface() to get flat polylines

  3. Compute layer-level phase:
     same_dir_idx = layer_id / 2
     layer_base_phase = (layer_id % 2 == 0 ? 0 : π)
                      + same_dir_idx × phase_offset × 2π

  4. Compute global_perp: direction perpendicular to fill angle
     (for XY lateral displacement)

  5. Build cross-layer safe zone:
     safe_zone = intersect(no_overlap_expolygons, no_overlap_above, no_overlap_below)
```

### 2.2 Per-Polyline Processing

```
for each polyline in polylines:
  phase_rad = layer_base_phase + (poly_idx % 2 == 1 ? π : 0)
  // ↑ odd lines are antiphase to even lines within same layer

  measure total_len_mm (for wall taper)

  pos_mm = 0  // accumulated position along polyline

  for each segment [pa, pb]:
    for each sub-step (n_steps = ceil(seg_len / step_mm)):

      compute t_mod_{start,end,mid} = sin(2π × pos / period + phase_rad)
      compute taper_val = smoothstep from wall edges
      compute gate = 1 if mid-point is in safe_zone, else 0

      // ── Z modulation ──
      z_start = z_amp_frac × layer_h × t_mod_start × taper_val × gate
      z_end   = z_amp_frac × layer_h × t_mod_end   × taper_val × gate

      // Lower bound safety:
      overlap_limit  = -(layer_h × z_overlap_pct / 100)
      absolute_floor = -(this->z - first_layer_h)
      min_z_diff = max(overlap_limit, absolute_floor)
      z_start = max(z_start, min_z_diff)
      z_end   = max(z_end,   min_z_diff)

      // Upper taper near top surface:
      x = min(infill_layers_above / top_taper_n, 1.0)
      upper_taper = x² × (3 - 2x)   // smoothstep
      max_z_up = z_amp_frac × layer_h × upper_taper
      z_start = min(z_start, max_z_up)
      z_end   = min(z_end,   max_z_up)

      // ── XY modulation ──
      xy_off = lateral_amp_mm × t_mod × taper_val
      // Shift points perpendicular to travel via global_perp

      // ── Width/flow modulation ──
      width_delta = xy_amp_frac × flow_width × t_mod_mid × taper_val × gate
      effective_width = clamp(flow_width + width_delta, 0.25×flow_width, nozzle_d)
      sub_mm3 = flow_mm3_per_mm × (effective_width / flow_width)

      // ── Emit sub-segment ──
      base_path = ExtrusionPath(role, sub_mm3, flow_width, flow_height)
      contoured = ExtrusionPathContoured(base_path, poly3_with_z, z_contoured=true)
      // collect into ExtrusionMultiPath
```

### 2.3 Output Collection

Sub-segments for one polyline → `ExtrusionMultiPath` → pushed to `ExtrusionEntityCollection` → added to `out`.

GCode.cpp receives standard `z_contoured` paths and emits `G1 X Y Z E` with automatic E-compensation via:
```cpp
extrusion_ratio = (path.height + z_diff) / path.height
```

---

## 3. Wave Modulator Architecture

`FlowWeavingZModulator` is an abstract base with a factory. Currently one implementation:

```cpp
class SineModulator : public FlowWeavingModulator {
    double compute(pos, period, phase_rad) const override {
        return sin(2π × pos / period + phase_rad);
    }
};
```

The `taper()` method (shared by all subclasses) returns smoothstep distance from wall:
```
dist_from_wall = min(pos, total_len - pos)
if dist < taper_len: smoothstep(dist / taper_len)
else: 1.0
```

**Adding new wave types:**
1. Add `SquareModulator`, `PerlinModulator` etc. as subclasses
2. Add entry to `ModulatorType` enum
3. Add case to `FlowWeavingModulator::create()` factory
4. Add config parameter to select type

---

## 4. Z-Safety System

### 4.1 Lower Bound (implemented)

Two limits, stricter one applies:

```
overlap_limit  = -(layer_h × z_overlap_pct / 100)
absolute_floor = -(this->z - first_layer_h)
min_z_diff = max(overlap_limit, absolute_floor)
```

The `absolute_floor` is the exact distance from current nominal_z to the top of the first layer. On all layers except the very first few infill layers, `overlap_limit` is the binding constraint.

**Example (layer_h=0.2, first_layer_h=0.2, z_overlap=50%):**
- `overlap_limit = -0.10mm`
- Layer 1 (z=0.30): `absolute_floor = -0.10mm` → both equal → limit is -0.10mm
- Layer 2 (z=0.40): `absolute_floor = -0.20mm` → `overlap_limit = -0.10mm` wins
- All upper layers: `overlap_limit = -0.10mm` always wins

### 4.2 Upper Bound (implemented)

```
x = clamp(infill_layers_above / top_taper_n, 0, 1)
upper_taper = x² × (3 - 2x)   // smoothstep ∈ [0, 1]
max_z_up = z_amp_frac × layer_h × upper_taper

z_diff = min(z_diff, max_z_up)  // only caps positive values
```

`infill_layers_above` is populated by `Fill.cpp::make_fills()`:
```cpp
// Count consecutive infill layers above, up to MAX_TAPER_LOOK=10
int above_count = 0;
for (size_t li = layer_idx + 1; li < layers.size() && above_count < 10; ++li) {
    bool has_infill = false;
    for (const LayerRegion* lr : layers[li]->regions())
        if (!lr->fill_no_overlap_expolygons.empty()) { has_infill = true; break; }
    if (!has_infill) break;
    ++above_count;
}
f->infill_layers_above = above_count;
```

---

## 5. Cross-Layer Safe Zone

The modulation gate prevents Z/XY modulation near perimeters (wall overlap zone). To avoid the wave pattern from poking into adjacent layers' solid regions:

```
safe_zone = no_overlap_expolygons   // current layer boundary
if (no_overlap_above not empty):
    safe_zone = intersect(safe_zone, no_overlap_above)
if (no_overlap_below not empty):
    safe_zone = intersect(safe_zone, no_overlap_below)

gate = point_in_polygon(sub_segment_midpoint, safe_zone) ? 1.0 : 0.0
```

`no_overlap_above` / `no_overlap_below` are populated by `make_fills()` only for fills where `needs_cross_layer_data()` returns `true`. `FillFlowWeaving` overrides this to return `true`.

---

## 6. Parameter Registration Pipeline

Every new FlowWeaving parameter must be registered in **7 locations**:

```
1. PrintConfig.hpp   — macro: ((ConfigOptionFloat, flow_weaving_xxx))
2. PrintConfig.cpp   — definition: label, tooltip, sidetext, min, max, mode, default
3. Preset.cpp        — s_Preset_print_options[] string list
4. Tab.cpp           — optgroup->append_single_option_line("flow_weaving_xxx")
5. ConfigManipulation.cpp line ~650  — toggle_line loop (print tab)
6. ConfigManipulation.cpp line ~996  — toggle_line loop (object tab)
7. GUI_Factories.cpp — PART_CATEGORY_SETTINGS modifier list
8. FillFlowWeaving.cpp — params.config->flow_weaving_xxx.value
```

---

## 7. GCode Output Analysis

### 7.1 Test File: Molding-FWeav.gcode (June 2026)

Settings: `z_amp=95%, period=1mm, xy_amp=50%, z_overlap=50%, phase_offset=0.5`

```
Total XYZE moves: 38,018
Z range: [0.3000 .. 3.9900]mm
Z < first_layer_h violations: 0 ✓
Expected span: 0.19 + 0.10 = 0.29mm
Actual span:   0.29mm ✓
```

### 7.2 Per-Layer Pattern

The pattern is a predictable alternation between "all up" and "all down" layers when viewing aggregate min/max:
- This is correct — it's the phase_offset=0.5 doing its job
- Even lines in a layer are in phase, odd lines are antiphase
- Adjacent layers are in antiphase to each other
- Result: layer N peaks nest into layer N+1 valleys

### 7.3 Observed Issue: Top Layer Overshoot

**Problem found in real print:** Nozzle protruded into top solid shell.
**Root cause:** z_amp = +0.190mm applied uniformly including last infill layer.
**Fix:** `flow_weaving_top_taper_layers` smooth ceiling (implemented).

---

## 8. ZAA Mechanism in GCode.cpp

FlowWeaving leverages the existing ZAA (Z Adaptive Architecture) in GCode.cpp.
**No changes were made to GCode.cpp.** The relevant existing code:

```
Line ~6867: if (path.z_contoured) compute first z for travel
Line ~6882: if (path.z_contoured) adjust Z before extrusion starts
Line ~6889: if (!path.z_contoured) reset Z after contoured block
Line ~7507: arc fitting disabled for z_contoured paths
Line ~7527: z_diff emission for non-variable-speed paths
Line ~7735: z_diff emission for variable-speed paths
```

E-compensation formula (line ~7527):
```cpp
double extrusion_ratio = (path.height + z_diff) / path.height;
// When nozzle dips: z_diff < 0, ratio < 1, less E extruded
// When nozzle rises: z_diff > 0, ratio > 1, more E extruded
```

---

## 9. Agent Working Rules

> These rules apply to all AI agents working on this codebase.

1. **Explain before changing:** Always describe planned changes and wait for user confirmation before editing files.

2. **No commits without explicit request:** Never propose `git commit` automatically. Only commit when user explicitly asks.

3. **No magic constants:** Never hardcode values like `0.1`, `MIN_SAFE_Z = 0.1`. Derive all limits from print structure parameters (`layer_h`, `first_layer_h`, `nozzle_d`, etc.).

4. **7-file parameter pipeline:** Every new FlowWeaving config parameter must be registered in all 7 locations listed in Section 6.

5. **No base class pollution:** All FlowWeaving-specific logic stays in `Fill/FlowWeaving/`. Do not add FlowWeaving fields to `ExtrusionEntity.hpp`, `GCode.cpp`, or other base classes unless absolutely unavoidable.

6. **Isolation principle:** The design goal is zero changes to `GCode.cpp` and `ExtrusionEntity.hpp`. Use existing mechanisms (`z_contoured`, `mm3_per_mm`, `ExtrusionPathContoured`).

---

## 10. Phase Alternation Implementation

Within a layer, odd-numbered infill lines carry a π-phase shift:

```cpp
// layer_base_phase: alternates between layers for interlocking
const size_t same_dir_idx     = this->layer_id / 2;
const double layer_base_phase = ((this->layer_id % 2 == 0) ? 0.0 : M_PI)
                              + same_dir_idx * phase_offset * 2.0 * M_PI;

// Per-polyline: odd lines are antiphase to even lines
const double phase_rad = layer_base_phase + ((poly_idx % 2 != 0) ? M_PI : 0.0);
```

Result:
- Even lines: wave `[−amp .. +amp]`
- Odd lines: wave `[+amp .. −amp]` (antiphase)
- Adjacent same-direction layers: base_phase shifts by `phase_offset × 2π`

This produces the zipper interlock pattern described in CONCEPT.md Section 3.2.

---

## 11. Architecture Decisions & Rationale

### Decision 1: Sub-segmentation over flow_factors[]
**Rejected:** `flow_factors[]` array in `ExtrusionPathContoured` (one factor per point).  
**Reason:** Pollutes base class with FlowWeaving-specific data — violates isolation principle.  
**Chosen:** Split each infill line into N short `ExtrusionPathContoured` sub-segments. Each has its own `mm3_per_mm` (standard existing field). Zero changes to `ExtrusionEntity.hpp`.

### Decision 2: Re-use ZAA `z_contoured` mechanism
**Why:** `GCode.cpp` already handles Z-contoured paths with `extrusion_ratio = (height + z_diff) / height`. E-compensation is automatic. No duplication of Z-emission logic.  
**Result:** Zero changes to `GCode.cpp` needed for Z modulation.

### Decision 3: first_layer_h as absolute floor, not hardcoded constant
**Rule:** No magic numbers. All limits derived from print structure:
- `first_layer_h = print_config->initial_layer_print_height.value`
- `layer_h = params.layer_height`
- `nozzle_d = params.flow.nozzle_diameter()`

### Decision 4: `infill_layers_above` counter in make_fills()
**Why:** Top taper requires knowing how many infill layers are above. Fill object only sees its own layer. `make_fills()` already iterates all layers — minimal cost to add a counter. Cap at `MAX_TAPER_LOOK=10` to limit loop cost.

### Decision 5: Smoothstep over linear taper
**Why:** Linear taper creates visible discontinuity in wave amplitude (derivative is discontinuous at boundaries). Smoothstep `f(x) = 3x² - 2x³` has zero derivative at both endpoints → transition is invisible in G-code output.

### Decision 6: Global perp direction for XY displacement
**Why:** Per-segment perpendicular flips direction on zigzag infill (odd lines reverse travel direction). Using the global fill-angle perpendicular ensures the XY wave goes consistently in the same spatial direction regardless of line parity. Without this, the XY displacement cancels out and produces no visible wave.

