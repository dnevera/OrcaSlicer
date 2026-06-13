// ── FillFlowWeaving.cpp ───────────────────────────────────────────────────────
//
// Flow Weaving infill — sub-segmentation architecture.
//
// Each infill polyline is subdivided into short sub-segments.  For each
// sub-segment we compute:
//   - Z offset      = z_amp × layer_h × sin(pos) × taper × gate
//   - flow          = nominal (constant, no modulation — pending print validation)
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
static constexpr double DEFAULT_Z_AMPLITUDE_PCT   = 25.0; // % of layer height
static constexpr double DEFAULT_XY_AMPLITUDE      = 10.0; // % of flow width (width modulation)
static constexpr double DEFAULT_XY_PATH_AMPLITUDE = 0.15; // mm lateral path displacement
static constexpr double DEFAULT_PERIOD_MM         = 3.0;  // mm per full wave
static constexpr double DEFAULT_PHASE_OFFSET      = 0.5;  // fraction of period (0-1)
static constexpr double DEFAULT_TAPER_LENGTH_MM   = 1.5;  // mm absolute taper near walls
static constexpr double DEFAULT_WALL_OVERLAP_MM   = 0.0;  // mm extra wall overlap

// Minimum distance from a point to the nearest edge of ExPolygons boundary (mm).
// Used to clamp width/displacement so the physical line edge stays inside walls.
static double min_dist_to_boundary_mm(const Point& pt, const ExPolygons& expolys)
{
    double min_dist_scaled = 1e18;
    auto check_ring = [&](const Points& pts) {
        for (size_t i = 0, n = pts.size(); i < n; ++i) {
            double d = Line(pts[i], pts[(i + 1) % n]).distance_to(pt);
            if (d < min_dist_scaled) min_dist_scaled = d;
        }
    };
    for (const ExPolygon& ep : expolys) {
        check_ring(ep.contour.points);
        for (const Polygon& hole : ep.holes)
            check_ring(hole.points);
    }
    return unscale<double>(min_dist_scaled);
}

