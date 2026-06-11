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
#include "../../PrintConfig.hpp"
#include "../../Surface.hpp"

#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace Slic3r {

// ── Defaults (used when config keys are missing) ─────────────────────────────
static constexpr double DEFAULT_Z_AMPLITUDE_PCT = 30.0;  // % of layer height
static constexpr double DEFAULT_XY_AMPLITUDE       = 50.0;  // % of flow width (width modulation)
static constexpr double DEFAULT_XY_PATH_AMPLITUDE   = 0.2;   // mm lateral path displacement
static constexpr double DEFAULT_PERIOD_MM           = 3.0;   // mm per full wave
static constexpr double DEFAULT_PHASE_OFFSET        = 0.5;   // fraction of period (0-1)
static constexpr double DEFAULT_TAPER_MM            = 1.5;   // mm absolute taper near walls

void FillFlowWeaving::fill_surface_extrusion(
    const Surface*          surface,
    const FillParams&       params,
    ExtrusionEntitiesPtr&   out)
{
    // ── 0. Read config ───────────────────────────────────────────────────────
    // flow_weaving_* live in PrintRegionConfig (Process settings).
    // params.config already merges global process profile + per-object overrides.
    // Using virtual option<>() instead of static_cast for safety.
    double z_amp_pct      = DEFAULT_Z_AMPLITUDE_PCT;
    double xy_amp         = DEFAULT_XY_AMPLITUDE;
    double xy_path_amp_mm = DEFAULT_XY_PATH_AMPLITUDE;
    double period_mm      = DEFAULT_PERIOD_MM;
    double phase_offset   = DEFAULT_PHASE_OFFSET;

    if (params.config) {
        if (const auto* v = params.config->option<ConfigOptionFloat>("flow_weaving_z_amplitude"))
            z_amp_pct = v->value;
        if (const auto* v = params.config->option<ConfigOptionFloat>("flow_weaving_xy_amplitude"))
            xy_amp = v->value;
        if (const auto* v = params.config->option<ConfigOptionFloat>("flow_weaving_xy_path_amplitude"))
            xy_path_amp_mm = v->value;
        if (const auto* v = params.config->option<ConfigOptionFloat>("flow_weaving_period"))
            period_mm = v->value;
        if (const auto* v = params.config->option<ConfigOptionFloat>("flow_weaving_phase_offset"))
            phase_offset = v->value;
    }

    const double z_amp_frac  = z_amp_pct / 100.0;
    const double xy_amp_frac = xy_amp / 100.0;
    const double layer_h     = (params.layer_height > 0.0) ? params.layer_height
                                                           : params.flow.height();
    const double nozzle_d    = params.flow.nozzle_diameter();

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

    // ── 4. Modulator ─────────────────────────────────────────────────────────
    auto modulator = FlowWeavingModulator::create(ModulatorType::Sine);

    // Base phase: layers alternate fill direction every layer (0°, 90°, 0°, 90°, ...).
    // Same-direction layers are: 0,2,4,... (set A) and 1,3,5,... (set B).
    // Within each set, apply progressive phase_offset so that waves shift
    // between layers sharing the same direction → better interlocking.
    //
    // same_dir_idx = layer_id / 2  (how many same-direction layers came before)
    // layer_base_phase = π for odd layers (antiphase vs even layers)
    //                  + same_dir_idx × phase_offset × 2π  (progressive shift)
    const size_t same_dir_idx = this->layer_id / 2;
    const double layer_base_phase = ((this->layer_id % 2 == 0) ? 0.0 : M_PI)
                                  + same_dir_idx * phase_offset * 2.0 * M_PI;

    // Global perpendicular to the ACTUAL fill direction (including layer rotation).
    // this->angle is the base angle from config; _layer_angle() adds +90° on odd
    // layers.  fill_surface() applies this rotation internally, so the output
    // polylines already go in the rotated direction.  global_perp MUST match that
    // rotated direction, otherwise the XY displacement ends up parallel to the
    // fill lines (invisible) instead of perpendicular (visible wave).
    const float fill_angle = this->angle
        + ((this->layer_id != size_t(-1) && !this->fixed_angle && !this->dont_alternate_fill_direction)
            ? this->_layer_angle(this->layer_id / surface->thickness_layers)
            : 0.f);
    const Vec2d global_perp(-std::sin(fill_angle), std::cos(fill_angle));

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
    for (size_t poly_idx = 0; poly_idx < polylines.size(); ++poly_idx) {
        Polyline& pl = polylines[poly_idx];
        if (pl.size() < 2)
            continue;

        // Alternate phase by line index: odd lines are in antiphase to even lines.
        const double phase_rad = layer_base_phase + ((poly_idx % 2 != 0) ? M_PI : 0.0);

        // Measure total path length (mm) for taper
        double total_len_mm = 0.0;
        for (size_t i = 1; i < pl.size(); ++i) {
            Vec2d a = unscale(pl.points[i - 1]);
            Vec2d b = unscale(pl.points[i]);
            total_len_mm += (b - a).norm();
        }

        if (total_len_mm < 1e-6)
            continue;

        // Taper: fixed absolute distance from wall (line endpoints)
        const double taper_len_mm = DEFAULT_TAPER_MM;

        // Path displacement: absolute mm from config
        const double lateral_amp_mm = xy_path_amp_mm;
        // Width modulation: percentage of flow width
        const double width_amp_mm   = xy_amp_frac * flow_width;

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

            // Recalculate dir for each segment (for taper and length calculation).
            // XY displacement uses global_perp, NOT per-segment perp, so that
            // zigzag direction reversal on odd lines doesn't cancel the phase offset.
            Vec2d dir = (b - a).normalized();

            int n_steps = std::max(1, static_cast<int>(std::ceil(seg_len_mm / step_mm)));

            for (int step = 0; step < n_steps; ++step) {
                double t_start = static_cast<double>(step) / n_steps;
                double t_end   = static_cast<double>(step + 1) / n_steps;
                double t_mid   = (t_start + t_end) * 0.5;

                double sub_start_pos = pos_mm + seg_len_mm * t_start;
                double sub_end_pos   = pos_mm + seg_len_mm * t_end;
                double sub_mid_pos   = pos_mm + seg_len_mm * t_mid;

                // Modulation values at sub-segment positions
                double t_mod_start = modulator->compute(sub_start_pos, period_mm, phase_rad);
                double t_mod_end   = modulator->compute(sub_end_pos,   period_mm, phase_rad);
                double t_mod_mid   = modulator->compute(sub_mid_pos,   period_mm, phase_rad);

                double taper_val = modulator->taper(sub_mid_pos, total_len_mm, taper_len_mm);

                // ── XY lateral displacement (the actual physical wave) ───────
                // Offset perpendicular to travel direction by sine × amplitude
                double xy_off_start = lateral_amp_mm * t_mod_start * taper_val;
                double xy_off_end   = lateral_amp_mm * t_mod_end   * taper_val;

                // Base XY positions along the straight segment
                Vec2d base_start = a + (b - a) * t_start;
                Vec2d base_end   = a + (b - a) * t_end;
                Vec2d base_mid   = a + (b - a) * t_mid;

                // Apply perpendicular displacement using GLOBAL perp direction.
                Vec2d disp_start = base_start + global_perp * xy_off_start;
                Vec2d disp_end   = base_end   + global_perp * xy_off_end;
                Vec2d disp_mid   = base_mid   + global_perp * ((xy_off_start + xy_off_end) * 0.5);

                // Convert to scaled integer coords
                Point pt_start(coord_t(std::round(disp_start.x() / SCALING_FACTOR)),
                                coord_t(std::round(disp_start.y() / SCALING_FACTOR)));
                Point pt_end(  coord_t(std::round(disp_end.x()   / SCALING_FACTOR)),
                                coord_t(std::round(disp_end.y()   / SCALING_FACTOR)));
                Point mid_pt(  coord_t(std::round(disp_mid.x()   / SCALING_FACTOR)),
                                coord_t(std::round(disp_mid.y()   / SCALING_FACTOR)));

                // Gate: is midpoint inside the safe zone?
                double gate = 1.0;
                if (have_safe_zone) {
                    gate = 0.0;
                    for (const ExPolygon& ep : safe_zone) {
                        if (ep.contains(mid_pt)) { gate = 1.0; break; }
                    }
                }

                // ── Z modulation ─────────────────────────────────────────
                double z_start = z_amp_frac * layer_h * t_mod_start * taper_val * gate;
                double z_end   = z_amp_frac * layer_h * t_mod_end   * taper_val * gate;

                // ── Flow/width modulation ─────────────────────────────
                // Line width varies with the sine.  Clamp between
                // 25% of flow_width (minimum) and nozzle diameter (maximum).
                double width_delta = width_amp_mm * t_mod_mid * taper_val * gate;
                double effective_width = std::clamp(flow_width + width_delta,
                                                   flow_width * 0.25, (double)nozzle_d);
                double sub_mm3 = flow_mm3_per_mm * (effective_width / flow_width);

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
