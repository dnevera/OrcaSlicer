#include "VortekPrintHooks.hpp"
#include "Print.hpp"
#include "PrintConfig.hpp"
#include "VortekLog.hpp"
#include "PresetBundle.hpp"
#include "AppConfig.hpp"
#include "Preset.hpp"
#include "libslic3r/Config.hpp"
#include <boost/format.hpp>
#include <boost/algorithm/string.hpp>
#include <set>
#include <unordered_map>

namespace Vortek {

bool is_h2c_printer(const Slic3r::Print& print) {
    return print.config().printer_model.value == "Bambu Lab H2C";
}

bool is_h2c_printer(const Slic3r::PrintConfig& config) {
    return config.printer_model.value == "Bambu Lab H2C";
}

bool is_h2c_printer(const Slic3r::ConfigBase& config) {
    if (config.has("printer_model")) {
        auto* opt = config.option("printer_model");
        if (auto* opt_str = dynamic_cast<const Slic3r::ConfigOptionString*>(opt)) {
            return opt_str->value == "Bambu Lab H2C";
        }
    }
    return false;
}

bool is_h2c_printer(const Slic3r::PresetBundle* preset_bundle) {
    if (!preset_bundle) return false;
    return is_h2c_printer(preset_bundle->printers.get_edited_preset());
}

bool is_h2c_printer(const Slic3r::Preset& preset) {
    return is_h2c_printer(preset.config);
}

template<typename OptType, typename ValueType>
static void trim_option_values(OptType *opt, const std::vector<int> &trim_param_indices)
{
    std::vector<ValueType> new_values;
    new_values.reserve(trim_param_indices.size());

    for (int idx : trim_param_indices) {
        new_values.emplace_back(opt->get_at(idx));
    }

    opt->values = std::move(new_values);
}

static void update_filament_config_values_for_multiple_extruders(
    Slic3r::DynamicPrintConfig &printer_config,
    const std::unordered_map<int, std::vector<Slic3r::ExtruderNozleInfo>> &filament_extruder_nozzle_infos,
    int extruder_count,
    int extruder_nozzle_volume_count,
    std::set<std::string> &key_set,
    std::string id_name,
    std::string variant_name)
{
    std::vector<int> filament_maps  = printer_config.option<Slic3r::ConfigOptionInts>("filament_map")->values;
    size_t           filament_count = filament_maps.size();

    auto opt_extruder_type      = dynamic_cast<const Slic3r::ConfigOptionEnumsGeneric *>(printer_config.option("extruder_type"));
    auto opt_nozzle_volume_type = dynamic_cast<const Slic3r::ConfigOptionEnumsGeneric *>(printer_config.option("nozzle_volume_type"));

    auto             opt_filament_volume_maps = dynamic_cast<const Slic3r::ConfigOptionInts *>(printer_config.option("filament_volume_map"));
    std::vector<int> filament_volume_maps;
    if (opt_filament_volume_maps) filament_volume_maps = opt_filament_volume_maps->values;
    auto             opt_ids = id_name.empty() ? nullptr : dynamic_cast<const Slic3r::ConfigOptionInts *>(printer_config.option(id_name));

    std::vector<int> trim_param_indices;
    trim_param_indices.reserve(filament_count * 2);
    for (int f_index = 0; f_index < (int)filament_count; f_index++) {
        Slic3r::ExtruderType extruder_type = (Slic3r::ExtruderType) (opt_extruder_type->get_at(filament_maps[f_index] - 1));
        Slic3r::NozzleVolumeType nozzle_volume_type = (Slic3r::NozzleVolumeType) (opt_nozzle_volume_type->get_at(filament_maps[f_index] - 1));
        auto iter = filament_extruder_nozzle_infos.find(f_index);
        if (iter != filament_extruder_nozzle_infos.end()) {
            std::vector<Slic3r::ExtruderNozleInfo> nozzle_infos = iter->second;
            for (Slic3r::ExtruderNozleInfo nozzle_info : nozzle_infos) {
                extruder_type = nozzle_info.extruder_type;
                nozzle_volume_type = nozzle_info.nozzle_volume_type;
                int param_index = printer_config.get_index_for_extruder(f_index + 1, id_name, extruder_type, nozzle_volume_type, variant_name);
                if (param_index < 0) {
                    param_index = 0;
                    if (opt_ids) {
                        for (int i = 0; i < (int)opt_ids->values.size(); i++) {
                            if (opt_ids->values[i] == (f_index + 1)) {
                                param_index = i;
                                break;
                            }
                        }
                    }
                }
                trim_param_indices.push_back(param_index);
            }
        } else {
            // filament not used in slicing
            if ((extruder_nozzle_volume_count > extruder_count) && (!filament_volume_maps.empty())) {
                nozzle_volume_type = (Slic3r::NozzleVolumeType) (filament_volume_maps[f_index]);
            }
            int param_index = printer_config.get_index_for_extruder(f_index + 1, id_name, extruder_type, nozzle_volume_type, variant_name);
            if (param_index < 0) {
                param_index = 0;
                if (opt_ids) {
                    for (int i = 0; i < (int)opt_ids->values.size(); i++) {
                        if (opt_ids->values[i] == (f_index + 1)) {
                            param_index = i;
                            break;
                        }
                    }
                }
            }
            trim_param_indices.push_back(param_index);
        }
    }

    const Slic3r::ConfigDef *config_def = printer_config.def();
    if (!config_def) return;
    for (auto &key : key_set) {
        const Slic3r::ConfigOptionDef *optdef = config_def->get(key);
        if (!optdef) continue;
        switch (optdef->type) {
        case Slic3r::coStrings: {
            trim_option_values<Slic3r::ConfigOptionStrings, std::string>(printer_config.option<Slic3r::ConfigOptionStrings>(key), trim_param_indices);
            break;
        }
        case Slic3r::coInts: {
            trim_option_values<Slic3r::ConfigOptionInts, int>(printer_config.option<Slic3r::ConfigOptionInts>(key), trim_param_indices);
            break;
        }
        case Slic3r::coFloats: {
            trim_option_values<Slic3r::ConfigOptionFloats, double>(printer_config.option<Slic3r::ConfigOptionFloats>(key), trim_param_indices);
            break;
        }
        case Slic3r::coFloatsOrPercents: {
            trim_option_values<Slic3r::ConfigOptionFloatsOrPercents, Slic3r::FloatOrPercent>(printer_config.option<Slic3r::ConfigOptionFloatsOrPercents>(key), trim_param_indices);
            break;
        }
        case Slic3r::coBools: {
            trim_option_values<Slic3r::ConfigOptionBools, unsigned char>(printer_config.option<Slic3r::ConfigOptionBools>(key), trim_param_indices);
            break;
        }
        case Slic3r::coEnums: {
            trim_option_values<Slic3r::ConfigOptionEnumsGeneric, int>(printer_config.option<Slic3r::ConfigOptionEnumsGeneric>(key), trim_param_indices);
            break;
        }
        default: break;
        }
    }
}

void PrintHooks::update_filament_maps_to_config(
    Slic3r::Print& print,
    const std::vector<int>& f_maps,
    const std::vector<int>& f_volume_maps,
    const std::vector<int>& f_nozzle_maps
)
{
    if (!is_h2c_printer(print)) {
        return;
    }
    // Step 1: Compute final_nozzle_maps FIRST so we can use them in the idempotency guard.
    // If f_nozzle_maps is empty or contains carousel slot collisions (multiple filaments on Extruder 2 mapping to the same slot),
    // we recalculate and derive nozzle assignments from filament_map using the H2C carousel rule:
    //   extruder 1 (Left)  → fixed slot 0
    //   extruder 2 (Right) → carousel slots 4, 3, 2, 1 (in order of first assignment)
    // Reference to BBS: BambuStudio/src/libslic3r/Preset.cpp PresetBundle::update_compatible (H2C carousel mappings)
    // Reference to BBS: BambuStudio/src/libslic3r/Format/bbs_3mf.cpp (nozzle map initialization)
    std::vector<int> final_nozzle_maps = f_nozzle_maps;
    bool needs_mapping = final_nozzle_maps.empty();
    
    if (!needs_mapping && !f_maps.empty() && final_nozzle_maps.size() == f_maps.size()) {
        std::set<int> carousel_slots_used;
        bool has_collision = false;
        for (size_t i = 0; i < final_nozzle_maps.size(); ++i) {
            if (f_maps[i] == 2) {
                int slot = final_nozzle_maps[i];
                if (carousel_slots_used.count(slot)) {
                    has_collision = true;
                }
                carousel_slots_used.insert(slot);
            }
        }
        if (has_collision) {
            VORTEK_LOG(warn, "update_filament_maps_to_config: detected slot collisions in loaded nozzle map, recalculating...");
            needs_mapping = true;
        }
    }

    if ((needs_mapping || final_nozzle_maps.size() != f_maps.size()) && !f_maps.empty()) {
        final_nozzle_maps.assign(f_maps.size(), 0);
        int next_carousel_nozzle = 4;
        for (size_t i = 0; i < f_maps.size(); ++i) {
            int ext_id = f_maps[i]; // 1-based (1 = Left, 2 = Right)
            if (ext_id == 1) {
                final_nozzle_maps[i] = 0;
            } else if (ext_id == 2) {
                final_nozzle_maps[i] = next_carousel_nozzle--;
                if (next_carousel_nozzle < 1)
                    next_carousel_nozzle = 4;
            }
        }
    } else if (!final_nozzle_maps.empty() && !f_maps.empty()) {
        // Sanitize nozzle map to ensure no slot ID exceeds the H2C physical limit of 0..4.
        // Slots should be: 0 for Left, 1..4 for Right (Carousel).
        for (size_t i = 0; i < final_nozzle_maps.size() && i < f_maps.size(); ++i) {
            int ext_id = f_maps[i]; // 1-based (1 = Left, 2 = Right)
            if (ext_id == 1) { // Left
                if (final_nozzle_maps[i] != 0) {
                    VORTEK_LOG(warning, "update_filament_maps_to_config: invalid left nozzle slot " 
                               << final_nozzle_maps[i] << " reset to 0");
                    final_nozzle_maps[i] = 0;
                }
            } else if (ext_id == 2) { // Right (Carousel slots 1..4)
                if (final_nozzle_maps[i] < 1 || final_nozzle_maps[i] > 4) {
                    VORTEK_LOG(warning, "update_filament_maps_to_config: invalid right nozzle slot " 
                               << final_nozzle_maps[i] << " reset to default carousel slot");
                    final_nozzle_maps[i] = 1 + (i % 4);
                }
            }
        }
    }

    // Step 2: Compute final_volume_maps from printer nozzle_volume_type per extruder (if not provided).
    std::vector<int> final_volume_maps = f_volume_maps;
    if (final_volume_maps.empty() && !f_maps.empty()) {
        auto opt_nozzle_volume_type = dynamic_cast<const Slic3r::ConfigOptionEnumsGeneric*>(
            print.m_ori_full_print_config.option("nozzle_volume_type"));
        final_volume_maps.resize(f_maps.size(), Slic3r::nvtStandard);
        for (size_t i = 0; i < f_maps.size(); ++i) {
            int ext_idx = f_maps[i] - 1; // 0-based
            if (opt_nozzle_volume_type && ext_idx >= 0 && ext_idx < (int)opt_nozzle_volume_type->size())
                final_volume_maps[i] = opt_nozzle_volume_type->get_at(ext_idx);
        }
    }

    // Step 2b: Normalize nvtHybrid → nvtStandard in volume map for H2C printers.
    // nvtHybrid is a UI-level indicator for the Hybrid extruder mode; it is NOT a physical
    // nozzle type. The carousel slots are Standard-type nozzles regardless of Hybrid mode.
    // LayeredNozzleGroupResult::create() matches each filament by (extruder_id, volume_type):
    //   - nozzle_list is built from extruder_nozzle_stats where Right carousel = nvtStandard
    //     (fixed in on_printer_model_change: nvtHybrid → nvtStandard, count=max_nozzle_count).
    //   - Therefore filament volume_type must also be nvtStandard, not nvtHybrid.
    // BBL reference: filament_volume_map=['0','0','0','0','0'] = all nvtStandard.
    // Reference to BBS: BambuStudio/src/libslic3r/Format/bbs_3mf.cpp filament_volume_map init.
    if (is_h2c_printer(print)) {
        for (auto& v : final_volume_maps) {
            if (v == static_cast<int>(Slic3r::nvtHybrid))
                v = static_cast<int>(Slic3r::nvtStandard);
        }
    }

    // Step 3: Always build and set LayeredNozzleGroupResult on the print object
    // to enable the GCodeProcessor's PreCooling and PreHeating post-processors.
    if (!f_maps.empty()) {
        std::vector<unsigned int> used_filaments;
        for (size_t i = 0; i < f_maps.size(); ++i) {
            used_filaments.push_back(i);
        }
        auto opt_stats = print.m_full_print_config.option<Slic3r::ConfigOptionStrings>("extruder_nozzle_stats");
        std::vector<std::string> stats_values = opt_stats ? opt_stats->values : std::vector<std::string>();
        auto nozzle_stats = Slic3r::MultiNozzleUtils::get_extruder_nozzle_stats(stats_values);

        auto opt_dia = print.m_full_print_config.option<Slic3r::ConfigOptionFloats>("nozzle_diameter");
        float nozzle_dia = (opt_dia && !opt_dia->values.empty()) ? (float)opt_dia->values.front() : 0.4f;

        std::vector<int> zero_based_filament_map = f_maps;
        std::transform(zero_based_filament_map.begin(), zero_based_filament_map.end(), zero_based_filament_map.begin(), [](int v) { return v - 1; });

        auto nozzle_result = Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult::create(
            used_filaments,
            zero_based_filament_map,
            final_volume_maps,
            final_nozzle_maps,
            nozzle_stats,
            nozzle_dia
        );

        if (nozzle_result) {
            print.set_nozzle_group_result(std::make_shared<Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult>(*nozzle_result));
            VORTEK_LOG(warn, "update_filament_maps_to_config: initialized m_nozzle_group_result in Print");
        } else {
            VORTEK_LOG(error, "update_filament_maps_to_config: failed to create LayeredNozzleGroupResult");
        }
    }

    // Calculate and silently update filament_map_2 and physical_extruder_map
    // BEFORE the idempotency guard, ensuring they are always correct.
    silent_update_derived_maps(print, f_maps, final_volume_maps);

    // Step 4: Idempotency guard — compare m_config against COMPUTED values.
    // This ensures that on the 2nd re-slice the values already match → no write → no invalidation → cycle stops.
    bool maps_changed   = (print.config().filament_map.values        != f_maps);
    bool volume_changed = (print.config().filament_volume_map.values != final_volume_maps);
    bool nozzle_changed = (!final_nozzle_maps.empty() &&
                           print.config().filament_nozzle_map.values != final_nozzle_maps);

    if (maps_changed || volume_changed || nozzle_changed) {
        VORTEK_LOG(warn, "update_filament_maps_to_config: maps changed, applying to full configs...");

        if (maps_changed) {
            if (auto* opt = print.m_ori_full_print_config.option<Slic3r::ConfigOptionInts>("filament_map", true)) {
                opt->values = f_maps;
            }
            if (auto* opt = print.m_full_print_config.option<Slic3r::ConfigOptionInts>("filament_map", true)) {
                opt->values = f_maps;
            }
        }

        if (volume_changed) {
            if (auto* opt = print.m_ori_full_print_config.option<Slic3r::ConfigOptionInts>("filament_volume_map", true)) {
                opt->values = final_volume_maps;
            }
            if (auto* opt = print.m_full_print_config.option<Slic3r::ConfigOptionInts>("filament_volume_map", true)) {
                opt->values = final_volume_maps;
            }
        }

        if (nozzle_changed) {
            VORTEK_LOG(warn, "update_filament_maps_to_config: applying filament_nozzle_map to full configs");
            if (auto* opt = print.m_ori_full_print_config.option<Slic3r::ConfigOptionInts>("filament_nozzle_map", true)) {
                opt->values = final_nozzle_maps;
            }
            if (auto* opt = print.m_full_print_config.option<Slic3r::ConfigOptionInts>("filament_nozzle_map", true)) {
                opt->values = final_nozzle_maps;
            }
        }
    } else {
        VORTEK_LOG(debug, "update_filament_maps_to_config: all maps unchanged, skipping (idempotent)");
        return; // Nothing to do — stop here to avoid unnecessary work
    }

    // Step 5: Rebuild extruder retract overrides (only when something changed).
    {
        // Rebuild m_full_print_config so that printer extruders are populated.
        print.m_full_print_config = print.m_ori_full_print_config;

        std::set<std::string> filament_keys = Slic3r::filament_options_with_variant;
        filament_keys.insert("filament_self_index");
        print.m_full_print_config.update_values_to_printer_extruders_for_multiple_filaments(
            print.m_full_print_config, filament_keys, "filament_self_index", "filament_extruder_variant");

        const std::vector<std::string>& extruder_retract_keys = Slic3r::print_config_def.extruder_retract_keys();
        const std::string               filament_prefix       = "filament_";
        Slic3r::t_config_option_keys    print_diff;
        Slic3r::DynamicPrintConfig      filament_overrides;

        for (auto& opt_key : extruder_retract_keys) {
            const Slic3r::ConfigOption* opt_new_filament = print.m_full_print_config.option(filament_prefix + opt_key);
            const Slic3r::ConfigOption* opt_new_machine  = print.m_full_print_config.option(opt_key);
            const Slic3r::ConfigOption* opt_old_machine  = print.m_full_print_config.option(opt_key);
            if (opt_new_filament)
                Slic3r::compute_filament_override_value(opt_key, opt_old_machine, opt_new_machine,
                    opt_new_filament, print.m_full_print_config, print_diff, filament_overrides,
                    print.m_full_print_config.option<Slic3r::ConfigOptionInts>("filament_map_2")->values);
        }

        if (!print_diff.empty()) {
            print.m_placeholder_parser.apply_config(filament_overrides);
            // Apply directly to full configs instead of m_config to prevent invalidation loop
            print.m_full_print_config.apply(filament_overrides);
            print.m_ori_full_print_config.apply(filament_overrides);
        }
    }
}


void PrintHooks::update_to_config_by_nozzle_group_result(
    Slic3r::Print& print,
    const Slic3r::MultiNozzleUtils::NozzleGroupResultBase& group_result
)
{
    if (!is_h2c_printer(print)) {
        return;
    }
    const auto* layered_result = dynamic_cast<const Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult*>(&group_result);
    if (layered_result) {
        std::vector<int> nozzle_map = layered_result->get_nozzle_map(-1);
        if (auto* opt = print.m_ori_full_print_config.option<Slic3r::ConfigOptionInts>("filament_nozzle_map", true)) {
            opt->values = nozzle_map;
        }
        if (auto* opt = print.m_full_print_config.option<Slic3r::ConfigOptionInts>("filament_nozzle_map", true)) {
            opt->values = nozzle_map;
        }
        VORTEK_LOG(warn, "update_to_config_by_nozzle_group_result: updated filament_nozzle_map in full configs");
    }

    int extruder_count = print.config().nozzle_diameter.values.size();
    VORTEK_LOG(warn, "update_to_config_by_nozzle_group_result: carriage count = " << extruder_count);
    int extruder_volume_type_count = 1;

    std::unordered_map<int, std::vector<Slic3r::ExtruderNozleInfo>> filament_extruder_map;

    auto filament_count = print.config().option<Slic3r::ConfigOptionStrings>("filament_type")->size();
    auto extruder_type  = print.config().option<Slic3r::ConfigOptionEnumsGeneric>("extruder_type")->values;

    for (int fidx = 0; fidx < (int)filament_count; ++fidx) {
        auto used_nozzles = group_result.get_nozzles_for_filament(fidx);
        std::set<Slic3r::ExtruderNozleInfo> extruder_nozzle_set;
        for (auto nozzle : used_nozzles) {
            Slic3r::ExtruderNozleInfo tmp;
            tmp.extruder_type = Slic3r::ExtruderType(extruder_type[nozzle.extruder_id]);
            tmp.nozzle_volume_type = nozzle.volume_type;
            extruder_nozzle_set.insert(tmp);
        }
        filament_extruder_map[fidx] = std::vector<Slic3r::ExtruderNozleInfo>(extruder_nozzle_set.begin(), extruder_nozzle_set.end());
    }

    print.m_full_print_config = print.m_ori_full_print_config;
    std::set<std::string> filament_keys = Slic3r::filament_options_with_variant;
    filament_keys.insert("filament_self_index");
    update_filament_config_values_for_multiple_extruders(print.m_full_print_config, filament_extruder_map, extruder_count, extruder_volume_type_count,
                                                          filament_keys, "filament_self_index", "filament_extruder_variant");

    const std::vector<std::string> &extruder_retract_keys = Slic3r::print_config_def.extruder_retract_keys();
    const std::string               filament_prefix       = "filament_";
    Slic3r::t_config_option_keys            print_diff;
    Slic3r::DynamicPrintConfig              filament_overrides;
    for (auto &opt_key : extruder_retract_keys) {
        const Slic3r::ConfigOption *opt_new_filament = print.m_full_print_config.option(filament_prefix + opt_key);
        const Slic3r::ConfigOption *opt_new_machine  = print.m_full_print_config.option(opt_key);
        const Slic3r::ConfigOption *opt_old_machine  = print.m_full_print_config.option(opt_key);

        if (opt_new_filament)
            Slic3r::compute_filament_override_value(opt_key, opt_old_machine, opt_new_machine, opt_new_filament, print.m_full_print_config, print_diff, filament_overrides,
                                            print.m_full_print_config.option<Slic3r::ConfigOptionInts>("filament_map_2")->values);
    }

    if (!print_diff.empty()) {
        print.m_placeholder_parser.apply_config(filament_overrides);
        print.m_full_print_config.apply(filament_overrides);
        print.m_ori_full_print_config.apply(filament_overrides);
    }
}

#ifndef L
#define L(s) (s)
#endif

void PrintHooks::init_vortek_params(Slic3r::PrintConfigDef* def_ptr)
{
    using namespace Slic3r;
    VORTEK_LOG(warn, "init_vortek_params: registering 18 Vortek configuration parameters");

    ConfigOptionDef* def = def_ptr->add("extruder_max_nozzle_count", coInts);
    def->mode = comDevelop;
    def->nullable = true;
    def->set_default_value(new ConfigOptionIntsNullable{ 1 });

    def = def_ptr->add("extruder_nozzle_stats", coStrings);
    def->set_default_value(new ConfigOptionStrings { });

    def = def_ptr->add("enable_filament_dynamic_map", coBool);
    def->label = "Enable filament dynamic map";
    def->tooltip = "Support filament map to different nozzle";
    def->set_default_value(new ConfigOptionBool{ false });

    def = def_ptr->add("has_filament_switcher", coBool);
    def->label = "Has filament switcher";
    def->tooltip = "Whether a filament switcher is connected to the printer";
    def->set_default_value(new ConfigOptionBool{ false });

    def = def_ptr->add("prime_volume_mode", coEnum);
    def->enum_values.push_back("Default");
    def->enum_values.push_back("Saving");
    def->enum_values.push_back("Fast");
    def->enum_labels.push_back(L("Default"));
    def->enum_labels.push_back(L("Saving"));
    def->enum_labels.push_back(L("Fast"));
    def->enum_keys_map = &ConfigOptionEnum<PrimeVolumeMode>::get_enum_values();
    def->set_default_value(new ConfigOptionEnum<PrimeVolumeMode>{ PrimeVolumeMode::pvmDefault });

    def = def_ptr->add("machine_hotend_change_time", coFloat);
    def->label = L("Hotend change time");
    def->tooltip = L("Time to change hotend.");
    def->sidetext = L("s");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloat(0.0));