void FillFlowWeaving::fill_surface_extrusion(const Surface* surface, const FillParams& params, ExtrusionEntitiesPtr& out)
{
    // ── 0. Read config ───────────────────────────────────────────────────────
    // flow_weaving_* live in PrintRegionConfig (Process settings).
    // params.config already merges global process profile + per-object overrides.
    // Using virtual option<>() instead of static_cast for safety.
    double z_amp_pct      = DEFAULT_Z_AMPLITUDE_PCT;
    double xy_amp         = DEFAULT_XY_AMPLITUDE;
    double xy_path_amp_mm = DEFAULT_XY_PATH_AMPLITUDE;
    double period_mm      = DEFAULT_PERIOD_MM;
    double phase_offset   = DEFAULT_PHASE_OFFSET;   // XY wave phase between layers
    double z_phase_offset = 0.0;                    // Z wave phase between layers
    double taper_len_mm   = DEFAULT_TAPER_LENGTH_MM;
    double wall_overlap   = DEFAULT_WALL_OVERLAP_MM;
    bool ironing_enabled  = true;
    int top_taper_n       = params.config ? params.config->flow_weaving_top_taper_layers.value : 0;

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
        if (const auto* v = params.config->option<ConfigOptionFloat>("flow_weaving_z_phase_offset"))
            z_phase_offset = v->value;
        if (const auto* v = params.config->option<ConfigOptionFloat>("flow_weaving_taper_length"))
            taper_len_mm = v->value;
        if (const auto* v = params.config->option<ConfigOptionFloat>("flow_weaving_wall_overlap"))
            wall_overlap = v->value;
        if (const auto* v = params.config->option<ConfigOptionBool>("flow_weaving_ironing"))
            ironing_enabled = v->value;
    }

    const double z_amp_frac  = z_amp_pct / 100.0;
    const double xy_amp_frac = xy_amp / 100.0;
    const double layer_h     = (params.layer_height > 0.0) ? params.layer_height : params.flow.height();
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

    ExtrusionEntityCollection* ironing_eec = nullptr;
    const bool generate_ironing = (top_taper_n > 0 && this->infill_layers_above == 0 && ironing_enabled);
    if (generate_ironing) {
        ironing_eec = new ExtrusionEntityCollection();
        ironing_eec->no_sort = this->no_sort();
    }

    // ── 4. Modulator ─────────────────────────────────────────────────────────
    auto modulator = FlowWeavingModulator::create(ModulatorType::Sine);

    // Base phase: layers alternate fill direction every layer (0°, 90°, 0°, 90°, ...).
    // Same-direction layers are: 0,2,4,... (set A) and 1,3,5,... (set B).
    // Within each set, apply progressive phase_offset so that waves shift
    // between layers sharing the same direction.
    //
    // Interlocking strategy: TROUGH INTO TROUGH
    //   same_dir_idx × phase_offset × 2π controls the progressive shift.
    //   No π-alternation between layers → troughs of layer N align with
    //   troughs of layer N+1: the nozzle physically dips into the valleys
    //   left by the previous layer's wave → mechanical Z-interlocking.
    //
    //   phase_offset = 0 → perfect trough-into-trough (maximum depth)
    //   phase_offset > 0 → gradual shift per same-direction layer pair
    const size_t same_dir_idx      = this->layer_id / 2;
    // XY and Z phases are INDEPENDENT:
    //   phase_offset   → XY displacement wave (visual pattern between layers)
    //   z_phase_offset → Z wave interlocking strategy
    //     0.0 = trough-into-trough (nozzle dips at previous layer's valley positions)
    //     0.5 = anti-phase (nozzle dips where previous layer peaked)
    const double xy_phase_rad = same_dir_idx * phase_offset   * 2.0 * M_PI;
    const double z_phase_rad  = same_dir_idx * z_phase_offset * 2.0 * M_PI;

    // Global perpendicular to the ACTUAL fill direction (including layer rotation).
    // this->angle is the base angle from config; _layer_angle() adds +90° on odd
    // layers.  fill_surface() applies this rotation internally, so the output
    // polylines already go in the rotated direction.  global_perp MUST match that
    // rotated direction, otherwise the XY displacement ends up parallel to the
    // fill lines (invisible) instead of perpendicular (visible wave).
    const float fill_angle = this->angle + ((this->layer_id != size_t(-1) && !this->fixed_angle && !this->dont_alternate_fill_direction) ?
                                                this->_layer_angle(this->layer_id / surface->thickness_layers) :
                                                0.f);
    const Vec2d global_perp(-std::sin(fill_angle), std::cos(fill_angle));
    // Fill direction (along the lines). Used for spatial-phase computation:
    // projecting XY position onto fill_dir gives a coordinate that is the
    // SAME for every parallel line at the same cross-section, regardless of
    // whether the line travels forward or backward (zigzag).
    const Vec2d fill_dir(std::cos(fill_angle), std::sin(fill_angle));

    // Sub-division step
    const int n_sub      = this->subdivisions_per_period();
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

        // ── Taper scale — shared by ALL Z bounds (up AND down) ───────────
        // Hard cutoff: the bottom `top_taper_n` infill layers are COMPLETELY
        // flat (taper_scale = 0) to provide a flat bed for the top solid shell.
        // Above that zone: smoothstep fade over another `top_taper_n` layers.
        //
        //  infill_layers_above:   0..N-1 │ N       N+1      N+2    2N  ...
        //  taper_scale:           0  ...0 │ s(1/N)  s(2/N)  ...   1.0  ...
        //                         ↑ hard cutoff ↑   ├── smoothstep ────────
        //  (N = top_taper_n, default 4)
        //
        // With top_taper_n=4: last 4 infill layers flat, then 4 layers of fade.
        // This ensures the top surface is free of Z-wave artifacts even at
        // short periods (0.5mm) where Z-ripples telegraph through thin top shells.
        // Using top_taper_n defined at the beginning of the function
        double taper_scale    = 1.0;
        if (top_taper_n > 0 && this->infill_layers_above < top_taper_n) {
            // Hard disable: last top_taper_n infill layers print flat
            taper_scale = 0.0;
        } else if (top_taper_n > 0) {
            // Smooth fade starting above the hard-cutoff zone.
            // x=0 at ia=top_taper_n, x=1 at ia=2*top_taper_n
            const double x = std::min(static_cast<double>(this->infill_layers_above - top_taper_n + 1) / static_cast<double>(top_taper_n),
                                      1.0);
            taper_scale    = x * x * (3.0 - 2.0 * x); // smoothstep ∈ [0, 1]
        }

        // Upper bound: max upward Z offset (zero on last 2 layers)
        const double max_z_up = z_amp_frac * layer_h * taper_scale;

        // Lower bound: nozzle may not dip more than z_overlap% of layer_h below nominal.
        // Tapered by the same scale → zero on last 2 layers, full overlap far from top.
        const double z_overlap_pct   = params.config ? params.config->flow_weaving_z_overlap.value : 0.0;
        const double overlap_tapered = -(layer_h * z_overlap_pct / 100.0) * taper_scale;
        // Absolute floor: nozzle must never go below the first layer surface.
        const double first_layer_h  = this->print_config ? this->print_config->initial_layer_print_height.value : layer_h;
        const double absolute_floor = -(this->z - first_layer_h);
        // Most restrictive (least negative) of the two lower limits.
        const double min_z_diff = std::max(overlap_tapered, absolute_floor);

        // Measure total path length (mm) for taper
        double total_len_mm = 0.0;
        for (size_t i = 1; i < pl.size(); ++i) {
            Vec2d a = unscale(pl.points[i - 1]);
            Vec2d b = unscale(pl.points[i]);
            total_len_mm += (b - a).norm();
        }

        if (total_len_mm < 1e-6)
            continue;

        // Taper: user defined or default distance from wall (line endpoints)

        // Path displacement: absolute mm from config
        const double lateral_amp_mm = xy_path_amp_mm;

        double pos_mm = 0.0; // accumulated position along polyline

        for (size_t seg = 0; seg < pl.size() - 1; ++seg) {
            const Point& pa = pl.points[seg];
            const Point& pb = pl.points[seg + 1];

            Vec2d a           = unscale(pa);
            Vec2d b           = unscale(pb);
            double seg_len_mm = (b - a).norm();

            if (seg_len_mm < 1e-9) {
                pos_mm += seg_len_mm;
                continue;
            }

            // Recalculate dir for each segment (for taper and length calculation).
            // XY displacement uses global_perp, NOT per-segment perp, so that
            // zigzag direction reversal on odd lines doesn't cancel the phase offset.
            int n_steps = std::max(1, static_cast<int>(std::ceil(seg_len_mm / step_mm)));

            for (int step = 0; step < n_steps; ++step) {
                double t_start = static_cast<double>(step) / n_steps;
                double t_end   = static_cast<double>(step + 1) / n_steps;
                double t_mid   = (t_start + t_end) * 0.5;

                double sub_start_pos = pos_mm + seg_len_mm * t_start;
                double sub_end_pos   = pos_mm + seg_len_mm * t_end;
                double sub_mid_pos   = pos_mm + seg_len_mm * t_mid;

                // Nominal (un-displaced) sub-segment endpoints
                Vec2d base_start = a + (b - a) * t_start;
                Vec2d base_end   = a + (b - a) * t_end;
                Vec2d base_mid   = a + (b - a) * t_mid;

                // Modulation — spatial projection onto fill_dir (same for all
                // parallel lines regardless of zigzag direction).
                // Z and XY use SEPARATE phases for independent control.
                const double sp_start = base_start.dot(fill_dir);
                const double sp_end   = base_end.dot(fill_dir);
                const double sp_mid   = base_mid.dot(fill_dir);
                // Z phase: controls inter-layer interlocking depth/alignment
                const double t_mod_z_start = modulator->compute(sp_start, period_mm, z_phase_rad);
                const double t_mod_z_end   = modulator->compute(sp_end,   period_mm, z_phase_rad);
                // XY phase: controls lateral displacement wave pattern
                const double t_mod_start   = modulator->compute(sp_start, period_mm, xy_phase_rad);
                const double t_mod_end     = modulator->compute(sp_end,   period_mm, xy_phase_rad);
                const double t_mod_mid     = modulator->compute(sp_mid,   period_mm, xy_phase_rad);

                double taper_val = modulator->taper(sub_mid_pos, total_len_mm, taper_len_mm);

                // ── Gate check on NOMINAL (un-displaced) midpoint ─────────
                // Must use base_mid (original trajectory), NOT the displaced point.
                // Using a displaced point would make the gate check unreliable near walls:
                // the displaced mid might land inside the zone even when the nominal is outside.
                Point base_mid_pt(coord_t(std::round(base_mid.x() / SCALING_FACTOR)), coord_t(std::round(base_mid.y() / SCALING_FACTOR)));

                // ── Wall-distance calculation ──────────────────────────────
                // Distance from nominal midpoint to nearest wall boundary (mm).
                // Always computed so it can be used for smooth Z tapering near walls.
                double dist_to_wall_mm = 1e18;
                if (!this->no_overlap_expolygons.empty())
                    dist_to_wall_mm = min_dist_to_boundary_mm(base_mid_pt, this->no_overlap_expolygons);

                // ── Combined Wall & Endpoint taper ─────────────────────────
                // taper_val is distance along the line from endpoints.
                // wall_taper is the actual 2D distance to the nearest wall.
                double wall_taper = 1.0;
                if (dist_to_wall_mm < taper_len_mm) {
                    double x = dist_to_wall_mm / taper_len_mm;
                    wall_taper = x * x * (3.0 - 2.0 * x); // smoothstep ∈ [0, 1]
                }
                double combined_taper = std::min(taper_val, wall_taper);

                // ── Smooth Gate check ──────────────────────────────────────
                // Transition coefficient from 0.0 (outside safe zone) to 1.0 (deep inside).
                // Uses distance to safe zone boundary to smoothly ramp the wave up.
                double gate = 1.0;
                if (have_safe_zone) {
                    bool inside = false;
                    for (const ExPolygon& ep : safe_zone) {
                        if (ep.contains(base_mid_pt)) {
                            inside = true;
                            break;
                        }
                    }
                    if (inside) {
                        double dist_to_safe_boundary_mm = min_dist_to_boundary_mm(base_mid_pt, safe_zone);
                        constexpr double transition_len_mm = 1.0; // 1.0 mm transition zone
                        if (dist_to_safe_boundary_mm < transition_len_mm) {
                            double x = dist_to_safe_boundary_mm / transition_len_mm;
                            gate = x * x * (3.0 - 2.0 * x); // smoothstep ∈ [0, 1]
                        } else {
                            gate = 1.0;
                        }
                    } else {
                        gate = 0.0;
                    }
                }

                // ── Width modulation (clamped to wall) ────────────────────
                double width_mod = 1.0;
                const double active_xy_amp_frac = xy_amp_frac * taper_scale;
                if (gate > 0.0 && active_xy_amp_frac > 0.0) {
                    width_mod = 1.0 + active_xy_amp_frac * t_mod_mid * combined_taper;
                    // Only clamp EXPANSION beyond nominal — never shrink below 1.0.
                    // Nominal half-width may already exceed dist_to_wall (normal
                    // perimeter overlap), so we only limit the EXTRA width.
                    // max_expansion = how much further the edge can go beyond nominal
                    double max_expansion_mm = std::max(0.0, dist_to_wall_mm - flow_width * 0.5 + wall_overlap);
                    double max_mod = 1.0 + 2.0 * max_expansion_mm / flow_width;
                    width_mod = std::max(1.0 - active_xy_amp_frac, std::min(width_mod, max_mod));
                }

                // ── XY lateral displacement (clamped to wall) ─────────────
                double xy_off_start = lateral_amp_mm * t_mod_start * combined_taper * gate * taper_scale;
                double xy_off_end   = lateral_amp_mm * t_mod_end   * combined_taper * gate * taper_scale;
                // Center + half_width must stay inside wall boundary (plus overlap allowance)
                double max_lateral = std::max(0.0, dist_to_wall_mm - flow_width * width_mod * 0.5 + wall_overlap);
                xy_off_start = std::max(-max_lateral, std::min(xy_off_start, max_lateral));
                xy_off_end   = std::max(-max_lateral, std::min(xy_off_end,   max_lateral));

                Vec2d disp_start = base_start + global_perp * xy_off_start;
                Vec2d disp_end   = base_end + global_perp * xy_off_end;

                // Convert to scaled integer coords
                Point pt_start(coord_t(std::round(disp_start.x() / SCALING_FACTOR)), coord_t(std::round(disp_start.y() / SCALING_FACTOR)));
                Point pt_end(coord_t(std::round(disp_end.x() / SCALING_FACTOR)), coord_t(std::round(disp_end.y() / SCALING_FACTOR)));

                // ── Z modulation (uses Z-specific phase, independent of XY) ──
                double z_start = z_amp_frac * layer_h * t_mod_z_start * combined_taper * gate * taper_scale;
                double z_end   = z_amp_frac * layer_h * t_mod_z_end   * combined_taper * gate * taper_scale;


                // Apply lower bound (max of two negative limits = shallower dip wins)
                z_start = std::max(z_start, min_z_diff);
                z_end   = std::max(z_end, min_z_diff);

                // Apply upper bound (smoothstep fade near top shell)
                z_start = std::min(z_start, max_z_up);
                z_end   = std::min(z_end, max_z_up);

                // ── Flow (width-modulated) ─────────────────────────────
                const double sub_mm3  = flow_mm3_per_mm * width_mod;
                const float  mod_width = float(flow_width * width_mod);

                ExtrusionPath base_path(params.extrusion_role, sub_mm3, mod_width, params.flow.height());

                base_path.z_contoured = true;

                // Polyline3: 2 points with Z offsets encoded in .z()
                Polyline3 pl3;
                pl3.points.reserve(2);
                pl3.points.push_back(Point3(pt_start.x(), pt_start.y(), coord_t(std::round(z_start / SCALING_FACTOR))));
                pl3.points.push_back(Point3(pt_end.x(), pt_end.y(), coord_t(std::round(z_end / SCALING_FACTOR))));

                // z_diffs: 1 entry for the single segment (start→end)
                std::vector<double> z_diffs;
                z_diffs.push_back(z_end);

                auto* contoured = new ExtrusionPathContoured(std::move(pl3), base_path, std::move(z_diffs));
                eec->entities.push_back(contoured);

                // ── Разглаживающий проход (Ironing) ──────────────────────────
                if (generate_ironing) {
                    const double ironing_mm3 = flow_mm3_per_mm * 0.15; // 15% поток
                    const float ironing_width = float(flow_width);
                    ExtrusionPath ironing_base_path(params.extrusion_role, ironing_mm3, ironing_width, params.flow.height());
                    ironing_base_path.z_contoured = true;

                    Polyline3 ironing_pl3;
                    ironing_pl3.points.reserve(2);
                    Point pt_iron_start(coord_t(std::round(base_start.x() / SCALING_FACTOR)), coord_t(std::round(base_start.y() / SCALING_FACTOR)));
                    Point pt_iron_end(coord_t(std::round(base_end.x() / SCALING_FACTOR)), coord_t(std::round(base_end.y() / SCALING_FACTOR)));
                    ironing_pl3.points.push_back(Point3(int64_t(pt_iron_start.x()), int64_t(pt_iron_start.y()), int64_t(0)));
                    ironing_pl3.points.push_back(Point3(int64_t(pt_iron_end.x()), int64_t(pt_iron_end.y()), int64_t(0)));

                    std::vector<double> ironing_z_diffs = {0.0};
                    auto* ironing_contoured = new ExtrusionPathContoured(std::move(ironing_pl3), ironing_base_path, std::move(ironing_z_diffs));
                    ironing_eec->entities.push_back(ironing_contoured);
                }
            }

            pos_mm += seg_len_mm;
        }
    }

    if (ironing_eec) {
        if (!ironing_eec->entities.empty()) {
            out.push_back(ironing_eec);
        } else {
            delete ironing_eec;
        }
    }

    // Gap fill: skipped — modulated extrusion covers the fill area.
    // Gap fill would conflict with Z modulation at line boundaries.
}

} // namespace Slic3r
