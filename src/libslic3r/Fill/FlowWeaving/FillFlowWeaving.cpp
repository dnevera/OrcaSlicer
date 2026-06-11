// ── FillFlowWeaving.cpp ───────────────────────────────────────────────────────
//
// Flow Weaving infill implementation.
//
// Overrides fill_surface_extrusion to inject sinusoidal Z-height and XY-width
// modulation into every infill line, mechanically interlocking adjacent layers.
//
// Safe zone: no_overlap_expolygons (field on Fill base class, set by Fill.cpp
// before calling fill_surface_extrusion) contains the cross-layer safe zone
// (intersection of current + adjacent layer expolygons).
// Modulation amplitude is gated to 0 for any sub-segment whose midpoint falls
// outside this zone.
//
// Algorithm per polyline:
//   1. Compute total path length for taper.
//   2. Subdivide each segment into sub-segments <= step_mm long.
//   3. For each sub-segment:
//      a. Compute wave value t in [-1,1] via SineModulator.
//      b. Taper near endpoints.
//      c. Gate by no_overlap_expolygons point-in-test on midpoint.
//      d. z_offset = z_amp * layer_h * t * taper * gate
//      e. flow_factor = 1 + xy_amp * t * taper * gate   (synchronized with Z)
//      f. Store in z_diffs and flow_factors for ExtrusionPathContoured.
//   4. Emit ExtrusionPathContoured with z_contoured = true.
//
// XY modulation physics:
//   At the Z peak (t > 0): flow_factor > 1 -> wider extrusion -> forms the "tooth"
//   At the Z trough (t < 0): flow_factor < 1 -> narrower -> makes room for
//   next-layer tooth to interlock.
//   Phase alternates every layer (0 vs pi), so peaks of layer N align with
//   troughs of layer N+1.
//
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

static constexpr double DEFAULT_Z_AMPLITUDE_PCT = 50.0; // % of layer height
static constexpr double DEFAULT_XY_AMPLITUDE     = 0.30; // fraction of base flow (0.30 = +/-30%)
static constexpr double DEFAULT_PERIOD_MM        = 3.0;  // one full wave per 3 mm (matches PrintConfig default)
static constexpr double DEFAULT_TAPER_FRACTION   = 0.15; // 15% of line length each end

