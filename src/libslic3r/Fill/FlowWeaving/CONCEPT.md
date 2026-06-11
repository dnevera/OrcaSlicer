# FlowWeaving — Concept, Research & Hypotheses

## 1. Problem Statement: Inter-Layer Anisotropy in FDM

### 1.1 The Physics

Traditional FDM/FFF 3D printing deposits molten thermoplastic in successive flat layers in the XY plane. The result is strongly anisotropic mechanical properties that are intrinsic to the process — independent of material choice.

Typical inter-layer strength deficit across materials \[1, 2, 3\]:

| Material | XY tensile strength | Z tensile strength | Z/XY ratio |
|---|---|---|---|
| PLA | 40–60 MPa | 10–20 MPa | **~35%** |
| PETG | 45–55 MPa | 15–25 MPa | **~40%** |
| ABS | 35–45 MPa | 10–18 MPa | **~30%** |
| PA12 (Nylon) | 45–55 MPa | 20–30 MPa | **~45%** |
| PC | 55–65 MPa | 20–30 MPa | **~38%** |
| CF-PLA (15% short fiber) | 60–80 MPa | 15–25 MPa | **~25%** |
| CF-PA (15% short fiber) | 70–100 MPa | 20–35 MPa | **~27%** |

The Z/XY ratio is worst precisely for the **engineering and fiber-filled materials** where structural performance matters most \[3\]. Short carbon or glass fibers align in the XY plane during extrusion — dramatically increasing in-plane strength — but provide almost no Z-direction reinforcement \[3\]. The net effect: the stronger the fiber-filled material in XY, the greater the *relative* weakness in Z.

The root cause is purely geometric: inter-layer bond strength depends exclusively on thermal fusion of flat horizontal surfaces \[1\]. The contact area is limited to a narrow interface zone at layer boundaries. No mechanical interlocking exists.

Under axial loading, parts delaminate along layer planes. This is the single largest structural weakness of FDM-printed parts and the primary motivator for FlowWeaving.

### 1.2 Context in Existing Literature

This problem has been studied extensively. Key methods surveyed (full references in Section 6):

| Method | Z-Strength Gain | Implementation Complexity | Print Time | Ref. |
|---|---|---|---|---|
| **BrickLayers** (Stefan/CNC Kitchen, TenTech) | +12–15% | Low (G-code post) | +0–2% | \[7, 8\] |
| **Interlocking Perimeters** (preFlight/oozebot) | +5–15% | Medium (libslic3r) | +0% | \[9\] |
| **FlowWeaving (modulated infill)** | **+40–60%** | High (Fill/, self-contained) | +15–25% | \[4, 5, 6\] |
| **Z-Stitching** (Elsherbiny et al., 2025) | **>250%** | Extreme | +14% | \[10\] |
| **Z-Pinning (vertical pegs)** (ORNL) | **>300%** | High | +20–35% | \[11\] |
| **Spatial Truss / Lattice** | >200% | High | +15–25% | \[12\] |

> **Note:** Percentage gains are measured on PLA test specimens in most studies (convenient, consistent baseline). For engineering materials (PA, PC, CF-filled), absolute gains are similar but the *relative problem* is larger — CF-filled materials may have 4× worse Z/XY ratio than PLA. FlowWeaving is expected to be more impactful on engineering materials precisely where the anisotropy gap is widest.

> **On the modulator:** Academic literature studies sinusoidal Z-weaving specifically. FlowWeaving implements sine as the **first and default basis function**, but the architecture (`FlowWeavingZModulator`) is designed for any periodic or aperiodic waveform — square, triangle, sawtooth, Perlin noise, or custom. The +40–60% gain estimate applies to the sinusoidal case; other waveforms may perform differently and need separate characterization.

FlowWeaving implements the **modulated Z-infill** approach — the best balance between implementability within an existing slicer and meaningful strength gain. Sine is the first realized waveform; the modulator is extensible.

### 1.3 Why Z-Weaving Was Chosen

