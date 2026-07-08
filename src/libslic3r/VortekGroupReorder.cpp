#include "VortekGroupReorder.hpp"
#include "VortekMultiNozzle.hpp"
#include "VortekLog.hpp"
#include "VortekKeys.hpp"
#include "VortekPrintHooks.hpp"
#include <algorithm>

namespace Vortek {

// Helper: build nozzle_list from extruder_nozzle_stats
// Shared by has_user_volume_map and auto-mode branches.
// Reference to BBS: BambuStudio/src/libslic3r/GCode/ToolOrdering.cpp nozzle_list construction
static std::vector<Slic3r::MultiNozzleUtils::NozzleInfo> build_nozzle_list_from_stats(
    const std::vector<std::map<Slic3r::NozzleVolumeType, int>>& nozzle_stats,
    const Slic3r::DynamicPrintConfig& config)
{
    std::vector<Slic3r::MultiNozzleUtils::NozzleInfo> nozzle_list;

    // Compute total right-extruder (carousel) nozzle count for unique group_id assignment.
    // BBS uses IDs like [1,2,3,5,6] for right carousel — descending from total_count.
    // Reference to BBS: BambuStudio/src/libslic3r/GCode/ToolOrdering.cpp nozzle_list construction
    int total_carousel_nozzles = 0;
    for (size_t i = 1; i < nozzle_stats.size(); ++i) {
        for (auto const& [vtype, count] : nozzle_stats[i]) {
            total_carousel_nozzles += count;
        }
    }
    int next_carousel_nozzle = total_carousel_nozzles;  // start from max, descend to 1

    for (size_t i = 0; i < nozzle_stats.size(); ++i) {
        auto slot_map = nozzle_stats[i];
        for (auto const& [vtype, count] : slot_map) {
            for (int c = 0; c < count; ++c) {
                Slic3r::MultiNozzleUtils::NozzleInfo nz;
                nz.extruder_id = (int)i;
                nz.volume_type = vtype;
                float dia = 0.4f;
                if (config.has("nozzle_diameter")) {
                    auto* opt = config.option<Slic3r::ConfigOptionFloats>("nozzle_diameter");
                    if (opt && nz.extruder_id < (int)opt->values.size())
                        dia = (float)opt->values[nz.extruder_id];
                }
                nz.diameter = Slic3r::MultiNozzleUtils::format_diameter_to_str(dia);
                if (nz.extruder_id == 0) {
                    nz.group_id = 0;
                } else {
                    nz.group_id = next_carousel_nozzle--;
                    if (next_carousel_nozzle < 1) next_carousel_nozzle = total_carousel_nozzles;
                }
                nozzle_list.push_back(nz);
            }
        }
    }
    return nozzle_list;
}

// Helper: volume_type-aware assignment of filaments to nozzle slots.
// Pass 1: match extruder_id + volume_type (unique).
// Pass 2: any unused slot on same extruder.
// Pass 3: overflow — share slot matching volume_type.
// Pass 4: last resort — any slot on same extruder.
// Reference to BBS: BambuStudio/src/libslic3r/FilamentGroup.cpp:1723 (conceptually equivalent)
static std::vector<int> assign_filaments_to_nozzles_volume_aware(
    const std::vector<int>& filament_maps,
    const std::vector<int>& volume_map,
    const std::vector<Slic3r::MultiNozzleUtils::NozzleInfo>& nozzle_list)
{
    std::vector<int> filament_nozzle_idx_map(filament_maps.size(), -1);
    std::vector<bool> used_nozzle(nozzle_list.size(), false);

    for (size_t i = 0; i < filament_maps.size(); ++i) {
        int target_ext = filament_maps[i] - 1;
        int filament_vtype = (i < volume_map.size()) ? volume_map[i] : 0;
        int assigned_idx = -1;

        // Pass 1: match BOTH extruder_id AND volume_type
        for (size_t ni = 0; ni < nozzle_list.size(); ++ni) {
            if (!used_nozzle[ni] &&
                nozzle_list[ni].extruder_id == target_ext &&
                nozzle_list[ni].volume_type == filament_vtype) {
                assigned_idx = (int)ni;
                used_nozzle[ni] = true;
                break;
            }
        }

        // Pass 2: any unused slot on same extruder
        if (assigned_idx == -1) {
            for (size_t ni = 0; ni < nozzle_list.size(); ++ni) {
                if (!used_nozzle[ni] && nozzle_list[ni].extruder_id == target_ext) {
                    assigned_idx = (int)ni;
                    used_nozzle[ni] = true;
                    break;
                }
            }
        }

        // Pass 3: overflow — share slot matching volume_type
        if (assigned_idx == -1) {
            for (size_t ni = 0; ni < nozzle_list.size(); ++ni) {
                if (nozzle_list[ni].extruder_id == target_ext &&
                    nozzle_list[ni].volume_type == filament_vtype) {
                    assigned_idx = (int)ni;
                    break;
                }
            }
        }

        // Pass 4: last resort — any slot on same extruder
        if (assigned_idx == -1) {
            for (size_t ni = 0; ni < nozzle_list.size(); ++ni) {
                if (nozzle_list[ni].extruder_id == target_ext) {
                    assigned_idx = (int)ni;
                    break;
                }
            }
        }

        filament_nozzle_idx_map[i] = assigned_idx;
        VORTEK_LOG(warn, "  filament[" << i << "] ext=" << target_ext
            << " vtype=" << filament_vtype
            << " -> nozzle_idx=" << assigned_idx
            << " (nozzle_vtype=" << (assigned_idx >= 0 ? nozzle_list[assigned_idx].volume_type : -1) << ")");
    }

    return filament_nozzle_idx_map;
}

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

