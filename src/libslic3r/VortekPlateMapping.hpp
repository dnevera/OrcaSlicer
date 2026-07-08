#ifndef VORTEK_PLATE_MAPPING_HPP
#define VORTEK_PLATE_MAPPING_HPP

#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include <vector>
#include <unordered_set>

namespace Slic3r {
    class PresetBundle;
}

namespace Vortek {

struct LoadMappingResult {
    std::vector<int> filament_nozzle_map;
    std::vector<int> filament_volume_map;
};

/**
 * @brief Handles mapping and synchronization of H2C nozzle/filament configuration for print plates.
 */
class PlateMapping {
public:
    /**
     * @brief Checks if H2C multi-nozzle features are enabled for the current print configuration.
     * 
     * @param print Pointer to the Print object
     * @return True if H2C is active, false otherwise
     */
    static bool is_h2c_multi_nozzle(const Slic3r::Print* print);

    /**
     * @brief Synchronizes configuration overrides to the active plate config after slicing completes.
     * 
     * @param plate_config Configuration of the active plate
     * @param filament_map_mode Mapping mode active
     * @param print Pointer to the Print object containing slicing results
     * @param preset_bundle Reference to the preset bundle
     */
    static void sync_after_slicing(
        Slic3r::DynamicPrintConfig& plate_config,
        Slic3r::FilamentMapMode filament_map_mode,
        const Slic3r::Print* print,
        Slic3r::PresetBundle& preset_bundle
    );

    /**
     * @brief Resizes nozzle/volume maps on filament count change.
     */
    static void handle_filament_count_changed(Slic3r::DynamicPrintConfig* config, int filament_count);

    /**
     * @brief Appends default nozzle/volume mappings when a new filament is added.
     */
    static void handle_filament_added(Slic3r::DynamicPrintConfig* config);

    /**
     * @brief Erases nozzle/volume mappings at a given index when a filament is deleted.
     */
    static void handle_filament_deleted(Slic3r::DynamicPrintConfig* config, int filament_id);

    /**
     * @brief Clears all nozzle/volume mapping keys from the config.
     */
    static void clear_mappings(Slic3r::DynamicPrintConfig* config);



    /**
     * @brief Ensures loaded project configs have matching nozzle/volume map dimensions.
     */
    static void sync_project_config_on_load(Slic3r::DynamicConfig& proj_cfg, int filament_count);

    /**
     * @brief Retrieves the actual nozzle map from Print if active, otherwise falls back to plate config.
     */
    static std::vector<int> get_nozzle_map_for_export(const Slic3r::Print* print, const Slic3r::DynamicPrintConfig& plate_config);

    /**
     * @brief Retrieves the actual volume map from Print if active, otherwise falls back to plate config.
     */
    static std::vector<int> get_volume_map_for_export(const Slic3r::Print* print, const Slic3r::DynamicPrintConfig& plate_config);



    /**
     * @brief Sets group_id in slice_filaments_info from filament_nozzle_map after parse_filament_info.
     *
     * Must be called AFTER PlateData::parse_filament_info(), as parse_filament_info() overwrites
     * slice_filaments_info and leaves group_id empty. Without group_id set, bbs_3mf.cpp falls
     * back to filament_maps[i]-1 (0-based extruder index) instead of carousel nozzle slot ID.
     *
     * @param plate_data  PlateData whose slice_filaments_info will be patched
     * @param filament_nozzle_map  Per-filament nozzle slot IDs (0=Left, 1-3=Right carousel)
     */
    static void patch_slice_filament_nozzle_groups(
        Slic3r::PlateData* plate_data,
        const std::vector<int>& filament_nozzle_map
    );

    /**
     * @brief Formats and patches nozzle configurations inside PlateData during project export.
     */
    static void patch_plate_data_for_export(
        Slic3r::PlateData* plate_data,
        const std::vector<int>& filament_nozzle_map,
        const std::vector<int>& filament_volume_map,
        const std::vector<int>& filament_maps,
        const Slic3r::DynamicPrintConfig& config,
        const Slic3r::Print* print = nullptr
    );









    /**
     * @brief Filters Vortek computed map keys from print_diff and handles vector size differences.
     * Does NOT filter full_config_diff (BBS never does — L1394/1402/1410 commented out).
     */
    static void filter_reslice_diffs(
        const Slic3r::Print& print,
        const Slic3r::ConfigBase& new_full_config,
        Slic3r::t_config_option_keys& print_diff,
        Slic3r::t_config_option_keys& full_config_diff);

    /**
     * @brief Syncs computed keys that were suppressed from print_diff directly into m_config.
     * Reproduces BBS L1398/1406/1414: m_config.filament_volume_map = *new_opt.
     * Reference to BBS: BambuStudio/src/libslic3r/PrintApply.cpp L1386-1415
     */
    static void sync_suppressed_to_config(
        Slic3r::PrintConfig& config,
        const Slic3r::DynamicPrintConfig& new_full_config);

    /**
     * @brief Logs config diff keys and values for debugging.
     */
    static void diag_log_config_diffs(
        const char* label,
        const Slic3r::t_config_option_keys& diff_keys,
        const Slic3r::ConfigBase& old_cfg,
        const Slic3r::ConfigBase& new_cfg);


    /**
     * @brief Overrides upstream filament variant expansion with BBS-style nozzle_group_result mapping.
     *
     * On second Print::apply() (after slicing computed nozzle groups), replaces upstream
     * update_values_to_printer_extruders_for_multiple_filaments expansion with
     * update_filament_config_values_for_multiple_extruders using the dynamic nozzle map.
     * No-op for non-H2C printers and on first apply (no group result yet).
     *
     * Reference to BBS: BambuStudio/src/libslic3r/PrintApply.cpp L1338-1362
     */
    static void override_filament_variant_expansion(
        Slic3r::Print& print,
        Slic3r::DynamicPrintConfig& new_full_config,
        const Slic3r::DynamicPrintConfig& ori_full_config);


    /**
     * @brief Checks if two printer models are compatible (with fallback mapping like O1C <-> O1C2).
     * 
     * @param model1 First printer model name
     * @param model2 Second printer model name
     * @return True if models are compatible, false otherwise
     */
    static bool are_models_compatible(const std::string& model1, const std::string& model2);
};

} // namespace Vortek

#endif // VORTEK_PLATE_MAPPING_HPP
