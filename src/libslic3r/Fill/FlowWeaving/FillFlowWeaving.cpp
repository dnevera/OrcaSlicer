// ── Flow Weaving Infill Pattern ────────────────────────────────────────────
//
// Creates inter-layer mechanical interlocking by sinusoidally modulating the
// extrusion width (XY plane) along each infill line.  The complementary
// Z-height modulation is handled at G-code generation time (GCode.cpp).
//
// How it works:
//   1. Base rectilinear lines are generated at 100% density.
//   2. Each line is subdivided into short sub-segments (~period/8).
//   3. Each sub-segment gets a width = base_width × factor, where
//      factor = 1 + (wratio - 1) × (sin(θ) + 1) / 2.
//   4. Phase alternates between layers (even=0, odd=π) so that wide
//      segments on layer N align with narrow ones on N+1 and vice versa.
//
// Parameters (from PrintConfig):
//   flow_weaving_xy_amplitude  – width modulation as % (e.g. 15 → ×1.15)
//   flow_weaving_period        – wave length in mm (default = nozzle diameter)
//
// The Z-modulation counterpart uses flow_weaving_z_amplitude (see GCode.cpp).
// ───────────────────────────────────────────────────────────────────────────

#include "FillFlowWeaving.hpp"
#include "../FillRectilinear.hpp"
#include "../../PrintConfig.hpp"
#include "../../ExtrusionEntity.hpp"
#include "../../ClipperUtils.hpp"

#include <cmath>