void FillFlowWeaving::fill_surface_extrusion(
    const Surface*          surface,
    const FillParams&       params,
    ExtrusionEntitiesPtr&   out)
{
    // ── 0. Read config parameters ─────────────────────────────────────────────
    double z_amp_pct   = DEFAULT_Z_AMPLITUDE_PCT;
    double xy_amp      = DEFAULT_XY_AMPLITUDE;
    double period_mm   = DEFAULT_PERIOD_MM;


    if (params.config) {
        if (params.config->has("flow_weaving_z_amplitude"))
            z_amp_pct  = params.config->option<ConfigOptionFloat>("flow_weaving_z_amplitude")->value;
        if (params.config->has("flow_weaving_xy_amplitude"))
            xy_amp     = params.config->option<ConfigOptionFloat>("flow_weaving_xy_amplitude")->value / 100.0;
        if (params.config->has("flow_weaving_period"))
            period_mm  = params.config->option<ConfigOptionFloat>("flow_weaving_period")->value;

    }

    const double z_amp_frac = z_amp_pct / 100.0; // fraction of layer height
    const double layer_h    = (params.layer_height > 0.0) ? params.layer_height
                                                           : params.flow.height();

    // ── 1. Generate base rectilinear polylines via parent class ───────────────
    Polylines polylines;
    try {
        polylines = this->fill_surface(surface, params);
    } catch (InfillFailedException&) {}

    if (polylines.empty())
        return;

    // ── 2. Flow calculation (mirrors FillBase::fill_surface_extrusion) ────────
    double flow_mm3_per_mm = params.flow.mm3_per_mm();
    double flow_width      = params.flow.width();
    if (!params.using_internal_flow) {
        Flow new_flow       = params.flow.with_spacing(this->spacing);
        flow_mm3_per_mm     = new_flow.mm3_per_mm();
        flow_width          = new_flow.width();
    }

    // ── 3. Create output container ────────────────────────────────────────────
    ExtrusionEntityCollection* eec = nullptr;
    out.push_back(eec = new ExtrusionEntityCollection());
    eec->no_sort = this->no_sort();

    // ── 4. Prepare modulator and safe zone ────────────────────────────────────
    auto modulator = FlowWeavingModulator::create(ModulatorType::Sine);

    // Phase alternation: even/odd layer index -> 0 or pi
    // layer_id is the absolute layer index (0-based). FW alternates phase each layer.
    const double phase_rad = (this->layer_id % 2 == 0) ? 0.0 : M_PI;

    // Sub-division step: n_sub points per wave period
    const int    n_sub     = this->subdivisions_per_period();
    const double step_mm   = period_mm / static_cast<double>(n_sub);

    // Safe zone for cross-layer gating: `no_overlap_expolygons` holds the
    // intersection of current + adjacent layers, pre-computed by Fill.cpp.
    const ExPolygons& safe_zone = this->no_overlap_expolygons;
    const bool have_safe_zone = !safe_zone.empty();

    // ── 5. Process each polyline ───────────────────────────────────────────────
    for (Polyline& pl : polylines) {
        if (pl.size() < 2)
            continue;

        // Measure total path length (mm, unscaled) for taper
        double total_len_mm = 0.0;
        for (size_t i = 1; i < pl.size(); ++i) {
            Vec2d a = unscale(pl.points[i-1]);
            Vec2d b = unscale(pl.points[i]);
            total_len_mm += (b - a).norm();
        }

        const double taper_len_mm = total_len_mm * DEFAULT_TAPER_FRACTION;

        // Build 3D polyline with Z modulation
        Polyline3           pl3;
        std::vector<double> z_diffs;       // per-segment Z offset (mm)
        std::vector<double> flow_factors;  // per-segment XY flow multiplier

        // We accumulate position along the line to pass to modulator
        double pos_mm = 0.0;

        // Add first point at nominal Z (z_offset = 0)
        pl3.points.push_back(Point3(pl.points[0].x(), pl.points[0].y(), coord_t(0)));

        for (size_t seg = 0; seg < pl.size() - 1; ++seg) {
            const Point& pa = pl.points[seg];
            const Point& pb = pl.points[seg + 1];

            Vec2d a = unscale(pa);
            Vec2d b = unscale(pb);
            double seg_len_mm = (b - a).norm();

            if (seg_len_mm < 1e-9) {
                // Degenerate segment — add end point with zero modulation
                pl3.points.push_back(Point3(pb.x(), pb.y(), coord_t(0)));
                z_diffs.push_back(0.0);
                flow_factors.push_back(1.0);
                continue;
            }

            int n_steps = std::max(1, static_cast<int>(std::ceil(seg_len_mm / step_mm)));
            for (int step = 0; step < n_steps; ++step) {
                double t_sub_mid = (static_cast<double>(step) + 0.5) / n_steps;
                double pos_end   = pos_mm + seg_len_mm * (static_cast<double>(step + 1) / n_steps);

                // XY midpoint for safe-zone test (in scaled integer coords)
                Point mid_pt(
                    pa.x() + coord_t(std::round((pb.x() - pa.x()) * t_sub_mid)),
                    pa.y() + coord_t(std::round((pb.y() - pa.y()) * t_sub_mid))
                );

                // Gate: is this sub-segment midpoint inside the safe zone?
                double gate = 1.0;
                if (have_safe_zone) {
                    gate = 0.0;
                    for (const ExPolygon& ep : safe_zone) {
                        if (ep.contains(mid_pt)) { gate = 1.0; break; }
                    }
                }

                // Modulation at end of this sub-segment
                double t_mod  = modulator->compute(pos_end, period_mm, phase_rad);
                double taper  = modulator->taper(pos_end, total_len_mm, taper_len_mm);

                // ── Z modulation ─────────────────────────────────────────────
                // Symmetric sine wave: z_amp_frac * layer_h gives the peak
                // deviation from nominal Z in each direction.
                double z_offset_mm = z_amp_frac * layer_h * t_mod * taper * gate;

                // ── XY flow modulation ───────────────────────────────────────
                // Synchronized with Z: same t_mod, taper, gate.
                // At peak (t>0): flow > 1 -> wider extrusion -> forms the "tooth"
                // At trough (t<0): flow < 1 -> narrower -> makes room for next tooth
                // Clamp to [0.2, 2.0] for printability safety.
                double flow_factor = 1.0 + xy_amp * t_mod * taper * gate;
                flow_factor = std::max(0.2, std::min(2.0, flow_factor));

                // Endpoint of this sub-segment in scaled XY coords
                double t_end = static_cast<double>(step + 1) / n_steps;
                Point end_pt(
                    pa.x() + coord_t(std::round((pb.x() - pa.x()) * t_end)),
                    pa.y() + coord_t(std::round((pb.y() - pa.y()) * t_end))
                );

                coord_t z_scaled = coord_t(std::round(z_offset_mm / SCALING_FACTOR));
                pl3.points.push_back(Point3(end_pt.x(), end_pt.y(), z_scaled));
                z_diffs.push_back(z_offset_mm);
                flow_factors.push_back(flow_factor);
            }

            pos_mm += seg_len_mm;
        }

        if (pl3.size() < 2)
            continue;

        // Pad/trim z_diffs and flow_factors to exactly pl3.size()-1 segments
        const size_t n_segs = pl3.size() - 1;
        z_diffs.resize(n_segs, 0.0);
        flow_factors.resize(n_segs, 1.0);

        // ── 6. Emit ExtrusionPathContoured ────────────────────────────────────
        ExtrusionPath base_path(params.extrusion_role, flow_mm3_per_mm,
                                float(flow_width), params.flow.height());
        base_path.z_contoured = true;

        // ExtrusionPathContoured takes ownership of pl3, z_diffs, flow_factors
        auto* contoured = new ExtrusionPathContoured(
            std::move(pl3), base_path,
            std::move(z_diffs),
            std::move(flow_factors));
        eec->entities.push_back(contoured);
    }

    // ── 7. Gap fill: skipped for FlowWeaving ──────────────────────────────────
    // Modulated extrusion already covers the fill area. Gap fill would conflict
    // with Z modulation at line boundaries.
}

} // namespace Slic3r
