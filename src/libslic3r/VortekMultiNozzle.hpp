#ifndef VORTEK_MULTI_NOZZLE_HPP
#define VORTEK_MULTI_NOZZLE_HPP

#include <vector>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <memory>
#include "PrintConfig.hpp"

namespace Slic3r {

struct FilamentInfo;

namespace MultiNozzleUtils {

// Format a nozzle diameter (e.g. 0.4) to a trimmed string ("0.4").
std::string format_diameter_to_str(double diameter);

/**
 * @brief Represents information about a single physical nozzle.
 */
struct NozzleInfo {
    std::string      diameter;          ///< Nozzle diameter (e.g., "0.4")
    NozzleVolumeType volume_type;       ///< Nozzle volume type (Standard, HighFlow, etc.)
    int              extruder_id{-1};   ///< 0-based logical extruder ID (T0 or T1)
    int              group_id{-1};      ///< Logical nozzle changer slot ID

    /**
     * @brief Serializes the nozzle information to a string representation.
     * @return Serialized string.
     */
    std::string serialize() const;

    bool operator<(const NozzleInfo& other) const {
        if (group_id != other.group_id) return group_id < other.group_id;
        if (extruder_id != other.extruder_id) return extruder_id < other.extruder_id;
        if (volume_type != other.volume_type) return volume_type < other.volume_type;
        return diameter < other.diameter;
    }
};

/**
 * @brief Groups count and type of nozzles together for a logical extruder.
 */
struct NozzleGroupInfo {
    std::string      diameter;          ///< Nozzle diameter
    NozzleVolumeType volume_type;       ///< Volume type
    int              extruder_id;       ///< Logical extruder ID
    int              nozzle_count;      ///< Number of nozzles in this group

    NozzleGroupInfo() = default;
    NozzleGroupInfo(const std::string& nozzle_diameter_, const NozzleVolumeType volume_type_, const int extruder_id_, const int nozzle_count_)
        : diameter(nozzle_diameter_), volume_type(volume_type_), extruder_id(extruder_id_), nozzle_count(nozzle_count_) {}

    inline bool operator<(const NozzleGroupInfo &rhs) const {
        if (extruder_id != rhs.extruder_id) return extruder_id < rhs.extruder_id;
        if (diameter != rhs.diameter) return diameter < rhs.diameter;
        if (volume_type != rhs.volume_type) return volume_type < rhs.volume_type;
        return nozzle_count < rhs.nozzle_count;
    }

    bool is_same_type(const NozzleGroupInfo &rhs) const {
        return diameter == rhs.diameter && volume_type == rhs.volume_type && extruder_id == rhs.extruder_id;
    }

    inline bool operator==(const NozzleGroupInfo &rhs) const {
        return diameter == rhs.diameter && volume_type == rhs.volume_type && extruder_id == rhs.extruder_id && nozzle_count == rhs.nozzle_count;
    }

    /**
     * @brief Serializes the nozzle group to a string representation.
     * @return Serialized string.
     */
    std::string serialize() const;

    /**
     * @brief Deserializes a string representation back to a NozzleGroupInfo.
     * @param str Serialized string.
     * @return Deserialized object or std::nullopt.
     */
    static std::optional<NozzleGroupInfo> deserialize(const std::string& str);
};

/**
 * @brief Parameters defining time requirements for physical filament load/unload.
 */
struct FilamentChangeTimeParams {
    float selector_load_time{0.0f};
    float selector_unload_time{0.0f};
    float standard_load_time{0.0f};
    float standard_unload_time{0.0f};
};

/**
 * @brief Base abstract class containing mapping results and queries for nozzle groups.
 */
class NozzleGroupResultBase {
protected:
    bool support_dynamic_nozzle_map{false}; ///< True if the machine supports dynamic mapping of filaments to nozzles

public:
    NozzleGroupResultBase(bool support_dynamic_map = false) : support_dynamic_nozzle_map(support_dynamic_map) {}
    virtual ~NozzleGroupResultBase() = default;

    virtual std::optional<NozzleInfo> get_nozzle_from_id(int nozzle_id) const = 0;
    virtual std::optional<NozzleInfo> get_first_nozzle_for_filament(int filament_id) const = 0;
    virtual std::vector<NozzleInfo> get_nozzles_for_filament(int filament_id) const = 0;

