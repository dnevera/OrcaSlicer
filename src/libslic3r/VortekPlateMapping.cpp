#include "VortekPlateMapping.hpp"
#include "VortekLog.hpp"
#include "PresetBundle.hpp"
#include "VortekPrintHooks.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include <algorithm>

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
    if (auto* opt = full_cfg.option<Slic3r::ConfigOptionInts>("filament_map_2"))
        map_2 = opt->values;
    if (auto* opt = full_cfg.option<Slic3r::ConfigOptionInts>("physical_extruder_map"))
        phys_map = opt->values;

    // CRITICAL: Sync ALL computed maps to preset_bundle.project_config.
    // full_config() is built from project_config (NOT plate_config), so without this
    // the next Print::apply() gets stale defaults → config drift → double-slicing.
    auto& proj = preset_bundle.project_config;
    proj.set_key_value("filament_nozzle_map", new Slic3r::ConfigOptionInts(nozzle_map));
    proj.set_key_value("filament_volume_map", new Slic3r::ConfigOptionInts(volume_map));
    if (!map_2.empty())
        proj.set_key_value("filament_map_2", new Slic3r::ConfigOptionInts(map_2));
    if (!phys_map.empty())
        proj.set_key_value("physical_extruder_map", new Slic3r::ConfigOptionInts(phys_map));

    if (filament_map_mode != Slic3r::fmmManual && filament_map_mode != Slic3r::fmmNozzleManual) {
        VORTEK_LOG(warn, "sync_after_slicing: auto mode — synced project_config only (plate_config skipped)");
        return;
    }

    VORTEK_LOG(warn, "sync_after_slicing: updating nozzle maps in plate config and project config");

    bool changed = false;
    auto* opt_nozzle = plate_config.option<Slic3r::ConfigOptionInts>("filament_nozzle_map");
    if (!opt_nozzle || opt_nozzle->values != nozzle_map) {
        plate_config.set_key_value("filament_nozzle_map", new Slic3r::ConfigOptionInts(nozzle_map));
        changed = true;
    }
    auto* opt_volume = plate_config.option<Slic3r::ConfigOptionInts>("filament_volume_map");
    if (!opt_volume || opt_volume->values != volume_map) {
        plate_config.set_key_value("filament_volume_map", new Slic3r::ConfigOptionInts(volume_map));
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
    if (config->has("filament_nozzle_map")) {
        config->option<Slic3r::ConfigOptionInts>("filament_nozzle_map")->values.resize(filament_count, 1);
    }
    if (config->has("filament_volume_map")) {
        config->option<Slic3r::ConfigOptionInts>("filament_volume_map")->values.resize(filament_count, 1);
    }
}

void PlateMapping::handle_filament_added(Slic3r::DynamicPrintConfig* config)
{
    if (!config) return;
    VORTEK_LOG(debug, "handle_filament_added: appending default mapping values");
    if (config->has("filament_nozzle_map")) {
        config->option<Slic3r::ConfigOptionInts>("filament_nozzle_map")->values.push_back(1);
    }
    if (config->has("filament_volume_map")) {
        config->option<Slic3r::ConfigOptionInts>("filament_volume_map")->values.push_back(1);
    }
}

void PlateMapping::handle_filament_deleted(Slic3r::DynamicPrintConfig* config, int filament_id)
{
    if (!config) return;
    VORTEK_LOG(debug, "handle_filament_deleted: erasing mapping at index " << filament_id);
    if (config->has("filament_nozzle_map")) {
        auto& vals = config->option<Slic3r::ConfigOptionInts>("filament_nozzle_map")->values;
        if (filament_id >= 0 && filament_id < (int)vals.size())
            vals.erase(vals.begin() + filament_id);
    }
    if (config->has("filament_volume_map")) {
        auto& vals = config->option<Slic3r::ConfigOptionInts>("filament_volume_map")->values;
        if (filament_id >= 0 && filament_id < (int)vals.size())
            vals.erase(vals.begin() + filament_id);
    }
}

void PlateMapping::clear_mappings(Slic3r::DynamicPrintConfig* config)
{
    if (!config) return;
    VORTEK_LOG(debug, "clear_mappings: clearing all nozzle/volume mappings");
    if (config->has("filament_nozzle_map")) {
        config->option<Slic3r::ConfigOptionInts>("filament_nozzle_map")->values.clear();
    }
    if (config->has("filament_volume_map")) {
        config->option<Slic3r::ConfigOptionInts>("filament_volume_map")->values.clear();
    }
}

