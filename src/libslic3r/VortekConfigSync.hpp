#ifndef VORTEK_CONFIG_SYNC_HPP
#define VORTEK_CONFIG_SYNC_HPP

/**
 * @file VortekConfigSync.hpp
 * @brief Vortek Config Synchronization Pipeline — orchestrator for H2C multi-nozzle
 *        configuration data flow across the Print lifecycle.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  1. OVERVIEW
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * H2C carousel printers use a multi-nozzle architecture where each physical
 * extruder holds multiple nozzle variants. A single print job may use N filaments
 * mapped to M nozzle variants across E physical extruders (e.g., 6 filaments
 * → 5 variants → 2 extruders).
 *
 * OrcaSlicer's upstream config pipeline (inherited from BBS) was designed for
 * simple 1:1 extruder-to-filament mapping. The Vortek layer extends it to
 * support N:M:E mapping by intercepting configuration at specific pipeline
 * stages and replacing upstream expansion with H2C-aware expansion.
 *
 * ConfigSync is the centralized orchestrator that manages all H2C-specific
 * configuration keys across four config objects in Print, ensuring data
 * consistency throughout the GUI → apply → slice → apply cycle.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  2. CONFIG OBJECTS
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * The Print pipeline operates on four config objects. Each holds a snapshot
 * of configuration at a different stage of processing:
 *
 *   ┌─────────────────────────┐
 *   │   new_full_config       │  Local variable in Print::apply().
 *   │   (DynamicPrintConfig)  │  Built by GUI: PresetBundle::full_fff_config().
 *   │                         │  Raw filament values, size = num_filaments (e.g. 4).
 *   │                         │  Undergoes variant expansion in Print::apply().
 *   └───────────┬─────────────┘
 *               │ snapshot before filament expansion
 *               ▼
 *   ┌─────────────────────────┐
 *   │ m_ori_full_print_config │  PrintBase member. Persists between apply() calls.
 *   │ (DynamicPrintConfig)    │  Snapshot of new_full_config AFTER printer/print
 *   │                         │  variant expansion but BEFORE filament expansion.
 *   │                         │  Used as the clean base for re-expansion.
 *   │                         │  BBS ref: PrintApply.cpp L1336
 *   └─────────────────────────┘
 *
 *   ┌─────────────────────────┐
 *   │   m_full_print_config   │  PrintBase member. Persists between apply() calls.
 *   │   (DynamicPrintConfig)  │  Fully expanded config WITH H2C variant resolution.
 *   │                         │  Written during slicing by update_filament_maps_to_config().
 *   │                         │  Contains the expanded vectors (size = num_variants, e.g. 6).
 *   │                         │  *** SOURCE OF TRUTH after first slice. ***
 *   │                         │  Exported to G-code, used by PlaceholderParser.
 *   └─────────────────────────┘
 *
 *   ┌─────────────────────────┐
 *   │      m_config           │  Print member (private, friend access).
 *   │      (PrintConfig)      │  Static config with fixed key set.
 *   │                         │  Used as the LEFT side of diff comparison:
 *   │                         │    print_config_diffs(m_config, new_full_config)
 *   │                         │  Updated at end of Print::apply() via apply_only().
 *   │                         │  *** COMPARISON BASELINE for reslice decisions. ***
 *   └─────────────────────────┘
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  3. DATA FLOW PIPELINE
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * ── Phase A: Print::apply() (UI thread) ──────────────────────────────────
 *
 *   A1. GUI builds new_full_config from presets
 *       new_full_config = PresetBundle::full_fff_config()
 *       Vectors sized to num_filaments (e.g. 4)
 *
 *   A2. Snapshot before filament expansion
 *       m_ori_full_print_config = new_full_config
 *       BBS ref: PrintApply.cpp L1176
 *
 *   A3. Upstream filament variant expansion (OrcaSlicer default)
 *       new_full_config.update_values_to_printer_extruders_for_multiple_filaments()
 *       Expands filament variant keys using upstream 1:1 logic (wrong for H2C)
 *       BBS ref: PrintApply.cpp L1177
 *
 *   A4. *** ConfigSync::align_incoming_config() ***
 *       Direction: m_full_print_config → new_full_config
 *       Replaces upstream expansion with the source-of-truth values
 *       from the last completed slice. All managed keys are copied,
 *       ensuring diff(m_config, new_full_config) = 0 for stable keys.
 *       No-op on first apply (m_full_print_config not yet populated).
 *
 *   A5. *** ConfigSync::restore_variants() ***
 *       Direction: m_ori_full_print_config → new_full_config
 *       Restores filament variant keys to their pre-expansion state,
 *       undoing A3's incorrect upstream expansion for keys that must
 *       preserve the raw GUI values until H2C-aware expansion in Phase B.
 *
 *   A6. Diff computation
 *       print_diff      = print_config_diffs(m_config, new_full_config, ...)
 *       full_config_diff = full_print_config_diffs(m_full_print_config, new_full_config)
 *       If both empty → no reslice needed.
 *
 *   A7. Apply diffs, update m_config
 *       m_config.apply_only(new_full_config, print_diff_keys)
 *       m_full_print_config = new_full_config  (for full_config_diff keys)
 *
 * ── Phase B: Slicing (background thread) ─────────────────────────────────
 *
 *   B1. Reset to clean base
 *       m_full_print_config = m_ori_full_print_config  (raw values)
 *
 *   B2. H2C-aware variant expansion
 *       update_filament_config_values_for_multiple_extruders()
 *       Uses nozzle_group_result to correctly map filaments to nozzle variants.
 *       Expands vectors to num_variants (e.g. 6).
 *       BBS ref: PrintApply.cpp L1338-1362
 *
 *   B3. Retract override application
 *       compute_filament_override_value() for all extruder_retract_keys
 *
 *   B4. *** ConfigSync::sync_baseline() ***
 *       Direction: m_full_print_config → m_config
 *       After expansion completes, the comparison baseline is updated
 *       to match the source of truth, so the next Print::apply() (Phase A)
 *       sees m_config == new_full_config for all managed keys → diff = 0.
 *
 * ── Steady State ─────────────────────────────────────────────────────────
 *
 *   After one full cycle (A → B), all four configs are consistent:
 *     m_config[managed_keys] == m_full_print_config[managed_keys]
 *   On next apply (A4), align copies m_full_print_config → new_full_config,
 *   resulting in: m_config == new_full_config → diff = 0 → no reslice.
 *
 *   A user-initiated change (e.g. filament_map_mode switch) produces
 *   a diff only for the actually-changed key → single expected reslice.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  4. MANAGED KEY CATEGORIES
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * ConfigSync manages four categories of keys that undergo H2C-specific
 * transformation. These keys must be synchronized consistently across
 * all config objects.
 *
 *   Category 1: Filament Variant Keys
 *     Source: filament_options_with_variant set + "filament_self_index"
 *     Contains: filament_flow_ratio, filament_max_volumetric_speed,
 *               filament_retraction_length, filament_z_hop, nozzle_temperature, ...
 *     Expanded by: update_filament_config_values_for_multiple_extruders()
 *     BBS ref: PrintConfig.cpp filament_options_with_variant
 *
 *   Category 2: Extruder Retract Keys (machine-level)
 *     Source: print_config_def.extruder_retract_keys()
 *     Contains: retraction_length, z_hop, wipe_distance, retraction_speed, ...
 *     Derived from filament_ counterparts via compute_filament_override_value()
 *     BBS ref: PrintConfig.cpp extruder_retract_keys()
 *
 *   Category 3: Computed Map Keys (Vortek-specific)
 *     Injected by Vortek hooks during slicing, not present in BBS upstream.
 *     Contains: filament_map, filament_volume_map, filament_nozzle_map,
 *               filament_map_2, filament_extruder_variant,
 *               physical_extruder_map, filament_self_index
 *
 *   Category 4: Temperature Keys
 *     Expanded by variant resolution with nozzle-specific temperature profiles.
 *     Contains: nozzle_temperature, nozzle_temperature_initial_layer,
 *               filament_pre_cooling_temperature_nc
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  5. DIFF FILTERING
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Despite synchronization, some residual diffs may appear due to vector
 * size differences between GUI-sized configs (num_filaments) and
 * H2C-expanded configs (num_variants). ConfigSync provides static
 * filter methods to suppress these known benign diffs:
 *
 *   filter_managed_keys()  — removes all managed keys from a diff vector
 *   filter_computed_keys() — removes computed map keys from a diff set
 *
 * These filters are a safety net; the goal is to eliminate them by ensuring
 * full idempotency of the sync pipeline.
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

    /// Phase A4: Align incoming config with source of truth.
    /// Direction: m_full_print_config → new_full_config
    /// @return Number of keys copied.
    int align_incoming_config(Slic3r::DynamicPrintConfig& new_full_config);

    /// Phase B4: Sync comparison baseline after variant expansion.
    /// Direction: m_full_print_config → m_config
    /// @return Number of keys synced.
    int sync_baseline();

    /// Phase A5: Restore filament variant keys to pre-expansion state.
    /// Direction: m_ori_full_print_config → new_full_config
    /// @return Number of keys restored.
    int restore_variants(Slic3r::DynamicPrintConfig& new_full_config);

    // ────────────────────── Key Sets (static) ──────────────────────

    /// Category 3: Computed map keys injected by Vortek hooks.
    static const std::vector<std::string>& computed_keys();

    /// Category 4: Temperature keys expanded by variant resolution.
    static const std::vector<std::string>& temperature_keys();

    /// Category 1: Filament variant keys (filament_options_with_variant + filament_self_index).
    static std::set<std::string> filament_variant_keys();

    /// Full set of all managed keys (union of categories 1-4).
    static const std::unordered_set<std::string>& managed_keys();

    // ────────────────────── Diff Filtering (static) ──────────────────────

    /// Filter all managed keys from a diff vector. Returns count suppressed.
    static size_t filter_managed_keys(Slic3r::t_config_option_keys& diff);

    /// Filter computed map keys from an unordered_set diff. Returns count suppressed.
    static size_t filter_computed_keys(std::unordered_set<std::string>& diff_set);

private:
    Slic3r::Print& m_print;

    static bool copy_key(const Slic3r::ConfigBase& src, Slic3r::ConfigBase& dst, const std::string& key);
    static int sync_all_keys(const Slic3r::ConfigBase& src, Slic3r::ConfigBase& dst);
    static int sync_variant_keys(const Slic3r::ConfigBase& src, Slic3r::ConfigBase& dst);
};

} // namespace Vortek

#endif // VORTEK_CONFIG_SYNC_HPP
