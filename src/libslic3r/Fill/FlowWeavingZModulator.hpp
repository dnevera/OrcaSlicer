// ── Flow Weaving Z-Modulator ───────────────────────────────────────────────
//
// Header-only utility that computes sinusoidal Z-height modulation for the
// Flow Weaving infill pattern.  Called from GCode.cpp during extrusion to
// vertically interlock adjacent layers.
//
// The XY width modulation counterpart lives in FillFlowWeaving.cpp (Fill level).
// Both share the same wave period and phase alternation logic.
//
// Usage:
//   if (FlowWeavingZModulator::is_active(pattern, z_amp, layer, role)) {
//       double z = FlowWeavingZModulator::compute_z(...);
//       // emit G1 X.. Y.. Z{z} E..
//   }
// ───────────────────────────────────────────────────────────────────────────

#ifndef slic3r_FlowWeavingZModulator_hpp_
#define slic3r_FlowWeavingZModulator_hpp_

#include "../PrintConfig.hpp"
#include "../ExtrusionEntity.hpp"

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace Slic3r {

class Layer;

class FlowWeavingZModulator
{
public:
    // Check whether Z-modulation should be applied for this extrusion context.
    //
    //  pattern     — current sparse_infill_pattern (must be ipFlowWeaving)
    //  z_amplitude — flow_weaving_z_amplitude value (%, >0 to activate)
    //  layer       — current layer pointer (must be non-null)
    //  role        — extrusion role (only solid/internal infill are modulated)
    static inline bool is_active(
        InfillPattern  pattern,
        double         z_amplitude,
        const Layer   *layer,
        ExtrusionRole  role)
    {
        return pattern == ipFlowWeaving
            && z_amplitude > 0
            && layer != nullptr
            && (role == erSolidInfill || role == erInternalInfill);
    }

    // Compute the modulated Z coordinate for a point along the extrusion path.
    //
    //  nominal_z    — the layer's nominal print Z (mm)
    //  path_length  — distance along the current extrusion path (mm)
    //  layer_height — layer height (mm), used to scale amplitude
    //  z_amplitude  — modulation amplitude as % of layer_height (e.g. 15 → 0.15)
    //  period       — wave period in mm
    //  layer_id     — layer index, used for phase alternation (even=0, odd=π)
    //
    // Returns: modulated Z, clamped to a minimum of 0.05 mm for safety.
    static inline double compute_z(
        double nominal_z,
        double path_length,
        double layer_height,
        double z_amplitude,
        double period,
        int    layer_id)
    {
        const double amp   = z_amplitude / 100.0;             // % → fraction
        const double phase = M_PI * (layer_id % 2);           // 0 or π
        const double t     = std::sin(2.0 * M_PI * path_length / period + phase);
        double z = nominal_z + amp * layer_height * t;
        if (z < 0.05)
            z = 0.05;                                          // safety floor
        return z;
    }
};

} // namespace Slic3r

#endif // slic3r_FlowWeavingZModulator_hpp_
