#ifndef VORTEK_GROUP_REORDER_HPP
#define VORTEK_GROUP_REORDER_HPP

#include <vector>
#include <set>
#include <map>
#include <memory>
#include "Print.hpp"

namespace Vortek {

class GroupReorder {
public:

/**
 * @brief Hook called during extruder reordering to handle the manual nozzle mapping mode (fmmNozzleManual).
 * 
 * If active, it resolves the mapping between logical filaments and active physical nozzles.
 * 
 * @param print Pointer to the Print object
 * @param print_config Pointer to the PrintConfig object
 * @param used_filaments List of logical filament IDs used in the current print
 * @param filament_maps Output vector mapping logical filaments to physical extruders
 * @param number_of_extruders The total count of physical extruders configured (usually 2)
 * @return True if the manual mapping was handled, false otherwise
 */
static bool handle_nozzle_manual_reorder(
    Slic3r::Print* print,
    const Slic3r::PrintConfig* print_config,
    const std::vector<unsigned int>& used_filaments,
    std::vector<int>& filament_maps,
    unsigned int number_of_extruders);

/**
 * @brief Hook for plain fmmManual mode on H2C printers.
 *
 * In Manual mode the user supplied an explicit filament→extruder map, so we must NOT
 * recompute it. However, update_filament_maps_to_config() has the side-effect of building
 * nozzle_group_result, which the GCodeProcessor pre-cooling/pre-heating post-processor
 * requires (its gate is get_nozzle_group_result()).
 * Skipping it (the previous behavior) left Manual-mode multi-nozzle prints with no carousel
 * M632 priming → the carousel can stall at nozzle changes.
 *
 * Only runs when the print actually uses the carousel (any filament mapped to extruder 2).
 * An all-Left (fixed-nozzle) print has no nozzle changes, needs no priming, and initializing
 * nozzle_group_result there would drive it down the multi-nozzle fake-wipe-tower path (crash).
 * No-op on non-H2C printers.
 *
 * Reference to BBS: BambuStudio PR#1 / commit 284ae6e2a5 — ToolOrdering.cpp sort_and_build_data.
 *
 * @param print Pointer to the Print object
 * @param filament_maps Current 1-based filament→extruder map (1=Left, 2=Right/carousel)
 */
static void handle_manual_mode_reorder(
    Slic3r::Print* print,
    const std::vector<int>& filament_maps);

}; // class GroupReorder
} // namespace Vortek

#endif // VORTEK_GROUP_REORDER_HPP
