#include "VortekVolumeMapSync.hpp"
#include "libslic3r/VortekConfigSync.hpp"
#include "libslic3r/VortekKeys.hpp"
#include "libslic3r/VortekLog.hpp"
#include "libslic3r/VortekMultiNozzle.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Widgets/MultiNozzleSync.hpp"

namespace Vortek { namespace VolumeMapSync {

// ---------------------------------------------------------------------------
// for_each_plate — shared plate iteration helper
// ---------------------------------------------------------------------------
void for_each_plate(Slic3r::GUI::Plater* plater,
                    const std::function<void(Slic3r::GUI::PartPlate*, int)>& callback)
{
    if (!plater) return;
    auto& partplate_list = plater->get_partplate_list();
    for (int idx = 0; idx < partplate_list.get_plate_count(); ++idx) {
        auto plate = partplate_list.get_plate(idx);
        if (!plate) continue;
        callback(plate, idx);
    }
}

// ---------------------------------------------------------------------------
// update_filament_volume_map
// ---------------------------------------------------------------------------
// Reference to BBS: BambuStudio/src/slic3r/GUI/Plater.cpp: update_filament_volume_map
// Updates the plates' filament volume types when the extruder's volume type changes.
// H2C-only: non-H2C printers exit immediately.
// Delegates computation to ConfigSync::compute_volume_map_for_extruder.
// ---------------------------------------------------------------------------
void update_filament_volume_map(Slic3r::GUI::Plater* plater, int extruder_id, int volume_type)
{
    if (!plater || !Slic3r::GUI::wxGetApp().preset_bundle) return;
    if (!Vortek::is_h2c_printer(Slic3r::GUI::wxGetApp().preset_bundle)) {
        return;
    }

    int selected = ConfigSync::compute_selected_volume_type(
        static_cast<Slic3r::NozzleVolumeType>(volume_type));

    for_each_plate(plater, [&](Slic3r::GUI::PartPlate* plate, int idx) {
        auto filament_map = plate->get_filament_maps();
        auto filament_volume_map = plate->get_filament_volume_maps();
        if (filament_map.empty() || filament_volume_map.empty()) return;

        auto [new_map, changed] = ConfigSync::compute_volume_map_for_extruder(
            filament_map, filament_volume_map, extruder_id, selected);

        if (changed) {
            plate->set_filament_volume_maps(new_map);
            VORTEK_LOG(warn, "update_filament_volume_map: extruder=" << extruder_id
                       << " volume_type=" << volume_type
                       << " selected=" << selected
                       << " plate=" << idx << " updated");
        }
    });
}

// ---------------------------------------------------------------------------
// on_extruder_volume_type_changed
// ---------------------------------------------------------------------------
// Reference to BBS: BambuStudio/src/slic3r/GUI/Tab.cpp: set_extruder_volume_type
// Orchestrator: stat switch + volume map update + display refresh.
// H2C-only: non-H2C printers exit immediately.
// ---------------------------------------------------------------------------
void on_extruder_volume_type_changed(Slic3r::PresetBundle* preset_bundle, int extruder_id, Slic3r::NozzleVolumeType type)
{
    if (!preset_bundle) return;
    if (!Vortek::is_h2c_printer(preset_bundle)) {
        return;
    }

    preset_bundle->extruder_nozzle_stat.on_volume_type_switch(extruder_id, type);
    if (Slic3r::GUI::wxGetApp().plater()) {
        update_filament_volume_map(Slic3r::GUI::wxGetApp().plater(), extruder_id, static_cast<int>(type));
    }
    Slic3r::GUI::updateNozzleCountDisplay(preset_bundle, extruder_id, type);
}

// ---------------------------------------------------------------------------
// sync_extruder_nozzle_stats_on_preset_select
// ---------------------------------------------------------------------------
// Reference to BBS: BambuStudio/src/slic3r/GUI/Tab.cpp: select_preset
// Loads prev_nozzle_volume_type from config and synchronizes both nozzle
// volume types and filament volume maps for H2C printers.
// H2C-only: non-H2C printers exit immediately.
// ---------------------------------------------------------------------------
void sync_extruder_nozzle_stats_on_preset_select(Slic3r::PresetBundle* preset_bundle, const std::string& base_preset_name)
{
    if (!preset_bundle) return;
    if (!Vortek::is_h2c_printer(preset_bundle)) {
        return;
    }

    std::string prev_nozzle_volume_type = Slic3r::GUI::wxGetApp().app_config->get_nozzle_volume_types_from_config(base_preset_name);
    if (prev_nozzle_volume_type.empty()) {
        prev_nozzle_volume_type = "Standard,Hybrid";
    }
    bool use_default = true;
    if (!prev_nozzle_volume_type.empty()) {
        auto* nozzle_volume_type_option = preset_bundle->project_config.option<Slic3r::ConfigOptionEnumsGeneric>(Vortek::Keys::k_nozzle_volume_type);
        if (nozzle_volume_type_option && nozzle_volume_type_option->deserialize(prev_nozzle_volume_type)) {
            for (size_t idx = 0; idx < nozzle_volume_type_option->size(); ++idx) {
                Slic3r::NozzleVolumeType volume_type = Slic3r::NozzleVolumeType(nozzle_volume_type_option->values[idx]);
                on_extruder_volume_type_changed(preset_bundle, idx, volume_type);
            }
            use_default = false;
        }
    }

    if (use_default) {
        auto* default_nozzle_volume_opt = preset_bundle->project_config.option<Slic3r::ConfigOptionEnumsGeneric>(Vortek::Keys::k_default_nozzle_volume_type);
        if (default_nozzle_volume_opt) {
            auto default_nozzle_volume_type = default_nozzle_volume_opt->values;
            for (size_t eid = 0; eid < default_nozzle_volume_type.size(); ++eid) {
                auto type = Slic3r::NozzleVolumeType(default_nozzle_volume_type[eid]);
                on_extruder_volume_type_changed(preset_bundle, eid, type);
            }
            preset_bundle->project_config.option<Slic3r::ConfigOptionEnumsGeneric>(Vortek::Keys::k_nozzle_volume_type)->values = default_nozzle_volume_type;
        }
    }
}

// ---------------------------------------------------------------------------
// save_filament_volume_maps_hook
// ---------------------------------------------------------------------------
// Reference to BBS: BambuStudio/src/slic3r/GUI/FilamentMapDialog.cpp – try_pop_up_before_slice
// Saves the filament volume map to plate object(s) and project_config.
// Delegates project_config write to ConfigSync::write_volume_map_to_config.
// H2C-only: non-H2C printers exit immediately.
// ---------------------------------------------------------------------------
void save_filament_volume_maps_hook(
    Slic3r::GUI::Plater* plater,
    Slic3r::GUI::PartPlate* plate,
    bool sync_plate,
    bool is_slice_all,
    Slic3r::FilamentMapMode mode,
    const std::vector<int>& volume_map)
{
    if (!Vortek::is_h2c_printer(Slic3r::GUI::wxGetApp().preset_bundle)) {
        VORTEK_LOG(warn, "save_filament_volume_maps_hook: NOT H2C printer, skipping");
        return;
    }

    {
        std::string vm_str;
        for (int v : volume_map) vm_str += std::to_string(v) + ",";
        VORTEK_LOG(warn, "save_filament_volume_maps_hook: mode=" << (int)mode
            << " volume_map=[" << vm_str << "] sync_plate=" << sync_plate << " is_slice_all=" << is_slice_all);
    }

    // project_config write: only in Manual mode (user explicitly set per-filament HF/Std).
    // Reference to BBS: BambuStudio/src/slic3r/GUI/FilamentMapDialog.cpp – try_pop_up_before_slice
    if (mode == Slic3r::fmmManual) {
        ConfigSync::write_volume_map_to_config(
            Slic3r::GUI::wxGetApp().preset_bundle->project_config, volume_map);
        VORTEK_LOG(warn, "save_filament_volume_maps_hook: WROTE to project_config.filament_volume_map");
    }

    // plate write: always save (BBS saves volume_map regardless of mode).
    // Reference to BBS: BambuStudio/src/slic3r/GUI/FilamentMapDialog.cpp:218,226 – set_filament_volume_maps unconditional
    if (sync_plate && plate) {
        if (is_slice_all) {
            for_each_plate(plater, [&](Slic3r::GUI::PartPlate* p, int) {
                p->set_filament_volume_maps(volume_map);
            });
        } else {
            plate->set_filament_volume_maps(volume_map);
        }
    }
}

// ---------------------------------------------------------------------------
// check_volume_maps_changed_hook
// ---------------------------------------------------------------------------
bool check_volume_maps_changed_hook(
    const Slic3r::GUI::PartPlate* plate,
    const Slic3r::DynamicConfig& g_config,
    const std::vector<int>& new_volume_map)
{
    if (!Vortek::is_h2c_printer(Slic3r::GUI::wxGetApp().preset_bundle)) return false;
    return get_real_filament_volume_maps(plate, g_config) != new_volume_map;
}

// ---------------------------------------------------------------------------
// get_real_filament_volume_maps
// ---------------------------------------------------------------------------
// Reads volume maps from plate, falls back to config via ConfigSync helper.
// ---------------------------------------------------------------------------
std::vector<int> get_real_filament_volume_maps(
    const Slic3r::GUI::PartPlate* plate,
    const Slic3r::DynamicConfig& g_config)
{
    if (!Vortek::is_h2c_printer(Slic3r::GUI::wxGetApp().preset_bundle)) return {};

    auto old_maps = plate->get_filament_volume_maps();
    if (old_maps.empty()) {
        old_maps = ConfigSync::read_volume_map_from_config(g_config);
    }
    return old_maps;
}

}} // namespace Vortek::VolumeMapSync
