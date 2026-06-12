# Technical Evaluation of FlowWeaving (Z-Interlocking) & Refinement Solutions

This document presents the technical analysis, initial print test results, identified engineering deficiencies, and the implemented parameters to optimize the **FlowWeaving** (Z-Interlocking) infill algorithm in OrcaSlicer.

---

## 1. Introduction and Core Purpose

Standard FDM 3D printing suffers from severe anisotropy: tensile strength along the Z-axis (inter-layer adhesion) is significantly lower than along the X/Y plane. **FlowWeaving** resolves this by introducing a three-dimensional interlocking structure between adjacent layers. 

By modulating the Z-height of the nozzle dynamically during infill extrusion and shifting the phase of these oscillations between layers, the algorithm creates a physical "sine-wave lock" (Z-interlocking) where layers mechanically key into one another.

---

## 2. Results of the First Print Tests (Specimens from `Molding-FWeav.gcode.3mf`)

To evaluate the legacy FlowWeaving behavior, a print test was conducted using two configurations sliced from the `Molding-FWeav.gcode.3mf` project:
1. **Cube 118 (Horizontal Specimen):** A rectangular prism lying flat on its side.
2. **Cube 117 (Vertical Specimen):** The same rectangular prism printed vertically (standing on its small end, cross-section `19.5 x 17 mm`).

Both models were printed using the legacy algorithm with a hardcoded **1.5 mm taper length** and **0.0 mm wall overlap**. The results showed critical printing defects and mechanical failures:

### Test Results for Specimen A: Cube 118 (Horizontal)
* **Visual Appearance:** The infill structure printed successfully in the middle section of the block, but suffered from severe surface finish degradation near the perimeters.
* **Mechanical Failure (Delamination):** When subjected to destructive testing (bending the part), the infill easily separated from the inner perimeter walls. 
* **Underlying Defect:** Near the walls, the wave amplitude faded to zero. The legacy algorithm terminated the infill lines right at the wall boundary without any overlap. Because the line ends were thin and flat, there was no physical fusion (no mechanical keying) between the infill and the walls, leading to border delamination under stress.

### Test Results for Specimen B: Cube 117 (Vertical)
* **Visual Appearance:** The part printed without crashing, but the Z-interlocking pattern was practically invisible.
* **Mechanical Failure (Inter-layer Shear):** Upon bending, the vertical specimen snapped cleanly along a horizontal layer line, behaving exactly like standard, weak FDM infill.
* **Underlying Defect:** Because the cross-section was very small (`19.5 x 17 mm`), the infill paths were extremely short. With the hardcoded taper length of 1.5 mm at each end (totaling 3.0 mm of dampening per path), the Z-wave never had enough room to reach its full amplitude. The Z-height oscillation was clamped to a negligible range of $\pm 0.024\text{ mm}$ (instead of the planned $\pm 0.09\text{ mm}$). Consequently, the mechanical Z-lock never formed.

---

## 3. Physical Constraints of Kinematics and Extrusion Systems

The initial print failures highlight key physical limits of modern desktop 3D printer hardware when processing high-frequency spatial modulations.

### A. Z-Axis Kinematics & Oscillation Frequency Limits
The oscillation frequency $f$ of the Z-axis is determined by the toolhead translation velocity $V$ and the spatial wave period $\lambda$:
$$f = \frac{V}{\lambda}$$

* **High-Frequency Torque Drop:** At a standard infill speed ($V = 180\text{ mm/s}$) and wave period ($\lambda = 3.0\text{ mm}$), the Z-axis must oscillate at $f = \mathbf{60\text{ Hz}}$. Standard lead-screw or belt-driven Z-stages (especially heavy heated beds on CoreXY or Cartesian printers) have high rotational and translational inertia. As stepper motor step-rates rise, their available torque drops exponentially. At 50–60 Hz of continuous directional reversals, Z-stepper motors lose synchronization, overheat, and skip steps (stall).
* **Mechanical Backlash:** At high frequencies, any mechanical play (backlash) in lead-screw nuts or belt assemblies results in severe positioning errors, flattening out the Z-wave and destroying the precision needed for Z-interlocking.

### B. Volumetric Flow Rate Limitations & Extrusion Pressure Lag
During the peak of the wave, the nominal flow rate is multiplied by both height and width factors:
$$E_{\text{peak}} = E_{\text{nominal}} \cdot \left(1 + \frac{A_Z}{100}\right) \cdot \left(1 + \frac{A_{XY}}{100}\right)$$
For a typical test ($A_Z = 55\%$, $A_{XY} = 50\%$):
$$E_{\text{peak}} = E_{\text{nominal}} \cdot 1.55 \cdot 1.50 = \mathbf{2.325 \cdot E_{\text{nominal}}}$$

