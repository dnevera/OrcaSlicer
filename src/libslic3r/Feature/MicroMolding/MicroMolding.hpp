// In-situ Micro-Injection Molding for OrcaSlicer
// Creates vertical cavities inside printed parts and injects molten plastic
// to form Z-reinforcing pins that improve inter-layer bonding strength.

#ifndef MICRO_MOLDING_HPP
#define MICRO_MOLDING_HPP

#include "libslic3r/PrintConfig.hpp"

#include <string>

namespace Slic3r {

class PrintObject;
class Layer;

class MicroMolding
{
public:
    // Phase 1: Subtract cylindrical cavities from lslices and region slices.
    // Called from PrintObject::prepare_infill() after combine_infill().
    // Stores injection point centres in the top layer of each span.
    static void subtract_cavities(PrintObject *print_object);

    // Phase 2: Enforce solid infill around cavities.
    // Splits stInternal fill_surfaces near cavities into stInternalSolid.
    static void enforce_solid_around_cavities(PrintObject *print_object);

    // Phase 3: Generate G-code for the injection step on a given layer.
    // Returns a G-code string to be appended after the layer's normal extrusions.
    static std::string generate_injection_gcode(
        const PrintObject *print_object,
        size_t             layer_id,
        double             print_z,
        double             layer_height,
        double             filament_diameter,
        double             nozzle_diameter,
        int                nozzle_temperature,
        bool               use_relative_e,
        const Vec2d       &copy_offset);
};

} // namespace Slic3r

#endif // MICRO_MOLDING_HPP
