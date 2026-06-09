#ifndef slic3r_FillFlowWeaving_hpp_
#define slic3r_FillFlowWeaving_hpp_

#include "../libslic3r.h"
#include "FillBase.hpp"

namespace Slic3r {

// Flow Weaving: generates rectilinear 100% infill paths with
// variable extrusion width (XY modulation) per sub-segment.
// Z modulation is applied later in GCode.cpp during extrusion.
class FillFlowWeaving : public Fill
{
public:
    FillFlowWeaving() {}
    Fill* clone() const override { return new FillFlowWeaving(*this); }

    bool use_bridge_flow() const override { return false; }
    bool is_self_crossing() override { return false; }

    // Override to generate ExtrusionMultiPath with variable width per sub-segment
    void fill_surface_extrusion(const Surface *surface,
                                const FillParams &params,
                                ExtrusionEntitiesPtr &out) override;

protected:
    void _fill_surface_single(
        const FillParams              &params,
        unsigned int                   thickness_layers,
        const std::pair<float, Point> &direction,
        ExPolygon                      expolygon,
        Polylines                     &polylines_out) override;
};

} // namespace Slic3r

#endif // slic3r_FillFlowWeaving_hpp_
