# FlowWeaving — 3D Inter-Layer Interlocking Infill

**Branch:** `flow_weaving_infill`  
**Location:** `src/libslic3r/Fill/FlowWeaving/`  
**Pattern ID:** `ipFlowWeaving` (OrcaSlicer UI: "Flow weaving")

---

## What Is It?

FlowWeaving is a custom FDM infill pattern that modulates both the **Z position** and the **extrusion width** of each infill line as a sinusoidal wave. The result is a 3D interlocking structure instead of flat 2D infill layers.

Standard FDM infill is essentially 2.5D — layers are stacked flat with no physical interlocking between them. This means inter-layer tensile strength is 50–75% lower than in-plane strength. FlowWeaving attacks this weakness directly.

```
Standard infill (side view):       FlowWeaving (side view):
━━━━━━━━━━━━━━━━━━━━━━━━          ∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿
━━━━━━━━━━━━━━━━━━━━━━━━   →     ∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿
━━━━━━━━━━━━━━━━━━━━━━━━          ∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿
```

Layers with a phase offset of 0.5 (half period) interlock mechanically:

```
Layer N   (phase=0):    ╰╮  ╰╮  ╰╮  ╰╮
Layer N+1 (phase=0.5):    ╮╰  ╮╰  ╮╰  ╮╰
```

---

## Parameters

| Parameter | UI Name | Default | Range | Description |
|---|---|---|---|---|
| `flow_weaving_z_amplitude` | Z weaving amplitude | 95% | 0–150% | Sine amplitude as % of layer height |
| `flow_weaving_xy_amplitude` | XY width modulation | 50% | 0–100% | Width variation as % of flow width |
| `flow_weaving_xy_path_amplitude` | XY path amplitude | 0.2mm | 0–2mm | Lateral path displacement perpendicular to travel |
| `flow_weaving_period` | Weaving period | 1mm | 0.5–20mm | Wavelength of one full sine cycle |
| `flow_weaving_phase_offset` | Layer phase offset | 0.5 | 0–1 | Phase shift between same-direction layers (0.5 = best interlocking) |
| `flow_weaving_z_overlap` | Z overlap into previous layer | 25% | 0–50% | How far nozzle may press into previous layer (% of layer_h) |
| `flow_weaving_top_taper_layers` | Top surface taper layers | 3 | 0–10 | Layers over which upward Z-amp tapers to 0 near top shell |

### Recommended Starting Settings (0.4mm nozzle, 0.2mm layer height)
- Period: 1–2mm (shorter = more interlocking, but slower Z axis)
- Z amplitude: 80–95% (nearly full layer height)
- XY width modulation: 30–50%
- Z overlap: 25% (= 0.05mm penetration into previous layer)
- Top taper layers: 3

---

## Architecture Overview

**Key principle: ZERO changes to GCode.cpp or ExtrusionEntity.hpp.**

FlowWeaving uses only existing OrcaSlicer mechanisms:
- `ExtrusionPathContoured` with `z_contoured = true` (ZAA mechanism)
- `mm3_per_mm` per sub-segment (standard extrusion field)
- `Polyline3` with Z coordinates baked in

Each infill polyline is subdivided into short sub-segments (8 per period by default). Each sub-segment becomes its own `ExtrusionPathContoured` with:
- Z offsets encoded in `Polyline3` coordinates
- Flow rate (`mm3_per_mm`) scaled by the XY width modulation factor
- `z_contoured = true` so GCode.cpp handles Z emission with E-compensation

See [IMPLEMENTATION.md](IMPLEMENTATION.md) for full technical details.

---

## Files

| File | Purpose |
|---|---|
| `FillFlowWeaving.cpp` | Main algorithm: sub-segmentation, Z/XY modulation, safety clamping |
| `FillFlowWeaving.hpp` | Class declaration |
| `FlowWeavingZModulator.hpp` | Sine modulator (header-only), factory for wave functions |
| `FlowWeavingContext.hpp` | Parameter context (if used) |
| `FlowWeavingFadeEnvelope.hpp` | Fade-to-wall taper logic |
| `FlowWeavingZClamp.hpp` | Z safety clamp helpers |
| `README.md` | This file |
| `CONCEPT.md` | Research, hypotheses, future directions |
| `IMPLEMENTATION.md` | Technical architecture and implementation log |

---

## Build Notes

**Agent rule: never trigger builds.** The user builds manually.

```bash
cd OrcaSlicerBuild
./build_clean_orca.sh release-arm64
```

Or incremental:
```bash
cmake --build build-arm64-release --target OrcaSlicer -j8
```

---

## Known Limitations / Work in Progress

1. **Wave symmetry**: On lower layers near the floor, the waveform is asymmetric (lower bound clamping cuts the negative half). This is by design for the first few layers.
2. **Top taper**: Implemented but not yet tested in print — needs real-world validation.
3. **Z overlap**: Research suggests 20–30% of layer_h is safe for bonding without delamination. 50% is aggressive. Optimal value is material/temperature dependent.
4. **GCode comments**: No `; FlowWeaving infill` marker in output yet (architecture constraint — would require touching GCode.cpp or adding ExtrusionRole).

---

## Git History

| Commit | Description |
|---|---|
| `f266e172c4` | Z safety clamp via first_layer_height |
| `a6ccf4571c` | Add flow_weaving_z_overlap parameter |
| *(current)* | Add flow_weaving_top_taper_layers + top-surface taper |
