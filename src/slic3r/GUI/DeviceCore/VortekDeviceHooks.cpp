#include "VortekDeviceHooks.hpp"
#include "VortekProtocolExtension.h"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceCore/DevNozzleSystem.h"
#include "slic3r/GUI/DeviceCore/VortekNozzleRack.h"
#include "slic3r/GUI/DeviceCore/VortekFilaSwitch.h"
#include "slic3r/GUI/DeviceCore/VortekMappingNozzle.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevUtil.h"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "libslic3r/VortekMultiNozzle.hpp"
#include "libslic3r/VortekLog.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <mutex>

#include <map>
#include <vector>
#include <algorithm>
#include <cctype>
#include <limits>
#include <boost/algorithm/string.hpp>

// ---------------------------------------------------------------------------
// H2C Debug: VORTEK_DEBUG_HF_NOZZLE_OVERRIDE is defined in VortekLog.hpp.
// Set it to true there to enable the full HF pipeline test (both CAPACITY and
// ASSIGNMENT sides must be flipped simultaneously).
// ---------------------------------------------------------------------------

namespace Vortek {

namespace DeviceHooks {

class VortekNozzleFilamentManager {
public:
    struct NozzleFilamentInfo {
        std::string id;
        std::string color;
    };

    static VortekNozzleFilamentManager& get_instance() {
        static VortekNozzleFilamentManager instance;
        return instance;
    }

    // --- Per-device nozzle slot data ---
    // Key: stable dev_id string (printer serial), not a raw pointer

    void set_filament_info(const Slic3r::DevNozzleSystem* system, int nozzle_id, const std::string& id, const std::string& color) {
        std::string dev_id = s_dev_id_of(system);
        if (dev_id.empty()) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_nozzle_filaments[dev_id][nozzle_id] = {id, color};
    }

    std::string get_filament_id(const Slic3r::DevNozzleSystem* system, int nozzle_id) {
        std::string dev_id = s_dev_id_of(system);
        if (dev_id.empty()) return "";
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_nozzle_filaments.find(dev_id);
        if (it != m_nozzle_filaments.end()) {
            auto it2 = it->second.find(nozzle_id);
            if (it2 != it->second.end()) return it2->second.id;
        }
        return "";
    }

    std::string get_filament_color(const Slic3r::DevNozzleSystem* system, int nozzle_id) {
        std::string dev_id = s_dev_id_of(system);
        if (dev_id.empty()) return "";
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_nozzle_filaments.find(dev_id);
        if (it != m_nozzle_filaments.end()) {
            auto it2 = it->second.find(nozzle_id);
            if (it2 != it->second.end()) return it2->second.color;
        }
        return "";
    }

    // Clears only per-device nozzle slot data on disconnect/reset.
    // The global filament name cache (m_custom_filament_names) is intentionally NOT cleared:
    // filament IDs are universal and names remain valid after printer reconnects.
    void clear_for_system(const Slic3r::DevNozzleSystem* system) {
        std::string dev_id = s_dev_id_of(system);
        if (dev_id.empty()) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_nozzle_filaments.erase(dev_id);
    }

    // --- Global filament name registry ---
    // filament_id -> display_name is universal: same ID = same filament on any printer.
    // Not scoped per-device. system parameter kept for API compatibility.

    void set_custom_filament_name(const Slic3r::DevNozzleSystem* /*system*/, const std::string& id, const std::string& name) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_custom_filament_names[id] = name;
    }

    std::string get_custom_filament_name(const Slic3r::DevNozzleSystem* /*system*/, const std::string& id) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_custom_filament_names.find(id);
        return (it != m_custom_filament_names.end()) ? it->second : "";
    }

private:
    VortekNozzleFilamentManager() = default;
    ~VortekNozzleFilamentManager() = default;
    VortekNozzleFilamentManager(const VortekNozzleFilamentManager&) = delete;
    VortekNozzleFilamentManager& operator=(const VortekNozzleFilamentManager&) = delete;

    // Returns the stable dev_id string for the given nozzle system.
    // Reference to BBS: BambuStudio/src/slic3r/GUI/DeviceManager.hpp#L107 (dev_id field)
    static std::string s_dev_id_of(const Slic3r::DevNozzleSystem* system) {
        if (!system || !system->GetOwner()) return "";
        return system->GetOwner()->get_dev_id();
    }

    std::mutex m_mutex;

    // Per-device nozzle slot data: dev_id -> nozzle_slot_id -> filament info
    // Cleared on printer disconnect/reset.
    std::map<std::string, std::map<int, NozzleFilamentInfo>> m_nozzle_filaments;

    // Global filament name registry: filament_id -> display_name
    // Shared across all printers. Never cleared (filament IDs are universally stable).
    std::map<std::string, std::string> m_custom_filament_names;
};


static std::map<const Slic3r::MachineObject*, std::shared_ptr<Slic3r::VortekNozzleRack>> s_nozzle_racks;
static std::map<const Slic3r::DevAms*, std::set<int>> s_ams_binded_extruders;
static std::map<const Slic3r::DevAms*, std::optional<int>> s_ams_binded_switcher_pos;
static std::map<const Slic3r::MachineObject*, std::shared_ptr<Slic3r::VortekNozzleMappingCtrl>> s_nozzle_mappings;
static std::map<const Slic3r::MachineObject*, std::shared_ptr<Slic3r::VortekFilaSwitch>> s_fila_switches;

// Forward declaration — defined at line ~679 (after store_wtm_firmware_info section).
bool is_h2c_printer(const Slic3r::MachineObject* obj);


bool is_nozzle_empty(const Slic3r::DevNozzle& nozzle) {
    return nozzle.m_nozzle_id == -1 || nozzle.m_nozzle_type == Slic3r::ntUndefine || nozzle.m_diameter < 0.01f;
}

bool is_nozzle_unknown(const Slic3r::DevNozzle& nozzle) {
    return nozzle.m_nozzle_id == -1 || nozzle.m_nozzle_type == Slic3r::ntUndefine;
}

bool is_nozzle_info_reliable(const Slic3r::DevNozzle& nozzle) {
    return !is_nozzle_unknown(nozzle) && nozzle.m_diameter > 0.01f;
}

bool is_nozzle_abnormal(const Slic3r::DevNozzle& nozzle) {
    return nozzle.m_diameter < 0.01f;
}

std::string get_nozzle_diameter_str(const Slic3r::DevNozzle& nozzle) {
    return std::to_string(nozzle.m_diameter);
}

Slic3r::NozzleFlowType get_nozzle_flow_type(const Slic3r::DevNozzle& nozzle) {
    return nozzle.m_nozzle_flow;
}

int get_logic_extruder_id(const Slic3r::DevNozzle& nozzle) {
    return nozzle.m_nozzle_id;
}