namespace Slic3r {

void FillFlowWeaving::_fill_surface_single(
    const FillParams              &params,
    unsigned int                   thickness_layers,
    const std::pair<float, Point> &direction,
    ExPolygon                      expolygon,
    Polylines                     &polylines_out)
{
    // Not used directly — fill_surface_extrusion() overrides the pipeline.
    // Required by base class virtual interface; left empty intentionally.
}

void FillFlowWeaving::fill_surface_extrusion(
    const Surface *surface, const FillParams &params,
    ExtrusionEntitiesPtr &out)
{
    // ── Step 1: Generate base rectilinear toolpaths ──────────────────────
    // We reuse the standard rectilinear infill engine to get evenly spaced
    // parallel lines, then apply width modulation on top.
    FillRectilinear recti;
    recti.layer_id              = this->layer_id;
    recti.z                     = this->z;
    recti.spacing               = this->spacing;
    recti.overlap               = this->overlap;
    recti.angle                 = this->angle;
    recti.link_max_length       = this->link_max_length;
    recti.loop_clipping         = this->loop_clipping;
    recti.bounding_box          = this->bounding_box;
    recti.print_config          = this->print_config;
    recti.print_object_config   = this->print_object_config;
    recti.no_overlap_expolygons = this->no_overlap_expolygons;

    // Force 100% density — Flow Weaving always fills fully
    FillParams fw_params = params;
    fw_params.density = 1.0f;
    fw_params.dont_adjust = false;

    Polylines polylines;
    try {
        polylines = recti.fill_surface(surface, fw_params);
    } catch (InfillFailedException&) {}
    if (polylines.empty())
        return;

    // ── Step 2: Read modulation parameters ───────────────────────────────
    // flow_weaving_period       — wave period in mm (default: nozzle diam)
    // flow_weaving_xy_amplitude — percentage, converted to width ratio:
    //   e.g. 15% → wratio = 1.15 → width oscillates from ×1.0 to ×1.15
    double fw_period = 0.4;
    double fw_wratio = 1.15;   // 15% default → ×1.15
    if (params.config) {
        fw_period = params.config->flow_weaving_period.value;
        fw_wratio = 1.0 + params.config->flow_weaving_xy_amplitude.value / 100.0;
    }

    // ── Step 3: Compute base flow dimensions ─────────────────────────────
    Flow base_flow = params.flow;
    if (!params.using_internal_flow)
        base_flow = params.flow.with_spacing(float(this->spacing));

    double base_mm3 = base_flow.mm3_per_mm();  // volumetric flow per mm
    float  base_w   = base_flow.width();        // nominal line width
    float  base_h   = base_flow.height();       // layer height

    // ── Step 4: Phase alternation ────────────────────────────────────────
    // Adjacent FW layers must alternate phase (0 vs π) for interlocking.
    // Using layer_id % 2 BREAKS with combine_infill: when infill is printed
    // every 2nd layer, all infill layer_ids are odd → same phase → no interlock.
    // Fix: divide layer_id by the combine step to get a sequential index.
    // Without combine_infill, combine_step = 1 → identical to layer_id % 2.
    int combine_step = (params.config && params.config->infill_combination.value) ? 2 : 1;
    int fw_phase_idx = this->layer_id / combine_step;
    double phase = M_PI * (fw_phase_idx % 2);

    // ── Step 5: Sub-segmentation ─────────────────────────────────────────
    // Each infill line is split into sub-segments of ~period/8 length.
    // This provides smooth sinusoidal width transitions (~8 samples/wave).
    double sub_target = fw_period / 8.0;

    // ── Step 6: Build output ExtrusionEntityCollection ────────────────────
    ExtrusionEntityCollection *eec = new ExtrusionEntityCollection();
    eec->no_sort = false;
    out.push_back(eec);

    // ── Step 7: Width-modulated sub-segment generation ────────────────────
    double xy_amplitude = (fw_wratio - 1.0) / 2.0;
    // Supplementary endpoint taper distance — smooths the transition at
    // line endpoints even when the safe zone extends all the way to the wall.
    double xy_taper_dist = base_w * xy_amplitude * 0.5;

    // Whether we have a precise safe zone from multi-layer wall intersection
    const bool have_safe_zone = !this->fw_safe_expolygons.empty();

    for (Polyline &pl : polylines) {
        if (pl.points.size() < 2)
            continue;

        // Pre-compute total polyline length for supplementary endpoint taper
        double total_pl_len = 0.0;
        for (size_t i = 1; i < pl.points.size(); ++i) {
            Vec2d seg = pl.points[i].cast<double>() - pl.points[i-1].cast<double>();
            total_pl_len += unscale<double>(seg.norm());
        }

        ExtrusionMultiPath *mp = new ExtrusionMultiPath();
        double accumulated = 0.0;

        for (size_t i = 1; i < pl.points.size(); ++i) {
            Vec2d a = pl.points[i - 1].cast<double>();
            Vec2d b = pl.points[i].cast<double>();
            double seg_len_scaled = (b - a).norm();
            double seg_len = unscale<double>(seg_len_scaled);

            if (seg_len < EPSILON)
                continue;

            int n_subs = std::max(1, (int)std::ceil(seg_len / sub_target));
            double sub_len = seg_len / n_subs;
            Vec2d dir = (b - a) / seg_len_scaled;

            for (int s = 0; s < n_subs; ++s) {
                double pos = accumulated + sub_len * (s + 0.5);
                double t = std::sin(2.0 * M_PI * pos / fw_period + phase);

                // ── Modulation gating ────────────────────────────────
                // Primary: check if sub-segment midpoint is inside the
                // safe zone (intersection of all adjacent layers' walls).
                // If outside → modulation = 0 (nominal width at wall).
                // If inside  → full modulation, with endpoint taper as
                //              supplementary smoothing.
                double taper = 1.0;
                if (have_safe_zone) {
                    // Compute midpoint in scaled coordinates
                    Point midpt = (a + dir * scale_(sub_len * (s + 0.5))).cast<coord_t>();
                    bool inside_safe = false;
                    for (const ExPolygon &ep : this->fw_safe_expolygons) {
                        if (ep.contains(midpt)) {
                            inside_safe = true;
                            break;
                        }
                    }
                    if (!inside_safe)
                        taper = 0.0;
                }

                // Supplementary: endpoint taper for smooth transition
                // at line start/end (always active, even without safe zone)
                if (taper > 0. && xy_taper_dist > 0.01 && total_pl_len > 0.) {
                    double eff_td = std::min(xy_taper_dist, total_pl_len * 0.45);
                    double d_wall = std::min(pos, total_pl_len - pos);
                    if (d_wall <= 0.)
                        taper = 0.0;
                    else if (d_wall < eff_td) {
                        double r = d_wall / eff_td;
                        taper *= r * r * (3.0 - 2.0 * r); // smoothstep
                    }
                }

                // Width modulation:
                //   inside safe zone + away from endpoints: full modulation
                //   outside safe zone OR at endpoints: nominal width
                double factor = 1.0 + xy_amplitude * taper * t;
                if (factor < 0.1)
                    factor = 0.1;
                double sub_mm3 = base_mm3 * factor;
                float  sub_w   = base_w * (float)factor;

                Point sub_a = (a + dir * scale_(sub_len * s)).cast<coord_t>();
                Point sub_b = (a + dir * scale_(sub_len * (s + 1))).cast<coord_t>();

                ExtrusionPath path(params.extrusion_role, sub_mm3, sub_w, base_h);
                path.polyline.points = { Point3(sub_a, 0), Point3(sub_b, 0) };
                mp->paths.push_back(std::move(path));
            }
            accumulated += seg_len;
        }

        if (!mp->empty())
            eec->entities.push_back(mp);
        else
            delete mp;
    }
}

} // namespace Slic3r