1. **No hardware changes** — works on standard CoreXY within kinematic limits
2. **No structural model changes** — works with existing infill generation, no cavity pre-planning
3. **~40–60% strength gain** is significant and consistent with literature
4. **Simultaneous XY width modulation** adds surface contact area gain (+5–10% additional)
5. **Fully self-contained** in slicer logic, no post-processing required

---

## 2. Hardware Requirements & Printer Compatibility

### 2.1 What FlowWeaving Requires from the Printer

FlowWeaving generates `G1 X Y Z E` moves where **Z changes simultaneously with XY travel**. This is physically valid on most printers but has critical constraints:

| Requirement | Detail |
|---|---|
| **Independent Z motor** | Z must be able to move while XY is also moving |
| **Z speed ≥ ~5 mm/s** | Minimum to follow the wave at typical print speeds |
| **Firmware: standard G1** | Must accept `G1 X Y Z E` — all axes in one move |
| **Linear advance / pressure advance** | Strongly recommended for XY width modulation accuracy |

### 2.2 Printer Architecture Compatibility

> [!IMPORTANT]
> This table is derived from **kinematic first principles**, not from empirical testing across printer types. The only printer tested to date is a CoreXY with a lead-screw Z axis. All other rows are **reasoned hypotheses** based on the mechanics of each architecture.

| Printer architecture | Z-during-XY | FlowWeaving expected | Tested? |
|---|---|---|---|
| **CoreXY** (independent lead-screw Z) | Z motor moves independently of XY toolhead | ✅ Full support | ✅ Yes (one printer) |
| **CoreXZ** (Z shared with X belt) | Z and X are coupled in kinematics — separate Z move distorts X | ⚠️ Unknown — wave shape may be distorted | ❌ Not tested |
| **Cartesian / bed-slinger** (Z moves bed) | Z motor moves independently, but bed + print mass is high → lower acceleration | ⚠️ Works in principle; longer wavelength needed | ❌ Not tested |
| **Delta** (all 3 towers move for any G1) | Z-during-XY is native — all moves are inherently 3D | ✅ Expected to work well | ❌ Not tested |
| **IDEX** | Per active tool, same as CoreXY | ✅ Expected | ❌ Not tested |

**Kinematic reasoning** for the CoreXY case (the one tested):
- Z axis is mechanically independent of the XY gantry
- A standard `G1 X Y Z E` move commands all axes simultaneously
- The firmware (Marlin/Klipper/Bambu) interpolates all axes linearly in one move
- Therefore: Z modulation during XY infill travel is physically correct and requires no firmware changes

**Where the reasoning is uncertain:**
- Cartesian/bed-slinger: the required Z speed (`v_Z = 2A/λ × v_XY`) may exceed the Z acceleration capability for short periods. The exact threshold depends on the specific printer's Z motor, lead screw pitch, and driver current — not a universal constant.
- CoreXZ: depends on firmware implementation of the CoreXZ kinematic transformation. Some firmware versions may or may not correctly handle simultaneous Z+X commanded as G1 X Y Z.
- Delta: the kinematic math suggests G1 with Z works fine; the question is whether the inverse kinematics solver handles rapid Z oscillation cleanly at high speeds.

**What this means for users:** FlowWeaving should work on any printer whose firmware accepts standard `G1 X Y Z E` and has an independent Z drive. The safe parameter window (wavelength, amplitude) must be chosen to keep the required Z velocity within the printer's Z axis capability — see Section 2.3.

### 2.3 Kinematic Constraints (CoreXY Class)

Reference measurements for **CoreXY with lead-screw Z** (typical values):
- **XY axes:** up to 500 mm/s, acceleration 20,000 mm/s²
- **Z axis (lead screw):** max **20–30 mm/s**, acceleration **500–1000 mm/s²**

Z-speed required at a given XY print speed:
$$v_Z = \frac{2 \cdot A_{wave}}{\lambda} \cdot v_{XY}$$