    /**
     * @brief Checks if dynamic nozzle map is enabled.
     * @return True if supported, false otherwise.
     */
    bool is_support_dynamic_nozzle_map() const { return support_dynamic_nozzle_map; }
    
    virtual int get_extruder_count() const = 0;
    virtual std::vector<NozzleInfo> get_used_nozzles_in_extruder(int extruder_id = -1) const = 0;
    virtual std::vector<int> get_used_extruders() const = 0;
    virtual std::vector<unsigned int> get_used_filaments() const = 0;
};

/**
 * @brief Represents layered/sliced mapping of filaments and tool sequences to physical nozzles.
 */
class LayeredNozzleGroupResult : public NozzleGroupResultBase {
private:
    std::vector<std::vector<int>>                      _layer_filament_nozzle_maps;     ///< Per-layer mapping of logical filament to physical nozzle group ID
    std::vector<std::vector<unsigned int>>             _layer_filament_sequences;       ///< Per-layer sequencing order of filaments
    std::vector<int>                                   _default_filament_nozzle_map;    ///< Fallback default filament-to-nozzle mapping
    std::vector<unsigned int>                          _used_filaments;                 ///< Global list of used filaments
    std::vector<NozzleInfo>                            _nozzle_list;                    ///< List of active physical nozzles

public:
    LayeredNozzleGroupResult(bool support_dynamic_map = false) : NozzleGroupResultBase(support_dynamic_map) {}

    /**
     * @brief Factory method creating a layered result using simple mapping.
     */
    static std::optional<LayeredNozzleGroupResult> create(
        const std::vector<int>&          filament_nozzle_map,
        const std::vector<NozzleInfo>&   nozzle_list,
        const std::vector<unsigned int>& used_filaments);

    /**
     * @brief Factory method creating a fully-specified layered result with sequence info.
     */
    static std::optional<LayeredNozzleGroupResult> create(
        const std::vector<std::vector<int>>&          layer_filament_nozzle_maps,
        const std::vector<NozzleInfo>&                nozzle_list,
        const std::vector<unsigned int>&              used_filaments,
        const std::vector<std::vector<unsigned int>>& layer_filament_sequences);

    /**
     * @brief Factory method constructing a mapping result based on print config maps.
     */
    static std::optional<LayeredNozzleGroupResult> create(
        const std::vector<unsigned int>&                    used_filaments,
        const std::vector<int>&                             filament_map,
        const std::vector<int>&                             filament_volume_map,
        const std::vector<int>&                             filament_nozzle_map,
        const std::vector<std::map<NozzleVolumeType, int>>& nozzle_count,
        float                                               diameter);

    /**
     * @brief Helper to query if two filaments print from the same physical extruder carriage.
     */
    bool are_filaments_same_extruder(int filament_id1, int filament_id2, int layer_id = -1) const;

    /**
     * @brief Helper to query if two filaments print from the exact same nozzle slot.
     */
    bool are_filaments_same_nozzle(int filament_id1, int filament_id2, int layer_id = -1) const;
    int get_extruder_count() const override;

    std::vector<NozzleInfo> get_used_nozzles_in_extruder(int target_extruder_id = -1) const override;
    std::vector<NozzleInfo> get_used_nozzles_in_extruder(int target_extruder_id, int layer_id) const;
    std::vector<int> get_used_extruders() const override;
    std::vector<int> get_used_extruders(int layer_id) const;

    std::vector<int> get_extruder_map(bool zero_based = true, int layer_id = -1) const;
    std::vector<int> get_nozzle_map(int layer_id = -1) const;
    std::vector<int> get_volume_map(int layer_id = -1) const;

    std::vector<unsigned int> get_used_filaments() const override { return _used_filaments; }
    std::vector<unsigned int> get_used_filaments(int layer_id) const;

    std::optional<NozzleInfo> get_nozzle_for_filament(int filament_id, int layer_id = -1) const;
    std::vector<NozzleInfo> get_nozzles_for_filament(int filament_id) const override;

    std::optional<NozzleInfo> get_nozzle_from_id(int nozzle_id) const override;
    std::optional<NozzleInfo> get_first_nozzle_for_filament(int filament_id) const override;
    int get_extruder_id(int filament_id, int layer_id = -1) const;
    int get_nozzle_id(int filament_id, int layer_id = -1) const;