    def = def_ptr->add("hotend_cooling_rate", coFloats);
    def->nullable = true;
    def->set_default_value(new ConfigOptionFloatsNullable{2});

    def = def_ptr->add("hotend_heating_rate", coFloats);
    def->nullable = true;
    def->set_default_value(new ConfigOptionFloatsNullable{2});

    def = def_ptr->add("enable_pre_heating", coBool);
    def->set_default_value(new ConfigOptionBool(false));

    def = def_ptr->add("filament_nozzle_map", coInts);
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInts{1});

    def = def_ptr->add("filament_volume_map", coInts);
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInts{(int)(NozzleVolumeType::nvtStandard)});

    def = def_ptr->add("filament_map_2", coInts);
    def->label = "Filament map plus for multi nozzle";
    def->tooltip = "Filament map to the index identified by extruder and nozzle_volume_type";
    def->mode = comDevelop;
    def->set_default_value(new ConfigOptionInts{1});

    // Reference to BBS: BambuStudio/src/libslic3r/PrintConfig.cpp — filament_pre_cooling_temperature
    def = def_ptr->add("filament_pre_cooling_temperature", coInts);
    def->label = L("Extruder change");
    def->tooltip = L("To prevent oozing, the nozzle temperature will be cooled during ramming. 0 means disabled.");
    def->mode = comAdvanced;
    def->sidetext = "°C";
    def->min = 0;
    def->nullable = true;
    def->set_default_value(new ConfigOptionIntsNullable{0});

    def = def_ptr->add("filament_pre_cooling_temperature_nc", coInts);
    def->mode = comAdvanced;
    def->sidetext = "°C";
    def->min = 0;
    def->nullable = true;
    def->set_default_value(new ConfigOptionIntsNullable{0});

    // Reference to BBS: BambuStudio/src/libslic3r/PrintConfig.cpp — filament_preheat_temperature_delta
    def = def_ptr->add("filament_preheat_temperature_delta", coFloats);
    def->label = L("Preheat temperature delta");
    def->tooltip = L("Temperature delta applied during pre-heating before tool change.");
    def->sidetext = "°C";
    def->mode = comDevelop;
    def->nullable = true;
    def->set_default_value(new ConfigOptionFloatsNullable{0});

    def = def_ptr->add("filament_ramming_volumetric_speed_nc", coFloats);
    def->label = L("Hotend change");
    def->tooltip = L("The maximum volumetric speed for ramming before a hotend change, where -1 means using the maximum volumetric speed.");
    def->sidetext = L("mm³/s");
    def->min = -1;
    def->max = 200;
    def->mode = comAdvanced;
    def->nullable = true;
    def->set_default_value(new ConfigOptionFloatsNullable{-1});

    def = def_ptr->add("filament_ramming_travel_time_nc", coFloats);
    def->label = L("Hotend change");
    def->tooltip = L("To prevent oozing, the nozzle will perform a reverse travel movement for a certain period after the ramming is complete. The setting define the travel time.");
    def->sidetext = "s";
    def->min = 0;
    def->nullable = true;
    def->set_default_value(new ConfigOptionFloatsNullable{0});

    def = def_ptr->add("filament_change_length_nc", coFloats);
    def->label = L("Hotend change");
    def->tooltip = L("When changing the hotend, it is recommended to extrude a certain length of filament from the original nozzle. This helps minimize nozzle oozing.");
    def->sidetext = L("mm");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloats{10});

    def = def_ptr->add("filament_prime_volume", coFloats);
    def->label = L("Filament change");
    def->tooltip = L("The volume of material required to prime the extruder on the tower, excluding a hotend change.");
    def->sidetext = L("mm³");
    def->min = 1.0;
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionFloats{45.});

    def = def_ptr->add("filament_prime_volume_nc", coFloats);
    def->label = L("Hotend change");
    def->tooltip = L("The volume of material required to prime the extruder for a hotend change on the tower.");
    def->sidetext = L("mm³");
    def->min = 1.0;
    def->mode = comSimple;
    def->set_default_value(new ConfigOptionFloats{60.});

    def = def_ptr->add("filament_retract_length_nc", coFloats);
    def->label = L("Nozzle Changer retraction length");
    def->tooltip = L("The length of retraction when changing a nozzle carriage");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloats{0.0});

    def = def_ptr->add("filament_retract_lift_nc", coFloats);
    def->label = L("Nozzle Changer retract lift");
    def->tooltip = L("The Z-hop distance when changing a nozzle carriage");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionFloats{0.0});

    def = def_ptr->add("filament_retract_speed_nc", coInts);
    def->label = L("Nozzle Changer retract speed");
    def->tooltip = L("The speed of retraction when changing a nozzle carriage");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionInts{0});

    def = def_ptr->add("filament_deretract_speed_nc", coInts);
    def->label = L("Nozzle Changer deretract speed");
    def->tooltip = L("The speed of deretraction when changing a nozzle carriage");
    def->min = 0;
    def->mode = comAdvanced;
    def->set_default_value(new ConfigOptionInts{0});
}

