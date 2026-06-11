// ── Flow Weaving Modulator ────────────────────────────────────────────────────
//
// Abstract base + factory for modulation bases used by the FlowWeaving infill.
// The modulator computes a normalised value t ∈ [-1, 1] as a function of the
// position along an infill line.  This value is then scaled by FillFlowWeaving
// to produce Z offsets and width factors.
//
// Currently available modulators:
//   • Sine   — smooth sinusoidal wave (default)
//
// Future bases (add new subclasses + factory entry to extend):
//   • Square — hard square wave (sharper interlocking ridges)
//   • Perlin — Perlin-noise-based organic variation
//
// Usage in FillFlowWeaving.cpp:
//
//   auto mod = FlowWeavingModulator::create(ModulatorType::Sine);
//   double t = mod->compute(pos_mm, period_mm, phase_rad);
//   double taper = mod->taper(pos_mm, line_len_mm, taper_len_mm);
//   double z_offset = z_amp * layer_h * t * taper * gate;
//
// ─────────────────────────────────────────────────────────────────────────────

#ifndef slic3r_FlowWeavingZModulator_hpp_
#define slic3r_FlowWeavingZModulator_hpp_

#include <cmath>
#include <memory>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace Slic3r {

// ─── Modulator type tag ──────────────────────────────────────────────────────
enum class ModulatorType {
    Sine,      // default: smooth sinusoidal wave
    // Square, // future: square wave
    // Perlin, // future: Perlin noise
};

// ─── Abstract base ───────────────────────────────────────────────────────────
class FlowWeavingModulator
{
public:
    virtual ~FlowWeavingModulator() = default;

    // Compute normalised modulation value t ∈ [-1, 1].
    //   pos        — current position along the infill line (mm, monotone ↑)
    //   period     — wavelength of one full cycle (mm)
    //   phase_rad  — phase offset in radians (use M_PI to alternate layers)
    virtual double compute(double pos, double period, double phase_rad) const = 0;

    // Smoothstep taper near line endpoints (wall proximity → t fades to 0).
    //   pos        — current position along the full infill line (mm)
    //   line_len   — total length of the infill line (mm)
    //   taper_len  — taper region length from each endpoint (mm)
    // Returns factor ∈ [0, 1]; 0 = at wall, 1 = far from wall.
    virtual double taper(double pos, double line_len, double taper_len) const;

    // Factory: create a modulator of the specified type.
    static std::unique_ptr<FlowWeavingModulator> create(ModulatorType type);
};

// ─── Default taper implementation (shared by all subclasses) ─────────────────
inline double FlowWeavingModulator::taper(double pos, double line_len, double taper_len) const
{
    if (line_len <= 0.0 || taper_len <= 0.0)
        return 1.0;
    double dist_from_start = pos;
    double dist_from_end   = line_len - pos;
    double dist_from_wall  = std::min(dist_from_start, dist_from_end);
    if (dist_from_wall >= taper_len)
        return 1.0;
    if (dist_from_wall <= 0.0)
        return 0.0;
    // Smoothstep: f(x) = 3x² − 2x³  (x = dist_from_wall / taper_len)
    double x = dist_from_wall / taper_len;
    return x * x * (3.0 - 2.0 * x);
}

// ─── Sine modulator ──────────────────────────────────────────────────────────
class SineModulator : public FlowWeavingModulator
{
public:
    // t = sin(2π·pos/period + phase_rad) ∈ [-1, 1]
    double compute(double pos, double period, double phase_rad) const override
    {
        if (period <= 0.0) return 0.0;
        return std::sin(2.0 * M_PI * pos / period + phase_rad);
    }
};

// ─── Factory ─────────────────────────────────────────────────────────────────
inline std::unique_ptr<FlowWeavingModulator> FlowWeavingModulator::create(ModulatorType type)
{
    switch (type) {
    case ModulatorType::Sine:
    default:
        return std::make_unique<SineModulator>();
    }
}

} // namespace Slic3r

#endif // slic3r_FlowWeavingZModulator_hpp_
