# Flow Weaving: Virtual 3D Layer Interlocking

## Concept

One method, two simultaneous effects on every solid infill line (100%):

1. **Z-modulation**: the nozzle "dives" along sin(x) ±A along the extrusion path
2. **Width modulation**: line width (flow/E) pulses with the same sin(x)
3. **Phase shift**: adjacent layer uses φ=π (inversion)

Result: a peak on one layer (thick + raised) fits into the trough of the other
(thin + lowered) = **3D mechanical lock without voids or injection cycles**.

---

## Visualization

### Side view (single pass, Z-axis ↑, X-axis →):

```
                 Z-modulation + Width-modulation (simultaneous)

Layer N+1 (φ=π):
            thin            thick           thin
              ╲              ╱══╲              ╲
               ╲            ╱════╲              ╲
                ╲──────────╱══════╲──────────────╲───
                          ↑ peak up + thick

Layer N (φ=0):
                ╱══╲              ╱══╲
               ╱════╲            ╱════╲
          ────╱══════╲──────────╱══════╲──────────
              ↑ peak up + thick

     Bonding zone: peaks of N fit into troughs of N+1 and vice versa
```

### Cross-section (end view of a line):

```
    Phase=0 (peak):             Phase=π (trough):

      ┌──────┐  ← wide           ┌──┐  ← narrow
      │██████│  ← Z raised       │██│  ← Z lowered
      │██████│                    │██│
      └──────┘                    └──┘
```

### 3D view of two layers (X → extrusion direction):

```
Layer N+1:  ──╌╌──▓▓▓▓──╌╌──▓▓▓▓──╌╌──    φ=π (shifted)
Layer N:    ▓▓▓▓──╌╌──▓▓▓▓──╌╌──▓▓▓▓──    φ=0

▓▓ = thick + Z raised (peak)
╌╌ = thin  + Z lowered (trough)

Peaks of one layer → in troughs of the other = INTERLOCK
```

---

## Parameters

| Parameter | Config key | Description | Default | Min | Max |
|-----------|-----------|-------------|---------|-----|-----|
| Mode | `flow_weaving` | None / Enabled | None | — | — |
| Z-amplitude | `flow_weaving_amplitude` | % of layer_height | 30 | 10 | 50 |
| Width ratio | `flow_weaving_width_ratio` | max/min extrusion width | 1.3 | 1.1 | 1.8 |
| Wavelength | `flow_weaving_period` | Modulation period along path, mm | 3.0 | 1.0 | 10.0 |

> Phase shift between layers is computed automatically: `φ = π × (layer_id % 2)` —
> even/odd layers are inverted.

---

## Mathematical Model

For each sub-segment (length ≤ period/8) along the infill path:

```cpp
// path_pos — accumulated distance along the path from line start
// layer_id — layer number

double phase = M_PI * (layer_id % 2);  // 0 or π
double t = sin(2.0 * M_PI * path_pos / period + phase);  // [-1, +1]

// Z-modulation: nozzle dives
double z_offset = amplitude * layer_height * t;
// → G1 Z{z_base + z_offset}

// Width-modulation: variable flow
// t=-1 → flow_factor = 1.0 (minimum)
// t=+1 → flow_factor = width_ratio (maximum)
double flow_factor = 1.0 + (width_ratio - 1.0) * (t + 1.0) / 2.0;
// → dE = e_per_mm * segment_length * flow_factor
```

Key insight: Z and Width are modulated by **the same** sinusoid.
Peak = thick + raised, trough = thin + lowered.
This maximizes the depth of the mechanical interlock.

---

## Implementation in OrcaSlicer

### Injection point in GCode.cpp

File: `src/libslic3r/GCode.cpp`, line ~7563:

```cpp
auto dE = e_per_mm * line_length;  // ← CURRENT code
```

Replaced with segment subdivision and modulation:

```cpp
// Flow Weaving: subdivide segment and modulate Z + E
if (flow_weaving_enabled && is_solid_infill(path.role())) {
    // Subdivide the line into sub-segments
    double sub_len = flow_weaving_period / 8.0;
    int n_subs = std::max(1, (int)ceil(line_length / sub_len));
    double actual_sub = line_length / n_subs;

    Vec2d start2d = point_to_gcode(line.a.to_point());
    Vec2d end2d   = point_to_gcode(line.b.to_point());
    Vec2d dir     = (end2d - start2d).normalized();

    for (int s = 0; s < n_subs; ++s) {
        double pos = accumulated_path + actual_sub * (s + 0.5);
        double t = sin(2.0 * M_PI * pos / period + phase);

        double z = z_base + amplitude * layer_height * t;
        double flow = 1.0 + (width_ratio - 1.0) * (t + 1.0) / 2.0;
        double sub_dE = e_per_mm * actual_sub * flow;

        Vec2d sub_end = start2d + dir * actual_sub * (s + 1);
        gcode += writer.extrude_to_xyz(sub_end.x(), sub_end.y(), z, sub_dE);
    }
    accumulated_path += line_length;
} else {
    auto dE = e_per_mm * line_length;  // original path
    ...
}
```

### Applied only to:
- `erSolidInfill` — internal solid infill
- `erInternalInfill` at 100% density — sparse infill at full density
- **NOT** applied to: perimeters, top/bottom surfaces, bridges, support

### Modified files:

| File | Changes |
|------|---------|
| `PrintConfig.hpp` | enum `FlowWeavingType` {None, Enabled} + 4 config fields in `PrintRegionConfig` |
| `PrintConfig.cpp` | enum map + option definitions with defaults |
| `Tab.cpp` | UI fields (Strength group or separate Flow Weaving section) |
| `ConfigManipulation.cpp` | toggle visibility when flow_weaving != None |
| `GUI_Factories.cpp` | per-object settings keys |
| `Preset.cpp` | registration |
| `GCode.cpp` | segment subdivision + Z/E modulation in `_extrude()` |

---

## Advantages vs Micro-Injection Molding

| | Micro-Injection | Flow Weaving |
|---|---|---|
| Voids | Yes — subtracted from infill | No |
| Injection | Yes — separate G-code cycle | No |
| Temperature | +20°C boost | Standard |
| Wipe tower | Required | Not needed |
| Print time | +15–30% (injections + cooling) | +5–10% (sub-segments) |
| Strength | High (physical locks) | Medium-high (friction + geometry) |
| Firmware | Any | Requires smooth Z-interpolation |

---

## Status

- [x] Config + UI (PrintConfig, Tab, ConfigManipulation, GUI_Factories, Preset)
- [x] G-code modulation (GCode.cpp — segment subdivision + Z/E)
- [x] Build and test
- [ ] G-code analysis
- [ ] Test print