std::vector<int> PrintHooks::get_filament_nozzle_maps(const Slic3r::Print& print)
{
    if (is_h2c_printer(print) && print.full_print_config().has("filament_nozzle_map")) {
        return print.full_print_config().option<Slic3r::ConfigOptionInts>("filament_nozzle_map")->values;
    }
    return {};
}

std::vector<int> PrintHooks::get_filament_volume_maps(const Slic3r::Print& print)
{
    if (is_h2c_printer(print) && print.full_print_config().has("filament_volume_map")) {
        return print.full_print_config().option<Slic3r::ConfigOptionInts>("filament_volume_map")->values;
    }
    return {};
}

void PrintHooks::compute_vortek_derived_maps(
    const Slic3r::Print& print,
    const std::vector<int>& f_maps,
    const std::vector<int>& final_volume_maps,
    std::vector<int>& out_filament_map_2,
    std::vector<int>& out_physical_extruder_map
) {
    if (f_maps.empty())
        return;

    // 1. Rebuild temporary config to obtain print_extruder_id assignments
    Slic3r::DynamicPrintConfig temp_full_config = print.m_ori_full_print_config;
    std::set<std::string> filament_keys = Slic3r::filament_options_with_variant;
    filament_keys.insert("filament_self_index");
    temp_full_config.update_values_to_printer_extruders_for_multiple_filaments(
        temp_full_config, filament_keys, "filament_self_index", "filament_extruder_variant");

    // 2. Compute filament_map_2
    out_filament_map_2.resize(f_maps.size(), 0);
    auto opt_extruder_type = dynamic_cast<const Slic3r::ConfigOptionEnumsGeneric*>(
        print.m_ori_full_print_config.option("extruder_type"));
    auto opt_nozzle_volume_type = dynamic_cast<const Slic3r::ConfigOptionEnumsGeneric*>(
        print.m_ori_full_print_config.option("nozzle_volume_type"));

    for (size_t index = 0; index < f_maps.size(); index++) {
        Slic3r::ExtruderType extruder_type = Slic3r::etDirectDrive;
        if (opt_extruder_type && (int)index < (int)opt_extruder_type->size())
            extruder_type = (Slic3r::ExtruderType)(opt_extruder_type->get_at(f_maps[index] - 1));

        Slic3r::NozzleVolumeType nozzle_volume_type = Slic3r::nvtStandard;
        if (!final_volume_maps.empty() && index < final_volume_maps.size())
            nozzle_volume_type = (Slic3r::NozzleVolumeType)(final_volume_maps[index]);
        else if (opt_nozzle_volume_type && (int)(f_maps[index] - 1) < (int)opt_nozzle_volume_type->size())
            nozzle_volume_type = (Slic3r::NozzleVolumeType)(opt_nozzle_volume_type->get_at(f_maps[index] - 1));

        int idx = temp_full_config.get_index_for_extruder(
            f_maps[index], "print_extruder_id", extruder_type, nozzle_volume_type, "print_extruder_variant");
        out_filament_map_2[index] = (idx >= 0) ? idx : (f_maps[index] - 1);
    }

    // 3. Read physical_extruder_map from printer preset (hardware property, not dependent on filament mapping mode)
    // Reference to BBS: physical_extruder_map defines the hardware mapping between logical extruder indices
    // and physical hotend T-numbers (e.g., [1,0] means extruder 0 → T1, extruder 1 → T0).
    // This is a fixed printer property defined in fdm_bbl_3dp_002_common.json and must NOT be
    // recalculated from print_extruder_id, which changes between auto/manual filament mapping modes.
    out_physical_extruder_map.clear();
    const auto* opt_preset_phys_map = print.m_ori_full_print_config.option<Slic3r::ConfigOptionInts>("physical_extruder_map");
    if (opt_preset_phys_map && !opt_preset_phys_map->values.empty()) {
        out_physical_extruder_map = opt_preset_phys_map->values;
        VORTEK_LOG(warn, "compute_vortek_derived_maps: using preset physical_extruder_map=["
            << out_physical_extruder_map[0] << "," << (out_physical_extruder_map.size() > 1 ? std::to_string(out_physical_extruder_map[1]) : "?") << "]");
    } else {
        // Fallback: compute from print_extruder_id if preset is missing
        const auto* opt_extruder_ids = temp_full_config.option<Slic3r::ConfigOptionInts>("print_extruder_id");
        if (opt_extruder_ids) {
            for (int ext_id : opt_extruder_ids->values) {
                out_physical_extruder_map.push_back(ext_id - 1);
            }
        }
        VORTEK_LOG(warning, "compute_vortek_derived_maps: preset physical_extruder_map missing, computed fallback");
    }
}