    auto nozzle_result = Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult::create_from_config(
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

// Reference to BBS: BambuStudio/src/libslic3r/GCode/ToolOrdering.cpp L2708-2719
bool GroupReorder::ensure_nozzle_group_result(
    Slic3r::Print* print,
    const Slic3r::DynamicPrintConfig& config,
    const std::vector<int>& filament_maps,
    int map_mode)
{
    if (!print) return false;

    std::vector<unsigned int> used_filaments = print->get_layered_nozzle_group_result() 
        ? print->get_layered_nozzle_group_result()->get_used_filaments()
        : std::vector<unsigned int>();

    if (used_filaments.empty()) {
        for (size_t i = 0; i < filament_maps.size(); ++i) {
            used_filaments.push_back(i);
        }
    }

    auto nozzle_stats = Slic3r::MultiNozzleUtils::get_extruder_nozzle_stats(
        config.option<Slic3r::ConfigOptionStrings>(Vortek::Keys::k_extruder_nozzle_stats)->values);
    float nozzle_dia = config.option<Slic3r::ConfigOptionFloats>("nozzle_diameter")->values.empty()
        ? 0.4f : (float)config.option<Slic3r::ConfigOptionFloats>("nozzle_diameter")->values.front();

    std::shared_ptr<Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult> nozzle_result;

    if (map_mode == Slic3r::fmmNozzleManual) {
        auto manual_filament_map = config.option<Slic3r::ConfigOptionInts>("filament_map")->values;
        std::transform(manual_filament_map.begin(), manual_filament_map.end(), manual_filament_map.begin(), [](int v) { return v - 1; });

        auto res = Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult::create_from_config(
            used_filaments,
            manual_filament_map,
            config.option<Slic3r::ConfigOptionInts>(Vortek::Keys::k_filament_volume_map)->values,
            config.option<Slic3r::ConfigOptionInts>(Vortek::Keys::k_filament_nozzle_map)->values,
            nozzle_stats,
            nozzle_dia
        );
        if (res) nozzle_result = std::make_shared<Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult>(*res);
    } else {
        // For fmmManual / fmmAuto: check if user has an explicit filament_volume_map
        // (any filament set to HF, i.e. value != 0). If so, respect it exactly like
        // fmmNozzleManual — the user dragged a filament to an HF nozzle in the dialog.
        // Without this, slicing always overwrites the assignment with auto-assignment
        // from nozzle_stats, reverting HF → Standard on every reslice.
        // Reference: VortekDeviceHooks.cpp save_filament_volume_maps_hook
        auto* vm_opt = config.option<Slic3r::ConfigOptionInts>(Vortek::Keys::k_filament_volume_map);
        bool has_user_volume_map = false;
        if (vm_opt && !vm_opt->values.empty() && vm_opt->values.size() == filament_maps.size()) {
            for (int v : vm_opt->values) if (v != 0) { has_user_volume_map = true; break; }
        }
        if (has_user_volume_map) {
            // User explicitly assigned filament(s) to HF nozzle.
            // RECOMPUTE nozzle slot assignment with volume_type-aware matching.
            // Reference to BBS: BambuStudio/src/libslic3r/FilamentGroup.cpp:1723
            VORTEK_LOG(warn, "ensure_nozzle_group_result: user volume_map set — recomputing nozzle slots with volume_type matching");

            auto nozzle_list = build_nozzle_list_from_stats(nozzle_stats, config);
            auto filament_nozzle_idx_map = assign_filaments_to_nozzles_volume_aware(
                filament_maps, vm_opt->values, nozzle_list);

            auto res = Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult::create_from_index_map(
                filament_nozzle_idx_map, nozzle_list, used_filaments);
            if (res) nozzle_result = std::make_shared<Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult>(*res);
        }
        if (!has_user_volume_map) {
            // Auto-mode: no user volume_map, all filaments treated as Standard (vtype=0)
            auto nozzle_list = build_nozzle_list_from_stats(nozzle_stats, config);
            std::vector<int> zero_volume_map(filament_maps.size(), 0);
            auto filament_nozzle_idx_map = assign_filaments_to_nozzles_volume_aware(
                filament_maps, zero_volume_map, nozzle_list);

            auto res = Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult::create_from_index_map(
                filament_nozzle_idx_map, nozzle_list, used_filaments);
            if (res) nozzle_result = std::make_shared<Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult>(*res);
        } // end if (!has_user_volume_map)
    }

    if (nozzle_result) {
        print->set_nozzle_group_result(nozzle_result);
        VORTEK_LOG(warn, "ensure_nozzle_group_result: created and set on Print"
            << " dynamic_nozzle_map=" << nozzle_result->is_support_dynamic_nozzle_map());
        return true;
    } else {
        VORTEK_LOG(warn, "ensure_nozzle_group_result: failed to create LayeredNozzleGroupResult");
        return false;
    }
}



// Reference to BBS: BambuStudio PR#1 / commit 284ae6e2a5 — ToolOrdering.cpp sort_and_build_data fmmManual branch
void GroupReorder::handle_manual_mode_reorder(
    Slic3r::Print* print,
    const std::vector<int>& filament_maps)
{
    if (!print || !is_h2c_printer(*print)) {
        return;
    }

    const bool uses_carousel = std::any_of(filament_maps.begin(), filament_maps.end(),
                                            [](int m) { return m == 2; });
    if (!uses_carousel) {
        VORTEK_LOG(warn, "handle_manual_mode_reorder: all filaments on Left nozzle, skipping carousel init");
        return;
    }

    // Read the user's volume map from m_full_print_config (not m_config).
    // m_full_print_config is updated by apply_h2c_variant_overrides → ensure_nozzle_group_result
    // BEFORE handle_manual_mode_reorder is called, and already reflects the plate_config value
    // written by sync_machine_nozzle_inventory_to_preset (e.g. [1,0,0,0,0] after HF inventory sync).
    // m_config (print->config()) gets updated only at end of update_filament_maps_to_config Step 4,
    // which runs AFTER this call — reading it here would always return the stale previous value.
    // Reference to BBS: BambuStudio/src/libslic3r/Format/bbs_3mf.cpp – filament_volume_map read
    std::vector<int> saved_volume_maps;
    if (auto* opt = print->full_print_config().option<Slic3r::ConfigOptionInts>(Vortek::Keys::k_filament_volume_map))
        saved_volume_maps = opt->values;
    {
        std::string vm_str;
        for (int v : saved_volume_maps) vm_str += std::to_string(v) + ",";
        VORTEK_LOG(warn, "handle_manual_mode_reorder: carousel used in Manual mode"
                   << " — saved_volume_maps=[" << vm_str << "]"
                   << (saved_volume_maps.empty() ? " (empty → auto-assign will run)" : " (user map → no auto-assign)"));
    }

    print->update_filament_maps_to_config(
        filament_maps,
        saved_volume_maps,
        std::vector<int>());

    ensure_nozzle_group_result(print, print->full_print_config(), filament_maps, Slic3r::fmmManual);
}

} // namespace Vortek