    size_t get_layer_count() const { return _layer_filament_nozzle_maps.size(); }
    const std::vector<int>& get_layer_filament_nozzle_map(int layer_id) const;
    const std::vector<std::vector<int>> &get_layer_filament_nozzle_maps() const { return _layer_filament_nozzle_maps; }
    const std::vector<std::vector<unsigned int>>& get_layer_filament_sequences() const { return _layer_filament_sequences; }
};

/**
 * @brief Records the state of physical nozzles during print sequencing (which filament is in which nozzle).
 */
class NozzleStatusRecorder {
private:
    std::unordered_map<int, int> nozzle_filament_status; ///< Nozzle ID -> Loaded Filament ID
    std::unordered_map<int, int> extruder_nozzle_status; ///< Extruder ID -> Currently mounted Nozzle ID
    int current_extruder_id_ = -1;                      ///< Active physical carriage ID

public:
    NozzleStatusRecorder() = default;
    bool is_nozzle_empty(int nozzle_id) const;
    int  get_filament_in_nozzle(int nozzle_id) const;
    int  get_nozzle_in_extruder(int extruder_id) const;
    int  get_current_extruder_id() const { return current_extruder_id_; }

    void clear_nozzle_status(int nozzle_id);
    void set_current_extruder_id(int extruder_id) { current_extruder_id_ = extruder_id; }
    void set_nozzle_status(int nozzle_id, int filament_id, int extruder_id = -1);

    const std::unordered_map<int, int>& get_nozzle_filament_map() const { return nozzle_filament_status; }
    const std::unordered_map<int, int>& get_extruder_nozzle_map() const { return extruder_nozzle_status; }
};

// ==================== Helper Functions ====================

/**
 * @brief Builds a physical nozzle list from group configurations.
 */
std::vector<NozzleInfo> build_nozzle_list(std::vector<NozzleGroupInfo> info);

/**
 * @brief Builds a physical nozzle list from raw mapping configurations.
 */
std::vector<NozzleInfo> build_nozzle_list(double diameter, const std::vector<int>& filament_nozzle_map,
                                          const std::vector<int>& filament_volume_map, const std::vector<int>& filament_map);

/**
 * @brief Parses device statistics strings representing installed nozzle parameters.
 */
std::vector<std::map<NozzleVolumeType, int>> get_extruder_nozzle_stats(const std::vector<std::string>& stats_strings);

} // namespace MultiNozzleUtils

class PresetBundle;

/**
 * @brief Manages nozzle type and statistics count per physical extruder.
 */
struct ExtruderNozzleStat
{
public:
    enum NozzleDataFlag {
        ndfMachine = 0,
        ndfNone
    };
public:
    ExtruderNozzleStat() = default;
    ExtruderNozzleStat(const std::vector<std::map<NozzleVolumeType, int>>& nozzle_counts, const NozzleDataFlag flag = ndfNone) : extruder_nozzle_counts(nozzle_counts), data_flag(flag) {}
    void on_volume_type_switch(int extruder_id, NozzleVolumeType type);
    void on_printer_model_change(PresetBundle* preset_bundle);
    void on_printer_model_change_cli(const std::vector<int> &nozzle_volume_type, const std::vector<int> &max_nozzle_count);
    void set_extruder_nozzle_count(int extruder_id, NozzleVolumeType type, int count, bool clear);
    int get_extruder_nozzle_count(int extruder_id, std::optional<NozzleVolumeType> volume_type = std::nullopt) const;

    const std::vector<std::map<NozzleVolumeType, int>> get_raw_stat() const { return extruder_nozzle_counts; }
    void set_raw_stat(const std::vector<std::map<NozzleVolumeType, int>>& data) { extruder_nozzle_counts = data; }

    void set_nozzle_data_flag(NozzleDataFlag flag){ data_flag = flag; }
    void set_force_keep_flag(bool flag) { force_keep_stat = flag; }
private:
    bool force_keep_stat{ false };
    std::vector<std::map<NozzleVolumeType,int>> extruder_nozzle_counts;
    NozzleDataFlag data_flag{ ndfNone };
};

std::vector<std::string> save_extruder_nozzle_stats_to_string(const std::vector<std::map<NozzleVolumeType,int>>& extruder_nozzle_stats);

} // namespace Slic3r

#endif // VORTEK_MULTI_NOZZLE_HPP
