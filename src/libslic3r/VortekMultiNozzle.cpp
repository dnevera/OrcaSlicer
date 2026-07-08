#include "VortekMultiNozzle.hpp"
#include "VortekLog.hpp"
#include "PresetBundle.hpp"
#include "VortekPrintHooks.hpp"
#include <numeric>
#include <boost/log/trivial.hpp>
#include <boost/format.hpp>
#include <sstream>
#include <algorithm>

namespace Slic3r {
namespace MultiNozzleUtils {

// Helper to format nozzle diameter to string
std::string format_diameter_to_str(double diameter) {
    char buf[64];
    sprintf(buf, "%.2f", diameter);
    std::string str(buf);
    while (!str.empty() && str.back() == '0') {
        str.pop_back();
    }
    if (!str.empty() && str.back() == '.') {
        str.pop_back();
    }
    return str;
}





// ==================== LayeredNozzleGroupResult Implementation ====================

static bool has_filament_mapped_to_multiple_nozzles(
    const std::vector<std::vector<int>>& layer_filament_nozzle_maps,
    const std::vector<unsigned int>& used_filaments) {
    if (layer_filament_nozzle_maps.empty() || used_filaments.empty()) {
        return false;
    }
    for (auto filament_id_u : used_filaments) {
        int filament_id = static_cast<int>(filament_id_u);
        std::set<int> nozzle_ids;
        for (const auto& map : layer_filament_nozzle_maps) {
            if (filament_id < 0 || filament_id >= static_cast<int>(map.size())) {
                continue;
            }
            int nozzle_id = map[filament_id];
            if (nozzle_id < 0) {
                continue;
            }
            nozzle_ids.insert(nozzle_id);
            if (nozzle_ids.size() > 1) {
                return true;
            }
        }
    }
    return false;
}

// [Auto-assign] Packages a pre-computed filament→nozzle index map into a result object.
// The caller already resolved which nozzle slot each filament goes to (by iterating nozzle_list).
// This overload does no matching logic — it simply stores and returns.
std::optional<LayeredNozzleGroupResult> LayeredNozzleGroupResult::create_from_index_map(
    const std::vector<int>& filament_nozzle_map,
    const std::vector<NozzleInfo>& nozzle_list,
    const std::vector<unsigned int>& used_filaments) {
    VORTEK_LOG(debug, "LayeredNozzleGroupResult::create_from_index_map: nozzles = " << nozzle_list.size() << ", filaments = " << used_filaments.size());
    if (filament_nozzle_map.empty() || nozzle_list.empty()) {
        return std::nullopt;
    }
    LayeredNozzleGroupResult result(false);
    result._default_filament_nozzle_map = filament_nozzle_map;
    result._nozzle_list = nozzle_list;
    result._used_filaments = used_filaments;
    return result;
}

// [Layer-sequence] Builds a result from per-layer nozzle maps with dynamic switching.
// Used by the GCode/ToolOrdering pipeline when filaments may change nozzle across layers.
// Sets support_dynamic_nozzle_map=true if any filament uses different nozzles on different layers.
std::optional<LayeredNozzleGroupResult> LayeredNozzleGroupResult::create_from_layer_sequence(
    const std::vector<std::vector<int>>& layer_filament_nozzle_maps,
    const std::vector<NozzleInfo>& nozzle_list,
    const std::vector<unsigned int>& used_filaments,
    const std::vector<std::vector<unsigned int>>& layer_filament_sequences) {
    VORTEK_LOG(debug, "LayeredNozzleGroupResult::create_from_layer_sequence: nozzles = " << nozzle_list.size() << ", layers = " << layer_filament_nozzle_maps.size());
    if (layer_filament_nozzle_maps.empty() || nozzle_list.empty()) {
        return std::nullopt;
    }
    bool support_dynamic_nozzle_map = has_filament_mapped_to_multiple_nozzles(layer_filament_nozzle_maps, used_filaments);
    LayeredNozzleGroupResult result(support_dynamic_nozzle_map);
    result._layer_filament_nozzle_maps = layer_filament_nozzle_maps;
    result._layer_filament_sequences = layer_filament_sequences;
    result._nozzle_list = nozzle_list;
    result._used_filaments = used_filaments;
    if (!layer_filament_nozzle_maps.empty()) {
        result._default_filament_nozzle_map = layer_filament_nozzle_maps[0];
    }
    return result;
}

// [Config-based] Reconstructs nozzle assignment from stored config arrays.
// Authoritative path for H2C Hybrid manual binding: respects filament_volume_map
// (user's explicit HF choice from FilamentMapDialog) when matching filaments to nozzles.
//
// Algorithm:
//   1. Build nozzle_list from nozzle_count (extruder_nozzle_stats): each {extruder, vol_type}
//      entry spawns `count` NozzleInfo slots with assigned group_id (carousel slot).
//   2. For each used filament, find a free nozzle slot with matching (extruder_id, volume_type).
//      volume_type comes from filament_volume_map (explicit HF=1 or Standard=0).
//   3. Build output_nozzle_map (filament→nozzle_list index) and delegate to create_from_index_map.
//
// Returns std::nullopt if any filament cannot be matched (e.g. HF nozzle requested but none exist).
std::optional<LayeredNozzleGroupResult> LayeredNozzleGroupResult::create_from_config(
    const std::vector<unsigned int>& used_filaments,
    const std::vector<int>& filament_map,
    const std::vector<int>& filament_volume_map,
    const std::vector<int>& filament_nozzle_map,
    const std::vector<std::map<NozzleVolumeType, int>>& nozzle_count,
    float diameter) {
    std::vector<NozzleGroupInfo> nozzle_groups;
    for (size_t extruder_id = 0; extruder_id < nozzle_count.size(); ++extruder_id) {
        for (auto elem : nozzle_count[extruder_id]) {
            NozzleGroupInfo group_info;
            group_info.diameter = format_diameter_to_str(diameter);
            group_info.volume_type = elem.first;
            group_info.nozzle_count = elem.second;
            group_info.extruder_id = static_cast<int>(extruder_id);
            nozzle_groups.emplace_back(group_info);
        }
    }
    auto nozzle_list = build_nozzle_list(nozzle_groups);
    VORTEK_LOG(warn, "LayeredNozzleGroupResult::create_from_config: stats size = " << nozzle_count.size() 
                      << ", nozzle_list size = " << nozzle_list.size() 
                      << ", diameter = " << diameter);
    
    // Log arrays content
    std::ostringstream uf_oss, fm_oss, fvm_oss, fnm_oss;
    for (auto v : used_filaments) uf_oss << v << " ";
    for (auto v : filament_map) fm_oss << v << " ";
    for (auto v : filament_volume_map) fvm_oss << v << " ";
    for (auto v : filament_nozzle_map) fnm_oss << v << " ";
    VORTEK_LOG(warn, "  used_filaments: " << uf_oss.str());
    VORTEK_LOG(warn, "  filament_map (extruder): " << fm_oss.str());
    VORTEK_LOG(warn, "  filament_volume_map (vol_type): " << fvm_oss.str());
    VORTEK_LOG(warn, "  filament_nozzle_map (nozzle_slot): " << fnm_oss.str());

    std::vector<bool> used_nozzle(nozzle_list.size(), false);
    std::map<int, int> input_nozzle_id_to_output;
    std::vector<int> output_nozzle_map(filament_nozzle_map.size(), 0);

    // Log nozzle_list for diagnostics
    for (size_t ni = 0; ni < nozzle_list.size(); ++ni) {
        VORTEK_LOG(warn, "  nozzle_list[" << ni << "] ext=" << nozzle_list[ni].extruder_id
            << " vol_type=" << (int)nozzle_list[ni].volume_type
            << " group_id=" << nozzle_list[ni].group_id
            << " dia=" << nozzle_list[ni].diameter);
    }

    for (auto filament_idx : used_filaments) {
        if (filament_idx >= filament_volume_map.size() || filament_idx >= filament_map.size() || filament_idx >= filament_nozzle_map.size()) {
            VORTEK_LOG(warn, "  FAIL: filament_idx=" << filament_idx
                << " out of bounds: vol_map.size=" << filament_volume_map.size()
                << " fil_map.size=" << filament_map.size()
                << " nozzle_map.size=" << filament_nozzle_map.size());
            return std::nullopt;
        }
        NozzleVolumeType req_type = NozzleVolumeType(filament_volume_map[filament_idx]);
        int req_extruder = filament_map[filament_idx];
        int input_nozzle_idx = filament_nozzle_map[filament_idx];

        if (input_nozzle_id_to_output.find(input_nozzle_idx) != input_nozzle_id_to_output.end()) {
            output_nozzle_map[filament_idx] = input_nozzle_id_to_output[input_nozzle_idx];
            VORTEK_LOG(warn, "  filament[" << filament_idx << "] req_ext=" << req_extruder
                << " req_type=" << (int)req_type << " input_nz=" << input_nozzle_idx
                << " -> CACHED output_nz=" << output_nozzle_map[filament_idx]);
            continue;
        }

        int output_nozzle_idx = -1;
        for (size_t nozzle_idx = 0; nozzle_idx < nozzle_list.size(); ++nozzle_idx) {
            if (used_nozzle[nozzle_idx]) continue;
            auto& nozzle_info = nozzle_list[nozzle_idx];
            // Match filament to nozzle by extruder AND volume_type (Standard vs HighFlow).
            // This is what makes filament_volume_map effective: a filament with vol_type=HF
            // will only match an HF nozzle slot, not a Standard slot.
            if (!(nozzle_info.extruder_id == req_extruder && nozzle_info.volume_type == req_type)) continue;

            output_nozzle_idx = static_cast<int>(nozzle_idx);
            input_nozzle_id_to_output[input_nozzle_idx] = output_nozzle_idx;
            used_nozzle[nozzle_idx] = true;
            break;
        }

        // Second pass: nozzle sharing (purge) — reuse an already-used nozzle of the same type.
        // This allows more filaments than physical nozzle slots (e.g. 3 HF filaments on 2 HF nozzles).
        // The printer will purge between filament switches on the shared nozzle.
        if (output_nozzle_idx == -1) {
            for (size_t nozzle_idx = 0; nozzle_idx < nozzle_list.size(); ++nozzle_idx) {
                auto& nozzle_info = nozzle_list[nozzle_idx];
                if (!(nozzle_info.extruder_id == req_extruder && nozzle_info.volume_type == req_type)) continue;
                output_nozzle_idx = static_cast<int>(nozzle_idx);
                input_nozzle_id_to_output[input_nozzle_idx] = output_nozzle_idx;
                VORTEK_LOG(warn, "  filament[" << filament_idx << "] SHARING nozzle " << output_nozzle_idx
                    << " (all " << (int)req_type << "-type nozzles occupied, purge required)");
                break;
            }
        }

        VORTEK_LOG(warn, "  filament[" << filament_idx << "] req_ext=" << req_extruder
            << " req_type=" << (int)req_type << " input_nz=" << input_nozzle_idx
            << " -> output_nz=" << output_nozzle_idx);

        if (output_nozzle_idx == -1) {
            VORTEK_LOG(warn, "  FAIL: no matching nozzle for filament[" << filament_idx
                << "] req_ext=" << req_extruder << " req_type=" << (int)req_type
                << " (available nozzles: " << nozzle_list.size() << ")");
            return std::nullopt;
        }
        output_nozzle_map[filament_idx] = output_nozzle_idx;
    }
    // Delegate final packaging to create_from_index_map — the output index map is now resolved.
    return create_from_index_map(output_nozzle_map, nozzle_list, used_filaments);
}

bool LayeredNozzleGroupResult::are_filaments_same_extruder(int filament_id1, int filament_id2, int layer_id) const {
    auto nozzle_info1 = get_nozzle_for_filament(filament_id1, layer_id);
    auto nozzle_info2 = get_nozzle_for_filament(filament_id2, layer_id);
    if (!nozzle_info1 || !nozzle_info2) return false;
    return nozzle_info1->extruder_id == nozzle_info2->extruder_id;
}

bool LayeredNozzleGroupResult::are_filaments_same_nozzle(int filament_id1, int filament_id2, int layer_id) const {
    auto nozzle_info1 = get_nozzle_for_filament(filament_id1, layer_id);
    auto nozzle_info2 = get_nozzle_for_filament(filament_id2, layer_id);
    if (!nozzle_info1 || !nozzle_info2) return false;
    return nozzle_info1->group_id == nozzle_info2->group_id;
}

int LayeredNozzleGroupResult::get_extruder_count() const {
    std::set<int> extruder_ids;
    for (const auto& nozzle : _nozzle_list) {
        extruder_ids.insert(nozzle.extruder_id);
    }
    return static_cast<int>(extruder_ids.size());
}

std::vector<NozzleInfo> LayeredNozzleGroupResult::get_used_nozzles_in_extruder(int target_extruder_id) const {
    return get_used_nozzles_in_extruder(target_extruder_id, -1);
}

std::vector<NozzleInfo> LayeredNozzleGroupResult::get_used_nozzles_in_extruder(int target_extruder_id, int layer_id) const {
    std::set<int> nozzle_ids;
    std::vector<NozzleInfo> result;
    std::vector<unsigned int> target_filaments = get_used_filaments(layer_id);

    for (unsigned int filament_id : target_filaments) {
        if (layer_id != -1) {
            auto nozzle_opt = get_nozzle_for_filament(static_cast<int>(filament_id), layer_id);
            if (nozzle_opt) {
                if (target_extruder_id == -1 || nozzle_opt->extruder_id == target_extruder_id) {
                    nozzle_ids.insert(nozzle_opt->group_id);
                }
            }
        } else {
            auto nozzles = get_nozzles_for_filament(static_cast<int>(filament_id));
            for (const auto &nozzle : nozzles) {
                if (target_extruder_id == -1 || nozzle.extruder_id == target_extruder_id) {
                    nozzle_ids.insert(nozzle.group_id);
                }
            }
        }
    }
    for (int nozzle_id : nozzle_ids) {
        if (nozzle_id >= 0 && nozzle_id < static_cast<int>(_nozzle_list.size())) {
            result.push_back(_nozzle_list[nozzle_id]);
        }
    }
    return result;
}

std::vector<int> LayeredNozzleGroupResult::get_used_extruders() const {
    return get_used_extruders(-1);
}

std::vector<int> LayeredNozzleGroupResult::get_used_extruders(int layer_id) const {
    std::set<int> used_extruders;
    std::vector<unsigned int> target_filaments = get_used_filaments(layer_id);
    for (auto filament_id : target_filaments) {
        if (layer_id != -1) {
            auto nozzle_opt = get_nozzle_for_filament(static_cast<int>(filament_id), layer_id);
            if (nozzle_opt) {
                used_extruders.insert(nozzle_opt->extruder_id);
            }
        } else {
            auto nozzles = get_nozzles_for_filament(static_cast<int>(filament_id));
            for (const auto &nozzle : nozzles) {
                used_extruders.insert(nozzle.extruder_id);
            }
        }
    }
    return std::vector<int>(used_extruders.begin(), used_extruders.end());
}

std::vector<int> LayeredNozzleGroupResult::get_extruder_map(bool zero_based, int layer_id) const {
    const std::vector<int>& filament_nozzle_map = get_layer_filament_nozzle_map(layer_id);
    std::vector<int> extruder_map(filament_nozzle_map.size());
    for (size_t idx = 0; idx < filament_nozzle_map.size(); ++idx) {
        int nozzle_id = filament_nozzle_map[idx];
        if (nozzle_id >= 0 && nozzle_id < static_cast<int>(_nozzle_list.size())) {
            extruder_map[idx] = _nozzle_list[nozzle_id].extruder_id;
        } else {
            extruder_map[idx] = -1;
        }
    }
    if (zero_based) {
        return extruder_map;
    }
    auto new_filament_map = extruder_map;
    std::transform(new_filament_map.begin(), new_filament_map.end(), new_filament_map.begin(), [](int val) { return val + 1; });
    return new_filament_map;
}

std::vector<int> LayeredNozzleGroupResult::get_nozzle_map(int layer_id) const {
    const std::vector<int>& filament_nozzle_map = get_layer_filament_nozzle_map(layer_id);
    std::vector<int> nozzle_map(filament_nozzle_map.size());
    for (size_t idx = 0; idx < filament_nozzle_map.size(); ++idx) {
        int nozzle_id = filament_nozzle_map[idx];
        if (nozzle_id >= 0 && nozzle_id < static_cast<int>(_nozzle_list.size())) {
            nozzle_map[idx] = _nozzle_list[nozzle_id].group_id;
        } else {
            nozzle_map[idx] = -1;
        }
    }
    return nozzle_map;
}

std::vector<int> LayeredNozzleGroupResult::get_volume_map(int layer_id) const {
    const std::vector<int>& filament_nozzle_map = get_layer_filament_nozzle_map(layer_id);
    std::vector<int> volume_map(filament_nozzle_map.size());
    for (size_t idx = 0; idx < filament_nozzle_map.size(); ++idx) {
        int nozzle_id = filament_nozzle_map[idx];
        if (nozzle_id >= 0 && nozzle_id < static_cast<int>(_nozzle_list.size())) {
            volume_map[idx] = _nozzle_list[nozzle_id].volume_type;
        } else {
            volume_map[idx] = -1;
        }
    }
    return volume_map;
}

std::vector<unsigned int> LayeredNozzleGroupResult::get_used_filaments(int layer_id) const {
    if (layer_id < 0 || layer_id >= static_cast<int>(_layer_filament_nozzle_maps.size())) {
        return _used_filaments;
    }
    if (!_layer_filament_sequences.empty() && layer_id < static_cast<int>(_layer_filament_sequences.size())) {
        return _layer_filament_sequences[layer_id];
    }
    return {};
}

std::optional<NozzleInfo> LayeredNozzleGroupResult::get_nozzle_for_filament(int filament_id, int layer_id) const {
    const std::vector<int>& filament_nozzle_map = get_layer_filament_nozzle_map(layer_id);
    if (filament_id < 0 || filament_id >= static_cast<int>(filament_nozzle_map.size())) {
        return std::nullopt;
    }
    int nozzle_id = filament_nozzle_map[filament_id];
    return get_nozzle_from_id(nozzle_id);
}

std::vector<NozzleInfo> LayeredNozzleGroupResult::get_nozzles_for_filament(int filament_id) const {
    std::set<int> nozzle_ids;
    if (!support_dynamic_nozzle_map) {
        if (filament_id >= 0 && filament_id < static_cast<int>(_default_filament_nozzle_map.size())) {
            nozzle_ids.insert(_default_filament_nozzle_map[filament_id]);
        }
    } else {
        for (const auto& map : _layer_filament_nozzle_maps) {
            if (filament_id >= 0 && filament_id < static_cast<int>(map.size())) {
                nozzle_ids.insert(map[filament_id]);
            }
        }
    }
    std::vector<NozzleInfo> result;
    for (int id : nozzle_ids) {
        if (id >= 0 && id < static_cast<int>(_nozzle_list.size())) {
            result.push_back(_nozzle_list[id]);
        }
    }
    return result;
}

std::optional<NozzleInfo> LayeredNozzleGroupResult::get_first_nozzle_for_filament(int filament_id) const {
    if (filament_id < 0) return std::nullopt;
    if (!support_dynamic_nozzle_map) {
        if (filament_id >= static_cast<int>(_default_filament_nozzle_map.size())) return std::nullopt;
        return get_nozzle_from_id(_default_filament_nozzle_map[filament_id]);
    }
    for (size_t layer = 0; layer < _layer_filament_nozzle_maps.size(); ++layer) {
        auto layer_used_filaments = get_used_filaments(layer);
        if (std::find(layer_used_filaments.begin(), layer_used_filaments.end(), static_cast<unsigned int>(filament_id)) == layer_used_filaments.end()){
            continue;
        }
        const auto& map = _layer_filament_nozzle_maps[layer];
        if (filament_id >= 0 && filament_id < static_cast<int>(map.size())) {
            int nozzle_id = map[filament_id];
            auto nozzle = get_nozzle_from_id(nozzle_id);
            if (nozzle) return nozzle;
        }
    }
    return std::nullopt;
}

std::optional<NozzleInfo> LayeredNozzleGroupResult::get_nozzle_from_id(int nozzle_id) const {
    if (nozzle_id < 0 || nozzle_id >= static_cast<int>(_nozzle_list.size())) {
        return std::nullopt;
    }
    return _nozzle_list[nozzle_id];
}

int LayeredNozzleGroupResult::get_extruder_id(int filament_id, int layer_id) const {
    auto nozzle_info = get_nozzle_for_filament(filament_id, layer_id);
    return nozzle_info ? nozzle_info->extruder_id : -1;
}

int LayeredNozzleGroupResult::get_nozzle_id(int filament_id, int layer_id) const {
    auto nozzle_info = get_nozzle_for_filament(filament_id, layer_id);
    return nozzle_info ? nozzle_info->group_id : -1;
}

const std::vector<int>& LayeredNozzleGroupResult::get_layer_filament_nozzle_map(int layer_id) const {
    if (layer_id >= 0 && layer_id < static_cast<int>(_layer_filament_nozzle_maps.size())) {
        return _layer_filament_nozzle_maps[layer_id];
    }
    return _default_filament_nozzle_map;
}

// ==================== NozzleStatusRecorder Implementation ====================

bool NozzleStatusRecorder::is_nozzle_empty(int nozzle_id) const {
    auto iter = nozzle_filament_status.find(nozzle_id);
    return iter == nozzle_filament_status.end() || iter->second == -1;
}

int NozzleStatusRecorder::get_filament_in_nozzle(int nozzle_id) const {
    auto iter = nozzle_filament_status.find(nozzle_id);
    return iter != nozzle_filament_status.end() ? iter->second : -1;
}

int NozzleStatusRecorder::get_nozzle_in_extruder(int extruder_id) const {
    auto iter = extruder_nozzle_status.find(extruder_id);
    return iter != extruder_nozzle_status.end() ? iter->second : -1;
}

void NozzleStatusRecorder::clear_nozzle_status(int nozzle_id) {
    nozzle_filament_status[nozzle_id] = -1;
    for (auto& elem : extruder_nozzle_status) {
        if (elem.second == nozzle_id) {
            elem.second = -1;
        }
    }
}

void NozzleStatusRecorder::set_nozzle_status(int nozzle_id, int filament_id, int extruder_id) {
    nozzle_filament_status[nozzle_id] = filament_id;
    if (extruder_id != -1) {
        extruder_nozzle_status[extruder_id] = nozzle_id;
    }
}

// ==================== Helper Functions ====================

std::vector<NozzleInfo> build_nozzle_list(std::vector<NozzleGroupInfo> nozzle_groups) {
    std::vector<NozzleInfo> ret;
    std::sort(nozzle_groups.begin(), nozzle_groups.end());
    int nozzle_id = 0;
    for (auto& group : nozzle_groups) {
        for (int i = 0; i < group.nozzle_count; ++i) {
            NozzleInfo tmp;
            tmp.diameter = group.diameter;
            tmp.extruder_id = group.extruder_id;
            tmp.volume_type = group.volume_type;
            tmp.group_id = nozzle_id++;
            ret.emplace_back(std::move(tmp));
        }
    }
    return ret;
}



// Parser for extruder_nozzle_stats (e.g. "0.4:4,0.6:4" per extruder stats)
std::vector<std::map<NozzleVolumeType, int>> get_extruder_nozzle_stats(const std::vector<std::string>& stats_strings) {
    VORTEK_LOG(debug, "get_extruder_nozzle_stats: stats size = " << stats_strings.size());
    std::vector<std::map<NozzleVolumeType, int>> ret;
    for (const auto& stat_str : stats_strings) {
        std::map<NozzleVolumeType, int> extruder_stats;
        if (stat_str.empty()) {
            ret.push_back(extruder_stats);
            continue;
        }

        // Check if it is the BBL format (Standard#7|High Flow#0)
        if (stat_str.find('#') != std::string::npos || stat_str.find('|') != std::string::npos) {
            std::stringstream ss(stat_str);
            std::string token;
            while (std::getline(ss, token, '|')) {
                if (token.empty()) continue;
                size_t hash_pos = token.find('#');
                if (hash_pos != std::string::npos) {
                    std::string vol_str = token.substr(0, hash_pos);
                    std::string count_str = token.substr(hash_pos + 1);
                    NozzleVolumeType type = NozzleVolumeType::nvtStandard;
                    if (vol_str == "High Flow" || vol_str == "1") {
                        type = NozzleVolumeType::nvtHighFlow;
                    }
                    try {
                        int count = std::stoi(count_str);
                        extruder_stats[type] = count;
                    } catch (...) {}
                }
            }
        } else {
            // Old format: e.g. "0.4:Standard:4,0.6:High Flow:4" or "0.4:4,0.6:4"
            std::stringstream ss(stat_str);
            std::string group_token;
            while (std::getline(ss, group_token, ',')) {
                if (group_token.empty()) continue;
                std::stringstream gss(group_token);
                std::vector<std::string> parts;
                std::string part;
                while (std::getline(gss, part, ':')) {
                    parts.push_back(part);
                }
                if (parts.size() >= 3) {
                    try {
                        int vol_type_val = std::stoi(parts[1]);
                        int count = std::stoi(parts[2]);
                        extruder_stats[static_cast<NozzleVolumeType>(vol_type_val)] = count;
                    } catch (...) {
                        std::string vol_str = parts[1];
                        NozzleVolumeType type = NozzleVolumeType::nvtStandard;
                        if (vol_str == "High Flow" || vol_str == "1") {
                            type = NozzleVolumeType::nvtHighFlow;
                        }
                        try {
                            int count = std::stoi(parts[2]);
                            extruder_stats[type] = count;
                        } catch (...) {}
                    }
                } else if (parts.size() == 2) {
                    try {
                        int vol_type_val = std::stoi(parts[0]);
                        int count = std::stoi(parts[1]);
                        extruder_stats[static_cast<NozzleVolumeType>(vol_type_val)] = count;
                    } catch (...) {
                        std::string vol_str = parts[0];
                        NozzleVolumeType type = NozzleVolumeType::nvtStandard;
                        if (vol_str == "High Flow" || vol_str == "1") {
                            type = NozzleVolumeType::nvtHighFlow;
                        }
                        try {
                            int count = std::stoi(parts[1]);
                            extruder_stats[type] = count;
                        } catch (...) {}
                    }
                }
            }
        }
        ret.push_back(extruder_stats);
    }
    return ret;
}

} // namespace MultiNozzleUtils

int ExtruderNozzleStat::get_extruder_nozzle_count(int extruder_id, std::optional<NozzleVolumeType> volume_type) const
{
    if(extruder_id < 0 || extruder_id >= extruder_nozzle_counts.size())
        return 0;
    // Reference to BBS: BambuStudio/src/libslic3r/PresetBundle.cpp:291
    // nvtHybrid is a layout marker; return the sum of all physical nozzle counts in the carousel.
    if (!volume_type.has_value() || volume_type == NozzleVolumeType::nvtHybrid)
        return std::accumulate(extruder_nozzle_counts[extruder_id].begin(), extruder_nozzle_counts[extruder_id].end(), 0,
            [](int sum, const std::pair<NozzleVolumeType, int>& p) { return sum + p.second; });

    auto iter = extruder_nozzle_counts[extruder_id].find(*volume_type);
    if(iter == extruder_nozzle_counts[extruder_id].end())
        return 0;
    return iter->second;
}

void ExtruderNozzleStat::on_printer_model_change(PresetBundle* preset_bundle)
{
    if (force_keep_stat)
        return;
    BOOST_LOG_TRIVIAL(info)<< __FUNCTION__ << boost::format(": reset extruder nozzle stat by printer model change : %1%") % preset_bundle->printers.get_selected_preset().name;
    auto nozzle_volume_type = preset_bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");
    auto max_nozzle_count = preset_bundle->printers.get_selected_preset().config.option<ConfigOptionIntsNullable>("extruder_max_nozzle_count");
    
    // Reference to BBS: BambuStudio/src/libslic3r/PresetBundle.cpp:301
    bool is_h2c = Vortek::is_h2c_printer(preset_bundle);
    
    extruder_nozzle_counts.resize(max_nozzle_count->size());
    for (size_t eid = 0; eid < extruder_nozzle_counts.size(); ++eid) {
        NozzleVolumeType type = nvtStandard;
        if (eid >= nozzle_volume_type->size())
            BOOST_LOG_TRIVIAL(error)<< __FUNCTION__ << boost::format(": eid out of bounds, use standard flow");
        else
            type = NozzleVolumeType(nozzle_volume_type->values[eid]);

        // For H2C Hybrid: nvtHybrid is a UI-mode marker, NOT a physical nozzle slot type.
        // Machine sync (sync_machine_nozzle_inventory_to_preset) writes {Standard:N, HighFlow:M}.
        // If we store {Hybrid: max_count} here, the diff with {Standard:N, HighFlow:M} causes a reslice
        // on every printer connect — the representations are structurally different.
        // Fix: store {Standard: max_count} as the offline default, matching BBS "Standard#4" pattern.
        // Machine sync will overwrite with actual {Standard:4, HighFlow:1} on connect.
        // Reference to BBS: BambuStudio/src/libslic3r/PresetBundle.cpp ~L301 ("Standard#4" for H2C Hybrid)
        NozzleVolumeType store_type = type;
        if (is_h2c && type == nvtHybrid) {
            store_type = nvtStandard;  // H2C: offline default — machine sync overwrites with real counts
        }
        int count = max_nozzle_count->values[eid];
        if (is_h2c && eid == 0) {
            count = 1;  // Left fixed nozzle: only 1 slot
        }
        set_extruder_nozzle_count(eid, store_type, count, true);

    }
}

void ExtruderNozzleStat::on_printer_model_change_cli(const std::vector<int>& nozzle_volume_type, const std::vector<int>& max_nozzle_count)
{
    if (force_keep_stat) return;
    
    // In CLI / offline mode, check if this is an H2C model
    // Reference to BBS: BambuStudio/src/libslic3r/PresetBundle.cpp:319
    bool is_h2c = (max_nozzle_count.size() == 2 && max_nozzle_count[1] == 6);
    
    extruder_nozzle_counts.resize(max_nozzle_count.size());
    for (size_t eid = 0; eid < extruder_nozzle_counts.size(); ++eid) {
        NozzleVolumeType type = nvtStandard;
        if (eid >= nozzle_volume_type.size())
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": eid out of bounds, use standard flow");
        else
            type = NozzleVolumeType(nozzle_volume_type[eid]);

        // For H2C CLI mode: same logic as GUI on_printer_model_change.
        // Left (eid=0) → count=1; Right carousel (eid>0) → keep max_nozzle_count.
        // nvtHybrid → nvtStandard: carousel slots are Standard-type for nozzle matching.
        // Reference to BBS: BambuStudio/src/libslic3r/PresetBundle.cpp ~L319
        // count: Left (eid=0) fixed to 1 slot; Right carousel uses max_nozzle_count.
        // nvtHybrid: do NOT collapse to nvtStandard. In CLI/offline mode without a live
        // machine sync, nvtHybrid is kept as-is. With VORTEK_DEBUG_HF_NOZZLE_OVERRIDE the
        // actual HighFlow counts will be injected during sync regardless.
        // Reference to BBS: BambuStudio/src/libslic3r/PresetBundle.cpp ~L319
        int count = max_nozzle_count[eid];
        if (is_h2c && eid == 0) {
            count = 1;  // Left fixed nozzle: only 1 slot
        }
        set_extruder_nozzle_count(eid, type, count, true);
    }
}

