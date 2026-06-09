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
    void reset_path() { m_path_offset = 0.; m_total_path_length = 0.; }

    // Set total length of the entire multi-path (infill line) — call once
    // at the start of each multi-path, before any advance() or compute_z().
    // Used for wall taper: near the line endpoints (walls), modulation
    // amplitude fades to zero to prevent wall collision at modulated Z.
    void set_total_path_length(double len) { m_total_path_length = len; }

    // Advance the cumulative offset after processing a sub-path.
    // `local_path_length` is the total length of lines within that sub-path.
    void advance(double local_path_length) { m_path_offset += local_path_length; }

    // ── Object boundary ─────────────────────────────────────────────────

    // Set the local Z ceiling — the Z of the nearest solid/top surface
    // layer above this infill region (e.g. Benchy deck at Z≈48mm, not
    // the chimney top at Z≈60mm).  compute_z() uses this as the upward
    // ceiling so material doesn't protrude through intermediate top faces.
    // Pass 0 or negative to disable the ceiling.
    void set_top_z(double top_z) { m_top_z = top_z; }

    // Set the local Z floor — the Z of the nearest solid/bottom surface
    // layer below this infill region.
    void set_floor_z(double floor_z) { m_floor_z = floor_z; }

    // ── Wall taper factor ────────────────────────────────────────────────
    //
    // Returns a factor in [0, 1] that fades modulation near the endpoints
    // of an infill line where it touches the perimeter wall.
    //
    //   taper_distance — distance over which modulation fades (mm)
    //   dist           — current position along the FULL multi-path (mm)
    //
    // At the wall (dist=0 or dist=total_length): factor = 0 (flat)
    // Beyond taper_distance from wall:          factor = 1 (full modulation)
    //
    // The taper_distance for Z = z_deflection = z_amplitude/100 * layer_height
    // The taper_distance for XY = peak_extra_half_width = base_w * amplitude / 2
    //
    // This is also exposed publicly for use by FillFlowWeaving (XY taper).
    inline double compute_taper(double dist, double taper_distance) const
    {
        if (m_total_path_length <= 0. || taper_distance <= 0.)
            return 1.0;
        double dist_from_start = dist;
        double dist_from_end   = m_total_path_length - dist;
        double dist_from_wall  = std::min(dist_from_start, dist_from_end);
        if (dist_from_wall >= taper_distance)
            return 1.0;
        if (dist_from_wall <= 0.)
            return 0.0;
        // Smooth taper using sin²  (0 → 1 over taper distance)
        double ratio = dist_from_wall / taper_distance;
        return ratio * ratio * (3.0 - 2.0 * ratio); // smoothstep
    }

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
    // Near the endpoints (walls), amplitude tapers to zero via smoothstep
    // to prevent the nozzle from visiting Z-levels where it would be
    // outside the perimeter wall.
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

        // Full desired deflection (no pre-reduction)
        const double desired = amp * layer_height;

        // Wall taper: fade modulation near line endpoints (walls).
        // The 3× multiplier accounts for angled walls: on a wall at angle θ
        // from vertical, the wall shifts inward by z_deflection × tan(θ).
        // Factor 3 covers walls up to ~72° from vertical (tan⁻¹(3) ≈ 72°),
        // handling virtually all FDM geometries.
        static constexpr double kWallAngleFactor = 3.0;
        double z_taper_dist = desired * kWallAngleFactor;
        // Don't let taper exceed half the line length (leave some modulation)
        if (m_total_path_length > 0.)
            z_taper_dist = std::min(z_taper_dist, m_total_path_length * 0.45);
        const double taper = compute_taper(dist, z_taper_dist);

        const double phase = M_PI * (layer_id % 2);           // 0 or π
        const double t     = std::sin(2.0 * M_PI * dist / period + phase);
        double z = nominal_z + desired * taper * t;

        // Safety clamp: protect floor (bottom solid surface) and ceiling (top solid surface)
        if (m_floor_z > 0. && z < m_floor_z)
            z = m_floor_z;
        else if (z < kMinZ)
            z = kMinZ;
        if (m_top_z > 0. && z > m_top_z)
            z = m_top_z;
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
    double m_path_offset       = 0.;
    double m_total_path_length = 0.;  // total length of current infill line
    double m_top_z             = 0.;  // local ceiling Z (nearest top surface above); 0 = no ceiling
    double m_floor_z           = 0.;  // local floor Z (nearest bottom surface below); 0 = use kMinZ
};

} // namespace Slic3r

#endif // slic3r_FlowWeavingZModulator_hpp_
