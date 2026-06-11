// ── FillFlowWeaving.cpp ───────────────────────────────────────────────────────
//
// Flow Weaving infill — sub-segmentation architecture.
//
// Each infill polyline is subdivided into short sub-segments.  For each
// sub-segment we compute:
//   - Z offset  = z_amp * layer_h * sin(pos) * taper * gate
//   - XY factor = 1 + xy_amp * sin(pos) * taper * gate
//
// Each sub-segment becomes its own ExtrusionPathContoured:
//   - mm3_per_mm already includes the XY flow factor
//   - Polyline3 Z coordinates encode the Z offset
//   - z_contoured = true → GCode.cpp handles Z emission + E compensation
//
// All sub-segments for one polyline are collected into ExtrusionMultiPath.
//
// ZERO changes to ExtrusionEntity.hpp or GCode.cpp.
// ─────────────────────────────────────────────────────────────────────────────

#include "FillFlowWeaving.hpp"
#include "FlowWeavingZModulator.hpp"

#include "../../ClipperUtils.hpp"
#include "../../ExPolygon.hpp"
#include "../../ExtrusionEntityCollection.hpp"
#include "../../Surface.hpp"

#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace Slic3r {

// ── Defaults (used when config keys are missing) ─────────────────────────────
static constexpr double DEFAULT_Z_AMPLITUDE_PCT = 30.0;  // % of layer height
static constexpr double DEFAULT_XY_AMPLITUDE    = 0.15;  // fraction (15%)
static constexpr double DEFAULT_PERIOD_MM       = 3.0;   // mm per full wave
static constexpr double DEFAULT_TAPER_FRACTION  = 0.15;  // 15% of line length each end