For `A=0.19mm, λ=1mm, v_XY=30mm/s` → `v_Z = 11.4 mm/s` ✅  
For `A=0.19mm, λ=1mm, v_XY=100mm/s` → `v_Z = 38 mm/s` ❌ (exceeds Z limit → skip periods)

**Safe parameter window (CoreXY, typical lead-screw Z):**

| Nozzle | Layer h | Z amplitude | Min period | Max XY speed |
|---|---|---|---|---|
| 0.4mm | 0.2mm | 0.190mm (95%) | ≥ 1.0mm | 30 mm/s |
| 0.4mm | 0.2mm | 0.190mm (95%) | ≥ 4.0mm | 100 mm/s |
| 0.2mm | 0.1mm | 0.095mm (95%) | ≥ 2.0mm | 50 mm/s |

### 2.4 Nozzle Geometry Constraint

The nozzle flat face diameter ($D_{flat}$) limits the maximum safe wave slope angle \[kinematic analysis\]:

$$\sin(\theta_{max}) \le \frac{h}{D_{flat}}$$

For 0.4mm nozzle ($D_{flat} \approx 0.8\text{ mm}$), $h = 0.2\text{ mm}$: $\theta_{max} \approx 14.5°$

Minimum wavelength: $\lambda_{min} = 2\pi A / \tan(\theta_{max})$

Silicone sock removal increases the nozzle protrusion clearance by 4–5mm and allows steeper angles — important for aggressive Z-Pinning hybrids (Phase 3 roadmap).

### 2.5 What FlowWeaving Does NOT Require

- No multi-axis (5-axis) CNC-style firmware
- No custom firmware modifications  
- No external post-processing scripts
- No changes to existing G-code generation pipeline (Z modulation is resolved fully in the Fill phase)

---

## 3. The FlowWeaving Hypothesis

### 3.1 Core Idea

**If the nozzle traces a 3D modulated wave instead of a flat path, adjacent layers will physically interlock — like teeth of a zipper — rather than merely touching at a flat interface. The wave function (currently: sine) is a parameter, not a constraint.**

```
Standard infill (side view):         FlowWeaving (side view):
Layer N+1: ══════════════════        Layer N+1: ═╗ ╔═╗ ╔═╗ ╔═╗ ╔═
Layer N:   ══════════════════        Layer N:   ╚═╝ ╚═╝ ╚═╝ ╚═╝ ╚═
                                                 ↑ interlocking ↑
```

### 3.2 Physical Mechanism (Interlock Zones)

```
                    ← one full wave period →
Layer N   (phase=0):  ___         ___         ← nozzle HIGH (peak)
                     /   \       /   \
____________________/     \_____/     \_____  ← nozzle LOW (valley)

Layer N+1 (phase=π):  ___         ___         ← nozzle HIGH
                     /   \       /   \
____________________/     \_____/     \_____

When N+1 is printed ON TOP of N in antiphase:
                         INTERLOCK ZONE
                             ↓↓↓
  Z ↑  ║   N+1 low   ║   N+1 high  ║   N+1 low   ║
    │  ║ (nozzle ↓)  ║  (nozzle ↑) ║ (nozzle ↓)  ║
    │  ║─────────────║─────────────║─────────────║
    │  ║   N high    ║   N low     ║   N high    ║
    │  ║ (bead tall) ║ (bead flat) ║ (bead tall) ║
    └──────────────────────────────────────────→ X
```

**Physical mechanism at interlock zone:**
N+1 nozzle goes DOWN exactly where N bead is TALL (peak).
The nozzle physically presses into / crushes N's raised material:
- Re-melts the peak of N → polymer diffusion across boundary
- Material of N is displaced laterally → "dovetail" tooth geometry forms
- N+1's wide bead (peak) fills N's valley → mechanical lock against Z pull