uint32_t get_flag_bits_no_border(const std::string& str, int start_idx, int count)
{
    if (start_idx < 0 || count <= 0) return 0;

    try {
        std::string hex = str;
        // --- 1) trim ---
        auto ltrim = [](std::string &s) { s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); })); };
        auto rtrim = [](std::string &s) { s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end()); };
        ltrim(hex);
        rtrim(hex);

        // --- 2) remove 0x/0X prefix ---
        if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) { hex.erase(0, 2); }

        // --- 3) keep only hex digits ---
        std::string hex_digits;
        hex_digits.reserve(hex.size());
        for (char c : hex) {
            if (std::isxdigit(static_cast<unsigned char>(c))) hex_digits.push_back(c);
        }
        if (hex_digits.empty()) return 0;

        // --- 4) use size_t for all index/bit math ---
        const size_t total_bits = hex_digits.size() * 4ULL;

        const size_t ustart = static_cast<size_t>(start_idx);
        if (ustart >= total_bits) return 0;

        const int    int_bits  = std::numeric_limits<uint32_t>::digits; // typically 32
        const size_t need_bits = static_cast<size_t>(std::min(count, int_bits));

        // [first_bit, last_bit]
        const size_t first_bit = ustart;
        const size_t last_bit  = std::min(ustart + need_bits, total_bits) - 1ULL;
        if (last_bit < first_bit) return 0;

        const size_t right_index = hex_digits.size() - 1ULL;

        const size_t first_nibble = first_bit / 4ULL;
        const size_t last_nibble  = last_bit / 4ULL;

        const size_t start_nibble_idx = right_index - last_nibble;
        const size_t end_nibble_idx   = right_index - first_nibble;
        if (end_nibble_idx < start_nibble_idx) return 0;

        const size_t sub_len = end_nibble_idx - start_nibble_idx + 1ULL;
        if (end_nibble_idx >= hex_digits.size()) return 0;

        const std::string sub_hex = hex_digits.substr(start_nibble_idx, sub_len);

        unsigned long long chunk = std::stoull(sub_hex, nullptr, 16);

        const unsigned           nibble_offset = static_cast<unsigned>(first_bit % 4ULL);
        const unsigned long long shifted       = (nibble_offset == 0U) ? chunk : (chunk >> nibble_offset);

        uint32_t mask;
        if (need_bits >= static_cast<size_t>(std::numeric_limits<uint32_t>::digits)) {
            mask = std::numeric_limits<uint32_t>::max();
        } else {
            mask = static_cast<uint32_t>((1ULL << need_bits) - 1ULL);
        }

        return static_cast<uint32_t>(shifted & mask);
    } catch (...) {
        return 0;
    }
}

bool handle_ams_extruder_binding(
    const Slic3r::MachineObject* obj,
    const json& ams_item,
    int& extruder_id,
    std::set<int>& binded_extruder_set,
    std::optional<int>& binded_switcher_pos)
{
    // Extruder ID 0xE (14) is a virtual extruder ID sent by the printer.
    // It indicates that the filament is fed via AMS in H2C mode (two-channel feed).
    if (extruder_id == 0xE) {
        auto fs = get_fila_switch(obj);
        bool fts_installed = fs && fs->IsInstalled();
        
        // 1. MUTATE ID: Replace virtual 0xE with physical MAIN_EXTRUDER_ID (0).
        // This is necessary so the OrcaSlicer core (DevFilaSystem class) does not erase
        // or ignore the AMS in the device list, as 0xE is not a valid extruder ID for UI.
        extruder_id = MAIN_EXTRUDER_ID;
        
        if (fts_installed) {
            // ── FTS MODE BRANCH ──────────────────────────────────────────────
            // If the physical FTS switcher is installed and active,
            // this AMS slot can route to both MAIN (0) and DEPUTY (1) extruders.
            if (ams_item.contains("info")) {
                const std::string& info = ams_item["info"].get<std::string>();
                // Physical channel number (0 or 1) is encoded in bits 24:4 of the info field.
                int bind_switch_in = Slic3r::DevUtil::get_flag_bits(info, 24, 4);
                if (bind_switch_in == 0 || bind_switch_in == 1) {
                    binded_extruder_set = { MAIN_EXTRUDER_ID, DEPUTY_EXTRUDER_ID };
                }
                // Switcher direction: 0 -> POS_IN_B (Deputy), 1 -> POS_IN_A (Main)
                if (bind_switch_in == 0) {
                    binded_switcher_pos = Slic3r::VortekFilaSwitch::SwitchPos::POS_IN_B;
                } else if (bind_switch_in == 1) {
                    binded_switcher_pos = Slic3r::VortekFilaSwitch::SwitchPos::POS_IN_A;
                }
                VORTEK_LOG(warn, "handle_ams_extruder_binding: mapped 0xE to MAIN/DEPUTY, SwitchPos=" 
                           << (binded_switcher_pos.has_value() ? std::to_string(binded_switcher_pos.value()) : "nullopt"));
            }
        } else {
            // ── NO-FTS MODE BRANCH ───────────────────────────────────────────
            // If FTS is physically absent on the printer,
            // this AMS slot is bound exclusively to the single available
            // extruder (MAIN_EXTRUDER_ID = 0). Switcher position is std::nullopt.
            binded_extruder_set = { MAIN_EXTRUDER_ID };
            binded_switcher_pos = std::nullopt;
            VORTEK_LOG(warn, "handle_ams_extruder_binding: mapped 0xE to MAIN only (no-FTS)");
        }
        return true;
    } else {
        // Standard single extruder (non-H2C)
        binded_extruder_set = { extruder_id };
        return true;
    }
}

void assign_ams_bindings(
    Slic3r::DevAms* curr_ams,
    const std::set<int>& binded_extruder_set,
    const std::optional<int>& binded_switcher_pos)
{
    if (!curr_ams) return;
    s_ams_binded_extruders[curr_ams] = binded_extruder_set;
    s_ams_binded_switcher_pos[curr_ams] = binded_switcher_pos;
}

void process_nozzle_placement(
    Slic3r::DevNozzleSystem* system,
    Slic3r::DevNozzle& nozzle_obj,
    int raw_id)
{
    if (!system) return;

    int physical_id = Slic3r::DevUtil::get_hex_bits(raw_id, 0);
    int is_on_rack = Slic3r::DevUtil::get_hex_bits(raw_id, 1);

    nozzle_obj.m_nozzle_id = physical_id;
    auto rack = get_nozzle_rack(system);
    if (rack) {
        rack->SetNozzleOnRack(physical_id, is_on_rack == 1);
    }

    if (is_on_rack == 1) {
        if (rack) {
            rack->AddRackNozzle(nozzle_obj);
            VORTEK_LOG(warn, "process_nozzle_placement: added nozzle id=" << physical_id << " to rack");
        }
    } else {
        VORTEK_LOG(warn, "process_nozzle_placement: added active head nozzle id=" << physical_id);
    }
}

