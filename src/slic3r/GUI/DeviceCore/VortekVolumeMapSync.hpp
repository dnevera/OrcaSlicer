#ifndef VORTEK_VOLUME_MAP_SYNC_HPP
#define VORTEK_VOLUME_MAP_SYNC_HPP

/**
 * @file VortekVolumeMapSync.hpp
 * @brief GUI-layer volume map orchestrator for H2C multi-nozzle printers.
 *
 * Thin adapter: reads from GUI objects (Plater, PartPlate, PresetBundle),
 * delegates computation to Vortek::ConfigSync (libslic3r, pure logic),
 * writes results back to GUI objects.
 *
 * Reference to BBS: BambuStudio/src/slic3r/GUI/Plater.cpp
 *   update_filament_volume_map, try_pop_up_before_slice
 */

#include <functional>
#include <string>
#include <vector>

#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
class PresetBundle;
class DynamicConfig;
namespace GUI {
class PartPlate;
class Plater;
}
}

namespace Vortek { namespace VolumeMapSync {

// ---------------------------------------------------------------------------
// Shared plate iteration helper
// ---------------------------------------------------------------------------
// Iterates all plates in plater and calls callback(plate, plate_index).
// Eliminates duplicated plate_list.get_plate_count() + null-check boilerplate.
// ---------------------------------------------------------------------------
void for_each_plate(Slic3r::GUI::Plater* plater,
                    const std::function<void(Slic3r::GUI::PartPlate*, int)>& callback);

// ---------------------------------------------------------------------------
// Volume map updates
// ---------------------------------------------------------------------------

// Reference to BBS: BambuStudio/src/slic3r/GUI/Plater.cpp: update_filament_volume_map
// Updates the plates' filament volume types when the extruder's volume type changes.
// H2C-only: non-H2C printers exit immediately.
void update_filament_volume_map(Slic3r::GUI::Plater* plater, int extruder_id, int volume_type);

// Reference to BBS: BambuStudio/src/slic3r/GUI/Tab.cpp: set_extruder_volume_type
// Orchestrator: stat switch + volume map update + display refresh.
// H2C-only: non-H2C printers exit immediately.
void on_extruder_volume_type_changed(Slic3r::PresetBundle* preset_bundle, int extruder_id, Slic3r::NozzleVolumeType type);

// Reference to BBS: BambuStudio/src/slic3r/GUI/Tab.cpp: select_preset
// Preset restore: loads nozzle_volume_type from app_config and syncs.
// H2C-only: non-H2C printers exit immediately.
void sync_extruder_nozzle_stats_on_preset_select(Slic3r::PresetBundle* preset_bundle, const std::string& base_preset_name);

// ---------------------------------------------------------------------------
// Volume map save/read/check hooks
// ---------------------------------------------------------------------------

// Reference to BBS: BambuStudio/src/slic3r/GUI/FilamentMapDialog.cpp: try_pop_up_before_slice
// Saves the filament volume map to plate object(s) and project_config.
// H2C-only: non-H2C printers exit immediately.
void save_filament_volume_maps_hook(
    Slic3r::GUI::Plater* plater,
    Slic3r::GUI::PartPlate* plate,
    bool sync_plate,
    bool is_slice_all,
    Slic3r::FilamentMapMode mode,
    const std::vector<int>& volume_map);

bool check_volume_maps_changed_hook(
    const Slic3r::GUI::PartPlate* plate,
    const Slic3r::DynamicConfig& g_config,
    const std::vector<int>& new_volume_map);

std::vector<int> get_real_filament_volume_maps(
    const Slic3r::GUI::PartPlate* plate,
    const Slic3r::DynamicConfig& g_config);

}} // namespace Vortek::VolumeMapSync

#endif // VORTEK_VOLUME_MAP_SYNC_HPP
