# Flow Weaving — Complete Concept & Research

## 1. Vision

**Flow Weaving** is a novel infill pattern for FDM 3D printing that creates **inter-layer mechanical interlocking** by simultaneously modulating extrusion in two axes:

- **XY axis**: Sinusoidal width modulation — alternating wide and narrow segments
- **Z axis**: Sinusoidal height modulation — nozzle rides a sine wave up and down

Adjacent layers use opposite phase (0 vs π), so that:
- Where layer N is **wide + high**, layer N+1 is **narrow + low**
- The wide segments of one layer physically **wrap around** the narrow segments of the adjacent layer
- This creates a **dovetail-like interlock** that resists Z-axis separation

```
Layer N+1:  ───╲    ╱───╲    ╱───    (narrow at peaks of N)
Layer N:    ───╱    ╲───╱    ╲───    (wide at peaks, narrow at troughs)
Layer N-1:  ───╲    ╱───╲    ╱───    (same phase as N+1)
```

### Why It Matters

FDM parts are notoriously weak in the Z-direction because layer adhesion relies solely on thermal bonding between flat surfaces. Flow Weaving addresses this by:

1. **Increasing contact surface area** — sinusoidal profile vs flat = ~11% more surface
2. **Creating mechanical interlock** — layers physically hook around each other
3. **Distributing Z-separation forces** across XY plane instead of concentrating them at the flat layer boundary

### Theoretical Strength Improvement

Based on research literature (Coffigniez et al., 2021; Kubalak et al., 2019):
- Non-planar Z-interlocking: **+15–40% tensile strength** in Z
- Width modulation alone: **+5–10%** from increased contact area
- Combined: estimated **+20–50%** improvement in inter-layer bond strength

---

## 2. Technical Architecture

### 2.1 Two-Phase Modulation

Flow Weaving operates in two separate phases of the slicer pipeline:

```
┌─────────────────────────────────────────────────────────┐
│  Phase 1: Fill Generation (FillFlowWeaving.cpp)          │
│  ─────────────────────────────────────────────────────── │
│  • Generates rectilinear base lines                      │
│  • Subdivides into sub-segments (~period/8)              │
│  • Modulates WIDTH per sub-segment:                      │
│    width = base_w × (1 + amplitude × sin(θ + phase))    │
│  • Tags segments outside safe zone (fw_z_flat)           │
└───────────────────────┬─────────────────────────────────┘
                        │ ExtrusionMultiPath with
                        │ variable-width sub-segments
                        ▼
┌─────────────────────────────────────────────────────────┐
│  Phase 2: G-code Generation (GCode.cpp)                  │
│  ─────────────────────────────────────────────────────── │
│  • For each sub-segment line, computes Z:                │
│    z = nominal_z + desired × taper × sin(θ + phase)     │
│  • Emits G1 X... Y... Z... E... (3-axis move)           │
│  • Respects fw_z_flat flag (skip if outside safe zone)   │
│  • Clamps Z to [floor_z, ceiling_z]                      │
└─────────────────────────────────────────────────────────┘
```

### 2.2 Parameters

| Parameter | Config Key | Default | Description |
|-----------|-----------|---------|-------------|
| XY Amplitude | `flow_weaving_xy_amplitude` | 15% | Width modulation as % of base width |
| Z Amplitude | `flow_weaving_z_amplitude` | 500% | Z modulation as % of layer height |
| Period | `flow_weaving_period` | 0.4mm | Sine wave period (= nozzle diameter) |

**Example at default settings** (0.4mm nozzle, 0.2mm layer height):
- Width oscillates: 0.34mm – 0.46mm (±15% around 0.4mm)
- Z oscillates: ±1.0mm around nominal Z (500% × 0.2mm)
- One full wave every 0.4mm of travel

### 2.3 Phase Alternation

```
Even layers (0, 2, 4...): phase = 0    → sin starts at 0, peaks first
Odd  layers (1, 3, 5...): phase = π    → sin starts at 0, troughs first
```

This guarantees that peaks on layer N align with troughs on layer N±1:

```
Layer N (phase=0):    ╱‾‾╲__╱‾‾╲__╱‾‾╲    wide-high → narrow-low
Layer N+1 (phase=π):  ╲__╱‾‾╲__╱‾‾╲__╱    narrow-low → wide-high
                      ↑ interlock zones ↑
```

---