void sync_machine_nozzle_inventory_to_preset(const Slic3r::MachineObject* obj, Slic3r::PresetBundle& preset_bundle)
{
    if (!obj || !obj->GetNozzleSystem()) {
        return;
    }

    auto nozzle_system = obj->GetNozzleSystem();
    auto nozzle_rack = get_nozzle_rack(nozzle_system);
    auto& stat = preset_bundle.extruder_nozzle_stat;

    // Only update if not overridden by user
    stat.set_nozzle_data_flag(Slic3r::ExtruderNozzleStat::ndfMachine);

    // Get number of extruders of active printer preset
    const Slic3r::Preset& current_printer = preset_bundle.printers.get_selected_preset();
    auto* nozzle_diameter_opt = static_cast<const Slic3r::ConfigOptionFloats*>(current_printer.config.option("nozzle_diameter"));
    if (!nozzle_diameter_opt) {
        return;
    }
    int num_extruders = nozzle_diameter_opt->values.size();

    VORTEK_LOG(warn, "sync_machine_nozzle_inventory_to_preset: starting sync for " << num_extruders << " extruders");

    if (nozzle_rack && nozzle_rack->IsSupported()) {
        // Nozzle rack is supported (Vortek tool-changer)
        // Count nozzles of each volume type on the rack + the active nozzles in toolhead(s)
        std::map<Slic3r::NozzleVolumeType, int> counts_left;
        std::map<Slic3r::NozzleVolumeType, int> counts_right;

        // Precompute preset diameter per extruder for diameter-based filtering.
        // Only nozzles matching the current preset diameter are counted — the carousel
        // may hold nozzles of different diameters (e.g. 0.2/0.4/0.6), but slicing uses
        // only the diameter selected in the active printer preset per extruder.
        // Reference to BBS: BambuStudio/src/libslic3r/PrintConfig.cpp – get_extruder_nozzle_stats
        std::vector<float> preset_diameters(num_extruders, 0.0f);
        for (int eid = 0; eid < num_extruders; ++eid) {
            if (eid < (int)nozzle_diameter_opt->values.size())
                preset_diameters[eid] = (float)nozzle_diameter_opt->values[eid];
        }
        constexpr float kDiameterTol = 0.05f; // tolerance for float diameter comparison

        // Iterate through rack nozzles (all belong to Right extruder).
        // Stats count ALL carousel nozzles by flow type, regardless of diameter.
        // Diameter filtering happens at filament assignment level, not at inventory stats.
        // Reference to BBS: BambuStudio/src/slic3r/GUI/Plater.cpp – counts all rack nozzles
        for (const auto& pair : nozzle_rack->GetRackNozzles()) {
            const auto& dev_nozzle = pair.second;
            // Skip slots that are not physically on the rack
            if (!nozzle_rack->IsNozzleOnRack(dev_nozzle.m_nozzle_id)) {
                continue;
            }
            // Skip empty/unidentified nozzle slots
            if (is_nozzle_empty(dev_nozzle)) {
                VORTEK_LOG(debug, "sync_machine_nozzle_inventory_to_preset: skipping empty rack slot id="
                           << dev_nozzle.m_nozzle_id);
                continue;
            }
            // Every nozzle is Standard-capable (HF nozzle can be used as Standard).
            // HF nozzles are ALSO counted as HighFlow — counts overlap, not mutually exclusive.
            // Reference to BBS: BambuStudio/src/slic3r/GUI/Plater.cpp:1977 – Hybrid auto-detection
            counts_right[Slic3r::nvtStandard]++;
            if (dev_nozzle.m_nozzle_flow == Slic3r::NozzleFlowType::H_FLOW) {
                counts_right[Slic3r::nvtHighFlow]++;
            }
            VORTEK_LOG(debug, "sync_machine_nozzle_inventory_to_preset: found rack nozzle id=" 
                       << dev_nozzle.m_nozzle_id << ", diameter=" << dev_nozzle.m_diameter 
                       << ", flow=" << (int)dev_nozzle.m_nozzle_flow);
        }

        // Iterate through active toolhead nozzles.
        // Left nozzle (id=0) is always counted here.
        // Right active nozzle: skip if already counted in rack loop above
        // (IsNozzleOnRack returns true for carousel nozzles even when in toolhead).
        for (const auto& pair : nozzle_system->GetNozzles()) {
            const auto& dev_nozzle = pair.second;
            // Left nozzle has physical id 0, right nozzle has physical id >= 1
            int eid_for_nozzle = (dev_nozzle.m_nozzle_id == 0) ? 0 : 1;
            // Right active nozzle: skip if already counted in rack loop above.
            if (eid_for_nozzle == 1 && nozzle_rack && nozzle_rack->IsNozzleOnRack(dev_nozzle.m_nozzle_id)) {
                VORTEK_LOG(debug, "sync_machine_nozzle_inventory_to_preset: skipping active nozzle id="
                           << dev_nozzle.m_nozzle_id << " (already counted in rack)");
                continue;
            }
            // Every nozzle is Standard-capable; HF nozzles also count as HighFlow.
            if (dev_nozzle.m_nozzle_id == 0) {
                counts_left[Slic3r::nvtStandard]++;
                if (dev_nozzle.m_nozzle_flow == Slic3r::NozzleFlowType::H_FLOW) {
                    counts_left[Slic3r::nvtHighFlow]++;
                }
            } else {
                counts_right[Slic3r::nvtStandard]++;
                if (dev_nozzle.m_nozzle_flow == Slic3r::NozzleFlowType::H_FLOW) {
                    counts_right[Slic3r::nvtHighFlow]++;
                }
            }
            VORTEK_LOG(debug, "sync_machine_nozzle_inventory_to_preset: found active head nozzle id=" 
                       << dev_nozzle.m_nozzle_id << ", diameter=" << dev_nozzle.m_diameter 
                       << ", flow=" << (int)dev_nozzle.m_nozzle_flow);
        }



        // [H2C Debug] Override: add 1 HighFlow nozzle to Right extruder (carousel).
        // This simulates a mixed carousel so we can test the Hybrid slicing pipeline
        // without physical HF hardware. Standard count is NOT decremented — HF nozzle
        // is still Standard-capable.
        // Reference to BBS: BambuStudio/src/libslic3r/PresetBundle.cpp – on_printer_model_change
        if (VORTEK_DEBUG_HF_NOZZLE_OVERRIDE) {
            if (counts_right.count(Slic3r::nvtStandard) && counts_right[Slic3r::nvtStandard] > 0) {
                counts_right[Slic3r::nvtHighFlow]++;
                VORTEK_LOG(warn, "[DEBUG] HF override active: Right extruder carousel → "
                           << counts_right[Slic3r::nvtStandard] << "x Standard + "
                           << counts_right[Slic3r::nvtHighFlow] << "x HighFlow");
            } else {
                VORTEK_LOG(warn, "[DEBUG] HF override: no Standard slots found on Right extruder");
            }
        }

        // Update extruder nozzle stats for each extruder
        for (int eid = 0; eid < num_extruders; ++eid) {
            const auto& counts = (eid == 0) ? counts_left : counts_right;
            bool clear = true;
            bool added = false;
            for (const auto& pair : counts) {
                if (pair.second > 0) {
                    stat.set_extruder_nozzle_count(eid, pair.first, pair.second, clear);
                    clear = false;
                    added = true;
                    VORTEK_LOG(debug, "sync_machine_nozzle_inventory_to_preset: set extruder=" << eid 
                               << ", volume_type=" << (int)pair.first << ", count=" << pair.second);
                }
            }
            if (!added) {
                // If no nozzles are found, write 1 default nozzle of the type from preset
                Slic3r::NozzleVolumeType def_type = Slic3r::nvtStandard;
                auto* nozzle_volume_type_opt = static_cast<const Slic3r::ConfigOptionEnumsGeneric*>(current_printer.config.option("nozzle_volume_type"));
                if (nozzle_volume_type_opt && eid < (int)nozzle_volume_type_opt->values.size()) {
                    def_type = static_cast<Slic3r::NozzleVolumeType>(nozzle_volume_type_opt->values[eid]);
                }
                stat.set_extruder_nozzle_count(eid, def_type, 1, true);
                VORTEK_LOG(debug, "sync_machine_nozzle_inventory_to_preset: no active nozzles found for extruder=" << eid 
                           << ", fallback to 1 default nozzle of type=" << (int)def_type);
            }
        }
    } else {
        // Standard printer without nozzle rack
        // Map the active nozzle from m_nozzles map
        for (int eid = 0; eid < num_extruders; ++eid) {
            if (nozzle_system->ContainsNozzle(eid)) {
                auto dev_nozzle = nozzle_system->GetNozzle(eid);
                Slic3r::NozzleVolumeType vol_type = Slic3r::nvtStandard;
                if (dev_nozzle.m_nozzle_flow == Slic3r::NozzleFlowType::H_FLOW) {
                    vol_type = Slic3r::nvtHighFlow;
                }
                stat.set_extruder_nozzle_count(eid, vol_type, 1, true);
                VORTEK_LOG(debug, "sync_machine_nozzle_inventory_to_preset: single extruder=" << eid 
                           << ", nozzle found, diameter=" << dev_nozzle.m_diameter 
                           << ", volume_type=" << (int)vol_type);
            } else {
                stat.set_extruder_nozzle_count(eid, Slic3r::nvtStandard, 0, true);
                stat.set_extruder_nozzle_count(eid, Slic3r::nvtHighFlow, 0, false);
                VORTEK_LOG(debug, "sync_machine_nozzle_inventory_to_preset: single extruder=" << eid 
                           << ", nozzle not found, resetting counts to 0");
            }
        }
    }

    // Derive nozzle_volume_type from actual stats and write to project_config.
    // If an extruder has both Standard AND HighFlow counts → nvtHybrid.
    // If only HighFlow → nvtHighFlow. Otherwise → nvtStandard.
    // This drives Step 2b in VortekPrintHooks (dynamic Hybrid resolution).
    // Reference to BBS: BambuStudio/src/slic3r/GUI/Plater.cpp:1977 – Hybrid auto-detection
    // Reference to BBS: BambuStudio/src/slic3r/GUI/Tab.cpp:7318 – set_extruder_volume_type
    auto* nozzle_volume_type_opt = preset_bundle.project_config.option<Slic3r::ConfigOptionEnumsGeneric>("nozzle_volume_type");
    if (nozzle_volume_type_opt) {
        for (int eid = 0; eid < num_extruders && eid < (int)nozzle_volume_type_opt->values.size(); ++eid) {
            int std_count = stat.get_extruder_nozzle_count(eid, Slic3r::nvtStandard);
            int hf_count  = stat.get_extruder_nozzle_count(eid, Slic3r::nvtHighFlow);

            Slic3r::NozzleVolumeType resolved_type = Slic3r::nvtStandard;
            if (hf_count > 0 && std_count > 0) {
                resolved_type = Slic3r::nvtHybrid;
            } else if (hf_count > 0 && std_count == 0) {
                resolved_type = Slic3r::nvtHighFlow;
            }

            nozzle_volume_type_opt->values[eid] = static_cast<int>(resolved_type);
            VORTEK_LOG(warn, "sync_machine_nozzle_inventory_to_preset: extruder=" << eid
                       << " → nozzle_volume_type=" << (resolved_type == Slic3r::nvtHybrid ? "Hybrid"
                                                        : resolved_type == Slic3r::nvtHighFlow ? "HighFlow" : "Standard")
                       << " (std_count=" << std_count << ", hf_count=" << hf_count << ")");
        }
    }

    VORTEK_LOG(warn, "sync_machine_nozzle_inventory_to_preset: completed sync successfully");
}


