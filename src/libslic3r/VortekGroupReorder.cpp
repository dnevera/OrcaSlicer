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
        config.option<Slic3r::ConfigOptionStrings>("extruder_nozzle_stats")->values);
    float nozzle_dia = config.option<Slic3r::ConfigOptionFloats>("nozzle_diameter")->values.empty()
        ? 0.4f : (float)config.option<Slic3r::ConfigOptionFloats>("nozzle_diameter")->values.front();

    std::shared_ptr<Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult> nozzle_result;

    if (map_mode == Slic3r::fmmNozzleManual) {
        auto manual_filament_map = config.option<Slic3r::ConfigOptionInts>("filament_map")->values;
        std::transform(manual_filament_map.begin(), manual_filament_map.end(), manual_filament_map.begin(), [](int v) { return v - 1; });

        auto res = Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult::create(
            used_filaments,
            manual_filament_map,
            config.option<Slic3r::ConfigOptionInts>("filament_volume_map")->values,
            config.option<Slic3r::ConfigOptionInts>("filament_nozzle_map")->values,
            nozzle_stats,
            nozzle_dia
        );
        if (res) nozzle_result = std::make_shared<Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult>(*res);
    } else {
        std::vector<Slic3r::MultiNozzleUtils::NozzleInfo> nozzle_list;
        int next_carousel_nozzle = 4;
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
                        if (opt && nz.extruder_id < (int)opt->values.size()) {
                            dia = (float)opt->values[nz.extruder_id];
                        }
                    }
                    nz.diameter = Slic3r::MultiNozzleUtils::format_diameter_to_str(dia);

                    if (nz.extruder_id == 0) {
                        nz.group_id = 0;
                    } else {
                        nz.group_id = next_carousel_nozzle--;
                        if (next_carousel_nozzle < 1) next_carousel_nozzle = 4;
                    }
                    nozzle_list.push_back(nz);
                }
            }
        }

        std::vector<int> filament_nozzle_idx_map(filament_maps.size(), -1);
        std::vector<bool> used_nozzle(nozzle_list.size(), false);
        for (size_t i = 0; i < filament_maps.size(); ++i) {
            int target_ext = filament_maps[i] - 1;
            int assigned_idx = -1;
            for (size_t ni = 0; ni < nozzle_list.size(); ++ni) {
                if (!used_nozzle[ni] && nozzle_list[ni].extruder_id == target_ext) {
                    assigned_idx = (int)ni;
                    used_nozzle[ni] = true;
                    break;
                }
            }
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
                << " -> nozzle_idx=" << filament_nozzle_idx_map[i]);
        }

        auto res = Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult::create(
            filament_nozzle_idx_map, nozzle_list, used_filaments);
        if (res) nozzle_result = std::make_shared<Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult>(*res);
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

// Reference to BBS: BambuStudio/src/libslic3r/GCode/ToolOrdering.cpp L2708-2719
void GroupReorder::handle_auto_mode_reorder(
    Slic3r::Print* print,
    const std::vector<int>& filament_maps)
{
    if (!print || !is_h2c_printer(*print)) return;

    VORTEK_LOG(warn, "handle_auto_mode_reorder: initializing nozzle_group_result for auto mode");
    ensure_nozzle_group_result(print, print->full_print_config(), filament_maps, print->config().filament_map_mode.value);
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

    VORTEK_LOG(warn, "handle_manual_mode_reorder: carousel used in Manual mode — initializing nozzle_group_result via update_filament_maps_to_config");

    print->update_filament_maps_to_config(
        filament_maps,
        std::vector<int>(),   
        std::vector<int>());  

    ensure_nozzle_group_result(print, print->full_print_config(), filament_maps, Slic3r::fmmManual);
}

} // namespace Vortek
