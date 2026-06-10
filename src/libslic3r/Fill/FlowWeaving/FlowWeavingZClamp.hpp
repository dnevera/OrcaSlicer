// ── Flow Weaving Z-Clamp (XY-aware boundary clamping) ────────────────────────
//
// Extracted from GCode.cpp _extrude() and made XY-aware.
//
// ## The problem with the old approach
//
// The original clamping code (GCode.cpp L7684-7754) used:
//
//   auto layer_has_external_surface = [](const Layer* l) -> bool {
//       for (const LayerRegion* r : l->regions())
//           for (const Surface& s : r->fill_surfaces.surfaces)
//               if (s.is_top() || s.is_bottom())
//                   return true;  // ← ANY top/bottom on the layer counts
//       return false;
//   };
//
// This is NOT XY-aware: if a Benchy chimney has a stTop surface at Z=9.2,
// the clamping kicks in for ALL hull infill lines on Z=8.8, even if their XY
// coordinates are hundreds of millimetres away from the chimney polygon.
// Result: ±0.2mm amplitude instead of the desired ±1.0mm.
//
// ## The fix
//
// `point_in_external_surface()` checks whether the specific extrusion point
// `pt` falls INSIDE a stTop/stBottom polygon on the candidate layer.  Only
// when the point is actually within an external surface does that surface act
// as a Z boundary.
//
// Hull infill P not inside chimney polygon → walk continues upward → correct,
// larger amplitude.  Chimney infill P inside chimney polygon → boundary found
// at Z=9.2 → clamped correctly.
//
// ─────────────────────────────────────────────────────────────────────────────

#ifndef slic3r_FlowWeavingZClamp_hpp_
#define slic3r_FlowWeavingZClamp_hpp_

#include "FlowWeavingContext.hpp"
#include "../../Layer.hpp"
#include "../../Surface.hpp"
#include "../../Point.hpp"

namespace Slic3r {

class FlowWeavingZClamp {
public:
    // ── Main entry point ─────────────────────────────────────────────────────
    //
    // Clamps `z` so the nozzle stays within model boundaries.
    // Only layers where the specific point `pt` lies inside a stTop/stBottom
    // polygon are treated as boundaries (XY-aware, unlike the old approach).
    //
    //  z           — sinusoidal Z computed by FlowWeavingZModulator
    //  nominal_z   — layer's nominal print Z (mm)
    //  layer       — current Layer pointer (must be non-null)
    //  pt          — scaled extrusion point (same as line.b.to_point())
    //  tol_zone    — tolerance distance (mm) = z_tol_layers × layer_height
    //  first_z     — absolute floor: nozzle must never go below this (mm)
    //  ctx         — context to update with diagnostic dbg_* fields
    //
    // Returns the clamped Z.  Also fills:
    //   ctx.dbg_dist_up    — upward boundary distance (mm), -1 = none
    //   ctx.dbg_dist_dn    — downward boundary distance (mm), -1 = none
    //   ctx.dbg_clamped_up — was Z clipped at upper boundary?
    //   ctx.dbg_clamped_dn — was Z clipped at lower boundary?
    //
    static double clamp(double z, double nominal_z, const Layer* layer,
                        const Point& pt, double tol_zone, double first_z,
                        FlowWeavingContext& ctx)
    {
        const double deflection = z - nominal_z;

        // Reset diagnostic fields
        ctx.dbg_dist_up    = -1.;
        ctx.dbg_dist_dn    = -1.;
        ctx.dbg_clamped_up = false;
        ctx.dbg_clamped_dn = false;

        if (deflection > 0.) {
            // ── Going UP: find nearest layer where pt ∈ stTop/stBottom ─────
            const Layer* boundary = nullptr;
            for (const Layer* scan = layer->upper_layer;
                 scan != nullptr; scan = scan->upper_layer) {
                if (point_in_external_surface(scan, pt)) {
                    boundary = scan;
                    break;
                }
            }

            if (boundary) {
                // boundary_z = top of boundary layer; nozzle must not exit above it.
                const double boundary_z = boundary->print_z;
                const double dist_up    = boundary_z - nominal_z;
                ctx.dbg_dist_up = dist_up;

                if (dist_up <= 0.) {
                    z = nominal_z;
                    ctx.dbg_clamped_up = true;
                } else {
                    // Proportional scale-down inside tolerance zone
                    if (tol_zone > 0. && dist_up < tol_zone) {
                        const double scale = dist_up / tol_zone;
                        z = nominal_z + deflection * scale;
                    }
                    if (z > boundary_z) {
                        z = boundary_z;
                        ctx.dbg_clamped_up = true;
                    }
                }
            }
            // No XY-matching boundary above → free space → no clamp

        } else if (deflection < 0.) {
            // ── Going DOWN: find nearest layer where pt ∈ stTop/stBottom ───
            const Layer* boundary = nullptr;
            for (const Layer* scan = layer->lower_layer;
                 scan != nullptr; scan = scan->lower_layer) {
                if (point_in_external_surface(scan, pt)) {
                    boundary = scan;
                    break;
                }
            }

            if (boundary) {
                // boundary_z = bottom of boundary layer; nozzle must not go below it.
                const double boundary_z = boundary->print_z - boundary->height;
                const double dist_dn    = nominal_z - boundary_z;
                ctx.dbg_dist_dn = dist_dn;

                if (dist_dn <= 0.) {
                    z = nominal_z;
                    ctx.dbg_clamped_dn = true;
                } else {
                    if (tol_zone > 0. && dist_dn < tol_zone) {
                        const double scale = dist_dn / tol_zone;
                        z = nominal_z + deflection * scale;
                    }
                    if (z < boundary_z) {
                        z = boundary_z;
                        ctx.dbg_clamped_dn = true;
                    }
                }
            }
            // No XY-matching boundary below → free space → no clamp
        }

        // Absolute floor: nozzle must never descend below the first printed layer.
        if (z < first_z)
            z = first_z;

        return z;
    }

private:
    // XY-aware external surface check.
    // Returns true ONLY if the specific point `pt` lies inside a stTop or
    // stBottom fill polygon on layer `l`.
    // Unlike layer_has_external_surface(), this correctly ignores surfaces
    // from geometrically disconnected parts of the model at the same Z height.
    static bool point_in_external_surface(const Layer* l, const Point& pt)
    {
        for (const LayerRegion* r : l->regions())
            for (const Surface& s : r->fill_surfaces.surfaces)
                if ((s.is_top() || s.is_bottom()) && s.expolygon.contains(pt))
                    return true;
        return false;
    }
};

} // namespace Slic3r

#endif // slic3r_FlowWeavingZClamp_hpp_
