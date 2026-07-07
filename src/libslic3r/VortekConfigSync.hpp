#ifndef VORTEK_CONFIG_SYNC_HPP
#define VORTEK_CONFIG_SYNC_HPP

/**
 * @file VortekConfigSync.hpp
 * @brief Orchestrator for config synchronization across the Print pipeline.
 *
 * === Config Object Hierarchy in Print::apply() ===
 *
 * BBS Reference: BambuStudio/src/libslic3r/PrintApply.cpp L1290-1380
 *
 * 1. new_full_config  (DynamicPrintConfig, local to Print::apply)
 *    - Incoming config from GUI (PresetBundle::full_fff_config()).
 *    - Contains RAW filament values BEFORE variant expansion.
 *    - Undergoes upstream expansion, then Vortek override.
 *    - Compared against m_config to determine if reslice is needed.
 *
 * 2. m_ori_full_print_config  (DynamicPrintConfig, PrintBase member)
 *    - Snapshot of new_full_config AFTER printer/print variant expansion
 *      but BEFORE filament variant expansion.
 *    - BBS Reference: BambuStudio/src/libslic3r/PrintApply.cpp L1336
 *
 * 3. m_full_print_config  (DynamicPrintConfig, PrintBase member)
 *    - Fully expanded config WITH variant resolution.
 *    - Written by update_filament_maps_to_config() during slicing.
 *    - *** SOURCE OF TRUTH *** for variant-expanded values after first slice.
 *
 * 4. m_config  (PrintConfig, Print member — private, friend access)
 *    - Comparison baseline: print_config_diffs(m_config, new_full_config) → reslice decision.
 *    - *** MUST BE SYNCED *** with m_full_print_config after variant expansion
 *      to prevent false diffs.
 *
 * === Sync Operations ===
 *
 *   align_incoming_config():
 *     Direction: m_full_print_config → new_full_config
 *     When: Print::apply(), BEFORE diff computation
 *     Why: new_full_config has upstream-expanded values (wrong for multi-nozzle).
 *          Copying from source of truth ensures diff = 0 on next compare.
 *
 *   sync_baseline():
 *     Direction: m_full_print_config → m_config
 *     When: update_filament_maps_to_config(), AFTER variant expansion
 *     Why: m_config retains upstream-expanded values from the previous Print::apply().
 *          Syncing ensures the comparison baseline matches the source of truth.
 *
 *   restore_variants():
 *     Direction: m_ori_full_print_config → new_full_config
 *     When: Print::apply(), AFTER upstream expansion, BEFORE Vortek override
 *     Why: Restores filament variant keys to pre-expansion state (bypass upstream).
 *
 * === Key Categories ===
 *
 * 1. Filament variant keys (filament_options_with_variant + filament_self_index)
 *    BBS Reference: BambuStudio/src/libslic3r/PrintConfig.cpp
 *
 * 2. Extruder retract keys (print_config_def.extruder_retract_keys())
 *    BBS Reference: BambuStudio/src/libslic3r/PrintConfig.cpp
 *
 * 3. Computed map keys: filament_map, filament_volume_map, filament_nozzle_map,
 *    filament_map_2, filament_extruder_variant, physical_extruder_map, filament_self_index
 *
 * 4. Temperature keys: nozzle_temperature, nozzle_temperature_initial_layer,
 *    filament_pre_cooling_temperature_nc
 */

#include "libslic3r/PrintConfig.hpp"
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace Slic3r {
class Print;
}

namespace Vortek {

class ConfigSync {
public:
    // ────────────────────── Orchestrator ──────────────────────

    /// Create orchestrator bound to a Print instance.
    /// All sync operations use Print's internal configs as source/destination.
    explicit ConfigSync(Slic3r::Print& print);

    /**
     * @brief Align incoming config with source of truth BEFORE diff computation.
     *
     * Direction: m_full_print_config → new_full_config
     * Called in: override_filament_variant_expansion (PrintApply.cpp)
     *
     * Copies all managed keys so that the subsequent
     * print_config_diffs(m_config, new_full_config) produces 0 diffs
     * for variant-expanded keys.
     *
     * @return Number of keys copied.
     */
    int align_incoming_config(Slic3r::DynamicPrintConfig& new_full_config);

    /**
     * @brief Sync comparison baseline AFTER variant expansion completes.
     *
     * Direction: m_full_print_config → m_config
     * Called in: update_filament_maps_to_config (VortekPrintHooks.cpp)
     *
     * Updates m_config to match the source of truth so the NEXT Print::apply()
     * diff comparison finds 0 diffs for these keys.
     * Safe: writing m_config doesn't trigger apply() — it's just the comparison baseline.
     *
     * @return Number of keys synced.
     */
    int sync_baseline();

    /**
     * @brief Restore filament variant keys from original (pre-expansion) config.
     *
     * Direction: m_ori_full_print_config → new_full_config
     * Called in: restore_filament_variant_overrides (PrintApply.cpp)
     *
     * Undoes upstream expansion that doesn't support multi-nozzle variant selection.
     *
     * @return Number of keys restored.
     */
    int restore_variants(Slic3r::DynamicPrintConfig& new_full_config);

    // ────────────────────── Key Sets (static) ──────────────────────

    /// Computed map keys — injected by Vortek hooks.
    static const std::vector<std::string>& computed_keys();

    /// Temperature keys — expanded by variant resolution.
    static const std::vector<std::string>& temperature_keys();

    /// Filament variant keys (filament_options_with_variant + filament_self_index).
    static std::set<std::string> filament_variant_keys();

    /// Full set of all managed keys (union of all categories).
    static const std::unordered_set<std::string>& managed_keys();

    // ────────────────────── Diff Filtering (static) ──────────────────────

    /// Filter all managed keys from a diff vector. Returns count suppressed.
    static size_t filter_managed_keys(Slic3r::t_config_option_keys& diff);

    /// Filter computed map keys from an unordered_set diff. Returns count suppressed.
    static size_t filter_computed_keys(std::unordered_set<std::string>& diff_set);

private:
    Slic3r::Print& m_print;

    /// Copy a single key from src to dst. Returns true if copied.
    static bool copy_key(const Slic3r::ConfigBase& src, Slic3r::ConfigBase& dst, const std::string& key);

    /// Copy all managed keys from src to dst. Returns count copied.
    static int sync_all_keys(const Slic3r::ConfigBase& src, Slic3r::ConfigBase& dst);

    /// Copy only filament variant keys from src to dst. Returns count copied.
    static int sync_variant_keys(const Slic3r::ConfigBase& src, Slic3r::ConfigBase& dst);
};

} // namespace Vortek

#endif // VORTEK_CONFIG_SYNC_HPP
