#include "VortekConfigSync.hpp"
#include "Print.hpp"
#include "VortekLog.hpp"

namespace Slic3r {
extern std::set<std::string> filament_options_with_variant;
extern const PrintConfigDef  print_config_def;
}

namespace Vortek {

// ────────────────────── Constructor ──────────────────────

ConfigSync::ConfigSync(Slic3r::Print& print) : m_print(print) {}

// ────────────────────── Key Sets ──────────────────────

const std::vector<std::string>& ConfigSync::computed_keys() {
    static const std::vector<std::string> keys = {
        "filament_map", "filament_volume_map", "filament_nozzle_map",
        "filament_map_2", "filament_extruder_variant",
        "physical_extruder_map", "filament_self_index"
    };
    return keys;
}

const std::vector<std::string>& ConfigSync::temperature_keys() {
    static const std::vector<std::string> keys = {
        "nozzle_temperature", "nozzle_temperature_initial_layer",
        "filament_pre_cooling_temperature_nc"
    };
    return keys;
}

std::set<std::string> ConfigSync::filament_variant_keys() {
    std::set<std::string> keys = Slic3r::filament_options_with_variant;
    keys.insert("filament_self_index");
    return keys;
}

const std::unordered_set<std::string>& ConfigSync::managed_keys() {
    static std::unordered_set<std::string> keys;
    if (keys.empty()) {
        for (const auto& k : Slic3r::filament_options_with_variant)
            keys.insert(k);
        keys.insert("filament_self_index");
        for (const auto& k : Slic3r::print_config_def.extruder_retract_keys())
            keys.insert(k);
        for (const auto& k : computed_keys())
            keys.insert(k);
        for (const auto& k : temperature_keys())
            keys.insert(k);
    }
    return keys;
}

// ────────────────────── Private Helpers ──────────────────────

bool ConfigSync::copy_key(const Slic3r::ConfigBase& src, Slic3r::ConfigBase& dst, const std::string& key) {
    const Slic3r::ConfigOption* opt_src = src.option(key);
    Slic3r::ConfigOption* opt_dst = dst.option(key);
    if (opt_src && opt_dst) {
        *opt_dst = *opt_src;
        return true;
    }
    return false;
}

int ConfigSync::sync_all_keys(const Slic3r::ConfigBase& src, Slic3r::ConfigBase& dst) {
    int copied = 0;

    // Filament variant keys
    for (const auto& key : Slic3r::filament_options_with_variant) {
        if (copy_key(src, dst, key)) ++copied;
    }
    if (copy_key(src, dst, "filament_self_index")) ++copied;

    // Retract keys (machine-level + filament_ prefixed)
    for (const auto& opt_key : Slic3r::print_config_def.extruder_retract_keys()) {
        if (copy_key(src, dst, opt_key)) ++copied;
        if (copy_key(src, dst, "filament_" + opt_key)) ++copied;
    }

    // Computed map keys
    for (const auto& key : computed_keys()) {
        if (copy_key(src, dst, key)) ++copied;
    }

    // Temperature keys
    for (const auto& key : temperature_keys()) {
        if (copy_key(src, dst, key)) ++copied;
    }

    return copied;
}

int ConfigSync::sync_variant_keys(const Slic3r::ConfigBase& src, Slic3r::ConfigBase& dst) {
    int copied = 0;
    for (const auto& key : Slic3r::filament_options_with_variant) {
        if (copy_key(src, dst, key)) ++copied;
    }
    return copied;
}

// ────────────────────── Orchestrated Operations ──────────────────────

int ConfigSync::align_incoming_config(Slic3r::DynamicPrintConfig& new_full_config) {
    // Direction: m_full_print_config (source of truth) → new_full_config
    // Ensures print_config_diffs(m_config, new_full_config) = 0 for managed keys
    int copied = sync_all_keys(m_print.m_full_print_config, new_full_config);
    VORTEK_LOG(warn, "ConfigSync::align_incoming_config: copied " << copied
        << " keys from m_full_print_config → new_full_config");
    return copied;
}

int ConfigSync::sync_baseline() {
    // Direction: m_full_print_config (source of truth) → m_config (comparison baseline)
    // After this, next Print::apply() will see m_config == new_full_config for managed keys
    int synced = sync_all_keys(m_print.m_full_print_config, m_print.m_config);
    VORTEK_LOG(warn, "ConfigSync::sync_baseline: synced " << synced
        << " keys from m_full_print_config → m_config");
    return synced;
}

int ConfigSync::restore_variants(Slic3r::DynamicPrintConfig& new_full_config) {
    // Direction: m_ori_full_print_config (pre-expansion) → new_full_config
    // Undoes wrong upstream expansion for filament variant keys
    int restored = sync_variant_keys(m_print.m_ori_full_print_config, new_full_config);
    VORTEK_LOG(warn, "ConfigSync::restore_variants: restored " << restored
        << " filament variant keys from m_ori_full_print_config → new_full_config");
    return restored;
}

// ────────────────────── Diff Filtering ──────────────────────

size_t ConfigSync::filter_managed_keys(Slic3r::t_config_option_keys& diff) {
    if (diff.empty()) return 0;
    const auto& keys = managed_keys();
    Slic3r::t_config_option_keys filtered;
    filtered.reserve(diff.size());
    for (const auto& k : diff) {
        if (keys.find(k) == keys.end())
            filtered.push_back(k);
    }
    size_t suppressed = diff.size() - filtered.size();
    if (suppressed > 0)
        diff = std::move(filtered);
    return suppressed;
}

size_t ConfigSync::filter_computed_keys(std::unordered_set<std::string>& diff_set) {
    size_t suppressed = 0;
    for (const auto& k : computed_keys()) {
        suppressed += diff_set.erase(k);
    }
    return suppressed;
}

} // namespace Vortek