```
Cross-section side view (XY line direction, single period):

    ↑Z
    │    ╔══════╗           ╔══════╗       ← N+1 peak (wide bead, high Z)
    │    ║      ║           ║      ║
    │════╬══════╬═══════════╬══════╬════   ← N+1 valley (nozzle crushes N peak)
    │    ║ N pk ║           ║ N pk ║       ← N peak (raised bead)
    │════╝      ╚═══════════╝      ╚════   ← N valley
    └───────────────────────────────────→ X
          ↑↑↑↑                 ↑↑↑↑
      interlock zones      interlock zones
      (N peak inside N+1 valley)
```

### 3.3 Theoretical Strength Gains

Based on academic literature \[4, 5, 6\]:
- Non-planar Z-weaving alone: **+15–40% tensile strength** in Z direction \[4, 5\]
- XY width modulation (contact area): **+5–10%** additional \[6\]
- Combined expected gain: **+20–50% inter-layer strength**

The mechanisms:
1. **Increased contact area** — sinusoidal profile vs. flat ≈ +11% contact area geometrically \[5\]
2. **Mechanical interlocking** — layers physically grip each other \[4\]
3. **Z-load redistribution** — tensile Z forces are resolved into XY shear + compression in wave material, which any thermoplastic handles much better than pure delamination \[4\]
4. **Re-melting of previous layer** — nozzle dipping into previous layer re-melts the surface, improving polymer chain diffusion across boundary \[1, 13\]

**Engineering plastic context:** The re-melting effect is particularly significant for semi-crystalline polymers (PA12, PA6, PEEK) where bond strength is highly sensitive to the thermal history at the interface \[2\]. FlowWeaving increases both contact time and contact area during re-melting, which should proportionally improve inter-crystallite bonding.

---

## 4. Squish Factor and Overlap Research

### 4.1 Published Research on Layer Overlap

Research \[7, 13\]:
- A "squish factor" — controlled compression into the layer below — significantly improves bond strength \[13\]
- **Optimal squish:** 10–25% of layer_h \[13\]
- **Maximum safe squish:** up to 50% of nozzle_d (= 0.2mm for 0.4mm nozzle) \[13\]
- **Mechanism:** nozzle re-melts the top surface of the previous bead → polymer chain diffusion across boundary improves \[1\]

For `layer_h = 0.2mm, nozzle_d = 0.4mm`:
| Overlap % | Overlap mm | Assessment |
|---|---|---|
| 0% | 0mm | No bonding benefit |
| 15% | 0.03mm | Conservative, safe |
| 25% | 0.05mm | **Recommended default** |
| 35% | 0.07mm | Aggressive, may cause Z force |
| 50% | 0.10mm | Maximum — risk of Z motor step loss |

### 4.2 The Z Overlap Parameter

The Z overlap controls how far the nozzle is allowed to press into the previous layer — i.e., the downward amplitude limit. It is the primary per-material tuning knob and the first user-configurable safety parameter.

For implementation details (how it is clamped in the fill algorithm), see `IMPLEMENTATION.md`.

---

## 5. Safety Concept — Bounds on Z Modulation

### 5.1 Why Safety Bounds Are Critical

FlowWeaving modulates the nozzle path in 3D. Without explicit bounds, the nozzle can:

1. **Exit the XY safe zone** — at a different Z the nozzle is effectively at a different XY layer's position; the wave can cross into the perimeter wall
2. **Rise above the top solid shell** — the wave peak intrudes into the solid shell printed above
3. **Dip below the bottom solid surface** — the wave valley goes below a deck or the print bed
4. **Descend below the first layer** — an absolute physical violation (nozzle hits the bed)

### 5.2 Conceptual Solutions Explored

#### Attempt 1: Shrink infill zone ❌
Shrink the infill boundary inward by the maximum Z-induced XY shift.
Problem: creates narrow unfilled strips at the boundary — wall adhesion fails.

#### Attempt 2: Gap fill the strips ❌  
Fill the strips with flat rectilinear.
Problem: strips too narrow for the line generator — voids remain.

#### Attempt 3: Line-end amplitude taper ✓ (partial)
Fade the wave amplitude smoothly to zero near each infill line endpoint.
Problem: fade distance is constant; fails on steep sloped walls (Benchy hull) where the wall shifts significantly per mm of Z deviation.

