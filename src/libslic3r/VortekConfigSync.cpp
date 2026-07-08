#include "VortekConfigSync.hpp"
#include "VortekKeys.hpp"
#include "Print.hpp"
#include "VortekLog.hpp"

namespace Slic3r {
extern const PrintConfigDef  print_config_def;
}

namespace Vortek {

// ────────────────────── Constructor ──────────────────────

ConfigSync::ConfigSync(Slic3r::Print& print) : m_print(print) {}

// ────────────────────── Private Helpers ──────────────────────

bool ConfigSync::copy_key(const Slic3r::ConfigBase& src, Slic3r::ConfigBase& dst, const std::string& key) {
    const Slic3r::ConfigOption* opt_src = src.option(key);
    if (!opt_src) return false;

    // For DynamicPrintConfig: clone+set_key_value guarantees full copy
    // including vector resize (e.g. GUI size=4 → H2C size=6).
    // operator= does NOT resize vectors when src and dst have different sizes.
    auto* dyn_dst = dynamic_cast<Slic3r::DynamicPrintConfig*>(&dst);
    if (dyn_dst) {
        dyn_dst->set_key_value(key, opt_src->clone());
        return true;
    }

    // For static configs (PrintConfig): operator= works within fixed storage
    Slic3r::ConfigOption* opt_dst = dst.option(key);
    if (opt_dst) {
        *opt_dst = *opt_src;
        return true;
    }
    return false;
}

// ────────────────────── Sync Helpers ──────────────────────

int ConfigSync::sync_keys_between(const Slic3r::ConfigBase& src, Slic3r::ConfigBase& dst,
                                   bool Keys::KeyDef::* attr) {
    int copied = 0;

    // 1. All registry keys matching the requested attribute
    for (const auto& kd : Keys::registry()) {
        if (kd.*attr && copy_key(src, dst, kd.name))
            ++copied;
    }

    // 2. BBS retract keys + filament_ prefixed counterparts
    // These are BBS base-layer keys in our registry as Retract group.
    // For baseline: always copy (retract keys have sync_baseline=true).
    // For align: skip retract keys (sync_align=false) to avoid double-override.
    // filament_ prefixed keys are always needed for override resolution.
    for (const auto& rk : Slic3r::print_config_def.extruder_retract_keys()) {
        // retract key itself is covered by registry loop above (step 1)
        // Only copy filament_ prefix here
        if (copy_key(src, dst, "filament_" + rk)) ++copied;
    }

    // 3. Filament variant keys (from filament_options_with_variant in BBS base layer)
    for (const auto& key : Slic3r::filament_options_with_variant) {
        if (copy_key(src, dst, key)) ++copied;
    }

    return copied;
}

// Overload for VortekGCode sync (uses sync_baseline by default)
int ConfigSync::sync_keys_between(const Slic3r::ConfigBase& src, Slic3r::ConfigBase& dst) {
    return sync_keys_between(src, dst, &Keys::KeyDef::sync_baseline);
}

// ────────────────────── Orchestrated Operations ──────────────────────

int ConfigSync::align_incoming_config(Slic3r::DynamicPrintConfig& new_full_config) {
    // Direction: m_full_print_config (source of truth) → new_full_config
    // Uses sync_align: skips retract keys (sync_align=false) to avoid double-override
    // in print_config_diffs which re-applies apply_override on these keys.
    int copied = sync_keys_between(m_print.m_full_print_config, new_full_config,
                                    &Keys::KeyDef::sync_align);

    VORTEK_LOG(warn, "ConfigSync::align_incoming_config: copied " << copied
        << " keys from m_full_print_config → new_full_config");
    return copied;
}

int ConfigSync::sync_baseline() {
    // Direction: m_full_print_config (source of truth) → m_config (comparison baseline)
    // Uses sync_baseline: includes retract keys (post-override values) so that
    // print_config_diffs comparison yields 0 diff for these keys.
    int synced = sync_keys_between(m_print.m_full_print_config, m_print.m_config,
                                    &Keys::KeyDef::sync_baseline);
    VORTEK_LOG(warn, "ConfigSync::sync_baseline: synced " << synced
        << " keys from m_full_print_config → m_config");
    return synced;
}

int ConfigSync::restore_variants(Slic3r::DynamicPrintConfig& new_full_config) {
    // Direction: m_ori_full_print_config (pre-expansion) → new_full_config
    // Restore filament variant keys to pre-expansion state
    int restored = 0;
    for (const auto& kd : Keys::registry()) {
        if (kd.variant_expanded &&
            copy_key(m_print.m_ori_full_print_config, new_full_config, kd.name))
            ++restored;
    }
    // Also restore BBS filament variant keys not in our registry
    for (const auto& key : Slic3r::filament_options_with_variant) {
        if (copy_key(m_print.m_ori_full_print_config, new_full_config, key))
            ++restored;
    }
    VORTEK_LOG(warn, "ConfigSync::restore_variants: restored " << restored
        << " filament variant keys from m_ori_full_print_config → new_full_config");
    return restored;
}

// ────────────────────── Diff Filtering ──────────────────────

size_t ConfigSync::filter_managed_keys(Slic3r::t_config_option_keys& diff) {
    if (diff.empty()) return 0;
    const auto& keys = Keys::managed_set();
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
    return Keys::filter_by_attr(diff_set, &Keys::KeyDef::computed);
}

// ────────────────────── Variant Override ──────────────────────

std::vector<int> ConfigSync::get_override_indices(const Slic3r::DynamicPrintConfig& config) {
    // H2C: filament_map_2 contains variant indices (0-based).
    // apply_override expects 1-based → +1.
    // Reference to BBS: BambuStudio/src/libslic3r/PrintApply.cpp
    auto* map2 = config.option<Slic3r::ConfigOptionInts>(Keys::k_filament_map_2);
    if (map2 && !map2->values.empty()) {
        std::vector<int> indices = map2->values;
        for (auto& v : indices) v += 1;  // apply_override uses 1-based indexing
        VORTEK_LOG(warn, "ConfigSync::get_override_indices: using filament_map_2 (H2C), size=" << indices.size());
        return indices;
    }
    // Standard: filament_map (extruder IDs, already 1-based)
    auto* map = config.option<Slic3r::ConfigOptionInts>("filament_map");
    return map ? map->values : std::vector<int>();
}

void ConfigSync::apply_retract_overrides(Slic3r::Print& print) {
    // Shared helper: compute retract overrides using correct variant indices.
    // Replaces duplicated code blocks in VortekPrintHooks.cpp.
    // Reference to BBS: BambuStudio/src/libslic3r/PrintConfig.cpp (compute_filament_override_value)
    const auto& retract_keys = Slic3r::print_config_def.extruder_retract_keys();
    auto indices = get_override_indices(print.m_full_print_config);

    if (indices.empty()) return;

    const std::string filament_prefix = "filament_";
    Slic3r::t_config_option_keys diff;
    Slic3r::DynamicPrintConfig overrides;

    for (const auto& opt_key : retract_keys) {
        const Slic3r::ConfigOption* opt_fil = print.m_full_print_config.option(filament_prefix + opt_key);
        const Slic3r::ConfigOption* opt_new = print.m_full_print_config.option(opt_key);
        if (opt_fil && opt_new)
            Slic3r::compute_filament_override_value(opt_key, opt_new, opt_new, opt_fil,
                print.m_full_print_config, diff, overrides, indices);
    }

    if (!diff.empty()) {
        print.m_placeholder_parser.apply_config(overrides);
        print.m_full_print_config.apply(overrides);
        print.m_ori_full_print_config.apply(overrides);
        VORTEK_LOG(warn, "ConfigSync::apply_retract_overrides: applied " << diff.size()
            << " retract overrides with variant indices");
    }
}

} // namespace Vortek