void ExtruderNozzleStat::on_volume_type_switch(int extruder_id, NozzleVolumeType type)
{
    if (data_flag == NozzleDataFlag::ndfMachine) {
        // do nothing here
    }
    // Reference to BBS: BambuStudio/src/libslic3r/PresetBundle.cpp: ExtruderNozzleStat::on_volume_type_switch
    else if (type != nvtHybrid) {
        int current_count = get_extruder_nozzle_count(extruder_id, std::nullopt);
        if (extruder_id >= extruder_nozzle_counts.size()) {
            extruder_nozzle_counts.resize(extruder_id + 1);
        }
        extruder_nozzle_counts[extruder_id].clear();
        extruder_nozzle_counts[extruder_id][type] = current_count;
    }
}

void ExtruderNozzleStat::set_extruder_nozzle_count(int extruder_id, NozzleVolumeType type, int count, bool clear)
{
    if (extruder_id >= extruder_nozzle_counts.size())
        extruder_nozzle_counts.resize(extruder_id + 1);
    if(clear)
        extruder_nozzle_counts[extruder_id].clear();
    extruder_nozzle_counts[extruder_id][type] = count;
}

std::vector<std::string> save_extruder_nozzle_stats_to_string(const std::vector<std::map<NozzleVolumeType,int>>& extruder_nozzle_stats)
{
    std::vector<std::string> extruder_nozzle_count_str;
    for (size_t idx = 0; idx < extruder_nozzle_stats.size(); ++idx) {
        std::ostringstream oss;
        const auto& item = extruder_nozzle_stats[idx];
        for (auto it = item.begin(); it != item.end(); ++it) {
            oss << get_nozzle_volume_type_string(it->first) << "#" << it->second;
            if (std::next(it) != item.end())
                oss << "|";
        }
        extruder_nozzle_count_str.emplace_back(oss.str());
    }
    return extruder_nozzle_count_str;
}

} // namespace Slic3r
