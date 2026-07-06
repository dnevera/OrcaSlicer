#ifndef VORTEK_PRINT_HOOKS_HPP
#define VORTEK_PRINT_HOOKS_HPP

#include <vector>
#include <memory>
#include <unordered_map>
#include <set>

namespace Slic3r {
    class Print;
    class PrintConfigDef;
    class PresetBundle;
    class AppConfig;
    class DynamicPrintConfig;
    class PrintConfig;
    class Preset;
    class ConfigBase;
    struct ExtruderNozleInfo;
    namespace MultiNozzleUtils {
        class NozzleGroupResultBase;
    }
}

namespace Vortek {

bool is_h2c_printer(const Slic3r::Print& print);
bool is_h2c_printer(const Slic3r::PrintConfig& config);
bool is_h2c_printer(const Slic3r::ConfigBase& config);
bool is_h2c_printer(const Slic3r::PresetBundle* preset_bundle);
bool is_h2c_printer(const Slic3r::Preset& preset);

/**
 * @brief Delegate hooks for modifying configuration values on Slic3r::Print.
 * 
 * Contains helpers for remapping physical extruders, overriding retraction distances, 
 * temperatures, and registering configuration schemas.
 */
class PrintHooks {
public:
    /**
     * @brief Registers the 18 custom Vortek H2C-specific parameters into the slicer's global configuration registry.
     * 
     * @param def Pointer to the PrintConfigDef object containing the configuration definition
     */
    static void init_vortek_params(Slic3r::PrintConfigDef* def);

    /**
     * @brief Remaps and trims logical filament configurations to align with physical active nozzles.
     * 
     * @param print Reference to the Print object
     * @param f_maps Extruder mapping array
     * @param f_volume_maps Volume/Nozzle type mapping array
     * @param f_nozzle_maps Physical nozzle slot index mapping array
     */
    static void update_filament_maps_to_config(
        Slic3r::Print& print,
        const std::vector<int>& f_maps,
        const std::vector<int>& f_volume_maps,
        const std::vector<int>& f_nozzle_maps
    );

    /**
     * @brief Evaluates the nozzle group result and updates all active parameters (diameters, retracts, flow caps) on the Print config.
     * 
     * @param print Reference to the Print object
     * @param group_result Reference to the resolved nozzle grouping result
     * @endif
     */
    static void update_to_config_by_nozzle_group_result(
        Slic3r::Print& print,
        const Slic3r::MultiNozzleUtils::NozzleGroupResultBase& group_result
    );

    /**
     * @brief Reads the filament nozzle mapping from the Print config.
     */
    static std::vector<int> get_filament_nozzle_maps(const Slic3r::Print& print);

    /**
     * @brief Reads the filament volume mapping from the Print config.
     */
    static std::vector<int> get_filament_volume_maps(const Slic3r::Print& print);

    /**
     * @brief Computes derived mappings (filament_map_2 and physical_extruder_map) from raw filament maps.
     */
    static void compute_vortek_derived_maps(
        const Slic3r::Print& print,
        const std::vector<int>& f_maps,
        const std::vector<int>& final_volume_maps,
        std::vector<int>& out_filament_map_2,
        std::vector<int>& out_physical_extruder_map
    );

    /**
     * @brief Silently applies derived mappings to the Print object config without triggering slicer invalidation.
     */
    static void silent_update_derived_maps(
        Slic3r::Print& print,
        const std::vector<int>& f_maps,
        const std::vector<int>& final_volume_maps
    );

    /**
     * @brief Adjusts the filament change purge volume. For H2C, bypasses purging if it is a carousel nozzle change.
     */
    static float adjust_purge_volume(
        const Slic3r::Print& print,
        int current_filament_id,
        int next_filament_id,
        size_t layer_idx,
        float default_volume
    );

    // Reference to BBS: BambuStudio/src/libslic3r/PrintApply.cpp L1341-1358
    static bool apply_h2c_variant_overrides(
        Slic3r::Print& print,
        Slic3r::DynamicPrintConfig& new_full_config
    );

private:
    static void update_filament_config_values_for_multiple_extruders(
        Slic3r::DynamicPrintConfig &printer_config,
        const std::unordered_map<int, std::vector<Slic3r::ExtruderNozleInfo>> &filament_extruder_nozzle_infos,
        int extruder_count,
        int extruder_nozzle_volume_count,
        std::set<std::string> &key_set,
        std::string id_name,
        std::string variant_name
    );
};

// Reference to BBS: BambuStudio/src/libslic3r/PresetBundle.hpp
class PresetBundleHooks {
public:
    static void load_nozzle_stats_from_config(
        Slic3r::PresetBundle* preset_bundle, 
        Slic3r::AppConfig& config, 
        const std::string& initial_printer_profile_name
    );

    static void save_nozzle_stats_to_config(
        const Slic3r::PresetBundle* preset_bundle, 
        Slic3r::AppConfig& config, 
        const std::string& printer_name
    );

    static void load_nozzle_stats_from_dynamic_config(
        Slic3r::PresetBundle* preset_bundle, 
        Slic3r::DynamicPrintConfig& config
    );

    static void update_nozzle_stat_on_compatibility_change(
        Slic3r::PresetBundle* preset_bundle
    );
};

} // namespace Vortek

#endif // VORTEK_PRINT_HOOKS_HPP