void set_support_nozzle_rack(Slic3r::MachineObject* obj, bool supported) {
    if (!obj) return;
    auto rack = get_or_create_nozzle_rack(obj);
    if (rack) {
        // H2C always has a carousel nozzle rack — firmware may not set bit-60 in `fun`,
        // so override here for H2C regardless of the reported flag.
        // Reference to BBS: DeviceManager.cpp parse_new_info, fun-bit-60 path.
        if (is_h2c_printer(obj))
            supported = true;
        rack->SetSupported(supported);
    }
}


void parse_device_state(Slic3r::MachineObject* obj, const nlohmann::json& device_json) {
    if (!obj) return;

    if (device_json.contains("holder")) {
        auto rack = get_or_create_nozzle_rack(obj);
        if (rack) {
            rack->ParseRackInfo(device_json["holder"]);
            // "holder" key present in firmware push ↔ physical rack is installed.
            // bit-60 of fun is a BBS capability flag that H2C firmware may not set,
            // so we cannot rely solely on set_support_nozzle_rack(…, get_flag_bits(fun,60)).
            // Reference: DeviceManager.cpp parse_new_info, VortekDeviceHooks.cpp set_support_nozzle_rack.
            rack->SetSupported(true);
        }
    }

    if (device_json.contains("nozzle")) {
        const auto& nozzle_json = device_json["nozzle"];
        if (nozzle_json.contains("state")) {
            int state_val = nozzle_json["state"].get<int>();
            auto rack = get_or_create_nozzle_rack(obj);
            if (rack) {
                int new_reading_idx = Slic3r::DevUtil::get_flag_bits(state_val, 8, 4);
                int new_reading_count = Slic3r::DevUtil::get_flag_bits(state_val, 4, 4);
                if (rack->GetReadingCount() != new_reading_count || rack->GetReadingIdx() != new_reading_idx) {
                    rack->SetReadingInfo(new_reading_idx, new_reading_count);
                    if (new_reading_count == 0) {
                        rack->SendReadingFinished();
                    }
                }
            }
        }
    }
}

std::shared_ptr<Slic3r::VortekNozzleRack> get_or_create_nozzle_rack(Slic3r::MachineObject* obj) {
    if (!obj) return nullptr;
    auto it = s_nozzle_racks.find(obj);
    if (it != s_nozzle_racks.end()) return it->second;
    auto rack = std::make_shared<Slic3r::VortekNozzleRack>(obj->GetNozzleSystem(), obj);
    s_nozzle_racks[obj] = rack;
    return rack;
}