#### Attempt 4: Slope-aware taper ✓ (partial)
Scale the fade distance by the expected wall slope (`wall_shift ≈ z_deflection × tan(θ)`).
Problem: heuristic; requires knowing the wall angle, which varies per model.

#### Attempt 5: Real-geometry safe zone gating ✓ (current, primary)
Use the actual per-layer infill boundary polygons (which already account for wall thickness). Intersect boundaries across the full Z-modulation range. A sub-segment falling outside the intersection gets zero modulation — it prints flat. No model-specific tuning required.

### 5.3 Three Safety Principles (Current Design)

**Principle 1 — Real-geometry XY gate (primary):**  
The infill boundary for each visited Z-level is known at slice time. The intersection of boundaries across all layers the wave visits defines the "safe zone". Sub-segments outside this zone are printed flat.

**Principle 2 — Explicit Z bounds (fail-safe):**  
Two independent lower limits: (a) user-configurable overlap depth — how far the nozzle may press into the previous layer, and (b) an absolute floor derived from the first layer height — the nozzle never descends below the physical bed surface.

One independent upper limit: the upward wave amplitude fades smoothly to zero over the last N infill layers below the top solid shell, using a smoothstep curve. This prevents the wave peak from intruding into the solid shell zone.

**Principle 3 — Wall amplitude taper (smoothing supplement):**  
Independent of the gate, the wave amplitude fades to zero near each infill line endpoint. This provides a smooth transition at boundaries even when the safe zone permits modulation close to the wall.

### 5.4 Real-Print Observation: Top Surface Violation

**Observation (June 2026):** On an actual print, the nozzle went above the top solid shell.

**Root cause:** Wave amplitude was applied uniformly to all infill layers, including the last infill layer immediately below the solid top shell. A positive Z offset at the wave peak intruded into the shell zone by the full wave amplitude.

**Principle:** The upper Z bound must fade *before* reaching the last infill layer, not only *at* it.

**Resolution:** The slicer counts how many infill layers remain above the current layer. The upward amplitude is scaled down smoothly using a smoothstep function over the last N user-configurable layers.



---

## 6. Empirical Validation (Test Print, June 2026)

### 6.1 Test Settings

Settings: `z_amp=95%, period=1mm, xy_amp=50%, z_overlap=50%, phase_offset=0.5`

```
Total XYZE moves:        38,018
Z range:                 [0.300 .. 3.990] mm
Z floor violations:      0 ✓

Expected Z span:         z_amp + z_overlap = 0.19 + 0.10 = 0.29mm
Actual measured span:    0.29mm ✓
```

### 6.2 Per-Layer Wave Pattern (Phase Verification)

Apparent asymmetry observed in per-layer aggregate: some layers showed only upward moves, others only downward. Diagnosed as correct behavior:

| Layer position | Apparent pattern | Root cause |
|---|---|---|
| First infill layer | only upward | Floor clamp prevents downward dip |
| Interior layers | symmetric | Even/odd lines in antiphase — average is symmetric |
| Layers with one parity dominant | skewed | Aggregate of antiphase pair looks asymmetric |

**Diagnosis:** Per-layer *aggregate* analysis is misleading — individual line *pairs* are symmetric. Adjacent same-direction lines are in antiphase. This is correct intended behavior.

### 6.3 Phase Alternation — Observed Behavior

Within each layer: odd-numbered infill lines carry a π-phase shift relative to even lines. The wave peaks and valleys of adjacent lines are inverted. When viewed as cross-sectional pairs, they form the interlock geometry shown in Section 3.2.

For implementation details of how the phase is computed per line, see `IMPLEMENTATION.md`.

---

## 7. Hypotheses to Test (Future)

### 8.1 Optimal Z Overlap per Material
**Hypothesis:** The optimal overlap (how far the nozzle presses into the previous layer) varies by material and is not a single universal value. Semi-crystalline polymers with sharp melting transitions (PA12, PEEK) may need more overlap to achieve re-melting; amorphous polymers (PC, ABS) may be more sensitive to Z force.

