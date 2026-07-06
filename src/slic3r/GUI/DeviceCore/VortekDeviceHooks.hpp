#ifndef VORTEK_DEVICE_HOOKS_HPP
#define VORTEK_DEVICE_HOOKS_HPP

#include <memory>
#include <set>
#include <string>
#include <optional>
#include <cstdint>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include <wx/string.h>

#include "DevDefs.h"
#include "DevNozzleSystem.h"
#include "DevFirmware.h"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
class PresetBundle;
class MachineObject;
class DevAms;
class DevNozzleSystem;
struct DevNozzle;
class VortekNozzleRack;
class VortekNozzleMappingCtrl;
class VortekFilaSwitch;
class DevFilaSystem;
namespace GUI {
class PartPlate;
class Plater;
}
}

namespace Vortek {
namespace DeviceHooks {

bool is_nozzle_empty(const Slic3r::DevNozzle& nozzle);
bool is_nozzle_unknown(const Slic3r::DevNozzle& nozzle);
bool is_nozzle_info_reliable(const Slic3r::DevNozzle& nozzle);
bool is_nozzle_abnormal(const Slic3r::DevNozzle& nozzle);
std::string get_nozzle_diameter_str(const Slic3r::DevNozzle& nozzle);
Slic3r::NozzleFlowType get_nozzle_flow_type(const Slic3r::DevNozzle& nozzle);
int get_logic_extruder_id(const Slic3r::DevNozzle& nozzle);
std::string get_nozzle_wear(const Slic3r::DevNozzle& nozzle);
std::string get_nozzle_filament_id(const Slic3r::DevNozzle& nozzle, const Slic3r::DevNozzleSystem* system = nullptr, bool is_on_rack = false);
std::string get_custom_filament_name(const Slic3r::DevNozzleSystem* system, const std::string& id);
void set_custom_filament_name(const Slic3r::DevNozzleSystem* system, const std::string& id, const std::string& name);
std::string get_nozzle_filament_color(const Slic3r::DevNozzle& nozzle, const Slic3r::DevNozzleSystem* system = nullptr, bool is_on_rack = false);
void parse_nozzle_filament(Slic3r::DevNozzleSystem* system, int nozzle_id, const nlohmann::json& njon);
bool is_nozzle_normal(const Slic3r::DevNozzle& nozzle);
int get_nozzle_id(const Slic3r::DevNozzle& nozzle);
std::string to_nozzle_flow_string(Slic3r::NozzleFlowType flow_type);
wxString get_nozzle_type_str(const Slic3r::DevNozzle& nozzle);
wxString get_nozzle_flow_type_str(const Slic3r::DevNozzle& nozzle);
std::string get_nozzle_type_string(Slic3r::NozzleType type);
Slic3r::DevFirmwareVersionInfo get_nozzle_firmware_info(const Slic3r::DevNozzle& nozzle, const Slic3r::DevNozzleSystem* system);
Slic3r::NozzleDiameterType get_nozzle_diameter_type(const Slic3r::DevNozzle& nozzle);
std::optional<int> get_replace_nozzle_tar(const Slic3r::DevNozzleSystem* system);

/**
 * @brief Extract bit flags from a hex string without going out of bounds.
 * @param str The source hex string (optionally starting with 0x).
 * @param start_idx The starting bit index.
 * @param count The number of bits to extract.
 * @return The extracted value as a 32-bit unsigned integer.
 */
uint32_t get_flag_bits_no_border(const std::string& str, int start_idx, int count = 1);

/**
 * @brief Handles AMS extruder bindings when extruder ID is 0xE (V1 protocol).
 * @param obj The MachineObject representing the printer.
 * @param ams_item The JSON object of the AMS from MQTT.
 * @param extruder_id Output physical extruder ID (updated to MAIN_EXTRUDER_ID if FTS is installed).
 * @param binded_extruder_set Output set of binded extruders.
 * @param binded_switcher_pos Output switcher position (0: POS_IN_B, 1: POS_IN_A).
 * @return true if the AMS is valid and parsing should continue; false otherwise.
 */
bool handle_ams_extruder_binding(
    const Slic3r::MachineObject* obj,
    const json& ams_item,
    int& extruder_id,
    std::set<int>& binded_extruder_set,
    std::optional<int>& binded_switcher_pos);

/**
 * @brief Applies the binded extruders and switcher position to the active DevAms object.
 * @param curr_ams The DevAms unit being updated.
 * @param binded_extruder_set The set of binded extruders.
 * @param binded_switcher_pos The optional binded switcher position.
 */
void assign_ams_bindings(
    Slic3r::DevAms* curr_ams,
    const std::set<int>& binded_extruder_set,
    const std::optional<int>& binded_switcher_pos);

std::set<int> get_ams_binded_extruder_set(const Slic3r::DevAms* ams);
std::optional<int> get_ams_binded_switcher_pos(const Slic3r::DevAms* ams);

/**
 * @brief Analyzes nozzle ID and places it either in physical extruder nozzles or in the rack.
 * @param system The DevNozzleSystem manager.
 * @param nozzle_obj The nozzle object to process.
 * @param raw_id The raw nozzle ID containing position and on-rack flag bits.
 */
void process_nozzle_placement(
    Slic3r::DevNozzleSystem* system,
    Slic3r::DevNozzle& nozzle_obj,
    int raw_id);

bool is_h2c_printer(const Slic3r::MachineObject* obj);
void store_wtm_firmware_info(Slic3r::MachineObject* obj, const Slic3r::DevFirmwareVersionInfo& info);
void clear_wtm_firmware_info(Slic3r::MachineObject* obj);

/**
 * @brief Synchronizes nozzle configurations from a connected machine to the current PresetBundle.
 * @param obj The MachineObject representing the printer.
 * @param preset_bundle The active PresetBundle to sync into.
 */
void sync_machine_nozzle_inventory_to_preset(const Slic3r::MachineObject* obj, Slic3r::PresetBundle& preset_bundle);

// Reference to BBS equivalent: DevNozzleSystem::ClearNozzles() in BambuStudio/src/slic3r/GUI/DeviceCore/DevNozzleSystem.cpp:458
void reset_nozzle_system(Slic3r::DevNozzleSystem* system);

void set_support_nozzle_rack(Slic3r::MachineObject* obj, bool supported);
void parse_device_state(Slic3r::MachineObject* obj, const nlohmann::json& device_json);

std::shared_ptr<Slic3r::VortekNozzleRack> get_or_create_nozzle_rack(Slic3r::MachineObject* obj);
std::shared_ptr<Slic3r::VortekNozzleRack> get_nozzle_rack(const Slic3r::DevNozzleSystem* system);
bool is_nozzle_on_rack_helper(const Slic3r::DevNozzleSystem* system, int nozzle_id);
bool contains_ext_nozzle(const Slic3r::DevNozzleSystem* system, int nozzle_id);
std::vector<std::vector<std::vector<float>>> get_full_flush_matrix_helper(const Slic3r::PresetBundle* preset_bundle);
Slic3r::DevNozzle get_nozzle_by_pos_id(const Slic3r::DevNozzleSystem* system, int pos_id);
int get_nozzle_pos_id(const Slic3r::DevNozzle& nozzle, const Slic3r::DevNozzleSystem* system);

std::shared_ptr<Slic3r::VortekNozzleMappingCtrl> get_or_create_nozzle_mapping(Slic3r::MachineObject* obj);
std::shared_ptr<Slic3r::VortekNozzleMappingCtrl> get_nozzle_mapping(const Slic3r::MachineObject* obj);
std::shared_ptr<Slic3r::VortekFilaSwitch> get_or_create_fila_switch(Slic3r::MachineObject* obj);
std::shared_ptr<Slic3r::VortekFilaSwitch> get_fila_switch(const Slic3r::MachineObject* obj);
void init_device_mappings(Slic3r::MachineObject* obj);
void clear_all_device_mappings(Slic3r::MachineObject* obj);
void clear_auto_nozzle_mapping(Slic3r::MachineObject* obj);
void preprocess_filament_json(Slic3r::MachineObject* obj, nlohmann::json& filament_json);
void apply_pending_ams_bindings(Slic3r::DevFilaSystem* fila_system);
bool apply_nozzle_mapping_from_device(Slic3r::MachineObject* obj, Slic3r::GUI::PartPlate* plate);

// H2C Vortek hook: single entry point for nozzle variant visibility in the extruder combo.
// Encapsulates both the standard preset check (extruder_variant_list) and H2C-specific
// Hybrid mode logic (nvtHybrid shown only for H2C with carousel rack).
// Reference to BBS: BambuStudio/src/slic3r/GUI/Plater.cpp extruder_variant_list lambda,
//   extruder_max_nozzle_count > 1 triggers Hybrid display.
// Parameters:
//   printer_model       — e.g. "Bambu Lab H2C"
//   variant_list_entry  — extruder_variants->values[extruder_idx] (comma-separated preset variants)
//   extruder_type_label — extruders_def->enum_labels[extruders->values[extruder_idx]]
//   nozzle_volumes_def  — the ConfigOptionEnumGeneric* for printer_nozzle_volume_type
//   nozzle_type_idx     — loop index i into nozzle_volumes_def
//   max_nozzle_count_opt— extruder_max_nozzle_count option (may be nullptr)
//   extruder_idx        — which extruder (0=left/DEPUTY, 1=right/MAIN for H2C)
bool should_show_nozzle_variant(
    const std::string&                              printer_model,
    const std::string&                              variant_list_entry,
    const std::string&                              extruder_type_label,
    const Slic3r::ConfigOptionDef*                  nozzle_volumes_def,
    size_t                                          nozzle_type_idx,
    const Slic3r::ConfigOptionIntsNullable*         max_nozzle_count_opt,
    int                                             extruder_idx);

// ---------------------------------------------------------------------------
// Tab extruder-tab UI hooks (H2C Hybrid)
// ---------------------------------------------------------------------------

/**
 * @brief For H2C Hybrid extruders, generates the two sub-tab labels.
 *        For all other printers / volume types, returns an empty vector
 *        (caller falls through to the standard single-tab path).
 *
 * @param printer_model    e.g. "Bambu Lab H2C"
 * @param extruder_name    Localised extruder name, e.g. "Right"
 * @param volume_type      NozzleVolumeType of the current extruder
 * @return vector of 2 wxStrings {"Right: Standard", "Right: High Flow"},
 *         or empty if no expansion is needed.
 *
 * Reference to BBS: BambuStudio/src/slic3r/GUI/Tab.cpp
 *   generate_extruder_options, nvtHybrid block.
 */
std::vector<wxString> get_hybrid_extruder_tab_names(
    const std::string&         printer_model,
    const wxString&            extruder_name,
    Slic3r::NozzleVolumeType   volume_type);

/**
 * @brief Returns the tab-list index for (extruder_id, nozzle_type),
 *        accounting for H2C Hybrid extruders occupying 2 slots each.
 *
 * @param printer_model    e.g. "Bambu Lab H2C"
 * @param extruder_nums    total number of extruders
 * @param volume_values    nozzle_volume_type values array (one per extruder)
 * @param extruder_id      target extruder index
 * @param nozzle_type      which sub-type to resolve (Standard / High Flow)
 * @return tab list index, or 0 on error.
 *
 * Reference to BBS: BambuStudio/src/slic3r/GUI/Tab.cpp
 *   calculate_selection_index_for_extruder, nvtHybrid block.
 */
int calculate_extruder_tab_selection_index(
    const std::string&                printer_model,
    int                               extruder_nums,
    const std::vector<int>&           volume_values,
    int                               extruder_id,
    Slic3r::NozzleVolumeType          nozzle_type);

/**
 * @brief Inverse of calculate_extruder_tab_selection_index.
 *        Given a flat tab-strip selection index, resolves (extruder_id, nozzle_type)
 *        accounting for H2C Hybrid extruders occupying 2 tab slots each.
 *
 * @param printer_model    e.g. "Bambu Lab H2C"
 * @param selection        flat tab index selected by the user
 * @param extruder_nums    total number of extruders
 * @param volume_values    nozzle_volume_type values array (one per extruder)
 * @param out_extruder_id  [out] resolved extruder index
 * @param out_nozzle_type  [out] resolved NozzleVolumeType (nvtStandard or nvtHighFlow)
 * @return true if the selection was handled (H2C Hybrid slot); false otherwise.
 *
 * Reference to BBS: BambuStudio/src/slic3r/GUI/Tab.cpp
 *   parse_extruder_selection, nvtHybrid block.
 */
bool parse_hybrid_extruder_selection(
    const std::string&              printer_model,
    int                             selection,
    int                             extruder_nums,
    const std::vector<int>&         volume_values,
    int&                            out_extruder_id,
    Slic3r::NozzleVolumeType&       out_nozzle_type);

// H2C Vortek hooks for synchronizing nozzle flow types and volume maps
// Reference to BBS: BambuStudio/src/slic3r/GUI/Plater.cpp: update_filament_volume_map
void update_filament_volume_map(Slic3r::GUI::Plater* plater, int extruder_id, int volume_type);

// Reference to BBS: BambuStudio/src/slic3r/GUI/Tab.cpp: set_extruder_volume_type
void on_extruder_volume_type_changed(Slic3r::PresetBundle* preset_bundle, int extruder_id, Slic3r::NozzleVolumeType type);

// Reference to BBS: BambuStudio/src/slic3r/GUI/Tab.cpp: select_preset
void sync_extruder_nozzle_stats_on_preset_select(Slic3r::PresetBundle* preset_bundle, const std::string& base_preset_name);

} // namespace DeviceHooks
} // namespace Vortek

#endif // VORTEK_DEVICE_HOOKS_HPP