std::shared_ptr<Slic3r::VortekNozzleRack> get_nozzle_rack(const Slic3r::DevNozzleSystem* system) {
    if (!system) return nullptr;
    for (auto const& pair : s_nozzle_racks) {
        if (pair.first->GetNozzleSystem() == system) {
            return pair.second;
        }
    }
    return nullptr;
}

std::set<int> get_ams_binded_extruder_set(const Slic3r::DevAms* ams) {
    if (!ams) return {};
    auto it = s_ams_binded_extruders.find(ams);
    return it != s_ams_binded_extruders.end() ? it->second : std::set<int>{};
}

std::optional<int> get_ams_binded_switcher_pos(const Slic3r::DevAms* ams) {
    if (!ams) return std::nullopt;
    auto it = s_ams_binded_switcher_pos.find(ams);
    return it != s_ams_binded_switcher_pos.end() ? it->second : std::nullopt;
}

std::string get_nozzle_wear(const Slic3r::DevNozzle& nozzle) { return "0"; }
std::string get_nozzle_filament_id(const Slic3r::DevNozzle& nozzle, const Slic3r::DevNozzleSystem* system, bool is_on_rack) {
    int key = is_on_rack ? (16 + nozzle.m_nozzle_id) : nozzle.m_nozzle_id;
    std::string res = VortekNozzleFilamentManager::get_instance().get_filament_id(system, key);
    std::string p_type = (system && system->GetOwner()) ? system->GetOwner()->printer_type : "unknown";
    VORTEK_LOG(warn, "get_nozzle_filament_id: system=" << system << ", physical_id=" << nozzle.m_nozzle_id << ", is_on_rack=" << is_on_rack << ", key=" << key << ", res=" << res << ", printer_type=" << p_type);
    return res;
}
std::string get_custom_filament_name(const Slic3r::DevNozzleSystem* system, const std::string& id) {
    return VortekNozzleFilamentManager::get_instance().get_custom_filament_name(system, id);
}
void set_custom_filament_name(const Slic3r::DevNozzleSystem* system, const std::string& id, const std::string& name) {
    VortekNozzleFilamentManager::get_instance().set_custom_filament_name(system, id, name);
}
std::string get_nozzle_filament_color(const Slic3r::DevNozzle& nozzle, const Slic3r::DevNozzleSystem* system, bool is_on_rack) {
    int key = is_on_rack ? (16 + nozzle.m_nozzle_id) : nozzle.m_nozzle_id;
    std::string res = VortekNozzleFilamentManager::get_instance().get_filament_color(system, key);
    VORTEK_LOG(warn, "get_nozzle_filament_color: system=" << system << ", physical_id=" << nozzle.m_nozzle_id << ", is_on_rack=" << is_on_rack << ", key=" << key << ", res=" << res);
    return res;
}
void parse_nozzle_filament(Slic3r::DevNozzleSystem* system, int nozzle_id, const nlohmann::json& njon) {
    if (!system) return;
    int raw_id = njon.contains("id") ? njon["id"].get<int>() : nozzle_id;
    std::string id = njon.contains("fila_id") ? njon["fila_id"].get<std::string>() : "";
    std::string color = njon.contains("color_m") ? njon["color_m"].get<std::string>() : "";
    VORTEK_LOG(warn, "parse_nozzle_filament: system=" << system << ", nozzle_id=" << nozzle_id << ", raw_id=" << raw_id << ", cate=" << id << ", color=" << color << ", raw_json=" << njon.dump());
    VortekNozzleFilamentManager::get_instance().set_filament_info(system, raw_id, id, color);

    // Reference to BBS: BambuStudio/src/slic3r/GUI/DeviceCore/DevNozzleSystem.cpp
    // Resolve human-readable filament name from PresetBundle and cache it.
    // Nozzle JSON only carries fila_id (e.g. "P1f749cd"), not tray_type/sub_brands,
    // so we cannot build a fallback name here — PresetBundle lookup is the only option.
    // The FilaSystem path (preprocess_filament_json) will overwrite the cache with a
    // richer fallback if the preset is absent, so this is safe.
    if (!id.empty()) {
        std::string cached = VortekNozzleFilamentManager::get_instance().get_custom_filament_name(system, id);
        if (cached.empty()) {
            std::string resolved = Vortek::VortekProtocolExtension::get_instance().resolve_filament_name(system, id);
            if (!resolved.empty()) {
                VortekNozzleFilamentManager::get_instance().set_custom_filament_name(system, id, resolved);
                VORTEK_LOG(warn, "parse_nozzle_filament: resolved name from PresetBundle: id=" << id << ", name=" << resolved);
            }
        }
    }
}

bool is_nozzle_normal(const Slic3r::DevNozzle& nozzle) { return nozzle.m_nozzle_id != -1; }
int get_nozzle_id(const Slic3r::DevNozzle& nozzle) { return nozzle.m_nozzle_id; }
std::string to_nozzle_flow_string(Slic3r::NozzleFlowType flow_type) {
    return (flow_type == Slic3r::NozzleFlowType::H_FLOW ? "high_flow" : "standard");
}
wxString get_nozzle_type_str(const Slic3r::DevNozzle& nozzle) {
    switch (nozzle.m_nozzle_type) {
    case Slic3r::ntHardenedSteel:   return _L("Hardened Steel");
    case Slic3r::ntStainlessSteel:  return _L("Stainless Steel");
    case Slic3r::ntTungstenCarbide: return _L("Tungsten Carbide");
    case Slic3r::ntBrass:           return _L("Brass");
    default: break;
    }
    return _L("Unknown");
}
wxString get_nozzle_flow_type_str(const Slic3r::DevNozzle& nozzle) {
    switch (nozzle.m_nozzle_flow) {
    case Slic3r::NozzleFlowType::H_FLOW: return _L("High Flow");
    case Slic3r::NozzleFlowType::S_FLOW: return _L("Standard");
    case Slic3r::NozzleFlowType::U_FLOW: return _L("TPU High Flow");
    default: break;
    }
    return _L("Unknown");
}
std::string get_nozzle_type_string(Slic3r::NozzleType type) {
    switch (type) {
    case Slic3r::ntHardenedSteel:   return "Hardened Steel";
    case Slic3r::ntStainlessSteel:  return "Stainless Steel";
    case Slic3r::ntTungstenCarbide: return "Tungsten Carbide";
    case Slic3r::ntBrass:           return "Brass";
    default:                        return "Unknown";
    }
}
std::optional<int> get_replace_nozzle_tar(const Slic3r::DevNozzleSystem* system) {
    return std::nullopt;
}

bool is_nozzle_on_rack_helper(const Slic3r::DevNozzleSystem* system, int nozzle_id) {
    auto rack = get_nozzle_rack(system);
    return rack ? rack->IsNozzleOnRack(nozzle_id) : false;
}