void FillFlowWeaving::fill_surface_extrusion(
    const Surface*          surface,
    const FillParams&       params,
    ExtrusionEntitiesPtr&   out)
{
    // ── 0. Read config ───────────────────────────────────────────────────────
    double z_amp_pct = DEFAULT_Z_AMPLITUDE_PCT;
    double xy_amp    = DEFAULT_XY_AMPLITUDE;
    double period_mm = DEFAULT_PERIOD_MM;

    if (params.config) {
        if (params.config->has("flow_weaving_z_amplitude"))
            z_amp_pct = params.config->option<ConfigOptionFloat>("flow_weaving_z_amplitude")->value;
        if (params.config->has("flow_weaving_xy_amplitude"))
            xy_amp = params.config->option<ConfigOptionFloat>("flow_weaving_xy_amplitude")->value / 100.0;
        if (params.config->has("flow_weaving_period"))
            period_mm = params.config->option<ConfigOptionFloat>("flow_weaving_period")->value;
    }

    const double z_amp_frac = z_amp_pct / 100.0;
    const double layer_h    = (params.layer_height > 0.0) ? params.layer_height
                                                          : params.flow.height();

    // ── 1. Base rectilinear polylines ────────────────────────────────────────
    Polylines polylines;
    try {
        polylines = this->fill_surface(surface, params);
    } catch (InfillFailedException&) {}

    if (polylines.empty())
        return;

    // ── 2. Flow calculation ──────────────────────────────────────────────────
    double flow_mm3_per_mm = params.flow.mm3_per_mm();
    double flow_width      = params.flow.width();
    if (!params.using_internal_flow) {
        Flow new_flow   = params.flow.with_spacing(this->spacing);
        flow_mm3_per_mm = new_flow.mm3_per_mm();
        flow_width      = new_flow.width();
    }

    // ── 3. Output container ──────────────────────────────────────────────────
    ExtrusionEntityCollection* eec = nullptr;
    out.push_back(eec = new ExtrusionEntityCollection());
    eec->no_sort = this->no_sort();

    // ── 4. Modulator + phase ─────────────────────────────────────────────────
    auto modulator = FlowWeavingModulator::create(ModulatorType::Sine);

    // Phase alternation: even layers → 0, odd layers → π
    const double phase_rad = (this->layer_id % 2 == 0) ? 0.0 : M_PI;

    // Sub-division step
    const int    n_sub   = this->subdivisions_per_period();
    const double step_mm = period_mm / static_cast<double>(n_sub);

    // Safe zone for cross-layer gating.
    // Intersect current layer's no_overlap with adjacent layers' data
    // (populated by make_fills() via needs_cross_layer_data() interface).
    ExPolygons cross_layer_safe = this->no_overlap_expolygons;
    if (!this->no_overlap_above.empty())
        cross_layer_safe = intersection_ex(cross_layer_safe, this->no_overlap_above);
    if (!this->no_overlap_below.empty())
        cross_layer_safe = intersection_ex(cross_layer_safe, this->no_overlap_below);
    const ExPolygons& safe_zone = cross_layer_safe;
    const bool have_safe_zone   = !safe_zone.empty();

    // ── 5. Process each polyline ─────────────────────────────────────────────
    for (Polyline& pl : polylines) {
        if (pl.size() < 2)
            continue;

        // Measure total path length (mm) for taper
        double total_len_mm = 0.0;
        for (size_t i = 1; i < pl.size(); ++i) {
            Vec2d a = unscale(pl.points[i - 1]);
            Vec2d b = unscale(pl.points[i]);
            total_len_mm += (b - a).norm();
        }

        if (total_len_mm < 1e-6)
            continue;

        const double taper_len_mm = total_len_mm * DEFAULT_TAPER_FRACTION;

        // Collect sub-segment paths for this polyline
        ExtrusionPaths sub_paths;
        double pos_mm = 0.0;  // accumulated position along polyline

        for (size_t seg = 0; seg < pl.size() - 1; ++seg) {
            const Point& pa = pl.points[seg];
            const Point& pb = pl.points[seg + 1];

            Vec2d a = unscale(pa);
            Vec2d b = unscale(pb);
            double seg_len_mm = (b - a).norm();

            if (seg_len_mm < 1e-9) {
                pos_mm += seg_len_mm;
                continue;
            }

            int n_steps = std::max(1, static_cast<int>(std::ceil(seg_len_mm / step_mm)));

            for (int step = 0; step < n_steps; ++step) {
                double t_start = static_cast<double>(step) / n_steps;
                double t_end   = static_cast<double>(step + 1) / n_steps;
                double t_mid   = (t_start + t_end) * 0.5;

                double sub_start_pos = pos_mm + seg_len_mm * t_start;
                double sub_end_pos   = pos_mm + seg_len_mm * t_end;
                double sub_mid_pos   = pos_mm + seg_len_mm * t_mid;

                // XY start/end points (scaled integer coords)
                Point pt_start(
                    pa.x() + coord_t(std::round((pb.x() - pa.x()) * t_start)),
                    pa.y() + coord_t(std::round((pb.y() - pa.y()) * t_start)));
                Point pt_end(
                    pa.x() + coord_t(std::round((pb.x() - pa.x()) * t_end)),
                    pa.y() + coord_t(std::round((pb.y() - pa.y()) * t_end)));

                // Midpoint for safe-zone test
                Point mid_pt(
                    pa.x() + coord_t(std::round((pb.x() - pa.x()) * t_mid)),
                    pa.y() + coord_t(std::round((pb.y() - pa.y()) * t_mid)));

                // Gate: is midpoint inside the safe zone?
                double gate = 1.0;
                if (have_safe_zone) {
                    gate = 0.0;
                    for (const ExPolygon& ep : safe_zone) {
                        if (ep.contains(mid_pt)) { gate = 1.0; break; }
                    }
                }

                // Modulation at sub-segment endpoints
                double t_mod_start = modulator->compute(sub_start_pos, period_mm, phase_rad);
                double t_mod_end   = modulator->compute(sub_end_pos, period_mm, phase_rad);
                double t_mod_mid   = modulator->compute(sub_mid_pos, period_mm, phase_rad);

                double taper_val = modulator->taper(sub_mid_pos, total_len_mm, taper_len_mm);

                // ── Z modulation ─────────────────────────────────────────
                double z_start = z_amp_frac * layer_h * t_mod_start * taper_val * gate;
                double z_end   = z_amp_frac * layer_h * t_mod_end   * taper_val * gate;

                // ── XY flow modulation ───────────────────────────────────
                // At peak (t>0): wider extrusion → forms the "tooth"
                // At trough (t<0): narrower → makes room for next-layer tooth
                double flow_factor = 1.0 + xy_amp * t_mod_mid * taper_val * gate;
                flow_factor = std::max(0.3, std::min(1.8, flow_factor));

                // ── Build sub-segment ExtrusionPathContoured ─────────────
                double sub_mm3 = flow_mm3_per_mm * flow_factor;

                ExtrusionPath base_path(params.extrusion_role, sub_mm3,
                                        float(flow_width), params.flow.height());
                base_path.z_contoured = true;

                // Polyline3: 2 points with Z offsets encoded in .z()
                Polyline3 pl3;
                pl3.points.reserve(2);
                pl3.points.push_back(Point3(pt_start.x(), pt_start.y(),
                                            coord_t(std::round(z_start / SCALING_FACTOR))));
                pl3.points.push_back(Point3(pt_end.x(), pt_end.y(),
                                            coord_t(std::round(z_end / SCALING_FACTOR))));

                // z_diffs: 1 entry for the single segment (start→end)
                std::vector<double> z_diffs;
                z_diffs.push_back(z_end);

                auto* contoured = new ExtrusionPathContoured(
                    std::move(pl3), base_path, std::move(z_diffs));
                eec->entities.push_back(contoured);
            }

            pos_mm += seg_len_mm;
        }
    }

    // Gap fill: skipped — modulated extrusion covers the fill area.
    // Gap fill would conflict with Z modulation at line boundaries.
}

} // namespace Slic3r
