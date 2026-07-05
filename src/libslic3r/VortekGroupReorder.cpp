#include "VortekGroupReorder.hpp"
#include "VortekMultiNozzle.hpp"
#include "VortekLog.hpp"
#include "VortekPrintHooks.hpp"
#include <algorithm>

namespace Vortek {

bool GroupReorder::handle_nozzle_manual_reorder(
    Slic3r::Print* print,
    const Slic3r::PrintConfig* print_config,
    const std::vector<unsigned int>& used_filaments,
    std::vector<int>& filament_maps,
    unsigned int number_of_extruders)
{
    if (!print || !print_config) return false;

    if (!is_h2c_printer(*print)) {
        return false;
    }

    // Check if we are in the nozzle manual mapping mode
    if (print_config->filament_map_mode.value != Slic3r::fmmNozzleManual) {
        return false;
    }

    VORTEK_LOG(warn, "processing fmmNozzleManual reorder for " << used_filaments.size() << " used filaments");

    // 1. Build manual filament map (0-based instead of 1-based GUI representation)
    auto manual_filament_map = print_config->filament_map.values;
    std::transform(manual_filament_map.begin(), manual_filament_map.end(), manual_filament_map.begin(), [](int v) { return v - 1; });

    // 2. Parse stats and create layered result
    auto nozzle_stats = Slic3r::MultiNozzleUtils::get_extruder_nozzle_stats(print_config->extruder_nozzle_stats.values);
    float nozzle_dia = print_config->nozzle_diameter.values.empty() ? 0.4f : print_config->nozzle_diameter.values.front();

    auto nozzle_result = Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult::create(
        used_filaments,
        manual_filament_map,
        print_config->filament_volume_map.values,
        print_config->filament_nozzle_map.values,
        nozzle_stats,
        nozzle_dia
    );

    if (!nozzle_result) {
        VORTEK_LOG(error, "failed to build nozzle group result from filament nozzle map!");
        return false;
    }

    VORTEK_LOG(warn, "nozzle group result built successfully, dynamic nozzle map = " << nozzle_result->is_support_dynamic_nozzle_map());

    // 3. Store result on print
    print->set_nozzle_group_result(std::make_shared<Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult>(*nozzle_result));

    // 4. Update config overrides (nozzle diameters, retracts etc.)
    print->update_to_config_by_nozzle_group_result(*nozzle_result);

    // 5. Update filament_maps for ToolOrdering output mapping
    for (size_t fid = 0; fid < number_of_extruders; ++fid) {
        filament_maps[fid] = nozzle_result->get_extruder_id(static_cast<int>(fid), -1);
        VORTEK_LOG(debug, "mapped logical filament " << fid << " to physical extruder " << filament_maps[fid]);
    }

    return true;
}

// Reference to BBS: BambuStudio PR#1 / commit 284ae6e2a5 — ToolOrdering.cpp sort_and_build_data fmmManual branch
void GroupReorder::handle_manual_mode_reorder(
    Slic3r::Print* print,
    const std::vector<int>& filament_maps)
{
    // Guard: only H2C printers with carousel need M632 priming initialization.
    if (!print || !is_h2c_printer(*print)) {
        return;
    }

    // filament_maps is 1-based: 1 = Left (fixed nozzle), 2 = Right (carousel).
    // Only run when at least one filament is assigned to the carousel (extruder 2).
    // An all-Left print has no nozzle changes and needs no priming; calling
    // update_filament_maps_to_config() there would drive the single-nozzle print down
    // the multi-nozzle fake-wipe-tower path (empty z_and_depth_pairs → crash).
    const bool uses_carousel = std::any_of(filament_maps.begin(), filament_maps.end(),
                                            [](int m) { return m == 2; });
    if (!uses_carousel) {
        VORTEK_LOG(warn, "handle_manual_mode_reorder: all filaments on Left nozzle, skipping carousel init");
        return;
    }

    VORTEK_LOG(warn, "handle_manual_mode_reorder: carousel used in Manual mode — initializing nozzle_group_result via update_filament_maps_to_config");

    // Call update_filament_maps_to_config with EMPTY volume/nozzle maps so that
    // Step 1 (nozzle slot assignment) and Step 2 (volume type from nozzle_volume_type)
    // are always fully recalculated from filament_maps.
    //
    // BUG that was fixed: previously we passed print->config().filament_volume_map.values
    // and print->config().filament_nozzle_map.values here. If the project was saved with
    // stale 1-element maps (e.g. loaded from an old 3MF), the idempotency guard in
    // update_filament_maps_to_config would see the 1-element maps as matching the
    // (also 1-element) computed result and return early → maps never expanded to full
    // filament count → 3MF saved with ['1'] and ['0'] instead of 5-element arrays.
    //
    // Reference to BBS: BambuStudio/src/libslic3r/Format/bbs_3mf.cpp (filament_nozzle_map write)
    // Reference to BBS: BambuStudio/src/libslic3r/PresetBundle.cpp (on_printer_model_change)
    print->update_filament_maps_to_config(
        filament_maps,
        std::vector<int>(),   // force Step 2 to rebuild volume types from nozzle_volume_type
        std::vector<int>());  // force Step 1 to rebuild carousel slot assignments from filament_maps
}

} // namespace Vortek