std::vector<std::vector<std::vector<float>>> get_full_flush_matrix_helper(const Slic3r::PresetBundle* preset_bundle) {
    std::vector<std::vector<std::vector<float>>> res;
    if (!preset_bundle) return res;
    
    const auto& config = preset_bundle->printers.get_selected_preset().config;
    auto* flush_volumes_matrix_opt = static_cast<const Slic3r::ConfigOptionFloats*>(config.option("flush_volumes_matrix"));
    if (!flush_volumes_matrix_opt) return res;
    
    const auto& values = flush_volumes_matrix_opt->values;
    size_t num_extruders = sqrt(values.size());
    if (num_extruders == 0) return res;
    
    std::vector<std::vector<float>> matrix_2d(num_extruders, std::vector<float>(num_extruders, 0.0f));
    for (size_t r = 0; r < num_extruders; ++r) {
        for (size_t c = 0; c < num_extruders; ++c) {
            size_t idx = r * num_extruders + c;
            if (idx < values.size()) {
                matrix_2d[r][c] = values[idx];
            }
        }
    }
    
    res.resize(4, matrix_2d);
    return res;
}

Slic3r::DevNozzle get_nozzle_by_pos_id(const Slic3r::DevNozzleSystem* system, int pos_id) {
    if (!system) return Slic3r::DevNozzle();
    if (pos_id >= 0x10) {
        auto rack = get_nozzle_rack(system);
        return rack ? rack->GetNozzle(pos_id - 0x10) : Slic3r::DevNozzle();
    } else {
        return system->GetNozzle(pos_id);
    }
}

int get_nozzle_pos_id(const Slic3r::DevNozzle& nozzle, const Slic3r::DevNozzleSystem* system) {
    auto rack = get_nozzle_rack(system);
    if (rack && rack->IsNozzleOnRack(nozzle.m_nozzle_id)) {
        return nozzle.m_nozzle_id + 0x10;
    }
    return nozzle.m_nozzle_id;
}

std::shared_ptr<Slic3r::VortekNozzleMappingCtrl> get_or_create_nozzle_mapping(Slic3r::MachineObject* obj) {
    if (!obj) return nullptr;
    auto it = s_nozzle_mappings.find(obj);
    if (it != s_nozzle_mappings.end()) {
        return it->second;
    }
    auto nm = std::make_shared<Slic3r::VortekNozzleMappingCtrl>(obj);
    s_nozzle_mappings[obj] = nm;
    return nm;
}

std::shared_ptr<Slic3r::VortekNozzleMappingCtrl> get_nozzle_mapping(const Slic3r::MachineObject* obj) {
    if (!obj) return nullptr;
    auto it = s_nozzle_mappings.find(obj);
    return it != s_nozzle_mappings.end() ? it->second : nullptr;
}

std::shared_ptr<Slic3r::VortekFilaSwitch> get_or_create_fila_switch(Slic3r::MachineObject* obj) {
    if (!obj) return nullptr;
    auto it = s_fila_switches.find(obj);
    if (it != s_fila_switches.end()) {
        return it->second;
    }
    auto fs = std::make_shared<Slic3r::VortekFilaSwitch>(obj);
    s_fila_switches[obj] = fs;
    return fs;
}

std::shared_ptr<Slic3r::VortekFilaSwitch> get_fila_switch(const Slic3r::MachineObject* obj) {
    if (!obj) return nullptr;
    auto it = s_fila_switches.find(obj);
    return it != s_fila_switches.end() ? it->second : nullptr;
}

bool is_h2c_printer(const Slic3r::MachineObject* obj) {
    if (!obj) return false;
    return obj->printer_type == "O1C" || obj->printer_type == "O1C2" || obj->printer_type == "Bambu Lab H2C";
}

void store_wtm_firmware_info(Slic3r::MachineObject* obj, const Slic3r::DevFirmwareVersionInfo& info) {
    if (!is_h2c_printer(obj)) return;
    auto rack = get_or_create_nozzle_rack(obj);
    if (!rack) return;

    static const std::string s_wtm_prefix = "wtm/";
    auto pos = info.name.find(s_wtm_prefix);
    if (pos == std::string::npos) {
        rack->SetExtruderNozzleFirmwareInfo(info);
    } else {
        try {
            auto str = info.name.substr(s_wtm_prefix.size());
            int rack_nozzle_id = std::stoi(str, nullptr, 0) - 0x10;
            rack->AddNozzleFirmwareInfo(rack_nozzle_id, info);
        } catch (...) {}
    }
}

void clear_wtm_firmware_info(Slic3r::MachineObject* obj) {
    if (!is_h2c_printer(obj)) return;
    auto rack = get_nozzle_rack(obj->GetNozzleSystem());
    if (rack) {
        rack->ClearNozzleFirmwareInfo();
    }
}

Slic3r::DevFirmwareVersionInfo get_nozzle_firmware_info(const Slic3r::DevNozzle& nozzle, const Slic3r::DevNozzleSystem* system) {
    auto rack = get_nozzle_rack(system);
    if (!rack) return Slic3r::DevFirmwareVersionInfo();
    if (rack->IsNozzleOnRack(nozzle.m_nozzle_id)) {
        return rack->GetNozzleFirmwareInfo(nozzle.m_nozzle_id);
    } else {
        return rack->GetExtruderNozzleFirmwareInfo();
    }
}

Slic3r::NozzleDiameterType get_nozzle_diameter_type(const Slic3r::DevNozzle& nozzle) {
    if (Slic3r::is_approx(nozzle.m_diameter, 0.2f))
        return Slic3r::NozzleDiameterType::NOZZLE_DIAMETER_0_2;
    else if(Slic3r::is_approx(nozzle.m_diameter, 0.4f))
        return Slic3r::NozzleDiameterType::NOZZLE_DIAMETER_0_4;
    else if(Slic3r::is_approx(nozzle.m_diameter, 0.6f))
        return Slic3r::NozzleDiameterType::NOZZLE_DIAMETER_0_6;
    else if(Slic3r::is_approx(nozzle.m_diameter, 0.8f))
        return Slic3r::NozzleDiameterType::NOZZLE_DIAMETER_0_8;
    else
        return Slic3r::NozzleDiameterType::NONE_DIAMETER_TYPE;
}

void init_device_mappings(Slic3r::MachineObject* obj) {
    if (!obj) return;
    get_or_create_nozzle_mapping(obj);
    get_or_create_fila_switch(obj);
}

void clear_all_device_mappings(Slic3r::MachineObject* obj) {
    if (!obj) return;
    if (obj->GetNozzleSystem()) {
        VortekNozzleFilamentManager::get_instance().clear_for_system(obj->GetNozzleSystem());
    }
    s_nozzle_racks.erase(obj);
    s_nozzle_mappings.erase(obj);
    s_fila_switches.erase(obj);
}

bool contains_ext_nozzle(const Slic3r::DevNozzleSystem* system, int nozzle_id) {
    if (!system) return false;
    const auto& nozzles = system->GetNozzles();
    auto it = nozzles.find(nozzle_id);
    return it != nozzles.end() && !is_nozzle_empty(it->second);
}

void clear_auto_nozzle_mapping(Slic3r::MachineObject* obj) {
    if (!obj) return;
    auto nm = get_nozzle_mapping(obj);
    if (nm) {
        nm->Clear();
    }
}