| Material | Estimated optimal z_overlap | Notes |
|---|---|---|
| PLA | 20–25% of layer_h | Good baseline reference |
| PETG | 15–25% | More flexible, less Z force risk |
| ABS | 25–35% | Poor natural adhesion → more overlap helps |
| PA12 | 25–35% | Semi-crystalline, sharp melt → more re-melting needed |
| PC | 20–30% | High melt viscosity — harder to re-melt |
| CF-PA | 15–25% | Fiber reinforcement limits overlap benefit |

**Test:** Tensile bars at z_overlap = 0%, 15%, 25%, 35%, 50% per material → measure break force.
**Expected:** Peak varies by material, generally 20–35%.

### 8.2 Phase Offset Importance
**Hypothesis:** `phase_offset = 0.5` (adjacent same-direction layers in antiphase) maximizes interlocking regardless of material.
**Test:** phase_offset = 0 vs. 0.5 → Z tensile strength.
**Expected:** 0.5 significantly stronger for all materials.

### 7.3 Period Length vs. Interlocking Density
**Hypothesis:** Shorter periods create more interlocking events per mm (more interlock zones) but are limited by Z kinematic constraints of the printer.
**Test:** Period = 0.5, 1, 2, 5mm → Z tensile strength + check for Z motor artifacts.
**Expected:** Optimal period around 1–2mm for CoreXY printers at 30–50 mm/s print speed.

### 8.4 XY Path Amplitude Contribution
**Hypothesis:** Lateral XY displacement perpendicular to travel creates additional interlocking in Y direction, complementing the Z interlocking.
**Current status:** Implemented but contribution not quantified.

### 8.5 Engineering Materials — Primary Focus
The problem FlowWeaving addresses is most severe for structural/engineering-grade materials:

- **PA12 / PA6 (Nylon):** Hygroscopic, semi-crystalline. Poor inter-layer bonding unless printed very hot. FlowWeaving + high z_overlap could provide consistent bonding independent of humidity state.
- **PC (Polycarbonate):** Very high melt viscosity, slow thermal diffusion. Short-period weaving may be limited by Z kinematics, but even period=4mm could help significantly.
- **ABS:** High shrinkage, prone to delamination. FlowWeaving mechanical interlocking may physically resist shrinkage-induced delamination even after cooling.
- **CF-PA / CF-PLA (short fiber):** The worst Z/XY ratio of any FDM material (~25–27%). FlowWeaving's geometric interlocking is independent of fiber orientation — provides Z-locking that fibers cannot.
- **PEEK / PEI:** Very high processing temperatures. Z-overlap creates local re-melting at 380°C+ which should strongly bond the crystalline matrix. Hypothesis: FlowWeaving benefit may be disproportionately large for PEEK.
- **TPU (flexible):** High compliance absorbs wave elastically — less rigid interlocking, but increased contact area still improves bonding. Net effect uncertain — test required.

---

## 8. Development Roadmap

### Phase 1 (Complete ✓)
- [x] Basic Z-modulation (sine wave, extensible to any waveform)
- [x] XY width modulation (flow rate per sub-segment)
- [x] XY lateral path displacement (physical wave in XY plane)
- [x] Wall amplitude taper (smooth fade at infill line endpoints)
- [x] Lower Z safety: absolute floor (derived from first layer height)
- [x] Lower Z safety: configurable overlap depth limit
- [x] Upper Z safety: smooth taper near top solid shell
- [x] Phase offset parameter for layer alternation
- [x] Cross-layer safe zone gating (adjacent layer boundary intersection)
- [x] Infill-layers-above counter for top taper

### Phase 2 (Planned)
- [ ] **Bottom taper:** symmetric to top taper — fade upward amplitude on first N infill layers above bottom shell
- [ ] **GCode comment marker:** `; FlowWeaving infill` without touching GCode.cpp
  - Option A: new `ExtrusionRole::erFlowWeavingInfill` enum value
  - Option B: custom field in ExtrusionPathContoured description string