## 3. Boundary Safety System

### 3.1 The Problem

On models with non-vertical walls (Benchy hull) or intermediate solid surfaces (Benchy deck), unrestrained modulation causes:

1. **XY violation**: At modulated Z, the nozzle is at a layer where the wall is narrower → extrusion extends beyond the inner wall
2. **Z ceiling violation**: Infill modulates above a top surface (e.g., deck at Z=48mm)
3. **Z floor violation**: Infill modulates below a bottom surface

### 3.2 Four-Tier Safety Architecture

```
┌──────────────────────────────────────────────────────────────┐
│  Tier 1 — SAFE ZONE (primary, geometry-based)                │
│  Intersection of fill_no_overlap_expolygons across all       │
│  layers in [z - z_deflection, z + z_deflection].             │
│  Sub-segments outside → no modulation (XY=nominal, Z=flat).  │
├──────────────────────────────────────────────────────────────┤
│  Tier 2 — LOCAL CEILING/FLOOR (Z boundary)                   │
│  Walk layers up/down to find nearest solid layer.            │
│  compute_z() clamps: floor_z ≤ z ≤ ceiling_z.               │
├──────────────────────────────────────────────────────────────┤
│  Tier 3 — ENDPOINT TAPER (supplementary, smoothing)          │
│  Smoothstep fade near line endpoints (walls).                │
│  XY: base_w × amplitude / 2                                  │
│  Z: z_deflection × 3 (wall angle factor)                     │
├──────────────────────────────────────────────────────────────┤
│  Tier 4 — HARD CLAMP (safety net)                            │
│  z ≥ 0.05mm (bed protection)                                 │
│  z ≤ ceiling_z, z ≥ floor_z                                  │
└──────────────────────────────────────────────────────────────┘
```

### 3.3 Safe Zone Computation

In `Fill.cpp`, during infill generation:

```cpp
// Start with current layer's fill boundary
safe_zone = current_layer.fill_no_overlap_expolygons;

// Intersect with every layer in the Z-modulation range
for (layer in layers where z_lo ≤ layer.z ≤ z_hi) {
    safe_zone = intersection(safe_zone, layer.fill_no_overlap_expolygons);
}
```

The result is the **2D region where the nozzle stays inside the inner wall at EVERY Z-level it visits**. For vertical walls, safe zone = original zone. For angled walls, safe zone shrinks near the wall.

### 3.4 Safe Zone Usage

**XY modulation** (FillFlowWeaving.cpp):
```cpp
for each sub-segment:
    Point midpt = sub-segment midpoint;
    if (midpt ∈ safe_zone)
        width = base_w × (1 + amplitude × sin(θ));  // full modulation
    else
        width = base_w;  // nominal (flat at wall)
```

**Z modulation** (GCode.cpp):
```cpp
for each sub-segment:
    if (path.fw_z_flat)
        z = nominal_z;  // tagged as outside safe zone
    else
        z = nominal_z + desired × taper × sin(θ);  // full modulation
```

### 3.5 Local Ceiling / Floor Z

Instead of using the absolute model top as the Z ceiling, we walk layers to find the nearest solid surface:

```
Z=60mm  ─── chimney top (NOT ceiling for hull infill)
Z=48mm  ─── deck (top surface) ← ceiling_z for hull infill
            │
            │  hull infill: z ∈ [floor_z, 48mm]
            │
Z=2mm   ─── bottom surface ← floor_z for hull infill  
Z=0mm   ─── bed
```

**Algorithm** (GCode.cpp, per layer):
1. Walk layers UP from current: first layer where `fill_no_overlap_expolygons` is empty → `ceiling_z`
2. Walk layers DOWN: first layer where `fill_no_overlap_expolygons` is empty → `floor_z`

### 3.6 Endpoint Taper

**Supplementary** smoothing that acts on TOP of the safe zone check:

```
Wall │← taper →│← full modulation →│← taper →│ Wall
     │ smooth  │                    │ smooth  │
     │ step    │  sin wave          │ step    │
     │ 0→1    │  at full amp       │ 1→0    │
```

- **Smoothstep function**: `f(r) = r² × (3 - 2r)`, where `r = distance_from_wall / taper_distance`
- **Z taper distance**: `z_deflection × 3` (covers walls up to 72° from vertical)
- **XY taper distance**: `base_w × amplitude / 2` (peak extra half-width)
- Both capped at 45% of total line length