static std::map<std::string, std::pair<std::set<int>, std::optional<int>>> s_pending_ams_bindings;

void preprocess_filament_json(Slic3r::MachineObject* obj, nlohmann::json& filament_json) {
    VORTEK_LOG(warn, "preprocess_filament_json: entered, obj=" << obj << ", contains_ams=" << (filament_json.contains("ams") ? "true" : "false"));
    if (!obj || !filament_json.contains("ams")) return;
    bool is_h2c = is_h2c_printer(obj);
    VORTEK_LOG(warn, "preprocess_filament_json: is_h2c_printer=" << (is_h2c ? "true" : "false") << ", printer_type=" << obj->printer_type);
    if (!is_h2c) return;
    auto fs = get_fila_switch(obj);
    bool fts_installed = fs && fs->IsInstalled();

    s_pending_ams_bindings.clear();

    VORTEK_LOG(warn, "preprocess_filament_json: full_json=" << filament_json.dump());

    // 1. Delegate filament name mapping and JSON mutation to the protocol extension sublayer
    Vortek::VortekProtocolExtension::get_instance().preprocess_filament_json(obj, filament_json);

    // 2. Perform AMS extruder binding normalization (0xE -> 0 / FTS)
    if (filament_json["ams"].contains("ams") && filament_json["ams"]["ams"].is_array()) {
        for (auto& ams_item : filament_json["ams"]["ams"]) {
            if (!ams_item.contains("id") || !ams_item.contains("extruder_id")) continue;
            
            // Skip parsing if it's not an int (sometimes it's a string, though normally it's an int)
            if (!ams_item["extruder_id"].is_number_integer()) continue;
            
            int ext_id = ams_item["extruder_id"].get<int>();
            std::string ams_id = ams_item["id"].get<std::string>();

            if (ext_id == 0xE) {
                // Mutate extruder_id in incoming JSON from 0xE to 0 (MAIN_EXTRUDER_ID).
                // This bypasses the strict core check in DevFilaSystem.cpp (line 375),
                // which erases the AMS if it sees an unmapped extruder_id of 0xE.
                ams_item["extruder_id"] = MAIN_EXTRUDER_ID;
                
                std::optional<int> binded_switcher_pos = std::nullopt;
                if (ams_item.contains("info")) {
                    const std::string& info = ams_item["info"].get<std::string>();
                    
                    // Extract FTS switch position from the original info string first!
                    int bind_switch_in = Slic3r::DevUtil::get_flag_bits(info, 24, 4);
                    if (bind_switch_in == 0) {
                        binded_switcher_pos = Slic3r::VortekFilaSwitch::SwitchPos::POS_IN_B;
                    } else if (bind_switch_in == 1) {
                        binded_switcher_pos = Slic3r::VortekFilaSwitch::SwitchPos::POS_IN_A;
                    }

                    // Mutate the info string to clear bits 8-11 (so it represents extruder ID 0 instead of 0xE)
                    try {
                        uint32_t val = std::stoul(info, nullptr, 16);
                        val = (val & ~0xF00);
                        std::stringstream ss;
                        ss << "0x" << std::hex << val;
                        ams_item["info"] = ss.str();
                    } catch (...) {
                        // Fallback
                    }
                }
                
                if (fts_installed) {
                    // ── FTS MODE BRANCH ──────────────────────────────────────────
                    // Store pending bindings to both physical extruders and resolve the FTS direction.
                    std::set<int> binded_extruder_set = { MAIN_EXTRUDER_ID, DEPUTY_EXTRUDER_ID };
                    s_pending_ams_bindings[ams_id] = { binded_extruder_set, binded_switcher_pos };
                    VORTEK_LOG(warn, "preprocess_filament_json: mapped 0xE to MAIN/DEPUTY for ams_id=" << ams_id);
                } else {
                    // ── NO-FTS MODE BRANCH ───────────────────────────────────────
                    // If FTS is not installed, bind slots only to the MAIN extruder (0),
                    // and clear switcher position.
                    std::set<int> binded_extruder_set = { MAIN_EXTRUDER_ID };
                    s_pending_ams_bindings[ams_id] = { binded_extruder_set, std::nullopt };
                    VORTEK_LOG(warn, "preprocess_filament_json: mapped 0xE to MAIN only (no-FTS) for ams_id=" << ams_id);
                }
            } else {
                // Ordinary single extruders
                std::set<int> binded_extruder_set = { ext_id };
                s_pending_ams_bindings[ams_id] = { binded_extruder_set, std::nullopt };
            }
        }
    }
}

void apply_pending_ams_bindings(Slic3r::DevFilaSystem* fila_system) {
    if (!fila_system) return;
    const auto& ams_list = fila_system->GetAmsList();
    for (const auto& pending : s_pending_ams_bindings) {
        auto ams_id = pending.first;
        auto it = ams_list.find(ams_id);
        if (it != ams_list.end() && it->second) {
            assign_ams_bindings(it->second, pending.second.first, pending.second.second);
            VORTEK_LOG(warn, "apply_pending_ams_bindings: applied pending bindings to ams_id=" << ams_id);
        }
    }
    s_pending_ams_bindings.clear();
}

bool apply_nozzle_mapping_from_device(Slic3r::MachineObject* obj, Slic3r::GUI::PartPlate* plate) {
    if (!obj || !plate) return false;

    auto rack = get_nozzle_rack(obj->GetNozzleSystem());
    if (!rack || !rack->IsSupported()) return false;

    auto mapping = get_nozzle_mapping(obj);
    if (!mapping) return false;

    std::vector<int> nozzle_map = mapping->GetFilamentNozzleMap();
    if (nozzle_map.empty()) return false;

    plate->set_filament_map_mode(Slic3r::FilamentMapMode::fmmNozzleManual);
    plate->set_filament_nozzle_maps(nozzle_map);
    VORTEK_LOG(warn, "apply_nozzle_mapping_from_device: applied nozzle map from MQTT/Device, mode=fmmNozzleManual");
    return true;
}

// Reference to BBS equivalent: DevNozzleSystem::ClearNozzles() in BambuStudio/src/slic3r/GUI/DeviceCore/DevNozzleSystem.cpp:458
void reset_nozzle_system(Slic3r::DevNozzleSystem* system)
{
    if (!system) return;
    VortekNozzleFilamentManager::get_instance().clear_for_system(system);
    auto rack = get_nozzle_rack(system);
    // Execute only for H2C printers with nozzle rack support
    if (rack && rack->IsSupported()) {
        rack->ClearRackNozzles();
    }
}