LoadMappingResult PlateMapping::load_from_3mf_structure(
    const Slic3r::PlateData* plate_data,
    int filament_count,
    Slic3r::GCodeProcessorResult* gcode_result
)
{
    LoadMappingResult res;
    if (!plate_data) return res;
    if (!is_h2c_printer(plate_data->config)) return res;

    VORTEK_LOG(warn, "load_from_3mf_structure: loading nozzle mappings");

    if (plate_data->config.has("filament_nozzle_map")) {
        res.filament_nozzle_map = plate_data->config.option<Slic3r::ConfigOptionInts>("filament_nozzle_map")->values;
    }
    if (plate_data->config.has("filament_volume_map")) {
        res.filament_volume_map = plate_data->config.option<Slic3r::ConfigOptionInts>("filament_volume_map")->values;
    }

    if (res.filament_nozzle_map.size() != filament_count) {
        res.filament_nozzle_map.resize(filament_count, 1);
    }
    if (res.filament_volume_map.size() != filament_count) {
        res.filament_volume_map.resize(filament_count, 1);
    }
    return res;
}

void PlateMapping::sync_project_config_on_load(Slic3r::DynamicConfig& proj_cfg, int filament_count)
{
    if (!is_h2c_printer(proj_cfg)) return;
    VORTEK_LOG(warn, "sync_project_config_on_load: verifying loaded map sizes");
    
    // Сброс MQTT-зависимых флагов, которые должны приходить с принтера, а не считываться из 3MF
    if (auto* p = proj_cfg.option<Slic3r::ConfigOptionBool>("has_filament_switcher"))
        p->value = false;
    if (auto* p = proj_cfg.option<Slic3r::ConfigOptionBool>("enable_filament_dynamic_map"))
        p->value = false;

    // Синхронизация filament_nozzle_map (дефолт 0 — первое сопло)
    auto* nozzle_map = proj_cfg.opt<Slic3r::ConfigOptionInts>("filament_nozzle_map", true);
    if ((int)nozzle_map->values.size() != filament_count) {
        nozzle_map->values.resize(filament_count, 0);
    }

    // Синхронизация filament_volume_map (дефолт 1 — стандартный объем / nvtStandard)
    auto* volume_map = proj_cfg.opt<Slic3r::ConfigOptionInts>("filament_volume_map", true);
    if ((int)volume_map->values.size() != filament_count) {
        volume_map->values.resize(filament_count, 1);
    }
}