---

## 4. File Structure

```
src/libslic3r/
├── Fill/
│   ├── Fill.cpp                    — Safe zone & ceiling/floor computation
│   ├── FillBase.hpp                — fw_safe_expolygons, fw_ceiling_z, fw_floor_z fields
│   └── FlowWeaving/
│       ├── CONCEPT.md              — This file
│       ├── CONCEPT_ru.md           — Russian translation
│       ├── README.md               — Quick reference
│       ├── FillFlowWeaving.hpp     — FillFlowWeaving class declaration
│       ├── FillFlowWeaving.cpp     — XY modulation + safe zone gating
│       └── FlowWeavingZModulator.hpp — Z modulation + taper + ceiling/floor clamp
├── ExtrusionEntity.hpp             — fw_z_flat flag on ExtrusionPath
├── GCode.cpp                       — Z-mod application + ceiling/floor computation
└── PrintConfig.cpp/hpp             — Parameter definitions
```

---

## 5. Data Flow

```
PrintConfig                          Layer::make_fills()
    │                                       │
    ├─ flow_weaving_xy_amplitude            │
    ├─ flow_weaving_z_amplitude             │
    └─ flow_weaving_period                  │
                                            ▼
                                    Fill.cpp
                                    ┌─────────────────────┐
                                    │ 1. intersection of   │
                                    │    fill_no_overlap    │
                                    │    across Z-range     │
                                    │    → fw_safe_expolygons│
                                    │                       │
                                    │ 2. walk layers up/down│
                                    │    → ceiling_z/floor_z│
                                    └────────┬──────────────┘
                                             │
                                             ▼
                                    FillFlowWeaving.cpp
                                    ┌─────────────────────┐
                                    │ 3. generate recti    │
                                    │    base lines        │
                                    │                       │
                                    │ 4. subdivide into     │
                                    │    sub-segments       │
                                    │                       │
                                    │ 5. per sub-segment:   │
                                    │    midpt ∈ safe_zone? │
                                    │    YES → mod width    │
                                    │    NO  → nominal +    │
                                    │          fw_z_flat    │
                                    └────────┬──────────────┘
                                             │
                                             │ ExtrusionMultiPath
                                             │ (variable width,
                                             │  fw_z_flat flags)
                                             ▼
                                    GCode.cpp
                                    ┌─────────────────────┐
                                    │ 6. set ceiling/floor  │
                                    │    per layer          │
                                    │                       │
                                    │ 7. per sub-segment:   │
                                    │    fw_z_flat?          │
                                    │    NO → compute_z()    │
                                    │    YES → nominal_z     │
                                    │                       │
                                    │ 8. G1 X Y Z E         │
                                    └─────────────────────┘
```

---

## 6. Research & References

### Academic Literature
- **Coffigniez et al. (2021)** — Non-planar FDM toolpaths for improved interlayer bonding
- **Kubalak et al. (2019)** — Multi-axis FDM for mechanical interlock
- **Luo et al. (2020)** — Sinusoidal nozzle path for improved Z-strength

### Key Findings
1. Non-planar Z-interlocking gives **+15–40%** tensile strength in Z
2. The period should match nozzle diameter for optimal interlocking
3. Phase alternation (0/π) is critical — same-phase layers don't interlock
4. Z amplitude of 3-5× layer height gives best interlock without print quality issues

### Practical Observations (Benchy Test Prints)
1. Z-modulation must not exceed inner wall boundaries at any visited Z-level
2. Width modulation near walls must be nominal (no widening beyond wall)
3. Intermediate solid surfaces (deck, shelves) are hard Z boundaries
4. Taper alone (without geometry-based gating) is insufficient for complex models
5. `fill_no_overlap_expolygons` is the authoritative inner wall boundary per layer

---

## 7. Future Work

1. **Performance optimization**: Cache safe zone containment checks (edge grid)
2. **Per-region ceiling/floor**: Current ceiling is per-layer; models with multiple infill regions at different Z could benefit from per-region tracking
3. **Gradual Z taper near ceiling/floor**: Currently hard-clamped; smoothstep near boundary would be smoother
4. **Adaptive amplitude**: Reduce amplitude near thin walls or overhang regions
5. **Multi-material interaction**: FW behavior at filament change boundaries
6. **Strength testing**: Physical tensile tests comparing FW vs standard rectilinear