// H2C Vortek hook: returns true if the given nozzle volume type should be shown in the extruder UI
// beyond what is already declared in the printer preset's extruder_variant_list.
// Currently: nvtHybrid is shown only for H2C printers with an active nozzle rack (carousel).
// Reference to BBS: BambuStudio/src/slic3r/GUI/Plater.cpp extruder_variant_list lambda,
//   extruder_max_nozzle_count > 1 check.
bool should_show_nozzle_variant(
    const std::string&                      printer_model,
    const std::string&                      variant_list_entry,
    const std::string&                      extruder_type_label,
    const Slic3r::ConfigOptionDef*          nozzle_volumes_def,
    size_t                                  nozzle_type_idx,
    const Slic3r::ConfigOptionIntsNullable* max_nozzle_count_opt,
    int                                     extruder_idx)
{
    // Reference to BBS: BambuStudio/src/slic3r/GUI/Plater.cpp extruder_variant_list lambda.
    // 1. Standard check: variant declared in preset's extruder_variant_list.
    const std::string& label = nozzle_volumes_def->enum_labels[nozzle_type_idx];
    if (boost::algorithm::contains(variant_list_entry, extruder_type_label + " " + label))
        return true;

    // 2. H2C-only: show nvtHybrid when carousel rack is present (max_nozzle_count > 1).
    //    Only for H2C printers; all other printers exit here immediately.
    if (!boost::algorithm::contains(printer_model, "H2C")) return false;

    // enum_values[i] is a string key, enum_keys_map maps string -> int enum value.
    // Reference to BBS: BambuStudio/src/slic3r/GUI/Plater.cpp extruder_variant_list lambda,
    //   nozzle_volumes_def->enum_keys_map->at(nozzle_volumes_def->enum_values[i]) == nvtHybrid check.
    const auto& enum_key = nozzle_volumes_def->enum_values[nozzle_type_idx];
    auto it = nozzle_volumes_def->enum_keys_map->find(enum_key);
    if (it == nozzle_volumes_def->enum_keys_map->end()) return false;
    const auto nvt = static_cast<Slic3r::NozzleVolumeType>(it->second);
    if (nvt != Slic3r::NozzleVolumeType::nvtHybrid) return false;

    // Carousel rack: extruder_max_nozzle_count > 1 for this extruder slot.
    int max_nozzle_cnt = (max_nozzle_count_opt && extruder_idx < (int)max_nozzle_count_opt->values.size())
        ? max_nozzle_count_opt->values[extruder_idx] : 1;
    return max_nozzle_cnt > 1;
}

} // namespace DeviceHooks
} // namespace Vortek


// =============================================================================
// Tab extruder-tab UI hooks — H2C Hybrid (Vortek layer)
// =============================================================================
//
// These functions encapsulate ALL H2C-specific logic for the extruder tab-strip
// so that Tab.cpp contains only thin, stateless call-through hooks.
//
// Reference to BBS: BambuStudio/src/slic3r/GUI/Tab.cpp
//   generate_extruder_options, nvtHybrid block;
//   calculate_selection_index_for_extruder, nvtHybrid block.

namespace Vortek {
namespace DeviceHooks {

// ---------------------------------------------------------------------------
// get_hybrid_extruder_tab_names
// ---------------------------------------------------------------------------
// Returns {"<extruder_name>: Standard", "<extruder_name>: High Flow"} for an
// H2C printer with a Hybrid extruder; returns an empty vector for all others.
// ---------------------------------------------------------------------------
std::vector<wxString> get_hybrid_extruder_tab_names(
    const std::string&        printer_model,
    const wxString&           extruder_name,
    Slic3r::NozzleVolumeType  volume_type)
{
    // Guard: only H2C printers with a Hybrid volume type need expansion.
    if (volume_type != Slic3r::NozzleVolumeType::nvtHybrid)
        return {};
    if (!boost::algorithm::contains(printer_model, "H2C"))
        return {};

    return {
        wxString::Format(_L("%s: %s"), extruder_name, _L("Standard")),
        wxString::Format(_L("%s: %s"), extruder_name, _L("High Flow"))
    };
}

// ---------------------------------------------------------------------------
// calculate_extruder_tab_selection_index
// ---------------------------------------------------------------------------
// Computes the flat index into the extruder tab-strip for (extruder_id,
// nozzle_type).  On H2C, a Hybrid extruder occupies 2 slots; on all other
// printers it occupies 1 slot.
// ---------------------------------------------------------------------------
int calculate_extruder_tab_selection_index(
    const std::string&         printer_model,
    int                        extruder_nums,
    const std::vector<int>&    volume_values,
    int                        extruder_id,
    Slic3r::NozzleVolumeType   nozzle_type)
{
    const bool is_h2c = boost::algorithm::contains(printer_model, "H2C");

    int index = 0;
    for (int i = 0; i < extruder_nums; ++i) {
        const auto vt = static_cast<Slic3r::NozzleVolumeType>(volume_values[i]);
        const bool is_hybrid_slot = (vt == Slic3r::NozzleVolumeType::nvtHybrid) && is_h2c;

        if (i == extruder_id) {
            // For H2C Hybrid: Standard → index, High Flow → index+1.
            if (is_hybrid_slot && nozzle_type == Slic3r::NozzleVolumeType::nvtHighFlow)
                return index + 1;
            return index;
        }

        // Skip-count: Hybrid occupies 2 slots on H2C, 1 everywhere else.
        index += is_hybrid_slot ? 2 : 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// parse_hybrid_extruder_selection
// ---------------------------------------------------------------------------
// Inverse of calculate_extruder_tab_selection_index.
// Given a flat tab-strip selection index, resolves (extruder_id, nozzle_type),
// accounting for H2C Hybrid extruders that occupy 2 slots (Standard + High Flow).
//
// Returns true and fills extruder_id/nozzle_type when the selection falls on
// an H2C Hybrid slot; returns false for all other printers / volume types so
// the caller falls through to the standard single-slot path.
//
// Reference to BBS: BambuStudio/src/slic3r/GUI/Tab.cpp parse_extruder_selection,
//   nvtHybrid block.
// ---------------------------------------------------------------------------
bool parse_hybrid_extruder_selection(
    const std::string&              printer_model,
    int                             selection,
    int                             extruder_nums,
    const std::vector<int>&         volume_values,
    int&                            out_extruder_id,
    Slic3r::NozzleVolumeType&       out_nozzle_type)
{
    // Guard: only H2C printers can have Hybrid extruders.
    if (!boost::algorithm::contains(printer_model, "H2C"))
        return false;

    int current_index = 0;
    for (int i = 0; i < extruder_nums; ++i) {
        if (i >= (int)volume_values.size())
            break;
        const auto vt = static_cast<Slic3r::NozzleVolumeType>(volume_values[i]);
        if (vt == Slic3r::NozzleVolumeType::nvtHybrid) {
            // This extruder occupies 2 tab slots.
            if (selection == current_index) {
                out_extruder_id  = i;
                out_nozzle_type  = Slic3r::NozzleVolumeType::nvtStandard;
                return true;
            } else if (selection == current_index + 1) {
                out_extruder_id  = i;
                out_nozzle_type  = Slic3r::NozzleVolumeType::nvtHighFlow;
                return true;
            }
            current_index += 2;
        } else {
            // Non-hybrid: 1 slot — not handled by this function.
            current_index += 1;
        }
    }
    return false;
}

} // namespace DeviceHooks
} // namespace Vortek