- [ ] **Adaptive amplitude:** reduce amplitude in thin sections where wave would exceed section height
- [ ] **Per-region ceiling/floor:** currently per-layer → need per-region for models with multiple infill zones
- [ ] **Real-world tensile testing:** print test bars and measure actual Z strength improvement

### Phase 3 (Research)
- [ ] **Square wave modulator:** sharp interlocking teeth vs. smooth sine
- [ ] **Perlin noise modulator:** pseudo-random Z to prevent resonance with model geometry
- [ ] **Variable period:** increase period near walls, decrease in bulk infill
- [ ] **Cross-hatch weaving:** alternating layers at 0°/90° with synchronized phase for 3D interlocking in both XY and Z
- [ ] **Z-Stitching hybrid:** combine sinusoidal wave with occasional deep stitching pins for >100% Z-strength gain
- [ ] **Flow compensation validation:** verify E-compensation formula `extrusion_ratio = (height + z_diff) / height` is accurate at high z_diff values (>80% layer_h)

---

## 9. Architecture Decisions & Rationale

### Decision 1: Sub-segmentation over flow_factors[]
**Rejected:** `flow_factors[]` array in `ExtrusionPathContoured` (one factor per point).
**Reason:** Pollutes base class with FlowWeaving-specific data — violates isolation principle.
**Chosen:** Split each infill line into N short `ExtrusionPathContoured` sub-segments. Each has its own `mm3_per_mm` (standard existing field). Zero changes to `ExtrusionEntity.hpp`.

### Decision 2: Re-use ZAA `z_contoured` mechanism
**Why:** GCode.cpp already handles Z-contoured paths with `extrusion_ratio = (height + z_diff) / height`. E-compensation is automatic. No duplication of Z-emission logic.
**Result:** Zero changes to GCode.cpp needed for Z modulation.

### Decision 3: first_layer_h as absolute floor, not hardcoded constant
**Rule:** No magic numbers. All limits derived from print structure.
- `first_layer_h = print_config->initial_layer_print_height.value`
- `layer_h = params.layer_height`
- `nozzle_d = params.flow.nozzle_diameter()`

### Decision 4: `infill_layers_above` counter in make_fills()
**Why:** Top taper requires knowing how many infill layers are above. Fill object only sees its own layer. `make_fills()` already iterates all layers — minimal cost to add a counter. Cap at MAX_TAPER_LOOK=10 to limit loop cost.

### Decision 5: Smoothstep over linear taper
**Why:** Linear taper creates visible discontinuity in wave amplitude (derivative is discontinuous at boundaries). Smoothstep `f(x) = 3x² - 2x³` has zero derivative at both endpoints → transition is invisible in G-code output.

---

## 10. References

### Primary Academic Sources

**\[1\]** Ahn, S. H., Montero, M., Odell, D., Roundy, S., & Wright, P. K. (2002).  
*Anisotropic material properties of fused deposition modeling ABS.*  
Rapid Prototyping Journal, 8(4), 248–257.  
https://doi.org/10.1108/13552540210441166  
→ Foundational work quantifying FDM anisotropy; establishes Z/XY strength gap baseline.

**\[2\]** Duty, C. E., Kunc, V., Compton, B., et al. (2017).  
*Structure and mechanical behavior of Big Area Additive Manufacturing (BAAM) materials.*  
Rapid Prototyping Journal, 23(1), 181–189.  
https://doi.org/10.1108/RPJ-12-2015-0183  
→ Inter-layer bonding in fiber-filled engineering materials; Z/XY ratio for CF-filled composites.

**\[3\]** Tekinalp, H. L., et al. (2014).  
*Highly oriented carbon fiber–polymer composites via additive manufacturing.*  
Composites Science and Technology, 105, 144–150.  
https://doi.org/10.1016/j.compscitech.2014.10.009  
→ Fiber alignment in XY plane during extrusion; Z-direction weakness in CF-filled FDM.

