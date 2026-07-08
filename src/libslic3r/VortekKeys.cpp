#include "VortekKeys.hpp"

// VortekKeys.cpp — Declarative registry implementation.
// All Vortek-relevant config keys with sync and expansion attributes.
//
// Reference to BBS: BambuStudio/src/libslic3r/PrintConfig.cpp
// (extruder_retract_keys, filament_options_with_variant)

namespace Vortek {
namespace Keys {

using O = Origin;
using G = Group;

// ─── Registry ───
//
// Column order:
//   name, origin, group,
//   computed, sync_align, sync_baseline,
//   variant_expanded, needs_variant_override

const std::vector<KeyDef>& registry() {
    static const std::vector<KeyDef> r = {

        // ═══════════════════════════════════════════════════════════════
        // Mapping — computed by Vortek hooks, suppressed from diffs
        // ═══════════════════════════════════════════════════════════════
        // Booleans: [1] computed  [2] sync_align  [3] sync_baseline
        //           [4] variant_expanded  [5] needs_variant_override
        {k_filament_map,              O::BBS,    G::Mapping,
            true, true, true, false, false},
        // filament_volume_map: computed=true, sync_align=FALSE.
        // In Manual mode: user-set — must not be overwritten by align_incoming_config.
        // In Auto mode: computed by ensure_nozzle_group_result — propagates via
        //   filter_print_diff_set (L323: full_print_config sync) + BackgroundSlicingProcess save.
        // BBS has no align_incoming_config — these values flow through plate→project_config.
        // Reference to BBS: BambuStudio/src/libslic3r/PrintApply.cpp L1400-1421
        {k_filament_volume_map,       O::Vortek, G::Mapping,
            true, false, true, false, false},
        // filament_nozzle_map: computed=true, sync_align=FALSE.
        // "not used in gui studio" (BBS PrintApply L1421) — erase from diff in Manual,
        // propagate via filter_print_diff_set in Auto. No align needed.
        {k_filament_nozzle_map,       O::Vortek, G::Mapping,
            true, false, true, false, false},
        {k_filament_map_2,            O::Vortek, G::Mapping,
            true, true, true, false, false},
        {k_filament_extruder_variant, O::BBS,    G::Mapping,
            true, true, true, false, false},
        {k_physical_extruder_map,     O::BBS,    G::Mapping,
            true, true, true, false, false},
        {k_filament_self_index,       O::BBS,    G::Mapping,
            true, true, true, false, false},

        // ═══════════════════════════════════════════════════════════════
        // Thermal — sync, variant expanded
        // ═══════════════════════════════════════════════════════════════
        {k_nozzle_temperature,              O::BBS,    G::Thermal,
            false, true, true, true, true},
        {k_nozzle_temperature_initial_layer, O::BBS,   G::Thermal,
            false, true, true, true, true},
        {k_filament_pre_cooling_temp_nc,    O::Vortek, G::Thermal,
            false, true, true, false, false},
        {k_filament_pre_cooling_temp,       O::Vortek, G::Thermal,
            false, false, false, false, false},
        {k_filament_preheat_temp_delta,     O::Vortek, G::Thermal,
            false, false, false, false, false},
        {k_hotend_cooling_rate,             O::Vortek, G::Thermal,
            false, false, false, false, false},
        {k_hotend_heating_rate,             O::Vortek, G::Thermal,
            false, false, false, false, false},
        {k_enable_pre_heating,              O::Vortek, G::Thermal,
            false, false, false, false, false},

        // ═══════════════════════════════════════════════════════════════
        // BBS Retract keys: sync_align=false — align must NOT copy post-override
        // values into new_full_config, because print_config_diffs will re-apply
        // apply_override and get double-override → false diff → reslice loop.
        // sync_baseline=true — copy post-override values into m_config so that
        // print_config_diffs comparison (m_config vs recomputed) yields 0 diff.
        // ═══════════════════════════════════════════════════════════════
        // Booleans: [1] computed  [2] sync_align  [3] sync_baseline
        //           [4] variant_expanded  [5] needs_variant_override
        {"retraction_length",           O::BBS, G::Retract,
            false, false, true, false, true},
        {"retraction_speed",            O::BBS, G::Retract,
            false, false, true, false, true},
        {"deretraction_speed",          O::BBS, G::Retract,
            false, false, true, false, true},
        {"retract_before_wipe",         O::BBS, G::Retract,
            false, false, true, false, true},
        {"retract_lift_above",          O::BBS, G::Retract,
            false, false, true, false, true},
        {"retract_lift_below",          O::BBS, G::Retract,
            false, false, true, false, true},
        {"retract_lift_enforce",        O::BBS, G::Retract,
            false, false, true, false, true},
        {"retract_restart_extra",       O::BBS, G::Retract,
            false, false, true, false, true},
        {"retract_when_changing_layer", O::BBS, G::Retract,
            false, false, true, false, true},
        {"retraction_minimum_travel",   O::BBS, G::Retract,
            false, false, true, false, true},
        {"wipe",                        O::BBS, G::Retract,
            false, false, true, false, true},
        {"wipe_distance",               O::BBS, G::Retract,
            false, false, true, false, true},
        {"z_hop",                       O::BBS, G::Retract,
            false, false, true, false, true},
        {"z_hop_types",                 O::BBS, G::Retract,
            false, false, true, false, true},
        {"travel_slope",                O::BBS, G::Retract,
            false, false, true, false, true},
        {"long_retractions_when_cut",   O::BBS, G::Retract,
            false, false, true, false, true},
        {"retraction_distances_when_cut", O::BBS, G::Retract,
            false, false, true, false, true},


        // ═══════════════════════════════════════════════════════════════
        // Nozzle Change — Vortek-specific, no sync needed
        // ═══════════════════════════════════════════════════════════════
        {k_filament_ramming_vol_speed_nc,   O::Vortek, G::NozzleChange,
            false, false, false, false, false},
        {k_filament_ramming_travel_time_nc, O::Vortek, G::NozzleChange,
            false, false, false, false, false},
        {k_filament_change_length_nc,       O::Vortek, G::NozzleChange,
            false, false, false, false, false},
        {k_filament_prime_volume,           O::Vortek, G::NozzleChange,
            false, false, false, false, false},
        {k_filament_prime_volume_nc,        O::Vortek, G::NozzleChange,
            false, false, false, false, false},
        {k_filament_retract_length_nc,      O::Vortek, G::NozzleChange,
            false, false, false, false, false},
        {k_filament_retract_lift_nc,        O::Vortek, G::NozzleChange,
            false, false, false, false, false},
        {k_filament_retract_speed_nc,       O::Vortek, G::NozzleChange,
            false, false, false, false, false},
        {k_filament_deretract_speed_nc,     O::Vortek, G::NozzleChange,
            false, false, false, false, false},
        {k_prime_volume_mode,               O::Vortek, G::NozzleChange,
            false, false, false, false, false},

        // ═══════════════════════════════════════════════════════════════
        // Hardware
        // ═══════════════════════════════════════════════════════════════
        {k_extruder_max_nozzle_count,       O::Vortek, G::Hardware,
            false, false, false, false, false},
        {k_extruder_nozzle_stats,           O::Vortek, G::Hardware,
            false, false, false, false, false},
        {k_machine_hotend_change_time,      O::Vortek, G::Hardware,
            false, false, false, false, false},
        {k_extruder_variant_list,           O::BBS,    G::Hardware,
            false, false, false, false, false},
        {k_default_nozzle_volume_type,      O::BBS,    G::Hardware,
            false, false, false, false, false},

        // ═══════════════════════════════════════════════════════════════
        // Capability
        // ═══════════════════════════════════════════════════════════════
        {k_enable_filament_dynamic_map,     O::Vortek, G::Capability,
            false, false, false, false, false},
        {k_has_filament_switcher,           O::Vortek, G::Capability,
            false, false, false, false, false},
    };
    return r;
}

// ─── Pre-built sets ───

static std::unordered_set<std::string> build_set(bool KeyDef::* attr) {
    std::unordered_set<std::string> s;
    for (const auto& k : registry())
        if (k.*attr)
            s.insert(k.name);
    return s;
}

const std::unordered_set<std::string>& computed_set() {
    static auto s = build_set(&KeyDef::computed);
    return s;
}

const std::unordered_set<std::string>& sync_align_set() {
    static auto s = build_set(&KeyDef::sync_align);
    return s;
}

const std::unordered_set<std::string>& sync_baseline_set() {
    static auto s = build_set(&KeyDef::sync_baseline);
    return s;
}

const std::unordered_set<std::string>& variant_override_set() {
    static auto s = build_set(&KeyDef::needs_variant_override);
    return s;
}

// ─── Query functions ───

bool needs_variant_override(const std::string& key) {
    return variant_override_set().count(key) > 0;
}

size_t filter_by_attr(std::unordered_set<std::string>& diff, bool KeyDef::* attr) {
    size_t suppressed = 0;
    for (const auto& k : registry()) {
        if (k.*attr)
            suppressed += diff.erase(k.name);
    }
    return suppressed;
}

} // namespace Keys
} // namespace Vortek