void PrintHooks::silent_update_derived_maps(
    Slic3r::Print& print,
    const std::vector<int>& f_maps,
    const std::vector<int>& final_volume_maps
) {
    std::vector<int> computed_map_2;
    std::vector<int> calculated_physical_map;
    compute_vortek_derived_maps(print, f_maps, final_volume_maps, computed_map_2, calculated_physical_map);

    if (computed_map_2.empty())
        return;

    // Apply ONLY to full and ori configs (do NOT touch print.m_config to avoid invalidation loops!)
    if (auto* opt = print.m_ori_full_print_config.option<Slic3r::ConfigOptionInts>("filament_map_2", true)) {
        opt->values = computed_map_2;
    }
    if (auto* opt = print.m_full_print_config.option<Slic3r::ConfigOptionInts>("filament_map_2", true)) {
        opt->values = computed_map_2;
    }

    // Note: physical_extruder_map is NOT updated here — it is a hardware property
    // from the printer preset (fdm_bbl_3dp_002_common.json) and must remain constant.
    // Reference to BBS: BambuStudio always uses preset physical_extruder_map=[1,0] regardless
    // of filament mapping mode (auto/manual).
}

float PrintHooks::adjust_purge_volume(
    const Slic3r::Print& print,
    int current_filament_id,
    int next_filament_id,
    size_t layer_idx,
    float default_volume
) {
    // Hook for H2C only
    if (!is_h2c_printer(print)) {
        return default_volume;
    }

    auto group_result = print.get_layered_nozzle_group_result();
    if (!group_result) {
        return default_volume;
    }

    // Reference to BBS: BambuStudio/src/libslic3r/Print.cpp
    // If it's a physical nozzle change on the same extruder (carousel switch), no purging is required.
    bool is_nozzle_change = group_result->are_filaments_same_extruder(current_filament_id, next_filament_id, layer_idx) &&
                           !group_result->are_filaments_same_nozzle(current_filament_id, next_filament_id, layer_idx);
    
    if (is_nozzle_change) {
        return 0.0f;
    }

    return default_volume;
}

