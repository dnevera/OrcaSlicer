// ── Flow Weaving Modulator ────────────────────────────────────────────────────
//
// Abstract base + factory for wave functions used by the FlowWeaving infill.
// Computes a normalised value t ∈ [-1, 1] as a function of position along an
// infill line.  FillFlowWeaving scales this to produce Z offsets and width
// modulation.
//
// Currently available:  Sine (smooth sinusoidal wave)
// Future extensions:    Square, Perlin — add subclass + factory entry.
//
// Header-only — no .cpp needed, not registered in CMakeLists.
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
    Sine,      // smooth sinusoidal wave (default)
    // Square, // future: square wave
    // Perlin, // future: Perlin noise
};

// ─── Abstract base ───────────────────────────────────────────────────────────
class FlowWeavingModulator
{
public:
    virtual ~FlowWeavingModulator() = default;

    // Compute normalised modulation value t ∈ [-1, 1].
    //   pos        — position along the infill line (mm, monotonically increasing)
    //   period     — wavelength of one full cycle (mm)
    //   phase_rad  — phase offset in radians (use M_PI to alternate layers)
    virtual double compute(double pos, double period, double phase_rad) const = 0;

    // Smoothstep taper near line endpoints.
    //   pos        — current position along the infill line (mm)
    //   line_len   — total length of the infill line (mm)
    //   taper_len  — length of the taper region from each endpoint (mm)
    // Returns ∈ [0, 1]:  0 at wall, 1 far from wall.
    virtual double taper(double pos, double line_len, double taper_len) const;

    // Factory
    static std::unique_ptr<FlowWeavingModulator> create(ModulatorType type);
};

// ─── Default taper (smoothstep, shared by all subclasses) ────────────────────
inline double FlowWeavingModulator::taper(double pos, double line_len, double taper_len) const
{
    if (line_len <= 0.0 || taper_len <= 0.0)
        return 1.0;
    double dist_from_wall = std::min(pos, line_len - pos);
    if (dist_from_wall >= taper_len)
        return 1.0;
    if (dist_from_wall <= 0.0)
        return 0.0;
    // Smoothstep: f(x) = 3x² − 2x³
    double x = dist_from_wall / taper_len;
    return x * x * (3.0 - 2.0 * x);
}

// ─── Sine modulator ──────────────────────────────────────────────────────────
class SineModulator : public FlowWeavingModulator
{
public:
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
