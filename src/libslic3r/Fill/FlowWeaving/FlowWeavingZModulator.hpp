// ── Flow Weaving Z-Modulator ───────────────────────────────────────────────
//
// Stateful utility that computes sinusoidal Z-height modulation for the
// Flow Weaving infill pattern.  Held as a member in GCode and called
// during extrusion to vertically interlock adjacent layers.
//
// The XY width modulation counterpart lives in FillFlowWeaving.cpp.
// Both share the same wave period and phase alternation logic.
//
// Usage (in GCode.cpp):
//
//   // at start of extrude_multi_path():
//   m_fw_z_mod.reset_path();
//
//   // inside _extrude(), for each Line in the sub-path:
//   if (m_fw_z_mod.is_active(pattern, z_amp, layer, role)) {
//       double z = m_fw_z_mod.compute_z(nominal_z, local_path_len, ...);
//   }
//
//   // after the line loop in _extrude():
//   m_fw_z_mod.advance(path_length);
//
// ───────────────────────────────────────────────────────────────────────────

#ifndef slic3r_FlowWeavingZModulator_hpp_
#define slic3r_FlowWeavingZModulator_hpp_

#include "../../PrintConfig.hpp"
#include "../../ExtrusionEntity.hpp"

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace Slic3r {

class Layer;

class FlowWeavingZModulator
{
public:
    // ── Activation check (stateless) ─────────────────────────────────────
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

    // ── Path distance tracking ───────────────────────────────────────────

    // Reset cumulative distance — call at the start of each multi-path
    // (i.e. each infill line composed of width-modulated sub-segments).
    void reset_path() { m_path_offset = 0.; }

    // Advance the cumulative offset after processing a sub-path.
    // `local_path_length` is the total length of lines within that sub-path.
    void advance(double local_path_length) { m_path_offset += local_path_length; }

    // ── Object boundary ─────────────────────────────────────────────────

    // Set the model's top Z — call once per object (e.g. from the last
    // layer's print_z).  compute_z() uses this as the upward ceiling
    // so material doesn't protrude above the top face.
    // Pass 0 or negative to disable the ceiling.
    void set_top_z(double top_z) { m_top_z = top_z; }

    // ── Z computation (uses internal state) ──────────────────────────────
    //
    //  nominal_z         — the layer's nominal print Z (mm)
    //  local_path_length — distance accumulated within the CURRENT sub-path (mm)
    //  layer_height      — layer height (mm), used to scale amplitude
    //  z_amplitude       — modulation amplitude as % of layer_height
    //  period            — wave period in mm
    //  layer_id          — layer index, used for phase alternation (even=0, odd=π)
    //
    // The effective distance along the wave is m_path_offset + local_path_length,
    // giving a continuous sine wave across all sub-paths of a multi-path.
    //
    // Amplitude is adaptively reduced near boundaries:
    //   - downward: limited so Z never drops below kMinZ (0.05 mm) — bed
    //   - upward:   limited so Z never exceeds m_top_z — model top face
    // On middle layers both headrooms are large, so full amplitude is used.
    // Interlocking into ADJACENT layers is preserved — only the absolute
    // model boundaries are enforced.
    inline double compute_z(
        double nominal_z,
        double local_path_length,
        double layer_height,
        double z_amplitude,
        double period,
        int    layer_id) const
    {
        static constexpr double kMinZ = 0.05;   // absolute Z floor (mm)

        const double dist  = m_path_offset + local_path_length;
        const double amp   = z_amplitude / 100.0;             // % → fraction

        // Desired deflection magnitude (symmetric ±)
        double desired = amp * layer_height;

        // Available headroom in each direction
        const double room_down = nominal_z - kMinZ;           // first layer bottleneck
        const double room_up   = (m_top_z > 0.)
                                     ? (m_top_z - nominal_z)  // last layers near top
                                     : desired;               // no ceiling set → unlimited

        // Effective deflection: scale symmetrically to the tighter constraint
        // so the sine wave stays smooth (no mid-wave discontinuities).
        double effective = desired;
        if (effective > room_down)
            effective = room_down;
        if (effective > room_up)
            effective = room_up;
        if (effective < 0.)
            effective = 0.;

        const double phase = M_PI * (layer_id % 2);           // 0 or π
        const double t     = std::sin(2.0 * M_PI * dist / period + phase);
        double z = nominal_z + effective * t;

        // Final safety clamp (belt-and-suspenders for numerical edge cases)
        if (z < kMinZ)
            z = kMinZ;
        return z;
    }

    // ── Z-reset suppression ──────────────────────────────────────────────
    //
    // When FW Z-modulation is active, the "reset Z after contouring" logic
    // in _extrude() must be skipped — otherwise it snaps Z back to nominal
    // between every sub-path, destroying the wave.
    static inline bool should_skip_z_reset(
        InfillPattern  pattern,
        double         z_amplitude,
        const Layer   *layer,
        ExtrusionRole  role)
    {
        return is_active(pattern, z_amplitude, layer, role);
    }

private:
    double m_path_offset = 0.;
    double m_top_z       = 0.;   // model top face Z; 0 = no ceiling
};

} // namespace Slic3r

#endif // slic3r_FlowWeavingZModulator_hpp_