#undef L

// Reference to BBS: BambuStudio/src/libslic3r/PresetBundle.cpp
void PresetBundleHooks::load_nozzle_stats_from_config(
    Slic3r::PresetBundle* preset_bundle, 
    Slic3r::AppConfig& config, 
    const std::string& initial_printer_profile_name) 
{
    if (!is_h2c_printer(preset_bundle)) {
        return;
    }

    std::vector<std::string> extruder_nozzle_stats_str;
    if (config.has_printer_setting(initial_printer_profile_name, "extruder_nozzle_stats")) {
        boost::algorithm::split(extruder_nozzle_stats_str, config.get_printer_setting(initial_printer_profile_name, "extruder_nozzle_stats"), boost::algorithm::is_any_of(","));
    }
    preset_bundle->extruder_nozzle_stat.set_raw_stat(Slic3r::MultiNozzleUtils::get_extruder_nozzle_stats(extruder_nozzle_stats_str));
}

void PresetBundleHooks::save_nozzle_stats_to_config(
    const Slic3r::PresetBundle* preset_bundle, 
    Slic3r::AppConfig& config, 
    const std::string& printer_name) 
{
    if (!is_h2c_printer(preset_bundle)) {
        return;
    }

    std::string extruder_nozzle_stats_str = boost::algorithm::join(
        Slic3r::save_extruder_nozzle_stats_to_string(preset_bundle->extruder_nozzle_stat.get_raw_stat()), 
        ","
    );
    config.set_printer_setting(printer_name, "extruder_nozzle_stats", extruder_nozzle_stats_str);
}

void PresetBundleHooks::load_nozzle_stats_from_dynamic_config(
    Slic3r::PresetBundle* preset_bundle, 
    Slic3r::DynamicPrintConfig& config) 
{
    if (!is_h2c_printer(preset_bundle)) {
        return;
    }

    if (config.has("extruder_nozzle_stats")) {
        std::vector<std::string> extruder_nozzle_stats = std::move(config.option<Slic3r::ConfigOptionStrings>("extruder_nozzle_stats", true)->values);
        config.erase("extruder_nozzle_stats");
        preset_bundle->extruder_nozzle_stat.set_raw_stat(Slic3r::MultiNozzleUtils::get_extruder_nozzle_stats(extruder_nozzle_stats));
    }
}

void PresetBundleHooks::update_nozzle_stat_on_compatibility_change(
    Slic3r::PresetBundle* preset_bundle) 
{
    const Slic3r::Preset &printer_preset = preset_bundle->printers.get_edited_preset();
    if (is_h2c_printer(printer_preset)) {
        preset_bundle->extruder_nozzle_stat.on_printer_model_change(preset_bundle);
    }
}

} // namespace Vortek