void PlateMapping::patch_export_config(Slic3r::DynamicPrintConfig& cfg)
{
    if (!cfg.has("filament_nozzle_map")) {
        cfg.set_key_value("filament_nozzle_map", new Slic3r::ConfigOptionInts({0}));
    }
    if (!cfg.has("filament_volume_map")) {
        cfg.set_key_value("filament_volume_map", new Slic3r::ConfigOptionInts({1}));
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
    if (plate_config.has("filament_nozzle_map")) {
        auto* plate_opt = plate_config.option<Slic3r::ConfigOptionInts>("filament_nozzle_map");
        if (plate_opt && plate_opt->values.size() > 1) {
            VORTEK_LOG(warn, "get_nozzle_map_for_export: using plate_config nozzle map (" << plate_opt->values.size() << " elements)");
            return plate_opt->values;
        }
    }
    // Fallback: try print->full_print_config() (valid immediately after slicing)
    if (print && plate_config.has("filament_nozzle_map")) {
        const auto& full_cfg = print->full_print_config();
        if (auto* opt = full_cfg.option<Slic3r::ConfigOptionInts>("filament_nozzle_map")) {
            if (opt->values.size() > 1) {
                VORTEK_LOG(warn, "get_nozzle_map_for_export: using print-derived nozzle map (" << opt->values.size() << " elements)");
                return opt->values;
            }
        }
    }
    if (plate_config.has("filament_nozzle_map")) {
        return plate_config.option<Slic3r::ConfigOptionInts>("filament_nozzle_map")->values;
    }
    return {};
}

std::vector<int> PlateMapping::get_volume_map_for_export(const Slic3r::Print* print, const Slic3r::DynamicPrintConfig& plate_config)
{
    // Reference to BBS: BambuStudio/src/libslic3r/Format/bbs_3mf.cpp – filament_volume_map export
    // Priority: plate_config is set by BackgroundSlicingProcess::process() AFTER slicing via
    // set_filament_volume_maps() with the correct multi-element array.
    // print->full_print_config() may have been reset by a subsequent Print::apply() call.
    // So: prefer plate_config if it has >1 element (post-slice initialized), else fall back to print.
    if (plate_config.has("filament_volume_map")) {
        auto* plate_opt = plate_config.option<Slic3r::ConfigOptionInts>("filament_volume_map");
        if (plate_opt && plate_opt->values.size() > 1) {
            VORTEK_LOG(warn, "get_volume_map_for_export: using plate_config volume map (" << plate_opt->values.size() << " elements)");
            return plate_opt->values;
        }
    }
    // Fallback: try print->full_print_config() (valid immediately after slicing)
    if (print && plate_config.has("filament_volume_map")) {
        const auto& full_cfg = print->full_print_config();
        if (auto* opt = full_cfg.option<Slic3r::ConfigOptionInts>("filament_volume_map")) {
            if (opt->values.size() > 1) {
                VORTEK_LOG(warn, "get_volume_map_for_export: using print-derived volume map (" << opt->values.size() << " elements)");
                return opt->values;
            }
        }
    }
    if (plate_config.has("filament_volume_map")) {
        return plate_config.option<Slic3r::ConfigOptionInts>("filament_volume_map")->values;
    }
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
    if (!print && !config.has("filament_nozzle_map")) return;

    VORTEK_LOG(warn, "patch_plate_data_for_export for plate index " << plate_data->plate_index);

    std::vector<int> nozzle_map = filament_nozzle_map;
    std::vector<int> volume_map = filament_volume_map;

    if (print) {
        nozzle_map = get_nozzle_map_for_export(print, config);
        volume_map = get_volume_map_for_export(print, config);
    }

    plate_data->config.set_key_value("filament_nozzle_map", new Slic3r::ConfigOptionInts(nozzle_map));
    plate_data->config.set_key_value("filament_volume_map", new Slic3r::ConfigOptionInts(volume_map));
}

void PlateMapping::handle_h2c_mapping_apply(
    Slic3r::Print* print,
    Slic3r::DynamicPrintConfig& new_full_config,
    const Slic3r::DynamicPrintConfig& old_full_config
)
{
    if (new_full_config.has("filament_nozzle_map") && old_full_config.has("filament_nozzle_map")) {
        auto new_nozzle = new_full_config.option<Slic3r::ConfigOptionInts>("filament_nozzle_map")->values;
        auto old_nozzle = old_full_config.option<Slic3r::ConfigOptionInts>("filament_nozzle_map")->values;
        if (new_nozzle != old_nozzle) {
            VORTEK_LOG(warn, "handle_h2c_mapping_apply: synchronizing config update");
        }
    }
}

void PlateMapping::handle_h2c_print_diff(
    Slic3r::Print* print,
    Slic3r::PrintConfig& config,
    Slic3r::DynamicPrintConfig& full_print_config,
    const Slic3r::DynamicPrintConfig& new_full_config,
    std::unordered_set<std::string>& print_diff_set
)
{
    VORTEK_LOG(debug, "handle_h2c_print_diff: checking " << print_diff_set.size() << " changed options");
    std::vector<std::string> keys_to_remove;
    for (const auto& key : print_diff_set) {
        // Suppress invalidation for dynamic override parameters
        if (key == "nozzle_diameter" || key == "retraction_length" || key == "z_hop" || key == "retraction_speed" || key == "deretraction_speed") {
            keys_to_remove.push_back(key);
        }
    }
    for (const auto& key : keys_to_remove) {
        print_diff_set.erase(key);
        VORTEK_LOG(debug, "suppressed false invalidation for key: " << key);
    }
}

bool PlateMapping::get_variant_override_serialized(const Slic3r::ConfigBase* config, const std::string& opt_key, std::string& out_serialized)
{
    if (!config || !config->has(opt_key)) return false;
    out_serialized = config->option(opt_key)->serialize();
    return true;
}

bool PlateMapping::get_variant_override_values(const Slic3r::ConfigBase* config, const std::string& opt_key, std::vector<std::string>& out_values)
{
    if (!config || !config->has(opt_key)) return false;
    out_values = { config->option(opt_key)->serialize() };
    return true;
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

    // Only filter for Vortek H2C — P2S/H2D and standard printers must NOT be affected.
    if (!is_h2c_printer(config)) return;

    // Build the suppression set once (lazy init).
    // Sources: extruder_retract_keys (retraction_length, z_hop, etc.) + Vortek computed maps.
    // These keys are recomputed mid-slice by update_to_config_by_nozzle_group_result
    // with multi-nozzle variant logic that differs from full_fff_config's per-filament logic.
    static std::unordered_set<std::string> s_suppressed_keys;
    if (s_suppressed_keys.empty()) {
        // Retract keys from PrintConfigDef — same set used by compute_filament_override_value
        for (const auto& k : Slic3r::print_config_def.extruder_retract_keys())
            s_suppressed_keys.insert(k);
        // Vortek computed map keys
        s_suppressed_keys.insert("filament_nozzle_map");
        s_suppressed_keys.insert("filament_volume_map");
        s_suppressed_keys.insert("filament_map_2");
        s_suppressed_keys.insert("physical_extruder_map");
        s_suppressed_keys.insert("filament_self_index");
    }

    Slic3r::t_config_option_keys filtered;
    filtered.reserve(full_config_diff.size());
    for (const auto& k : full_config_diff) {
        if (s_suppressed_keys.find(k) == s_suppressed_keys.end())
            filtered.push_back(k);
    }

    size_t suppressed = full_config_diff.size() - filtered.size();
    if (suppressed > 0) {
        VORTEK_LOG(warn, "filter_full_config_diff: suppressed " << suppressed
                         << " variant-transformed keys from full_config_diff");
        full_config_diff = std::move(filtered);
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

    // Vortek computed map keys injected by sync_after_slicing — must not trigger re-slice
    static const std::vector<std::string> s_vortek_map_keys = {
        "filament_nozzle_map", "filament_volume_map",
        "filament_map_2", "physical_extruder_map",
        "filament_self_index"
    };

    for (const auto& k : s_vortek_map_keys) {
        if (print_diff_set.erase(k) > 0) {
            // Sync the value in full_print_config to match new_full_config,
            // so next Print::apply won't see this key in diff again
            if (new_full_config.has(k)) {
                auto* new_opt = new_full_config.option(k);
                if (new_opt) {
                    full_print_config.set_key_value(k, new_opt->clone());
                }
            }
            VORTEK_LOG(warn, "filter_print_diff_set: suppressed and synced key '" << k << "'");
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
    // All Vortek computed/derived keys that are recomputed on every apply()
    // and must not trigger re-slice invalidation.
    static const std::vector<std::string> s_vortek_keys = {
        "filament_map_2", "filament_nozzle_map", "filament_volume_map",
        "physical_extruder_map"
    };
    for (const auto& key : s_vortek_keys) {
        erase_key(print_diff, key);
        erase_key(full_config_diff, key);
    }

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

void PlateMapping::apply_filament_retract_overrides(
    Slic3r::DynamicPrintConfig& new_full_config,
    const std::vector<int>& filament_maps
)
{
    // Rule: Vortek hooks are isolated to H2C printers only
    if (!is_h2c_printer(new_full_config) || filament_maps.empty()) {
        return;
    }

    // Copy to non-const vector because Slic3r's apply_override signature requires std::vector<int>&
    std::vector<int> default_maps = filament_maps;

    const std::vector<std::string> &extruder_retract_keys = Slic3r::print_config_def.extruder_retract_keys();
    const std::string               filament_prefix       = "filament_";
    for (const auto &opt_key : extruder_retract_keys) {
        Slic3r::ConfigOption *opt_new_machine  = new_full_config.option(opt_key);
        const Slic3r::ConfigOption *opt_new_filament = new_full_config.option(filament_prefix + opt_key);
        if (opt_new_machine && opt_new_filament) {
            const auto* new_fil_vec = dynamic_cast<const Slic3r::ConfigOptionVectorBase*>(opt_new_filament);
            if (new_fil_vec && default_maps.size() == new_fil_vec->size()) {
                opt_new_machine->apply_override(opt_new_filament, default_maps);
            }
        }
    }
}

} // namespace Vortek
