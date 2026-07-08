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
    //
    // EXCEPTION: In Manual mode, filament_volume_map is USER-SET (via dialog drag).
    // ToolOrdering computes volume_map based on filament properties, NOT user intent.
    // Overwriting user-set with computed causes oscillation → infinite reslice.
    bool is_manual = (filament_map_mode == Slic3r::fmmManual || filament_map_mode == Slic3r::fmmNozzleManual);

    auto& proj = preset_bundle.project_config;
    proj.set_key_value(Vortek::Keys::k_filament_nozzle_map, new Slic3r::ConfigOptionInts(nozzle_map));
    if (!is_manual) {
        proj.set_key_value(Vortek::Keys::k_filament_volume_map, new Slic3r::ConfigOptionInts(volume_map));
    }
    if (!map_2.empty())
        proj.set_key_value(Vortek::Keys::k_filament_map_2, new Slic3r::ConfigOptionInts(map_2));
    if (!phys_map.empty())
        proj.set_key_value(Vortek::Keys::k_physical_extruder_map, new Slic3r::ConfigOptionInts(phys_map));

    if (!is_manual) {
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
    // NOTE: filament_volume_map is NOT synced to plate_config in Manual mode.
    // It is user-set (via dialog drag) and already in plate_config.
    // Overwriting with ToolOrdering-computed value causes oscillation.

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
    // Determine Manual mode once for the exceptions below.
    bool is_manual_mode = false;
    {
        auto* opt = new_full_config.option<Slic3r::ConfigOptionEnum<Slic3r::FilamentMapMode>>("filament_map_mode");
        if (opt && opt->value == Slic3r::fmmManual)
            is_manual_mode = true;
    }

    // Data-driven: suppress computed keys from print_diff only.
    // BBS NEVER erases mapping keys from full_config_diff (L1394/1402/1410 — commented out).
    // Keeping full_config_diff !empty ensures L1292 block fires → m_full_print_config = new_full_config.
    // Reference to BBS: BambuStudio/src/libslic3r/PrintApply.cpp L1386-1469
    for (const auto& kd : Vortek::Keys::registry()) {
        if (!kd.computed) continue;

        // Manual mode: filament_volume_map is USER-SET → do NOT suppress.
        // User's assignment in FilamentMapDialog must trigger reslice.
        // Plate config is the source of truth (read via get_real_filament_volume_maps → project_config).
        // Reference to BBS: BambuStudio/src/libslic3r/PrintApply.cpp L1417-1465
        if (is_manual_mode && kd.name == Vortek::Keys::k_filament_volume_map) {
            VORTEK_LOG(warn, "filter_reslice_diffs: Manual mode — volume_map NOT suppressed (user-set, triggers reslice)");
            continue;
        }

        // Manual mode: filament_nozzle_map — "not used in gui studio" (BBS L1421).
        // Auto mode: all computed keys — suppress from print_diff to prevent reslice.
        // In both cases: do NOT erase from full_config_diff (BBS pattern).
        erase_key(print_diff, kd.name);
    }

    // H2C: suppress retract keys + filament_options_with_variant from print_diff.
    // override_filament_variant_expansion syncs m_full → new_full for full_config_diff,
    // but print_config_diffs compares m_config vs new_full. m_config may be stale
    // (not yet updated by apply_only), so these diffs are false positives.
    // sync_suppressed_to_config keeps m_config consistent after filtering.
    // Reference to BBS: BambuStudio/src/libslic3r/PrintApply.cpp L1495-1523
    for (const auto& rk : Slic3r::print_config_def.extruder_retract_keys()) {
        erase_key(print_diff, rk);
    }
    for (const auto& key : Slic3r::filament_options_with_variant) {
        erase_key(print_diff, key);
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

// Sync computed keys that were suppressed (erased) from print_diff directly into m_config.
// Without this, m_config.apply_only(new_full_config, print_diff) does NOT update these keys
// (since they were erased from print_diff), leaving m_config stale → ghost diffs on next apply.
// Reproduces BBS pattern: BambuStudio/src/libslic3r/PrintApply.cpp L1398/1406/1414
//   m_config.filament_volume_map = *new_opt;
//   m_config.filament_nozzle_map = *new_opt;
// Reference to BBS: BambuStudio/src/libslic3r/PrintApply.cpp L1386-1415
void PlateMapping::sync_suppressed_to_config(
    Slic3r::PrintConfig& config,
    const Slic3r::DynamicPrintConfig& new_full_config)
{
    if (!is_h2c_printer(config)) return;

    bool is_manual = false;
    if (auto* opt = new_full_config.option<Slic3r::ConfigOptionEnum<Slic3r::FilamentMapMode>>("filament_map_mode"))
        is_manual = (opt->value == Slic3r::fmmManual);

    int synced = 0;

    // Sync computed mapping keys (filament_map, filament_volume_map, etc.)
    // Retract keys and filament_options_with_variant are NOT synced here —
    // override_filament_variant_expansion already aligned them in new_full_config,
    // and apply_only(new_full_config, print_diff) handles m_config update.
    for (const auto& kd : Vortek::Keys::registry()) {
        if (!kd.computed) continue;
        // In Manual mode, volume_map is NOT suppressed — m_config updates via apply_only.
        if (is_manual && kd.name == Vortek::Keys::k_filament_volume_map) continue;
        // Sync suppressed key: new_full_config → m_config
        if (auto* new_opt = new_full_config.option<Slic3r::ConfigOptionInts>(kd.name)) {
            if (auto* cfg_opt = config.option<Slic3r::ConfigOptionInts>(kd.name, true)) {
                cfg_opt->values = new_opt->values;
                ++synced;
            }
        }
    }

    // 2. Sync retract keys: suppressed from print_diff by filter_reslice_diffs.
    //    apply_only won't update m_config for these → must sync from new_full_config.
    //    Reference to BBS: BambuStudio/src/libslic3r/PrintApply.cpp L1523
    for (const auto& rk : Slic3r::print_config_def.extruder_retract_keys()) {
        const auto* src = new_full_config.option(rk);
        if (!src) continue;
        auto* dst = config.option(rk, true);
        if (dst && *dst != *src) {
            dst->set(src);
            ++synced;
        }
    }

    // 3. Sync filament_options_with_variant (suppressed from print_diff)
    for (const auto& key : Slic3r::filament_options_with_variant) {
        const auto* src = new_full_config.option(key);
        if (!src) continue;
        auto* dst = config.option(key, true);
        if (dst && *dst != *src) {
            dst->set(src);
            ++synced;
        }
    }

    VORTEK_LOG(warn, "sync_suppressed_to_config: synced " << synced << " keys to m_config"
        << (is_manual ? " [Manual: volume_map skipped]" : " [Auto]"));
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

    // Strategy: copy variant-expanded keys from m_full_print_config → new_full_config.
    // This makes full_print_config_diffs(m_full_print_config, new_full_config) = 0 for variant keys.
    //
    // Why this works:
    //   - First apply: m_full = base, new_full = base → both base → no diff
    //   - After slicing: m_full = expanded (by VortekPrintHooks), new_full = base
    //     → copy expanded → both expanded → no diff
    //
    // Reference to BBS: BambuStudio/src/libslic3r/PrintApply.cpp L1341-1358

    const auto& m_full = print.full_print_config();

    // 1. Sync Vortek registry keys (mapping + thermal) from m_full → new_full.
    // After slicing, m_full contains stale mapping values that diverge from plate_config.
    // Sync eliminates diffs for nozzle_map, filament_map, etc.
    //
    // EXCEPTION: filament_volume_map in Manual mode arrives correctly via plate_config →
    // get_real_filament_volume_maps → full_config(false, f_maps, f_volume_maps).
    // In Auto mode, volume_map is computed by ToolOrdering → sync from m_full is correct.
    bool is_manual = false;
    if (auto* opt = new_full_config.option<Slic3r::ConfigOptionEnum<Slic3r::FilamentMapMode>>("filament_map_mode"))
        is_manual = (opt->value == Slic3r::fmmManual);

    std::vector<int> plate_volume_map;
    if (is_manual) {
        if (auto* opt = new_full_config.option<Slic3r::ConfigOptionInts>(Vortek::Keys::k_filament_volume_map))
            plate_volume_map = opt->values;
    }

    Vortek::ConfigSync sync(print);
    int registry_copied = sync.sync_keys_between(m_full, new_full_config, &Keys::KeyDef::sync_baseline);

    // Manual mode: restore plate-level volume_map (user-set, not computed)
    if (is_manual && !plate_volume_map.empty()) {
        new_full_config.set_key_value(Vortek::Keys::k_filament_volume_map,
            new Slic3r::ConfigOptionInts(plate_volume_map));
    }

    // 2. Sync filament_options_with_variant (BBS base layer: retract, thermal, flow, etc.)
    // ONLY when volume_map is unchanged. When volume_map changed (user drag in Manual mode
    // or ToolOrdering recomputed in Auto), upstream expansion (L1178 PrintApply) already
    // re-expanded per-filament speeds with the new volume_map. Syncing from m_full would
    // overwrite correct HF speeds with old Std values.
    bool volume_map_changed = false;
    {
        auto* opt_mfull = m_full.option<Slic3r::ConfigOptionInts>(Vortek::Keys::k_filament_volume_map);
        auto* opt_new = new_full_config.option<Slic3r::ConfigOptionInts>(Vortek::Keys::k_filament_volume_map);
        if (opt_mfull && opt_new && opt_mfull->values != opt_new->values)
            volume_map_changed = true;
    }

    int variant_copied = 0;
    if (!volume_map_changed) {
        for (const auto& key : Slic3r::filament_options_with_variant) {
            const auto* src = m_full.option(key);
            if (!src) continue;
            auto* dst = new_full_config.option(key, true);
            if (dst && *dst != *src) {
                dst->set(src);
                ++variant_copied;
            }
        }
    }

    // 3. Sync printer-level retract keys (retraction_length, z_hop, wipe_distance, etc.)
    for (const auto& rk : Slic3r::print_config_def.extruder_retract_keys()) {
        const auto* src = m_full.option(rk);
        if (!src) continue;
        auto* dst = new_full_config.option(rk, true);
        if (dst && *dst != *src) {
            dst->set(src);
            ++variant_copied;
        }
    }

    VORTEK_LOG(warn, "override_filament_variant_expansion: synced " << registry_copied << " registry + "
        << variant_copied << " variant keys from m_full_print_config → new_full_config"
        << (is_manual ? " [Manual: plate volume_map preserved]" : " [Auto]")
        << (volume_map_changed ? " [volume_map CHANGED: variant sync skipped]" : ""));
}

} // namespace Vortek
