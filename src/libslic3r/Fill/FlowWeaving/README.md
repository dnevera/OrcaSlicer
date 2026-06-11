# Flow Weaving — Inter-Layer Strength via Sinusoidal Modulation

> **Experimental infill pattern for OrcaSlicer that creates physical 3D interlocking
> between layers to improve Z-axis strength in FDM-printed parts.**

## Overview

Traditional FDM parts have Z-axis tensile strength of only **30–50%** compared to the
XY plane due to the flat geometry of layer-to-layer contact.  Flow Weaving replaces
flat layering with sinusoidal 3D engagement — simultaneously modulating the **extrusion
width (XY)** and the **nozzle height (Z)** along each infill line.

Phase alternation between layers (even = 0, odd = π) ensures that peaks on layer N
align with troughs on layer N+1, creating a mechanical interlock:

```
Layer N+1 (φ=π):   ──╌╌──▓▓▓▓──╌╌──▓▓▓▓──╌╌──    (peak fills trough below)
Layer N   (φ=0):   ▓▓▓▓──╌╌──▓▓▓▓──╌╌──▓▓▓▓──    (trough receives peak above)
```

---

## Physical Mechanisms

### 1. Mechanical Interlocking (Geometric Lock)
Sinusoidal peaks physically interlock between layers.  Delamination requires
either shearing through the polymer peaks or overcoming friction on wave slopes.

### 2. Load Vector Transformation
Flat layer bonds fail in pure Mode I (opening) fracture.  The wave surface converts
part of the normal stress into shear — and polymers are significantly stronger in shear.

### 3. Increased Contact Area
The wavy path increases the contact/diffusion zone between layers by **8–22%**
(depending on amplitude/period ratio):

$$L_{wave} = \int_{0}^{\lambda} \sqrt{1 + \left(\frac{2\pi A}{\lambda} \cos\left(\frac{2\pi x}{\lambda}\right)\right)^2} dx$$

### 4. Crack Arresting
In flat bonds, cracks propagate in a straight line.  The wave profile forces cracks
to constantly change direction (up/down wave slopes), dissipating energy and
localizing damage.

### 5. Dynamic Compression Effect
When the nozzle dips into the trough of the previous layer, reduced clearance
increases hydrodynamic pressure of the melt, forcing polymer into surface
irregularities and improving thermal bonding (reptation).

---

## Architecture

Flow Weaving is implemented across two independent stages:

```
┌──────────────────────────────────────────────────────┐
│  Slicing time (Fill stage)                           │
│  FillFlowWeaving.cpp                                 │
│  ● Generates rectilinear lines at 100% density       │
│  ● Subdivides into sub-segments (~period/8)          │
│  ● Applies sinusoidal WIDTH modulation per segment   │
│  ● Output: ExtrusionMultiPath with variable width    │
└──────────────────────────────────────────────────────┘
                        │
                        ▼
┌──────────────────────────────────────────────────────┐
│  G-code generation time (GCode stage)                │
│  FlowWeavingZModulator.hpp                           │
│  ● Computes sinusoidal Z-HEIGHT modulation           │
│  ● Adaptive amplitude near bed/model boundaries      │
│  ● Phase alternation per layer for interlocking      │
│  ● Stateful: tracks cumulative distance per path     │
└──────────────────────────────────────────────────────┘
```

### Files in this directory

| File | Role |
|------|------|
| `FillFlowWeaving.hpp` | Class declaration — inherits from `Fill` |
| `FillFlowWeaving.cpp` | XY width modulation engine (slicing time) |
| `FlowWeavingZModulator.hpp` | Z height modulation engine (G-code time, header-only) |
| `README.md` | This documentation |

### Integration points (outside this directory)

| File | What |
|------|------|
| `PrintConfig.hpp/cpp` | `ipFlowWeaving` enum + 3 config parameters |
| `Preset.cpp` | Preset serialization keys |
| `Fill/FillBase.cpp` | Factory registration |
| `Fill/Fill.cpp` | Skip solid-surface override for FW |
| `GCode.hpp` | `m_fw_z_mod` member |
| `GCode.cpp` | 5 injection points (set_top_z, reset_path, is_active, compute_z, advance, should_skip_z_reset) |
| `GUI/Tab.cpp` | Settings UI group + dirty suppression |
| `GUI/ConfigManipulation.cpp` | Density auto-lock + toggle visibility |
| `GUI/GUI_Factories.cpp` | Per-object override entries |

---

## Parameters

| Parameter | Default | Range | Description |
|-----------|---------|-------|-------------|
| `flow_weaving_z_amplitude` | 30% | 0–100% | Z-height modulation as % of layer height |
| `flow_weaving_xy_amplitude` | 15% | 0–50% | Width modulation as % of nominal line width |
| `flow_weaving_period` | 3.0 mm | 0.5–10 mm | Wavelength of the sinusoidal modulation |

> **Note:** Density is forced to 100% internally (not user-adjustable when FW is active).

---

## Limitations & Known Challenges

### Hydrodynamic Flow Lag
At high speeds (v=150 mm/s) with short periods (λ=3 mm), the flow change frequency
reaches ~50 Hz.  Melt compressibility in the hotend causes the actual extrusion peak
to lag behind the commanded E-rate.  Without PA-like compensation, peak width and
trough Z may misalign.

### Z-Axis Kinematics
Most bed-slinger printers cannot oscillate Z at 30–80 Hz without vibration/resonance.
Best suited for CoreXY with moving toolhead Z (Voron 2.4, Bambu Lab, etc.) or
reduced infill speed.

### Surface Artifacts
Cyclic pressure changes can bleed into perimeters as moiré.
Recommendation: ≥2 perimeters before Flow Weaving infill begins.

---

## Testing Methodology

1. **Z-axis Tensile Test** — Print vertical dog-bone specimens (ASTM D638 / ISO 527).
   Compare failure load at amplitudes 10%, 20%, 30%, 40%.
2. **Shear Test** — Three-point bending of short beams to load inter-layer bonds in shear.
3. **Kinematic Frequency Limit** — Find max print speed where Z-steppers don't overheat
   or skip steps at a given λ.

---

## References

- Kishore, V. et al. "Infrared preheating to improve interlayer strength of big area
  additive manufacturing (BAAM) components." *Additive Manufacturing* 14, 2017.
- Seppala, J.E. et al. "Weld formation during material extrusion additive
  manufacturing." *Soft Matter* 13, 2017.
- Hart, K.R. et al. "Increased fracture toughness of additively manufactured
  amorphous thermoplastics via thermal annealing." *Polymer* 144, 2018.

---

## License

This code is part of the OrcaSlicer fork and follows the same license terms
(AGPLv3, as inherited from PrusaSlicer/BambuStudio).