* **Volumetric Flow Cap:** If the nominal extrusion rate is $12\text{ mm}^3/\text{с}$, the peak rate jumps to $\mathbf{27.9\text{ mm}^3/\text{с}}$. This significantly exceeds the maximum melting capacity of standard hotends (typically $15\text{–}20\text{ mm}^3/\text{с}$ for PLA). The excess solid filament cannot melt in time, causing the extruder drive gears to grind the plastic.
* **Hydrodynamic Pressure Lag:** The melt zone inside a hotend acts as a hydraulic capacitor. The time delay between the extruder motor movement and the actual change in nozzle output pressure (pressure advance/lag) ranges between **20 ms and 80 ms**. At $60\text{ Hz}$ oscillation, a single wave cycle lasts only **16.6 ms**. Since the oscillation period is shorter than the pressure lag time, the pressure inside the nozzle never reaches the required peak or valley states. Instead, it smooths out, while the extruder motor struggles with massive alternating backpressure, causing motor overheating and filament grinding.

---

## 4. Implemented Engineering Refinements

To resolve these physical constraints, improve surface quality, and maximize mechanical strength, three new configurable parameters were introduced:

```
                  ◄─── Weaving Taper Length ───►
                  (User-defined: e.g., 0.6 mm)
                  ┌────────────────────────────┐
 ─────────┐       │   Wave amplitude fades     │   ┌───────────────
          │       │   out to nominal values.   │   │
  Inner   │       │                            │   │  Active Wave
  Wall    │◄─────►│                            │   │  (Full Z & XY
          │Overlap│                            │   │  Modulation)
          │(e.g., │                            │   │
          │0.15mm)│                            │   │
 ─────────┘       └────────────────────────────┘   └───────────────
```

### 1. Speed Capping (`flow_weaving_speed`)
* **Default Value:** `50.0 mm/s` (separately defined from `sparse_infill_speed`).
* **Purpose:** Limits the toolhead speed to keep Z-axis oscillation frequency within a safe kinematic and thermal envelope:
  $$f = \frac{50\text{ mm/s}}{3\text{ mm}} \approx \mathbf{16.6\text{ Hz}}$$
  This prevents Z-stepper stalls, lowers mechanical vibration, and keeps the peak volumetric flow well within the hotend's melting limits.

### 2. Weaving Wall Overlap (`flow_weaving_wall_overlap`)
* **Default Value:** `0.0 mm` (Safe mode); Recommended: `0.10 - 0.15 mm`.
* **Purpose:** Modifies the wall-clamping limits (`max_expansion_mm` and `max_lateral`) in `FillFlowWeaving.cpp`. 
* **Mechanism:** Allows the lateral peak of the XY-wave to physically penetrate the inner perimeter lines by the specified amount:
  $$\text{max\_lateral} = \max(0.0, \text{dist\_to\_wall} - \text{flow\_width} \cdot \text{width\_mod} \cdot 0.5 + \text{wall\_overlap})$$
  This fuses the infill wave directly into the wall perimeter, creating a solid chemical/thermal bond and eliminating the infill-to-wall delamination failure mode.

### 3. Weaving Taper Length (`flow_weaving_taper_length`)
* **Default Value:** `1.5 mm` (Legacy behavior); Recommended for small parts: `0.5 - 0.8 mm`.
* **Purpose:** Replaces the hardcoded `1.5 mm` dampening limit. 
* **Mechanism:** Compresses the transition zone near boundaries. By choosing a smaller taper length (e.g., `0.6 mm`), the wave retains its full vertical and lateral interlocking amplitude much closer to the perimeters, restoring structural strength in narrow cross-sections and vertical specimens.

### 4. Top Surface Taper Fix (XY and Flow/Width modulation)
* **The Issue:** The legacy top-surface taper code scaled down only the Z-axis amplitude. The XY lateral path displacement (zigzag) and the line width modulation (flow pulses) were not multiplied by `taper_scale`. When printing solid structures (e.g., at 100% infill density), this left a highly textured, wavy XY pattern on the infill layers directly beneath the flat top shell. The top shell, when laid down, duplicated this waviness, leading to severe surface defects and rough ridges.
* **The Solution:** The `taper_scale` factor is now applied to both the active XY path displacement amplitude (`xy_off_start` / `xy_off_end`) and the width/flow modulation factor (`active_xy_amp_frac`), driving them to nominal straight-line values (displacement = 0, width modulation = 0) on the last `top_taper_layers` before the top shell. This ensures a perfectly flat and solid bed for the final outer layers.

---

## 5. Experimental Verification Protocol

After compilation, verify the behavior of these parameters as follows:

### Specimen A (Vertical Rectangular Prism - Cube 117 configuration)
* **Settings:** `flow_weaving_taper_length = 0.6 mm`, `flow_weaving_wall_overlap = 0.12 mm`, `flow_weaving_speed = 50 mm/s`.
* **Verification:** Inspect the sliced G-code. Ensure that on short infill tracks (under 4.0 mm), Z-oscillations occur over a wider percentage of the path compared to the legacy 1.5 mm taper, and the lateral peak coordinates ($X$/$Y$) overlap into the inner wall path.

### Specimen B (Horizontal Specimen - Cube 118 configuration)
* **Settings:** `flow_weaving_taper_length = 1.2 mm`, `flow_weaving_wall_overlap = 0.15 mm`.
* **Verification:** Destructive testing ("breaking the part") should result in a cohesive material failure (tearing of the polymer strands) rather than a clean adhesive separation between the infill block and the outer shell.
