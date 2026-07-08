#include "VortekPlateMapping.hpp"
#include "VortekConfigSync.hpp"
#include "VortekLog.hpp"
#include "PresetBundle.hpp"
#include "VortekPrintHooks.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include <set>
#include <string>

namespace Vortek {

bool PlateMapping::is_h2c_multi_nozzle(const Slic3r::Print* print)
{
    if (!print) return false;
    return is_h2c_printer(*print);
}

void PlateMapping::sync_after_slicing(
    Slic3r::DynamicPrintConfig& plate_config,
    Slic3r::FilamentMapMode filament_map_mode,
    const Slic3r::Print* print,
    Slic3r::PresetBundle& preset_bundle
)
{
    if (!print) return;
    auto group_result = print->get_layered_nozzle_group_result();
    if (!group_result) return;

    auto nozzle_map = group_result->get_nozzle_map(-1);
    auto volume_map = group_result->get_volume_map(-1);

    // Retrieve derived maps from print's full config (computed by silent_update_derived_maps)
    const auto& full_cfg = print->full_print_config();
    std::vector<int> map_2;
    std::vector<int> phys_map;
    if (auto* opt = full_cfg.option<Slic3r::ConfigOptionInts>(Vortek::Keys::k_filament_map_2))
        map_2 = opt->values;
    if (auto* opt = full_cfg.option<Slic3r::ConfigOptionInts>(Vortek::Keys::k_physical_extruder_map))
        phys_map = opt->values;

    // CRITICAL: Sync ALL computed maps to preset_bundle.project_config.
    // full_config() is built from project_config (NOT plate_config), so without this
    // the next Print::apply() gets stale defaults → config drift → double-slicing.
    auto& proj = preset_bundle.project_config;
    proj.set_key_value(Vortek::Keys::k_filament_nozzle_map, new Slic3r::ConfigOptionInts(nozzle_map));
    proj.set_key_value(Vortek::Keys::k_filament_volume_map, new Slic3r::ConfigOptionInts(volume_map));
    if (!map_2.empty())
        proj.set_key_value(Vortek::Keys::k_filament_map_2, new Slic3r::ConfigOptionInts(map_2));
    if (!phys_map.empty())
        proj.set_key_value(Vortek::Keys::k_physical_extruder_map, new Slic3r::ConfigOptionInts(phys_map));

    if (filament_map_mode != Slic3r::fmmManual && filament_map_mode != Slic3r::fmmNozzleManual) {
        VORTEK_LOG(warn, "sync_after_slicing: auto mode — synced project_config only (plate_config skipped)");
        return;
    }

    VORTEK_LOG(warn, "sync_after_slicing: updating nozzle maps in plate config and project config");

    bool changed = false;
    auto* opt_nozzle = plate_config.option<Slic3r::ConfigOptionInts>(Vortek::Keys::k_filament_nozzle_map);
    if (!opt_nozzle || opt_nozzle->values != nozzle_map) {
        plate_config.set_key_value(Vortek::Keys::k_filament_nozzle_map, new Slic3r::ConfigOptionInts(nozzle_map));
        changed = true;
    }
    auto* opt_volume = plate_config.option<Slic3r::ConfigOptionInts>(Vortek::Keys::k_filament_volume_map);
    if (!opt_volume || opt_volume->values != volume_map) {
        plate_config.set_key_value(Vortek::Keys::k_filament_volume_map, new Slic3r::ConfigOptionInts(volume_map));
        changed = true;
    }

    if (changed) {
        VORTEK_LOG(warn, "sync_after_slicing: nozzle maps changed, updated plate + project config");
    } else {
        VORTEK_LOG(warn, "sync_after_slicing: nozzle maps unchanged in plate config, synced to project config");
    }
}

void PlateMapping::handle_filament_count_changed(Slic3r::DynamicPrintConfig* config, int filament_count)
{
    if (!config) return;
    VORTEK_LOG(debug, "handle_filament_count_changed: resizing maps to " << filament_count);
    if (config->has(Vortek::Keys::k_filament_nozzle_map)) {
        config->option<Slic3r::ConfigOptionInts>(Vortek::Keys::k_filament_nozzle_map)->values.resize(filament_count, 1);
    }
    if (config->has(Vortek::Keys::k_filament_volume_map)) {
        config->option<Slic3r::ConfigOptionInts>(Vortek::Keys::k_filament_volume_map)->values.resize(filament_count, 1);
    }
}

void PlateMapping::handle_filament_added(Slic3r::DynamicPrintConfig* config)
{
    if (!config) return;
    VORTEK_LOG(debug, "handle_filament_added: appending default mapping values");
    if (config->has(Vortek::Keys::k_filament_nozzle_map)) {
        config->option<Slic3r::ConfigOptionInts>(Vortek::Keys::k_filament_nozzle_map)->values.push_back(1);
    }
    if (config->has(Vortek::Keys::k_filament_volume_map)) {
        config->option<Slic3r::ConfigOptionInts>(Vortek::Keys::k_filament_volume_map)->values.push_back(1);
    }
}

void PlateMapping::handle_filament_deleted(Slic3r::DynamicPrintConfig* config, int filament_id)
{
    if (!config) return;
    VORTEK_LOG(debug, "handle_filament_deleted: erasing mapping at index " << filament_id);
    if (config->has(Vortek::Keys::k_filament_nozzle_map)) {
        auto& vals = config->option<Slic3r::ConfigOptionInts>(Vortek::Keys::k_filament_nozzle_map)->values;
        if (filament_id >= 0 && filament_id < (int)vals.size())
            vals.erase(vals.begin() + filament_id);
    }
    if (config->has(Vortek::Keys::k_filament_volume_map)) {
        auto& vals = config->option<Slic3r::ConfigOptionInts>(Vortek::Keys::k_filament_volume_map)->values;
        if (filament_id >= 0 && filament_id < (int)vals.size())
            vals.erase(vals.begin() + filament_id);
    }
}

void PlateMapping::clear_mappings(Slic3r::DynamicPrintConfig* config)
{
    if (!config) return;
    VORTEK_LOG(debug, "clear_mappings: clearing all nozzle/volume mappings");
    if (config->has(Vortek::Keys::k_filament_nozzle_map)) {
        config->option<Slic3r::ConfigOptionInts>(Vortek::Keys::k_filament_nozzle_map)->values.clear();
    }
    if (config->has(Vortek::Keys::k_filament_volume_map)) {
        config->option<Slic3r::ConfigOptionInts>(Vortek::Keys::k_filament_volume_map)->values.clear();
    }
}



void PlateMapping::sync_project_config_on_load(Slic3r::DynamicConfig& proj_cfg, int filament_count)
{
    if (!is_h2c_printer(proj_cfg)) return;
    VORTEK_LOG(warn, "sync_project_config_on_load: verifying loaded map sizes");
    
    // Сброс MQTT-зависимых флагов, которые должны приходить с принтера, а не считываться из 3MF
    if (auto* p = proj_cfg.option<Slic3r::ConfigOptionBool>(Vortek::Keys::k_has_filament_switcher))
        p->value = false;
    if (auto* p = proj_cfg.option<Slic3r::ConfigOptionBool>(Vortek::Keys::k_enable_filament_dynamic_map))
        p->value = false;

    // Синхронизация filament_nozzle_map (дефолт 0 — первое сопло)
    auto* nozzle_map = proj_cfg.opt<Slic3r::ConfigOptionInts>(Vortek::Keys::k_filament_nozzle_map, true);
    if ((int)nozzle_map->values.size() != filament_count) {
        nozzle_map->values.resize(filament_count, 0);
    }

    // Синхронизация filament_volume_map (дефолт 1 — стандартный объем / nvtStandard)
    auto* volume_map = proj_cfg.opt<Slic3r::ConfigOptionInts>(Vortek::Keys::k_filament_volume_map, true);
    if ((int)volume_map->values.size() != filament_count) {
        volume_map->values.resize(filament_count, 1);
    }
}



void PlateMapping::patch_slice_filament_nozzle_groups(
    Slic3r::PlateData* plate_data,
    const std::vector<int>& filament_nozzle_map
)
{
    // Called AFTER parse_filament_info() which populates slice_filaments_info but
    // leaves group_id empty. Without group_id, bbs_3mf.cpp falls back to
    // f_maps[i]-1 (0-based extruder index) — wrong for H2C carousel.
    //
    // BBS's FilamentInfo::group_id = nozzle slot ID (0=Left, 1-3=Right carousel)
    // This makes bbs_3mf.cpp write correct <filament group_id="N"> and
    // generates correct 4-nozzle <nozzle> list in slice_info.config.
    if (!plate_data || filament_nozzle_map.empty()) return;

    int patched = 0;
    for (auto& fi : plate_data->slice_filaments_info) {
        if (fi.id >= 0 && fi.id < (int)filament_nozzle_map.size()) {
            fi.group_id = {filament_nozzle_map[fi.id]};
            ++patched;
        }
    }
    VORTEK_LOG(warn, "patch_slice_filament_nozzle_groups: patched " << patched
                     << " filaments with nozzle group_ids from filament_nozzle_map");
}

std::vector<int> PlateMapping::get_nozzle_map_for_export(const Slic3r::Print* print, const Slic3r::DynamicPrintConfig& plate_config)
{
    // Reference to BBS: BambuStudio/src/libslic3r/Format/bbs_3mf.cpp – filament_nozzle_map export
    // Priority: plate_config is set by BackgroundSlicingProcess::process() AFTER slicing via
    // set_filament_nozzle_maps() with the correct multi-element array.
    // print->full_print_config() may have been reset by a subsequent Print::apply() call.
    // So: prefer plate_config if it has >1 element (post-slice initialized), else fall back to print.
    if (plate_config.has(Vortek::Keys::k_filament_nozzle_map)) {
        auto* plate_opt = plate_config.option<Slic3r::ConfigOptionInts>(Vortek::Keys::k_filament_nozzle_map);
        if (plate_opt && plate_opt->values.size() > 1) {
            VORTEK_LOG(warn, "get_nozzle_map_for_export: using plate_config nozzle map (" << plate_opt->values.size() << " elements)");
            return plate_opt->values;
        }
    }
    // Fallback: try print->full_print_config() (valid immediately after slicing)
    if (print && plate_config.has(Vortek::Keys::k_filament_nozzle_map)) {
        const auto& full_cfg = print->full_print_config();
        if (auto* opt = full_cfg.option<Slic3r::ConfigOptionInts>(Vortek::Keys::k_filament_nozzle_map)) {
            if (opt->values.size() > 1) {
                VORTEK_LOG(warn, "get_nozzle_map_for_export: using print-derived nozzle map (" << opt->values.size() << " elements)");
                return opt->values;
            }
        }
    }
    if (plate_config.has(Vortek::Keys::k_filament_nozzle_map)) {
        return plate_config.option<Slic3r::ConfigOptionInts>(Vortek::Keys::k_filament_nozzle_map)->values;
    }
    return {};
}

std::vector<int> PlateMapping::get_volume_map_for_export(const Slic3r::Print* print, const Slic3r::DynamicPrintConfig& plate_config)
{
    // Reference to BBS: BambuStudio/src/libslic3r/Format/bbs_3mf.cpp – filament_volume_map export
    //
    // Priority order:
    //   1. plate_config (PartPlate::m_config) — set by BackgroundSlicingProcess after slice
    //      via set_filament_volume_maps(). Always a correctly sized full array.
    //      NOTE: condition was previously `size() > 1` which incorrectly skipped single-filament
    //      plates. Changed to `!empty()` — a 1-element map is a valid single-filament assignment.
    //   2. print->full_print_config() — valid immediately after slicing, before a subsequent
    //      Print::apply() call may have reset it.
    //   3. Empty — no volume map available, caller handles the absence.
    if (plate_config.has(Vortek::Keys::k_filament_volume_map)) {
        auto* plate_opt = plate_config.option<Slic3r::ConfigOptionInts>(Vortek::Keys::k_filament_volume_map);
        if (plate_opt && !plate_opt->values.empty()) {
            VORTEK_LOG(warn, "get_volume_map_for_export: using plate_config volume map ("
                       << plate_opt->values.size() << " elements)");
            return plate_opt->values;
        }
    }
    // Fallback: try print->full_print_config() (valid immediately after slicing)
    if (print) {
        const auto& full_cfg = print->full_print_config();
        if (auto* opt = full_cfg.option<Slic3r::ConfigOptionInts>(Vortek::Keys::k_filament_volume_map)) {
            if (!opt->values.empty()) {
                VORTEK_LOG(warn, "get_volume_map_for_export: plate_config empty, using print-derived volume map ("
                           << opt->values.size() << " elements)");
                return opt->values;
            }
        }
    }
    VORTEK_LOG(warn, "get_volume_map_for_export: no volume map found, returning empty");
    return {};
}

void PlateMapping::patch_plate_data_for_export(
    Slic3r::PlateData* plate_data,
    const std::vector<int>& filament_nozzle_map,
    const std::vector<int>& filament_volume_map,
    const std::vector<int>& filament_maps,
    const Slic3r::DynamicPrintConfig& config,
    const Slic3r::Print* print
)
{
    if (!plate_data) return;
    if (print && !is_h2c_printer(*print)) return;
    if (!print && !config.has(Vortek::Keys::k_filament_nozzle_map)) return;

    VORTEK_LOG(warn, "patch_plate_data_for_export for plate index " << plate_data->plate_index);

    std::vector<int> nozzle_map = filament_nozzle_map;
    std::vector<int> volume_map = filament_volume_map;

    if (print) {
        nozzle_map = get_nozzle_map_for_export(print, config);
        volume_map = get_volume_map_for_export(print, config);
    }

    plate_data->config.set_key_value(Vortek::Keys::k_filament_nozzle_map, new Slic3r::ConfigOptionInts(nozzle_map));
    plate_data->config.set_key_value(Vortek::Keys::k_filament_volume_map, new Slic3r::ConfigOptionInts(volume_map));
}









bool PlateMapping::are_models_compatible(const std::string& model1, const std::string& model2)
{
    if (model1 == model2) return true;
    if ((model1 == "O1C" && model2 == "O1C2") || (model1 == "O1C2" && model2 == "O1C")) {
        VORTEK_LOG(debug, "are_models_compatible: matched H2C fallback compatibility for model1: " + model1 + " and model2: " + model2);
        return true;
    }
    return false;
}

void PlateMapping::filter_full_config_diff(Slic3r::t_config_option_keys& full_config_diff, const Slic3r::PrintConfig& config)
{
    if (full_config_diff.empty()) return;
    if (!is_h2c_printer(config)) return;

    size_t suppressed = Vortek::ConfigSync::filter_managed_keys(full_config_diff);
    if (suppressed > 0) {
        VORTEK_LOG(warn, "filter_full_config_diff: suppressed " << suppressed
                         << " variant-transformed keys from full_config_diff");
    }
}

void PlateMapping::filter_print_diff_set(
    std::unordered_set<std::string>& print_diff_set,
    const Slic3r::PrintConfig& config,
    Slic3r::DynamicPrintConfig& full_print_config,
    const Slic3r::DynamicPrintConfig& new_full_config)
{
    if (print_diff_set.empty()) return;

    // Only for Vortek H2C — P2S/H2D and standard printers must NOT be affected.
    if (!is_h2c_printer(config)) return;

    // Data-driven: suppress all computed keys from print_diff.
    // Computed keys are injected by Vortek hooks during slice (not from GUI),
    // so any diff for them is expected and should not trigger reslice.
    for (const auto& kd : Vortek::Keys::registry()) {
        if (kd.computed && print_diff_set.erase(kd.name) > 0) {
            // Sync value in full_print_config to match new_full_config,
            // so next Print::apply won't see this key in diff again
            if (new_full_config.has(kd.name)) {
                auto* new_opt = new_full_config.option(kd.name);
                if (new_opt) {
                    full_print_config.set_key_value(kd.name, new_opt->clone());
                }
            }
            VORTEK_LOG(warn, "filter_print_diff_set: suppressed computed key '" << kd.name << "'");
        }
    }
}

void PlateMapping::filter_reslice_diffs(
    const Slic3r::Print& print,
    const Slic3r::ConfigBase& new_full_config,
    Slic3r::t_config_option_keys& print_diff,
    Slic3r::t_config_option_keys& full_config_diff)
{
    if (!is_h2c_printer(print.config()))
        return;

    auto erase_key = [](Slic3r::t_config_option_keys& keys, const std::string& key) {
        keys.erase(std::remove(keys.begin(), keys.end(), key), keys.end());
    };
    // Data-driven: suppress all computed keys from diffs.
    // Computed keys are injected by Vortek hooks, not from GUI.
    for (const auto& kd : Vortek::Keys::registry()) {
        if (kd.computed) {
            erase_key(print_diff, kd.name);
            erase_key(full_config_diff, kd.name);
        }
    }

    // H2C Hybrid: suppress extruder_nozzle_stats diff when only std_count changes.
    // The std count fluctuates 3↔4 as slot-6 "In Use" nozzle appears/disappears across
    // firmware pings. This is harmless — HF count drives slot assignment, std count does not.
    // Suppress if: for every extruder, hf_count is unchanged (even if std_count differs).
    // Also suppress Standard#N → Standard#M|HighFlow#K (offline→online first connect):
    // that diff is structural and always happens once — machine sync handles the real update.
    // Reference: VortekDeviceHooks.cpp sync_machine_nozzle_inventory_to_preset HF-only trigger.
    auto suppress_nozzle_stats_if_hf_stable = [&](Slic3r::t_config_option_keys& diff_keys,
                                                   const Slic3r::ConfigBase& old_cfg)
    {
        static const std::string key = Vortek::Keys::k_extruder_nozzle_stats;
        auto it = std::find(diff_keys.begin(), diff_keys.end(), key);
        if (it == diff_keys.end()) return;

        const auto* opt_old = old_cfg.option<Slic3r::ConfigOptionStrings>(key);
        const auto* opt_new = new_full_config.option<Slic3r::ConfigOptionStrings>(key);
        if (!opt_old || !opt_new || opt_old->size() != opt_new->size()) return;

        // Parse "Type#count|Type#count" strings into {type → count} maps.
        // Types: "Standard"=0, "High Flow"=1, "Hybrid"=2
        auto parse_stats = [](const std::string& s) -> std::map<std::string, int> {
            std::map<std::string, int> result;
            std::istringstream ss(s);
            std::string token;
            while (std::getline(ss, token, '|')) {
                auto pos = token.rfind('#');
                if (pos == std::string::npos) continue;
                std::string type = token.substr(0, pos);
                int count = 0;
                try { count = std::stoi(token.substr(pos + 1)); } catch (...) {}
                result[type] += count;
            }
            return result;
        };

        bool hf_stable = true;
        for (size_t eid = 0; eid < opt_old->size(); ++eid) {
            auto old_map = parse_stats(opt_old->values[eid]);
            auto new_map = parse_stats(opt_new->values[eid]);
            int old_hf = old_map.count("High Flow") ? old_map.at("High Flow") : 0;
            int new_hf = new_map.count("High Flow") ? new_map.at("High Flow") : 0;
            if (old_hf != new_hf) { hf_stable = false; break; }
        }
        if (hf_stable) {
            VORTEK_LOG(warn, "filter_reslice_diffs: suppressing extruder_nozzle_stats diff"
                             " (HF count stable, std-only change)");
            diff_keys.erase(it);
        }
    };
    suppress_nozzle_stats_if_hf_stable(print_diff, print.config());
    suppress_nozzle_stats_if_hf_stable(full_config_diff, print.full_print_config());

    // Erase keys whose only difference is vector size expansion where elements are equal up to the smaller size
    // Reference to BBS: BambuStudio/src/libslic3r/PrintApply.cpp L1445-1463
    auto filter_vector_size_diffs = [&](Slic3r::t_config_option_keys& diff_keys, const Slic3r::ConfigBase& old_cfg) {
        auto it = diff_keys.begin();
        while (it != diff_keys.end()) {
            const std::string& key = *it;
            const Slic3r::ConfigOption* opt_old = old_cfg.option(key);
            const Slic3r::ConfigOption* opt_new = new_full_config.option(key);
            if (opt_old && opt_new) {
                const auto* old_vec = dynamic_cast<const Slic3r::ConfigOptionVectorBase*>(opt_old);
                const auto* new_vec = dynamic_cast<const Slic3r::ConfigOptionVectorBase*>(opt_new);
                if (old_vec && new_vec) {
                    size_t size_old = old_vec->size();
                    size_t size_new = new_vec->size();
                    if (size_old != size_new) {
                        std::vector<std::string> vals_old = old_vec->vserialize();
                        std::vector<std::string> vals_new = new_vec->vserialize();
                        size_t min_size = std::min(size_old, size_new);
                        bool elements_equal = true;
                        for (size_t i = 0; i < min_size; ++i) {
                            if (vals_old[i] != vals_new[i]) {
                                elements_equal = false;
                                break;
                            }
                        }
                        if (elements_equal) {
                            VORTEK_LOG(warn, "filter_reslice_diffs: suppressing vector size difference for key: " << key
                                             << " (old_size=" << size_old << ", new_size=" << size_new << ")");
                            it = diff_keys.erase(it);
                            continue;
                        }
                    }
                }
            }
            ++it;
        }
    };

    filter_vector_size_diffs(print_diff, print.config());
    filter_vector_size_diffs(full_config_diff, print.full_print_config());

    // [Vortek DIAG] Log what keys SURVIVED all filters — these will trigger re-slice
    auto log_survivors = [](const Slic3r::t_config_option_keys& diff_keys, const char* label,
                            const Slic3r::ConfigBase& old_cfg, const Slic3r::ConfigBase& new_cfg) {
        if (diff_keys.empty()) return;
        for (const auto& key : diff_keys) {
            const Slic3r::ConfigOption* opt_old = old_cfg.option(key);
            const Slic3r::ConfigOption* opt_new = new_cfg.option(key);
            std::string old_val = opt_old ? opt_old->serialize() : "<missing>";
            std::string new_val = opt_new ? opt_new->serialize() : "<missing>";
            size_t old_sz = 0, new_sz = 0;
            if (const auto* v = dynamic_cast<const Slic3r::ConfigOptionVectorBase*>(opt_old)) old_sz = v->size();
            if (const auto* v = dynamic_cast<const Slic3r::ConfigOptionVectorBase*>(opt_new)) new_sz = v->size();
            if (old_val.size() > 80) old_val = old_val.substr(0, 80) + "...";
            if (new_val.size() > 80) new_val = new_val.substr(0, 80) + "...";
            VORTEK_LOG(warn, "RESLICE_TRIGGER [" << label << "] key='" << key
                << "' old_size=" << old_sz << " new_size=" << new_sz
                << " old='" << old_val << "' new='" << new_val << "'");
        }
    };
    log_survivors(print_diff,       "print_diff",       print.config(),            new_full_config);
    log_survivors(full_config_diff, "full_config_diff",  print.full_print_config(), new_full_config);
}

// [Vortek DIAG] Log config diff keys and their old/new values for re-slice debugging.
// No-op for non-H2C printers.
void PlateMapping::diag_log_config_diffs(
    const char* label,
    const Slic3r::t_config_option_keys& diff_keys,
    const Slic3r::ConfigBase& old_cfg,
    const Slic3r::ConfigBase& new_cfg)
{
    if (!is_h2c_printer(old_cfg))
        return;
    if (diff_keys.empty()) return;
    std::string keys_str;
    for (const auto& k : diff_keys) keys_str += k + " ";
    VORTEK_LOG(warn, "DIAG " << label << " keys (" << diff_keys.size() << "): " << keys_str);
    for (const auto& k : diff_keys) {
        const Slic3r::ConfigOption* old_opt = old_cfg.option(k);
        const Slic3r::ConfigOption* new_opt = new_cfg.option(k);
        std::string old_val = old_opt ? old_opt->serialize() : "<missing>";
        std::string new_val = new_opt ? new_opt->serialize() : "<missing>";
        // Truncate long values for readability
        if (old_val.size() > 120) old_val = old_val.substr(0, 120) + "...";
        if (new_val.size() > 120) new_val = new_val.substr(0, 120) + "...";
        VORTEK_LOG(warn, "DIAG   " << label << " [" << k << "] old=" << old_val << " new=" << new_val);
    }
}

void PlateMapping::restore_filament_variant_overrides_h2c(
    Slic3r::Print& print,
    Slic3r::DynamicPrintConfig& new_full_config
)
{
    if (!is_h2c_printer(new_full_config)) {
        return;
    }

    // If align_incoming_config already ran (nozzle_group_result exists),
    // skip — align already wrote correct H2C-expanded values.
    auto group_result = print.get_nozzle_group_result();
    if (group_result) {
        return;
    }

    // First apply: do NOT call restore_variants.
    // GUI's full_fff_config() already correctly expands filament variant keys
    // to size = N_filaments × N_nozzle_types (e.g. 6 × 2 = 12).
    // restore_variants would overwrite these with pre-expansion values from
    // m_ori_full_print_config (size=6), causing persistent diff → reslice.
    VORTEK_LOG(warn, "restore_filament_variant_overrides_h2c: first apply, "
        "keeping GUI variant expansion as-is (no restore_variants)");
}

// [Vortek] Override upstream filament variant expansion with BBS-style nozzle_group_result mapping.
// Reference to BBS: BambuStudio/src/libslic3r/PrintApply.cpp L1338-1362
void PlateMapping::override_filament_variant_expansion(
    Slic3r::Print& print,
    Slic3r::DynamicPrintConfig& new_full_config,
    const Slic3r::DynamicPrintConfig& ori_full_config)
{
    if (!is_h2c_printer(new_full_config)) {
        return;
    }

    // No nozzle_group_result on first apply — nothing to override yet
    auto group_result = print.get_nozzle_group_result();
    if (!group_result) {
        VORTEK_LOG(warn, "override_filament_variant_expansion: no nozzle_group_result yet (first apply), skipping");
        return;
    }

    // Delegate to orchestrator: m_full_print_config → new_full_config
    Vortek::ConfigSync sync(print);
    sync.align_incoming_config(new_full_config);
}

} // namespace Vortek