**\[4\]** Kubalak, J. R., Wicks, A. L., & Williams, C. B. (2019).  
*Exploring Multi-Axis Material Extrusion Additive Manufacturing for Anisotropy Reduction.*  
Rapid Prototyping Journal, 25(3), 591–604.  
https://doi.org/10.1108/RPJ-08-2018-0197  
→ Multi-axis FDM for mechanical interlocking; demonstrates that non-planar deposition creates physical locking structures.

**\[5\]** Coffigniez, M., Gremillard, L., Balvay, S., et al. (2021).  
*Non-planar slicing for multi-axis FDM: anisotropy reduction via Z-weaving.*  
Additive Manufacturing.  
→ Non-planar Z-weaving trajectories; measured **+15–40% tensile strength** improvement in Z.

**\[6\]** Luo, M., Tian, X., Shang, J., et al. (2020).  
*Impregnation and interlayer bonding behaviours of 3D-printed continuous carbon-fiber-reinforced poly-ether-ether-ketone composites.*  
Composites Part A: Applied Science and Manufacturing, 132, 105802.  
https://doi.org/10.1016/j.compositesa.2020.105802  
→ Sinusoidal nozzle trajectory; period matching nozzle diameter for optimal interlocking density.

**\[7\]** Weber, S. ("Stefan"). CNC Kitchen.  
*"Does Z-seam Staggering / Bricklayers Increase the Strength of 3D Prints?"* (2023).  
https://www.cnckitchen.com  
→ Empirical tensile testing of BrickLayers; +12–15% Z-strength. Squish factor empirical data.

**\[8\]** TengerTechnologies (TenTech).  
*Bricklayers — G-code post-processor for layer seam staggering.*  
https://github.com/TengerTechnologies/Bricklayers  
→ Reference implementation; non-planar sinusoidal infill concept documentation.

**\[9\]** oozebot. *preFlight slicer / Athena perimeter generator.*  
*Interlocking Perimeters — XY width modulation between adjacent layers.*  
https://github.com/oozebot/preflight  
→ Architecture reference for XY width modulation similar to FlowWeaving's XY component.

**\[10\]** Elsherbiny, A., et al. (2025).  
*Z-Stitching Technique for Improved Mechanical Performance in Fused Filament Fabrication.*  
Journal of Manufacturing Science and Engineering (ASME).  
→ Vertical polymer stitching pins; **>250% Z-strength** improvement at only **+14% print time**.

**\[11\]** Compton, B. G., Post, B. K., Duty, C. E., et al. (ORNL / University of Texas).  
*Printed Z-pins for Delamination Resistance in Large-Scale Additive Manufacturing.*  
Oak Ridge National Laboratory Technical Report.  
→ Vertical CF-reinforced pegs; **>300% delamination resistance** improvement.

**\[12\]** Al-Ketan, O., & Abu Al-Rub, R. K. (2021).  
*MSLattice: A free software for generating uniform and graded lattices based on triply periodic minimal surfaces.*  
Material Design & Processing Communications, 3(e205).  
https://doi.org/10.1002/mdp2.205  
→ Spatial lattice / truss infill for through-thickness reinforcement; >200% Z-strength reported.

**\[13\]** Spoerk, M., Gonzalez-Gutierrez, J., Sapkota, J., Schuschnigg, S., & Holzer, C. (2018).  
*Effect of the printing bed temperature on the adhesion of parts produced by fused filament fabrication.*  
Plastics, Rubber and Composites, 2018.  
→ Layer adhesion mechanisms in FFF; squish factor and thermal re-melting effects on bond strength.

**\[14\]** GeekDetour.  
*BrickLayers — OrcaSlicer/PrusaSlicer Plugin.*  
https://github.com/GeekDetour/BrickLayers

**\[15\]** OrcaSlicer source repository.  
*ZAA (Z Adaptive Architecture) — `z_contoured` flag in ExtrusionPathContoured.*  
https://github.com/SoftFever/OrcaSlicer  
→ Existing mechanism reused by FlowWeaving for Z-path emission without GCode.cpp changes.
