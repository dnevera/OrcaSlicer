#pragma once
// VortekKeys.hpp — Declarative registry of all Vortek (H2C) config keys
// with behavioural attributes for sync, filtering, and variant override.
//
// Central source of truth: all key names, sync/filter behaviour, and
// variant expansion attributes are defined here. ConfigSync, PlateMapping,
// PrintHooks, VortekGCode, and PreCooling are consumers of this registry.
//
// Reference to BBS: BambuStudio/src/libslic3r/PrintConfig.cpp
// (extruder_retract_keys, filament_options_with_variant, printer_options_with_variant_1)

#include <string>
#include <vector>
#include <unordered_set>
#include <functional>

namespace Vortek {
namespace Keys {

// ─── Enums ───

enum class Origin {
    Vortek,  // Key defined by Vortek isolation layer
    BBS      // Key defined in BBS/Orca base layer, referenced by Vortek
};

enum class Group {
    Mapping,       // filament_nozzle_map, filament_map_2, physical_extruder_map, etc.
    Thermal,       // nozzle_temperature, pre_cooling, heating/cooling rates
    Retract,       // BBS extruder_retract_keys consumed by WipeTower/GCode
    NozzleChange,  // prime volumes, retract_nc, ramming_nc
    Hardware,      // extruder stats, nozzle counts, variant lists
    Capability,    // enable_*, has_*
    Placeholder    // vortek_* placeholder parser keys (GCode runtime)
};

// ─── KeyDef ───
//
// ┌──────────────────────────────────────────────────────────────────────────┐
// │                    ATTRIBUTE REFERENCE & DATA FLOW                       │
// ├──────────────────────────────────────────────────────────────────────────┤
// │                                                                          │
// │  Print::apply() pipeline (PrintApply.cpp):                               │
// │                                                                          │
// │    GUI config (new_full_config)                                          │
// │         │                                                                │
// │         ▼                                                                │
// │    ┌──────────────────────┐                                              │
// │    │ align_incoming_config│ ← sync_align: copies keys from              │
// │    │ (m_full → new_full)  │   m_full_print_config → new_full_config     │
// │    └──────────┬───────────┘   so that managed keys have correct          │
// │               │               expanded values for diff comparison.       │
// │               ▼                                                          │
// │    ┌──────────────────────┐                                              │
// │    │  print_config_diffs  │ ← computed keys: suppressed via              │
// │    │  (m_config vs        │   filter_print_diff_set (uses computed flag) │
// │    │   recomputed via     │                                              │
// │    │   apply_override)    │                                              │
// │    └──────────┬───────────┘                                              │
// │               │                                                          │
// │               ▼                                                          │
// │    ┌──────────────────────┐                                              │
// │    │ full_config_diffs    │ ← computed keys: suppressed via              │
// │    │ (m_full vs new_full) │   filter_reslice_diffs (uses computed flag)  │
// │    └──────────┬───────────┘                                              │
// │               │                                                          │
// │               ▼                                                          │
// │    ┌──────────────────────┐                                              │
// │    │ Reslice / No reslice │   Normal keys participate in diff honestly.  │
// │    └──────────────────────┘   Variant-expanded keys arrive pre-expanded  │
// │                               from GUI full_fff_config() → no false      │
// │                               diffs (restore_variants disabled).         │
// │                                                                          │
// │  Post-slice pipeline (VortekPrintHooks.cpp):                             │
// │                                                                          │
// │    apply_retract_overrides(print) → writes post-override retract        │
// │    values into m_full_print_config using filament_map_2                  │
// │         │                                                                │
// │         ▼                                                                │
// │    sync_baseline() ← sync_baseline: copies post-override values from   │
// │    (m_full → m_config)   m_full_print_config → m_config so that the     │
// │                          NEXT Print::apply() diff comparison finds 0.    │
// │                                                                          │
// │  GCode export (VortekGCode.cpp):                                         │
// │                                                                          │
// │    sync_keys_between(full_cfg, mutable_config)                           │
// │    Uses sync_baseline attribute (default overload) to sync all           │
// │    managed keys into the GCode writer's local config.                    │
// │                                                                          │
// └──────────────────────────────────────────────────────────────────────────┘
//
// ═══════════════════════════════════════════════════════════════════════════
//  ATTRIBUTE DESCRIPTIONS
// ═══════════════════════════════════════════════════════════════════════════
//
//  computed:
//    true = Key is INJECTED by Vortek hooks during slice (e.g. physical_extruder_map,
//           filament_self_index). Not set by GUI. Suppressed from both
//           print_diff and full_config_diff to prevent false invalidation.
//    Consumers: PlateMapping::filter_print_diff_set(),
//               PlateMapping::filter_reslice_diffs()
//
//  sync_align:
//    true = Copy this key in align_incoming_config (m_full → new_full).
//           Ensures new_full_config has variant-expanded values before diff.
//    false = Do NOT copy. For retract keys: prevents double-override because
//            print_config_diffs re-applies apply_override internally.
//    Consumers: ConfigSync::sync_keys_between(..., &KeyDef::sync_align)
//
//  sync_baseline:
//    true = Copy this key in sync_baseline (m_full → m_config) AFTER slice.
//           Ensures m_config contains post-override values so the next
//           Print::apply() sees 0 diff for these keys.
//    Also used as default for sync_keys_between() (VortekGCode sync).
//    Consumers: ConfigSync::sync_baseline(),
//               ConfigSync::sync_keys_between(src, dst) [default overload]
//
//  variant_expanded:
//    true = Key is expanded by BBS filament_options_with_variant mechanism.
//           Its vector size changes from N_extruders to N_variants during
//           update_filament_config_values_for_multiple_extruders.
//    Informational flag, used by restore_variants().
//
//  needs_variant_override:
//    true = When computing apply_override for this key, use filament_map_2
//           (variant indices, +1 for 1-based) instead of filament_map
//           (extruder IDs). Only relevant for H2C printers.
//           Standard printers fall back to filament_map automatically.
//    Consumers: ConfigSync::get_override_indices() checks this via registry,
//               apply_retract_overrides() applies overrides for these keys.
//
// ═══════════════════════════════════════════════════════════════════════════
//  HOW TO ADD A NEW KEY
// ═══════════════════════════════════════════════════════════════════════════
//
//  1. Add constexpr string below (k_your_key_name).
//  2. Add entry to registry() in VortekKeys.cpp with correct attributes:
//
//     {k_your_key_name, Origin::Vortek/BBS, Group::XXX,
//         computed, sync_align, sync_baseline,
//         variant_expanded, needs_variant_override},
//
//  3. Decision guide for attributes:
//
//     Q: Is this key injected during slice (not from GUI)?
//        → computed=true, sync_align=true, sync_baseline=true
//
//     Q: Is this a BBS retract key that uses apply_override?
//        → computed=false, sync_align=FALSE (avoid double-override),
//          sync_baseline=true, needs_variant_override=true
//
//     Q: Is this a Vortek-only key (nc params, hardware, capabilities)?
//        → All false (no sync needed, Vortek manages internally)
//
//     Q: Is this a BBS thermal key expanded by variants?
//        → sync_align=true, sync_baseline=true, variant_expanded=true
//
// ═══════════════════════════════════════════════════════════════════════════

struct KeyDef {
    const char* name;
    Origin      origin;
    Group       group;

