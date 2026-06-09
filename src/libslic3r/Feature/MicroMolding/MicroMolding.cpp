// In-situ Micro-Injection Molding for OrcaSlicer
//
// Creates vertical cavities inside printed parts and injects molten plastic
// to form Z-reinforcing pins that improve inter-layer bonding strength.
//
// Integration point: PrintObject::prepare_infill() AFTER combine_infill().

#include "MicroMolding.hpp"

#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/Flow.hpp"

#include <boost/log/trivial.hpp>
#include <cmath>
#include <random>
#include <sstream>
#include <iomanip>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace Slic3r {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

// Create an approximate circle polygon centred at `center` with radius `r` (scaled).
static Polygon make_circle(const Point &center, coord_t radius, int segments = 32)
{
    Polygon poly;
    poly.points.reserve(segments);
    for (int i = 0; i < segments; ++i) {
        double angle = 2.0 * M_PI * i / segments;
        poly.points.emplace_back(
            center.x() + coord_t(radius * cos(angle)),
            center.y() + coord_t(radius * sin(angle)));
    }
    return poly;
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 1: subtract_cavities
// ─────────────────────────────────────────────────────────────────────────────

void MicroMolding::subtract_cavities(PrintObject *print_object)
{
    if (!print_object || print_object->layers().empty())
        return;

    // Check if any region has micro_molding enabled
    bool any_enabled = false;
    MicroMoldingType molding_type = MicroMoldingType::None;
    double cavity_dia   = 1.0;
    int    layers_span  = 10;
    double lock_ratio_cfg  = 1.8;
    int    neck_layers_cfg = 2;
    int    head_layers_cfg = 1;

    for (size_t region_id = 0; region_id < print_object->num_printing_regions(); ++region_id) {
        const PrintRegionConfig &cfg = print_object->printing_region(region_id).config();
        if (cfg.micro_molding.value != MicroMoldingType::None) {
            any_enabled  = true;
            molding_type = cfg.micro_molding.value;
            cavity_dia   = cfg.micro_molding_cavity_diameter.value;
            layers_span  = cfg.micro_molding_layers_span.value;
            lock_ratio_cfg  = cfg.micro_molding_lock_ratio.value;
            neck_layers_cfg = cfg.micro_molding_neck_layers.value;
            head_layers_cfg = cfg.micro_molding_head_layers.value;
            break;
        }
    }

    if (!any_enabled)
        return;

    BOOST_LOG_TRIVIAL(info) << "MicroMolding: subtract_cavities start, cavity_dia=" << cavity_dia
                            << " layers_span=" << layers_span;

    const auto &layers = print_object->layers();
    const size_t num_layers = layers.size();

    if (num_layers < (size_t)layers_span + 2)
        return; // Model too thin for micro-molding

    // Get basic geometry parameters
    const double nozzle_dia  = print_object->print()->config().nozzle_diameter.get_at(0);
    const double wall_width  = nozzle_dia * 1.2; // approximate external perimeter width
    const int    wall_loops  = print_object->printing_region(0).config().wall_loops.value;

    // Interlocking cavity profile parameters (from config):
    // Cavities alternate between narrow necks and wide heads to create
    // mechanical interlocking — the solidified plastic cannot slide out.
    const double lock_ratio   = lock_ratio_cfg;
    const int    neck_layers  = neck_layers_cfg;
    const int    head_layers  = head_layers_cfg;
    const int    lock_cycle   = neck_layers + head_layers;

    const double neck_radius  = cavity_dia / 2.0;                   // mm
    const double head_radius  = (cavity_dia * lock_ratio) / 2.0;    // mm
    const coord_t neck_radius_scaled = scale_(neck_radius);
    const coord_t head_radius_scaled = scale_(head_radius);

    // Safety margins: keep cavities away from walls (use HEAD radius — the largest)
    const double margin_from_walls = wall_width * wall_loops + head_radius + 0.5; // mm
    const coord_t margin_scaled = scale_(margin_from_walls);

    // Grid spacing for cavity placement (based on head diameter)
    const double head_dia = cavity_dia * lock_ratio;
    const double grid_spacing = std::max(head_dia * 3.0, 3.0); // mm, minimum 3mm between centres
    const coord_t grid_step = scale_(grid_spacing);

    // Determine safe layer range: skip bottom/top shells
    const int bottom_shells = print_object->printing_region(0).config().bottom_shell_layers.value;
    const int top_shells    = print_object->printing_region(0).config().top_shell_layers.value;
    const size_t first_layer = std::max(1, bottom_shells);
    const size_t last_layer  = num_layers > (size_t)top_shells ? num_layers - top_shells - 1 : 0;

    if (first_layer >= last_layer)
        return;

    // RNG for random jitter mode
    std::mt19937 rng(42); // deterministic seed for reproducibility
    std::uniform_real_distribution<double> jitter_dist(-grid_spacing * 0.25, grid_spacing * 0.25);

    // Process span by span
    for (size_t span_start = first_layer; span_start + layers_span <= last_layer; span_start += layers_span) {
        size_t span_end = span_start + layers_span - 1; // inclusive
        size_t injection_layer_idx = span_end; // top layer of span = injection point

        // Compute safe zone: intersection of all inset lslices in the span
        // Start with the first layer's lslices, inset by margin
        ExPolygons safe_zone = offset_ex(layers[span_start]->lslices, -margin_scaled);

        for (size_t li = span_start + 1; li <= span_end && !safe_zone.empty(); ++li) {
            ExPolygons layer_inset = offset_ex(layers[li]->lslices, -margin_scaled);
            safe_zone = intersection_ex(safe_zone, layer_inset);
        }

        if (safe_zone.empty())
            continue;

        // Get bounding box of safe zone for grid generation
        BoundingBox bbox = get_extents(safe_zone);

        // Generate cavity centres on a regular grid within safe zone
        std::vector<Point> cavity_centres;

        for (coord_t x = bbox.min.x(); x <= bbox.max.x(); x += grid_step) {
            for (coord_t y = bbox.min.y(); y <= bbox.max.y(); y += grid_step) {
                Point candidate(x, y);

                // Apply jitter for random mode
                if (molding_type == MicroMoldingType::Random) {
                    candidate.x() += scale_(jitter_dist(rng));
                    candidate.y() += scale_(jitter_dist(rng));
                }

                // Check if candidate is inside safe zone
                bool inside = false;
                for (const ExPolygon &ep : safe_zone) {
                    if (ep.contains(candidate)) {
                        inside = true;
                        break;
                    }
                }

                if (inside)
                    cavity_centres.push_back(candidate);
            }
        }

        if (cavity_centres.empty())
            continue;

        BOOST_LOG_TRIVIAL(debug) << "MicroMolding: span [" << span_start << ".." << span_end
                                 << "] has " << cavity_centres.size() << " cavities";

        // Subtract interlocking cavity profile from each layer in the span.
        // Alternate between narrow necks and wide heads to create mechanical locks.
        for (size_t li = span_start; li <= span_end; ++li) {
            Layer *layer = layers[li];

            // Determine radius for this layer: neck or head?
            int layer_in_span = (int)(li - span_start);
            int pos_in_cycle  = layer_in_span % lock_cycle;
            bool is_head      = (pos_in_cycle >= neck_layers);
            coord_t radius    = is_head ? head_radius_scaled : neck_radius_scaled;

            // Build subtraction polygons for THIS layer's radius
            Polygons subtraction_polys;
            subtraction_polys.reserve(cavity_centres.size());
            for (const Point &center : cavity_centres) {
                subtraction_polys.push_back(make_circle(center, radius));
            }

            // Subtract from lslices
            layer->lslices = diff_ex(layer->lslices, subtraction_polys);

            // Update bounding boxes
            layer->lslices_bboxes.clear();
            layer->lslices_bboxes.reserve(layer->lslices.size());
            for (const ExPolygon &expoly : layer->lslices)
                layer->lslices_bboxes.emplace_back(get_extents(expoly));

            // Subtract from each region's slices and fill_surfaces
            for (LayerRegion *region : layer->regions()) {
                // Subtract from region slices
                ExPolygons new_slices = diff_ex(region->slices.surfaces, subtraction_polys);
                region->slices.set(std::move(new_slices), stInternal);

                // Subtract from fill surfaces
                SurfaceCollection new_fill_surfaces;
                for (const Surface &surface : region->fill_surfaces.surfaces) {
                    ExPolygons remaining = diff_ex(ExPolygons{surface.expolygon}, subtraction_polys);
                    for (ExPolygon &ep : remaining) {
                        new_fill_surfaces.surfaces.emplace_back(surface, std::move(ep));
                    }
                }
                region->fill_surfaces = std::move(new_fill_surfaces);
            }
        }

        // Store injection points on the top layer of the span
        layers[injection_layer_idx]->micro_molding_injection_points = std::move(cavity_centres);
    }

    BOOST_LOG_TRIVIAL(info) << "MicroMolding: subtract_cavities done";
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 2: enforce_solid_around_cavities
// ─────────────────────────────────────────────────────────────────────────────

void MicroMolding::enforce_solid_around_cavities(PrintObject *print_object)
{
    if (!print_object || print_object->layers().empty())
        return;

    // Check if any region has micro_molding enabled
    bool any_enabled = false;
    double cavity_dia = 1.0;
    int layers_span = 10;

    for (size_t region_id = 0; region_id < print_object->num_printing_regions(); ++region_id) {
        const PrintRegionConfig &cfg = print_object->printing_region(region_id).config();
        if (cfg.micro_molding.value != MicroMoldingType::None) {
            any_enabled = true;
            cavity_dia  = cfg.micro_molding_cavity_diameter.value;
            layers_span = cfg.micro_molding_layers_span.value;
            break;
        }
    }

    if (!any_enabled)
        return;

    BOOST_LOG_TRIVIAL(info) << "MicroMolding: enforce_solid_around_cavities start";

    const auto &layers = print_object->layers();
    const size_t num_layers = layers.size();
    const int bottom_shells = print_object->printing_region(0).config().bottom_shell_layers.value;
    const int top_shells    = print_object->printing_region(0).config().top_shell_layers.value;
    const size_t first_layer = std::max(1, bottom_shells);
    const size_t last_layer  = num_layers > (size_t)top_shells ? num_layers - top_shells - 1 : 0;

    // Enforce radius: cavity_dia + 1mm on each side
    const double enforce_radius = cavity_dia / 2.0 + 1.0; // mm
    const coord_t enforce_radius_scaled = scale_(enforce_radius);

    // Process span by span — find injection layers and apply enforcers
    for (size_t span_start = first_layer; span_start + layers_span <= last_layer; span_start += layers_span) {
        size_t span_end = span_start + layers_span - 1;
        Layer *injection_layer = layers[span_end];

        if (injection_layer->micro_molding_injection_points.empty())
            continue;

        // Build enforcer polygons: circles of enforce_radius around each cavity centre
        Polygons enforcer_polys;
        enforcer_polys.reserve(injection_layer->micro_molding_injection_points.size());
        for (const Point &center : injection_layer->micro_molding_injection_points) {
            enforcer_polys.push_back(make_circle(center, enforce_radius_scaled));
        }

        // Apply to all layers in the span
        for (size_t li = span_start; li <= span_end; ++li) {
            Layer *layer = layers[li];
            for (LayerRegion *region : layer->regions()) {
                SurfaceCollection new_fill_surfaces;
                for (const Surface &surface : region->fill_surfaces.surfaces) {
                    if (surface.surface_type == stInternal || surface.surface_type == stInternalVoid) {
                        // Split: intersect with enforcer → solid, diff → sparse
                        ExPolygons solid_part = intersection_ex(ExPolygons{surface.expolygon}, enforcer_polys);
                        ExPolygons sparse_part = diff_ex(ExPolygons{surface.expolygon}, enforcer_polys);

                        for (ExPolygon &ep : solid_part) {
                            Surface s(surface);
                            s.surface_type = stInternalSolid;
                            s.expolygon = std::move(ep);
                            new_fill_surfaces.surfaces.push_back(std::move(s));
                        }
                        for (ExPolygon &ep : sparse_part) {
                            new_fill_surfaces.surfaces.emplace_back(surface, std::move(ep));
                        }
                    } else {
                        new_fill_surfaces.surfaces.push_back(surface);
                    }
                }
                region->fill_surfaces = std::move(new_fill_surfaces);
            }
        }
    }

    BOOST_LOG_TRIVIAL(info) << "MicroMolding: enforce_solid_around_cavities done";
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 3: generate_injection_gcode
// ─────────────────────────────────────────────────────────────────────────────

std::string MicroMolding::generate_injection_gcode(
    const PrintObject *print_object,
    size_t             layer_id,
    double             print_z,
    double             layer_height,
    double             filament_diameter,
    double             nozzle_diameter,
    int                nozzle_temperature,
    bool               use_relative_e,
    const Vec2d       &copy_offset)
{
    if (!print_object || layer_id >= print_object->layers().size())
        return {};

    const Layer *layer = print_object->get_layer(layer_id);
    if (!layer || layer->micro_molding_injection_points.empty())
        return {};

    // Get config
    double cavity_dia       = 1.0;
    int    layers_span      = 10;
    double flow_multiplier  = 1.05;
    int    temp_offset      = 20;
    double lock_ratio       = 1.8;
    int    neck_layers      = 2;
    int    head_layers      = 1;

    for (size_t region_id = 0; region_id < print_object->num_printing_regions(); ++region_id) {
        const PrintRegionConfig &cfg = print_object->printing_region(region_id).config();
        if (cfg.micro_molding.value != MicroMoldingType::None) {
            cavity_dia      = cfg.micro_molding_cavity_diameter.value;
            layers_span     = cfg.micro_molding_layers_span.value;
            flow_multiplier = cfg.micro_molding_flow_multiplier.value;
            temp_offset     = cfg.micro_molding_temp_offset.value;
            lock_ratio      = cfg.micro_molding_lock_ratio.value;
            neck_layers     = cfg.micro_molding_neck_layers.value;
            head_layers     = cfg.micro_molding_head_layers.value;
            break;
        }
    }

    // Calculate injection volume for interlocking cavity profile.
    // Cavity alternates between neck (cavity_dia) and head (cavity_dia * lock_ratio).
    const int    lock_cycle   = neck_layers + head_layers;

    const double neck_radius  = cavity_dia / 2.0;
    const double head_radius  = (cavity_dia * lock_ratio) / 2.0;

    // Sum volume layer by layer
    double cavity_volume = 0.0;
    for (int i = 0; i < layers_span; ++i) {
        int pos_in_cycle = i % lock_cycle;
        bool is_head = (pos_in_cycle >= neck_layers);
        double r = is_head ? head_radius : neck_radius;
        cavity_volume += M_PI * r * r * layer_height;
    }

    // E length = V / (π × (filament_dia/2)²) × flow_multiplier
    const double filament_radius = filament_diameter / 2.0;
    const double filament_area = M_PI * filament_radius * filament_radius;
    const double e_inject = (cavity_volume / filament_area) * flow_multiplier;

    // Injection feedrate (slow for pressure build-up)
    const double inject_feedrate = 60.0; // mm/min (1 mm/s — very slow)

    const int target_temp = nozzle_temperature + temp_offset;

    std::ostringstream gcode;
    gcode << std::fixed << std::setprecision(3);

    gcode << "\n; --- IN-SITU MICRO-INJECTION MOLDING ---\n";
    gcode << "; Layer " << layer_id << ", " << layer->micro_molding_injection_points.size()
          << " injection points\n";
    gcode << "; Cavity: D=" << cavity_dia << "mm, span=" << layers_span
          << " layers, E_inject=" << e_inject << "mm\n";

    // Raise temperature if offset > 0
    if (temp_offset > 0) {
        gcode << "M109 S" << target_temp << " ; Micro-molding: raise temp\n";
    }

    for (const Point &center : layer->micro_molding_injection_points) {
        // Convert from object-local coordinates to absolute plate coordinates
        double x = unscale<double>(center.x()) + copy_offset.x();
        double y = unscale<double>(center.y()) + copy_offset.y();

        gcode << "; Injection point at (" << x << ", " << y << ")\n";

        // Retract
        gcode << "G1 E-1.000 F1800 ; Retract\n";

        // Travel to injection point
        gcode << "G1 X" << x << " Y" << y << " F9000 ; Move to injection point\n";

        // Lower Z slightly for seal (nozzle pressed onto surface)
        gcode << "G1 Z" << (print_z - 0.05) << " F1200 ; Seal nozzle\n";

        // De-retract
        gcode << "G1 E1.000 F1800 ; De-retract\n";

        // Inject
        if (use_relative_e) {
            gcode << "G1 E" << e_inject << " F" << inject_feedrate << " ; Inject plastic\n";
        } else {
            // For absolute E, we'd need the current E position. For now use relative block.
            gcode << "M83 ; Relative E for injection\n";
            gcode << "G1 E" << e_inject << " F" << inject_feedrate << " ; Inject plastic\n";
            gcode << "M82 ; Restore absolute E\n";
        }

        // Dwell under pressure
        gcode << "G4 P500 ; Dwell 500ms under pressure\n";

        // Retract to detach from cavity
        gcode << "G1 E-1.000 F1800 ; Retract after injection\n";

        // Lift Z back
        gcode << "G1 Z" << print_z << " F1200 ; Lift\n";

        // Wipe move to break string
        gcode << "G1 X" << (x + 1.0) << " Y" << (y + 1.0) << " F3000 ; Wipe\n";
    }

    // Post-injection cooling sequence
    if (temp_offset > 0) {
        // Restore temperature (async — don't wait)
        gcode << "M104 S" << nozzle_temperature << " ; Micro-molding: restore temp\n";
    }

    // Blast part cooling fan to solidify injected plastic and cool nozzle
    gcode << "M106 P1 S255 ; Cooling fan max for solidification\n";
    gcode << "G4 P3000 ; Dwell 3s for cooling\n";
    gcode << "M106 P1 S0 ; Restore fan off (normal fan control resumes)\n";

    gcode << "; --- END MICRO-INJECTION MOLDING ---\n\n";

    return gcode.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 4: generate_post_injection_cleanup
// ─────────────────────────────────────────────────────────────────────────────
//
// After injection, travel to the wipe tower, purge residual over-pressure
// material and wipe the nozzle to prevent contamination of the next extrusion.
//
// Sequence: retract → Z-lift → travel to tower → lower → de-retract + purge
//           → wipe moves → retract → comment

std::string MicroMolding::generate_post_injection_cleanup(
    double print_z,
    double wipe_tower_x,
    double wipe_tower_y)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(3);

    out << "; Micro-molding: nozzle cleaning at wipe tower\n";
    // Retract before travel
    out << "G1 E-0.800 F1800 ; Retract for travel to tower\n";
    // Lift Z to clear the model
    out << "G1 Z" << (print_z + 2.0) << " F1200 ; Lift Z for travel\n";
    // Travel to wipe tower
    out << "G1 X" << wipe_tower_x << " Y" << wipe_tower_y << " F9000 ; Travel to wipe tower\n";
    // Lower to print Z
    out << "G1 Z" << print_z << " F1200 ; Lower to print Z\n";
    // De-retract + purge a small amount to clean nozzle
    out << "G1 E1.500 F300 ; De-retract + purge nozzle\n";
    // Small wipe moves on the tower
    out << "G1 X" << (wipe_tower_x + 10.0) << " Y" << wipe_tower_y << " F1500 ; Wipe on tower\n";
    out << "G1 X" << (wipe_tower_x + 10.0) << " Y" << (wipe_tower_y + 2.0) << " F1500 ; Wipe on tower\n";
    // Retract after purge
    out << "G1 E-0.800 F1800 ; Retract after purge\n";
    out << "; End nozzle cleaning\n";

    return out.str();
}

} // namespace Slic3r

