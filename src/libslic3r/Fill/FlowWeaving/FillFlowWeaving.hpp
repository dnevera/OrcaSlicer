// ── FillFlowWeaving ────────────────────────────────────────────────────────────
//
// Flow Weaving infill pattern.
//
// Generates rectilinear infill polylines with Z modulation and XY width
// modulation baked directly into the extrusion entities.  The nozzle oscillates
// above and below the nominal layer Z, mechanically interlocking adjacent layers
// and improving inter-layer tensile strength.
//
// Safe zone:
//   fw_safe_expolygons (inherited from FillBase) contains the cross-layer safe
//   zone pre-computed by Layer::make_fills() as the intersection of the current,
//   upper, and lower layers' no_overlap_expolygons.  Modulation amplitude is
//   gated to 0 outside this zone to prevent nozzle collision with walls.
//
// Algorithm:
//   1. Generate base rectilinear polylines (via FillRectilinear).
//   2. Subdivide each polyline segment into sub-segments of length ≤ step_mm.
//   3. For each sub-segment midpoint:
//      a. Compute modulator value t ∈ [-1, 1] (sine wave).
//      b. Compute taper factor (fade near endpoints).
//      c. Compute gate = 1 if midpoint is inside fw_safe_expolygons, else 0.
//      d. z_offset = z_amp * layer_h * t * taper * gate
//      e. width_factor = 1 + xy_amp * |t| * taper * gate
//   4. Emit ExtrusionPath with z_contoured=true and adjusted mm3_per_mm.
//
// ─────────────────────────────────────────────────────────────────────────────

#ifndef slic3r_FillFlowWeaving_hpp_
#define slic3r_FillFlowWeaving_hpp_

#include "../FillRectilinear.hpp"

namespace Slic3r {

class FillFlowWeaving : public FillRectilinear
{
public:
    Fill* clone() const override { return new FillFlowWeaving(*this); }
    ~FillFlowWeaving() override = default;

    // Override fill_surface_extrusion to inject Z/XY modulation.
    void fill_surface_extrusion(
        const Surface*          surface,
        const FillParams&       params,
        ExtrusionEntitiesPtr&   out) override;

protected:
    // Returns the number of sub-segments per mm for wave subdivision.
    // Default: 8 points per period (overridable for testing).
    virtual int subdivisions_per_period() const { return 8; }
};

} // namespace Slic3r

#endif // slic3r_FillFlowWeaving_hpp_