    // ─── Sync attributes (see flow diagram above) ───
    bool computed;              // Injected by Vortek hooks during slice, not from GUI
    bool sync_align;            // Copy in align_incoming_config (m_full → new_full)
    bool sync_baseline;         // Copy in sync_baseline (m_full → m_config)

    // ─── Expansion attributes ───
    bool variant_expanded;      // Expanded by filament_options_with_variant
    bool needs_variant_override; // apply_override must use filament_map_2 for H2C
};

// ─── Registry ───

/// Full registry of all Vortek-relevant keys (lazily initialised, thread-safe)
const std::vector<KeyDef>& registry();

// ─── Pre-built query sets ───

const std::unordered_set<std::string>& computed_set();
const std::unordered_set<std::string>& sync_align_set();
const std::unordered_set<std::string>& sync_baseline_set();
const std::unordered_set<std::string>& variant_override_set(); // needs_variant_override=true

// ─── Query functions ───

/// Returns true if this key needs filament_map_2 for apply_override (H2C only)
bool needs_variant_override(const std::string& key);

/// Generic filter: erase from diff all keys whose attribute is true.
/// @return Count of suppressed keys.
size_t filter_by_attr(std::unordered_set<std::string>& diff, bool KeyDef::* attr);

// ─── String constants (replaces hardcoded literals) ───

// Mapping
constexpr const char* k_filament_map              = "filament_map";
constexpr const char* k_filament_volume_map       = "filament_volume_map";
constexpr const char* k_filament_nozzle_map       = "filament_nozzle_map";
constexpr const char* k_filament_map_2            = "filament_map_2";
constexpr const char* k_filament_extruder_variant = "filament_extruder_variant";
constexpr const char* k_physical_extruder_map     = "physical_extruder_map";
constexpr const char* k_filament_self_index       = "filament_self_index";

// Thermal
constexpr const char* k_nozzle_temperature               = "nozzle_temperature";
constexpr const char* k_nozzle_temperature_initial_layer  = "nozzle_temperature_initial_layer";
constexpr const char* k_filament_pre_cooling_temp         = "filament_pre_cooling_temperature";
constexpr const char* k_filament_pre_cooling_temp_nc      = "filament_pre_cooling_temperature_nc";
constexpr const char* k_filament_preheat_temp_delta       = "filament_preheat_temperature_delta";
constexpr const char* k_hotend_cooling_rate               = "hotend_cooling_rate";
constexpr const char* k_hotend_heating_rate               = "hotend_heating_rate";
constexpr const char* k_enable_pre_heating                = "enable_pre_heating";

// Nozzle Change
constexpr const char* k_filament_ramming_vol_speed_nc     = "filament_ramming_volumetric_speed_nc";
constexpr const char* k_filament_ramming_travel_time_nc   = "filament_ramming_travel_time_nc";
constexpr const char* k_filament_change_length_nc         = "filament_change_length_nc";
constexpr const char* k_filament_prime_volume             = "filament_prime_volume";
constexpr const char* k_filament_prime_volume_nc          = "filament_prime_volume_nc";
constexpr const char* k_filament_retract_length_nc        = "filament_retract_length_nc";
constexpr const char* k_filament_retract_lift_nc          = "filament_retract_lift_nc";
constexpr const char* k_filament_retract_speed_nc         = "filament_retract_speed_nc";
constexpr const char* k_filament_deretract_speed_nc       = "filament_deretract_speed_nc";
constexpr const char* k_prime_volume_mode                 = "prime_volume_mode";

// Hardware
constexpr const char* k_nozzle_volume_type                = "nozzle_volume_type";
constexpr const char* k_extruder_max_nozzle_count         = "extruder_max_nozzle_count";
constexpr const char* k_extruder_nozzle_stats             = "extruder_nozzle_stats";
constexpr const char* k_machine_hotend_change_time        = "machine_hotend_change_time";
constexpr const char* k_extruder_variant_list             = "extruder_variant_list";
constexpr const char* k_default_nozzle_volume_type        = "default_nozzle_volume_type";

// Capability
constexpr const char* k_enable_filament_dynamic_map       = "enable_filament_dynamic_map";
constexpr const char* k_has_filament_switcher             = "has_filament_switcher";

// Placeholder (GCode runtime)
constexpr const char* k_pp_extruders_unloaded_mask        = "vortek_extruders_unloaded_mask";
constexpr const char* k_pp_toolchange_count               = "vortek_toolchange_count";
constexpr const char* k_pp_last_filament_id               = "vortek_last_filament_id";

} // namespace Keys
} // namespace Vortek
