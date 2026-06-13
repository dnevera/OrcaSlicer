// ── FillFlowWeaving ────────────────────────────────────────────────────────────
//
// Flow Weaving infill pattern — Z and XY width modulation for inter-layer
// mechanical interlocking.
//
// Architecture (sub-segmentation):
//   Each infill polyline is subdivided into short sub-segments.  Each sub-segment
//   becomes its own ExtrusionPathContoured with:
//     - z_contoured = true  (ZAA mechanism handles Z emission in GCode.cpp)
//     - Polyline3 with Z offsets baked into the point coordinates
//     - mm3_per_mm already scaled by the XY flow factor
//   Sub-segments are collected into an ExtrusionMultiPath.
//
// This design requires ZERO changes to ExtrusionEntity or GCode.cpp.
// All FW logic is self-contained in Fill/FlowWeaving/.
//
// Safe zone:
//   no_overlap_expolygons (standard Fill field) is pre-tightened by Fill.cpp
//   make_fills() to the intersection of current + adjacent layers.
//   Modulation is gated to 0 outside this zone.
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

    // Request adjacent-layer no_overlap data from make_fills().
    bool needs_cross_layer_data() const override { return true; }

    // FlowWeaving runs at any density, including 100% where stInternal→stInternalSolid.
    // Returning true redirects those solid internal surfaces back through our fill code.
    bool handles_solid_internal() const override { return true; }

    // Override to inject Z/XY modulation via sub-segmented ExtrusionPathContoured.
    void fill_surface_extrusion(
        const Surface*          surface,
        const FillParams&       params,
        ExtrusionEntitiesPtr&   out) override;

    bool can_filter_gcode() const override { return true; }
    std::string filter_gcode(const std::string &gcode, const FullPrintConfig &config) const override;
    void validate_gcode(const std::string &gcode, const FullPrintConfig &config) const override;

protected:
    // Sub-segments per wave period (overridable for testing).
    virtual int subdivisions_per_period() const { return 8; }
};

} // namespace Slic3r

#endif // slic3r_FillFlowWeaving_hpp_
