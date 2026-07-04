#include "VortekPreCooling.hpp"
#include "VortekLog.hpp"
#include "GCodeReader.hpp"
#include "Print.hpp"
#include "VortekPrintHooks.hpp"
#include <regex>
#include <algorithm>
#include <sstream>
#include <cmath>
#include <limits>

namespace Vortek {

// Reference to BBS: BambuStudio/src/libslic3r/GCode/GCodeProcessor.cpp — constructor of PreCoolingInjector
PreCooling::PreCooling(
    const std::vector<Slic3r::GCodeProcessorResult::MoveVertex>& moves,
    const Slic3r::MultiNozzleUtils::LayeredNozzleGroupResult& nozzle_group_result,
    const std::vector<int>& filament_nozzle_temps,
    const std::vector<int>& filament_nozzle_temps_initial_layer,
    const std::vector<int>& physical_extruder_map,
    int valid_machine_id,
    float inject_time_threshold,
    bool handle_hotend_as_extruder,
    bool has_filament_switcher,
    int standby_temp_delta,
    const std::vector<int>& pre_cooling_temp_nc,
    const std::vector<int>& filament_idle_temps,
    const std::vector<double>& cooling_rate,
    const std::vector<double>& heating_rate,
    const std::vector<std::pair<unsigned int, unsigned int>>& skippable_blocks,
    const std::vector<int>& extruder_max_nozzle_count,
    const std::vector<double>& filament_preheat_temperature_delta,
    const std::vector<double>& filament_max_temperature_drop_when_ec,
    unsigned int machine_start_gcode_end_id,
    unsigned int machine_end_gcode_start_id,
    const std::vector<Slic3r::ExtruderType>& extruder_types,
    const std::vector<double>& nozzle_diameter
) :
    m_moves(moves),
    m_nozzle_group_result(nozzle_group_result),
    m_filament_nozzle_temps(filament_nozzle_temps),
    m_filament_nozzle_temps_initial_layer(filament_nozzle_temps_initial_layer),
    m_physical_extruder_map(physical_extruder_map),
    m_valid_machine_id(valid_machine_id),
    m_inject_time_threshold(inject_time_threshold),
    m_handle_hotend_as_extruder(handle_hotend_as_extruder),
    m_has_filament_switcher(has_filament_switcher),
    m_standby_temp_delta(standby_temp_delta),
    m_filament_pre_cooling_temps_nc(pre_cooling_temp_nc),
    m_filament_idle_temps(filament_idle_temps),
    m_cooling_rate(cooling_rate),
    m_heating_rate(heating_rate),
    m_skippable_blocks(skippable_blocks),
    m_extruder_max_nozzle_count(extruder_max_nozzle_count),
    m_filament_preheat_temperature_delta(filament_preheat_temperature_delta),
    m_filament_max_temperature_drop_when_ec(filament_max_temperature_drop_when_ec),
    m_machine_start_gcode_end_id(machine_start_gcode_end_id),
    m_machine_end_gcode_start_id(machine_end_gcode_start_id),
    m_extruder_types(extruder_types),
    m_nozzle_diameter(nozzle_diameter)
{
    std::sort(m_moves.begin(), m_moves.end(), [](const auto& a, const auto& b) {
        return a.gcode_id < b.gcode_id;
    });

    // CRITICAL: OrcaSlicer stores block_time (delta, duration of single move) in MoveVertex::time[mode],
    // while BBS stores cumulative time. We must accumulate deltas ourselves.
    // Reference to BBS: GCodeProcessor.cpp:413 — writes `time` (cumulative)
    // Reference to Orca: GCodeProcessor.cpp:426 — writes `block_time` (delta)
    m_cumulative_times.resize(m_moves.size(), 0.0f);
    double accum = 0.0;
    for (size_t i = 0; i < m_moves.size(); ++i) {
        accum += m_moves[i].time[m_valid_machine_id];
        m_cumulative_times[i] = static_cast<float>(accum);
    }
    VORTEK_LOG(warning, "PreCooling ctor: moves=" << m_moves.size()
        << " valid_machine_id=" << m_valid_machine_id
        << " total_cumulative_time=" << accum << "s"
        << " filament_temps_count=" << m_filament_nozzle_temps.size()
        << " cooling_rate_count=" << m_cooling_rate.size()
        << " heating_rate_count=" << m_heating_rate.size()
        << " start_gcode_end=" << m_machine_start_gcode_end_id
        << " end_gcode_start=" << m_machine_end_gcode_start_id);
}

float PreCooling::get_cum_time(std::vector<Slic3r::GCodeProcessorResult::MoveVertex>::const_iterator it) const
{
    if (it == m_moves.cend())
        return 0.0f;
    auto idx = std::distance(m_moves.cbegin(), it);
    if (idx < 0 || idx >= static_cast<long long>(m_cumulative_times.size()))
        return 0.0f;
    return m_cumulative_times[idx];
}

// Reference to BBS: BambuStudio/src/libslic3r/GCode/GCodeProcessor.cpp — process_pre_cooling_and_heating
void PreCooling::process_pre_cooling_and_heating(InsertedLinesMap& inserted_operation_lines)
{
    VORTEK_LOG(warning, "process_pre_cooling_and_heating: free blocks count = " << m_extruder_free_blocks.size());
    bool is_multiple_nozzle = std::any_of(m_extruder_max_nozzle_count.begin(), m_extruder_max_nozzle_count.end(), [](auto& elem) { return elem > 1; });
    auto get_nozzle_temp = [this, is_multiple_nozzle](int filament_id, bool is_first_layer, bool from_or_to, bool consider_preheat_temperature_delta) {
        if (filament_id == -1)
            return from_or_to ? 140 : 0;
        double temp = (is_first_layer ? m_filament_nozzle_temps_initial_layer[filament_id] : m_filament_nozzle_temps[filament_id]);
        if (consider_preheat_temperature_delta)
            return (int)(temp - m_filament_preheat_temperature_delta[filament_id]);
        else
            return (int)(temp);
    };

    bool has_mixed_extruder_types = m_extruder_types.size() > 1 &&
        std::adjacent_find(m_extruder_types.begin(), m_extruder_types.end(), std::not_equal_to<>()) != m_extruder_types.end();
    
    float first_nozzle_dia = m_nozzle_diameter.empty() ? 0.4 : m_nozzle_diameter.front();
    float switcher_temp_offset = (first_nozzle_dia >= 0.6 - 1e-5) ? 40.f : 20.f;

    std::map<int, std::vector<ExtruderFreeBlock>> per_extruder_free_blocks;
    for (auto& block : m_extruder_free_blocks)
        per_extruder_free_blocks[block.extruder_id].emplace_back(block);

    for (auto& elem : per_extruder_free_blocks) {
        int extruder_id = elem.first;
        // Carousel detection: an extruder with more than one nozzle slot is a carousel.
        // Galantcev PR f58acc2a56 operated on inject_cooling_heating_command() generically
        // (no per-extruder type check). We split carousel handling into a dedicated path
        // here so that the logic is explicit and cannot accidentally affect fixed nozzles.
        // ExtruderType::Carousel does not exist in this codebase — use m_extruder_max_nozzle_count > 1.
        bool ext_is_carousel = (extruder_id >= 0 && extruder_id < (int)m_extruder_max_nozzle_count.size() && m_extruder_max_nozzle_count[extruder_id] > 1);
        auto& extruder_free_blocks = elem.second;
        for (auto iter = extruder_free_blocks.begin(); iter != extruder_free_blocks.end(); ++iter) {
            bool is_end = std::next(iter) == extruder_free_blocks.end();
            bool apply_pre_cooling = true;
            bool apply_pre_heating = is_end ? false : true;

            if (ext_is_carousel) {
                if (iter->last_filament_id < 0) {
                    VORTEK_LOG(warning, "process_pre_cooling_and_heating: SKIP carousel ext " << extruder_id
                        << " sentinel block (last_fil=-1) — nothing to cooldown");
                    continue;
                }
                // Carousel park/standby target temperature — 3-level hierarchy (no hardcoded constants):
                //   1. filament_pre_cooling_temperature_nc > 0  →  NC-specific park temp (carousel slot idle)
                //   2. idle_temperature > 0                     →  generic standby configured in preset
                //   3. room_temperature (25°C), preheat=false   →  nothing configured; firmware handles heating
                //
                // WHY different from Galantcev PR f58acc2a56:
                //   That PR added a hardcoded fallback of 180°C inside inject_cooling_heating_command()
                //   as a floor on mid_temp and cooling_temp. The 180°C came from "the same safe fallback
                //   the intra-extruder nozzle-change branch uses", but that branch itself has the same
                //   180°C hardcode — so the constant propagated without a preset basis.
                //
                //   Our approach: the TARGET temperature for cooling+preheating is set HERE at the
                //   process_pre_cooling_and_heating level, from the actual preset values, before
                //   inject_cooling_heating_command() is called. inject_cooling_heating_command() remains
                //   generic (no carousel-specific floor). If no temperature is configured in the preset
                //   we do NOT invent a value — we cool to room_temp and suppress the preheat, letting
                //   firmware NC handle the heating autonomously.
                //
                // Reference to BBS: BambuStudio WipeTower.cpp — park temp hierarchy for carousel nozzles.
                int blk_park_temp_nc = 0;
                if (iter->last_filament_id >= 0 && iter->last_filament_id < (int)m_filament_pre_cooling_temps_nc.size())
                    blk_park_temp_nc = m_filament_pre_cooling_temps_nc[iter->last_filament_id];

                int blk_idle_temp = 0;
                if (blk_park_temp_nc <= 0) {
                    if (iter->last_filament_id >= 0 && iter->last_filament_id < (int)m_filament_idle_temps.size())
                        blk_idle_temp = m_filament_idle_temps[iter->last_filament_id];
                }

                constexpr float room_temp_floor = 25.f;
                float blk_target_temp;
                bool blk_do_preheat;
                if (blk_park_temp_nc > 0) {
                    blk_target_temp = (float)blk_park_temp_nc;
                    blk_do_preheat  = apply_pre_heating; // !is_end
                    VORTEK_LOG(warning, "process_pre_cooling_and_heating: carousel ext " << extruder_id
                        << " last_fil=" << iter->last_filament_id
                        << " — using park_temp_nc=" << blk_park_temp_nc);
                } else if (blk_idle_temp > 0) {
                    blk_target_temp = (float)blk_idle_temp;
                    blk_do_preheat  = apply_pre_heating; // !is_end
                    VORTEK_LOG(warning, "process_pre_cooling_and_heating: carousel ext " << extruder_id
                        << " last_fil=" << iter->last_filament_id
                        << " — no park_temp_nc, using idle_temp=" << blk_idle_temp);
                } else {
                    // Nothing configured in preset: cool to room temp, no preheat.
                    // Firmware NC will heat from room temp to print temp on its own.
                    blk_target_temp = room_temp_floor;
                    blk_do_preheat  = false;
                    VORTEK_LOG(warning, "process_pre_cooling_and_heating: carousel ext " << extruder_id
                        << " last_fil=" << iter->last_filament_id
                        << " — no park/idle temp in preset, cooldown to room_temp only (no preheat)");
                }

                {
                    float curr_temp = get_nozzle_temp(iter->last_filament_id, false, true, false);
                    VORTEK_LOG(warning, "process_block: ext=" << extruder_id
                        << " last_fil=" << iter->last_filament_id << " next_fil=" << iter->next_filament_id
                        << " last_nozzle=" << iter->last_nozzle_id << " next_nozzle=" << iter->next_nozzle_id
                        << " lower_gid=" << iter->free_lower_gcode_id << " upper_gid=" << iter->free_upper_gcode_id
                        << " curr_temp=" << curr_temp << " target_temp=" << blk_target_temp
                        << " cooling=" << apply_pre_cooling << " heating=" << blk_do_preheat
                        << " ignore_tower=" << iter->ignore_cooling_before_tower);
                    inject_cooling_heating_command(inserted_operation_lines, *iter, curr_temp, blk_target_temp,
                                                  apply_pre_cooling, blk_do_preheat, false);
                }
                continue;
            }

            bool suppress_cooling_emission = (iter->last_filament_id == -1);

            float curr_temp = get_nozzle_temp(iter->last_filament_id, false, true, false);
            float target_temp = get_nozzle_temp(iter->next_filament_id, false, false, !iter->ignore_cooling_before_tower);
            
            if (m_has_filament_switcher && has_mixed_extruder_types && apply_pre_heating) {
                float print_temp = get_nozzle_temp(iter->next_filament_id, false, false, false);
                target_temp = std::min(target_temp, print_temp - switcher_temp_offset);
            }
            VORTEK_LOG(warning, "process_block: ext=" << extruder_id
                << " last_fil=" << iter->last_filament_id << " next_fil=" << iter->next_filament_id
                << " last_nozzle=" << iter->last_nozzle_id << " next_nozzle=" << iter->next_nozzle_id
                << " lower_gid=" << iter->free_lower_gcode_id << " upper_gid=" << iter->free_upper_gcode_id
                << " curr_temp=" << curr_temp << " target_temp=" << target_temp
                << " cooling=" << apply_pre_cooling << " heating=" << apply_pre_heating
                << " ignore_tower=" << iter->ignore_cooling_before_tower);
            inject_cooling_heating_command(inserted_operation_lines, *iter, curr_temp, target_temp, apply_pre_cooling, apply_pre_heating, suppress_cooling_emission);
        }
    }
}

// Reference to BBS: GCodeProcessor.cpp:6472-6478 — build_extruder_free_blocks
void PreCooling::build_extruder_free_blocks(
    const std::vector<FilamentUsageBlock>& filament_usage_blocks,
    const std::vector<ExtruderUsageBlock>& extruder_usage_blocks
)
{
    // BBS routing: use build_by_extruder_blocks when extruder_usage_blocks > 1 (H2C path),
    // fallback to build_by_filament_blocks when only 1 block (no nozzle changes).
    if (extruder_usage_blocks.size() <= 1) {
        VORTEK_LOG(warning, "build_extruder_free_blocks: using build_by_filament_blocks (extruder_blocks=" << extruder_usage_blocks.size() << ")");
        build_by_filament_blocks(filament_usage_blocks);
    } else {
        VORTEK_LOG(warning, "build_extruder_free_blocks: using build_by_extruder_blocks (extruder_blocks=" << extruder_usage_blocks.size() << ")");
        build_by_extruder_blocks(extruder_usage_blocks);
    }
}

// Reference to BBS: GCodeProcessor.cpp — build_by_filament_blocks
void PreCooling::build_by_filament_blocks(const std::vector<FilamentUsageBlock>& filament_usage_blocks)
{
    VORTEK_LOG(warning, "build_by_filament_blocks: input filament_blocks=" << filament_usage_blocks.size());
    for (size_t i = 0; i < filament_usage_blocks.size(); ++i) {
        const auto& b = filament_usage_blocks[i];
        VORTEK_LOG(warning, "  filament_block[" << i << "]: fil=" << b.filament_id
            << " ext=" << b.extruder_id << " nozzle=" << b.nozzle_id
            << " lower=" << b.lower_gcode_id << " upper=" << b.upper_gcode_id);
    }
    m_extruder_free_blocks.clear();
    std::map<int, std::vector<FilamentUsageBlock>> per_extruder_usage_blocks;
    for (auto& block : filament_usage_blocks) {
        per_extruder_usage_blocks[block.extruder_id].emplace_back(block);
    }

    FilamentUsageBlock start_filament_block(-1, -1, -1, 0, m_machine_start_gcode_end_id);
    FilamentUsageBlock end_filament_block(-1, -1, -1, m_machine_end_gcode_start_id, std::numeric_limits<unsigned int>::max());

    for (auto& elem : per_extruder_usage_blocks) {
        auto &blocks = elem.second;
        blocks.insert(blocks.begin(), start_filament_block);
        blocks.emplace_back(end_filament_block);
    }

    for (auto& elem : per_extruder_usage_blocks) {
        int extruder_id = elem.first;
        const auto& filament_blocks = elem.second;

        for (auto iter = filament_blocks.begin(); iter < filament_blocks.end(); ++iter) {
            auto niter = std::next(iter);
            if (niter == filament_blocks.end())
                break;
            ExtruderFreeBlock block;
            block.free_lower_gcode_id  = iter->upper_gcode_id;
            block.last_filament_id     = iter->filament_id;
            block.last_nozzle_id       = iter->nozzle_id;
            block.free_upper_gcode_id  = niter->lower_gcode_id;
            block.next_filament_id     = niter->filament_id;
            block.next_nozzle_id       = niter->nozzle_id;
            if (block.last_nozzle_id == -1)
                block.last_nozzle_id = block.next_nozzle_id;
            block.extruder_id = extruder_id;
            block.partial_free_lower_id = block.free_lower_gcode_id;
            block.partial_free_upper_id = block.free_lower_gcode_id;
            m_extruder_free_blocks.emplace_back(block);
        }
    }
    // For H2C, ignore_cooling_before_tower should be false to enable preheat_temperature_delta
    // (pre-heat to lower temp like 200°C instead of full 220°C, matching BBS behavior).
    // Reference to BBS: build_by_filament_blocks does NOT force this flag for H2C.
    std::for_each(m_extruder_free_blocks.begin(), m_extruder_free_blocks.end(), [](ExtruderFreeBlock &block) { block.ignore_cooling_before_tower = false; });
    std::sort(m_extruder_free_blocks.begin(), m_extruder_free_blocks.end(), [](const auto& a, const auto& b) {
        return a.free_lower_gcode_id < b.free_lower_gcode_id || (a.free_lower_gcode_id == b.free_lower_gcode_id && a.free_upper_gcode_id < b.free_upper_gcode_id);
    });
    VORTEK_LOG(warning, "build_by_filament_blocks: output free_blocks=" << m_extruder_free_blocks.size());
    for (size_t i = 0; i < m_extruder_free_blocks.size(); ++i) {
        const auto& fb = m_extruder_free_blocks[i];
        VORTEK_LOG(warning, "  free_block[" << i << "]: ext=" << fb.extruder_id
            << " last_fil=" << fb.last_filament_id << " next_fil=" << fb.next_filament_id
            << " last_nzl=" << fb.last_nozzle_id << " next_nzl=" << fb.next_nozzle_id
            << " lower=" << fb.free_lower_gcode_id << " upper=" << fb.free_upper_gcode_id
            << " partial_lower=" << fb.partial_free_lower_id << " partial_upper=" << fb.partial_free_upper_id);
    }
}

// Reference to BBS: GCodeProcessor.cpp:6749-6801 — build_by_extruder_blocks
void PreCooling::build_by_extruder_blocks(const std::vector<ExtruderUsageBlock>& extruder_usage_blocks)
{
    VORTEK_LOG(warning, "build_by_extruder_blocks: input extruder_blocks=" << extruder_usage_blocks.size());
    for (size_t i = 0; i < extruder_usage_blocks.size(); ++i) {
        const auto& b = extruder_usage_blocks[i];
        VORTEK_LOG(warning, "  extruder_block[" << i << "]: ext=" << b.extruder_id
            << " start_fil=" << b.start_filament << " end_fil=" << b.end_filament
            << " start_nzl=" << b.start_nozzle_id << " end_nzl=" << b.end_nozzle_id
            << " start=" << b.start_id << " end=" << b.end_id
            << " post_start=" << b.post_extrusion_start_id << " post_end=" << b.post_extrusion_end_id
            << " ignore_tower=" << b.ignore_cooling_before_tower);
    }

    m_extruder_free_blocks.clear();
    std::map<int, std::vector<ExtruderUsageBlock>> per_extruder_usage_blocks;
    for (auto& block : extruder_usage_blocks)
        per_extruder_usage_blocks[block.extruder_id].emplace_back(block);

    // Add sentinel blocks for each extruder (same as BBS)
    for (auto& elem : per_extruder_usage_blocks) {
        size_t extruder_id = elem.first;
        auto& blocks = elem.second;

        ExtruderUsageBlock start_block;
        start_block.initialize_step_1(extruder_id, 0, -1, -1);
        start_block.initialize_step_2(m_machine_start_gcode_end_id);
        start_block.initialize_step_3(m_machine_start_gcode_end_id, -1, m_machine_start_gcode_end_id, -1);

        ExtruderUsageBlock end_block;
        end_block.initialize_step_1(extruder_id, m_machine_end_gcode_start_id, -1, -1);
        end_block.initialize_step_2(std::numeric_limits<int>::max());
        end_block.initialize_step_3(std::numeric_limits<int>::max(), -1, std::numeric_limits<int>::max(), -1);

        blocks.insert(blocks.begin(), start_block);
        blocks.emplace_back(end_block);
    }

    for (auto& elem : per_extruder_usage_blocks) {
        size_t extruder_id = elem.first;
        const auto& blocks = elem.second;
        for (auto iter = blocks.begin(); iter != blocks.end(); ++iter) {
            auto niter = std::next(iter);
            if (niter == blocks.end())
                break;
            ExtruderFreeBlock block;
            block.free_lower_gcode_id   = iter->end_id;
            block.last_filament_id      = iter->end_filament;
            block.last_nozzle_id        = iter->end_nozzle_id;
            block.free_upper_gcode_id   = niter->start_id;
            block.next_filament_id      = niter->start_filament;
            block.next_nozzle_id        = niter->start_nozzle_id;
            if (block.last_nozzle_id == -1)
                block.last_nozzle_id = block.next_nozzle_id;
            block.extruder_id           = extruder_id;
            block.partial_free_lower_id = iter->post_extrusion_start_id;
            block.partial_free_upper_id = iter->post_extrusion_end_id;
            block.ignore_cooling_before_tower = niter->ignore_cooling_before_tower;
            m_extruder_free_blocks.emplace_back(block);
        }
    }

    std::sort(m_extruder_free_blocks.begin(), m_extruder_free_blocks.end(), [](const auto& a, const auto& b) {
        return a.free_lower_gcode_id < b.free_lower_gcode_id || (a.free_lower_gcode_id == b.free_lower_gcode_id && a.free_upper_gcode_id < b.free_upper_gcode_id);
    });

    VORTEK_LOG(warning, "build_by_extruder_blocks: output free_blocks=" << m_extruder_free_blocks.size());
    for (size_t i = 0; i < m_extruder_free_blocks.size(); ++i) {
        const auto& fb = m_extruder_free_blocks[i];
        VORTEK_LOG(warning, "  free_block[" << i << "]: ext=" << fb.extruder_id
            << " last_fil=" << fb.last_filament_id << " next_fil=" << fb.next_filament_id
            << " last_nzl=" << fb.last_nozzle_id << " next_nzl=" << fb.next_nozzle_id
            << " lower=" << fb.free_lower_gcode_id << " upper=" << fb.free_upper_gcode_id
            << " partial_lower=" << fb.partial_free_lower_id << " partial_upper=" << fb.partial_free_upper_id
            << " ignore_tower=" << fb.ignore_cooling_before_tower);
    }
}

// Reference to BBS: GCodeProcessor.cpp — inject_cooling_heating_command
void PreCooling::inject_cooling_heating_command(
    InsertedLinesMap& inserted_operation_lines,
    const ExtruderFreeBlock& block,
    float curr_temp,
    float target_temp,
    bool pre_cooling,
    bool pre_heating,
    bool suppress_cooling_emission
)
{
    VORTEK_LOG(warning, "inject_cooling_heating_command: extruder " << block.extruder_id 
                      << ", curr_temp = " << curr_temp << ", target_temp = " << target_temp 
                      << ", pre_cooling = " << pre_cooling << ", pre_heating = " << pre_heating);

    auto get_valid_extruder_id = [&](int last_nozzle_id) {
        auto nozzle_opt = m_nozzle_group_result.get_nozzle_from_id(last_nozzle_id);
        // NozzleInfo::extruder_id is 0-based (matches physical_extruder_map indexing)
        return nozzle_opt ? nozzle_opt->extruder_id : 0;
    };

    auto is_pre_cooling_valid = [&nozzle_temps = m_filament_nozzle_temps, &pre_cooling_temps = m_filament_pre_cooling_temps_nc](int idx) -> bool {
        if (idx < 0 || idx >= (int)pre_cooling_temps.size())
            return false;
        return pre_cooling_temps[idx] > 0 && pre_cooling_temps[idx] < nozzle_temps[idx];
    };

    auto get_partial_free_cooling_thres = [&nozzle_temps = m_filament_nozzle_temps, &pre_cooling_temps = m_filament_pre_cooling_temps_nc](int idx) -> float {
        if (idx < 0 || idx >= (int)pre_cooling_temps.size())
            return 30.f;
        float temp_in_tower = nozzle_temps[idx];
        return temp_in_tower - (float)(pre_cooling_temps[idx]);
    };

    auto gcode_move_comp = [](const Slic3r::GCodeProcessorResult::MoveVertex& a, unsigned int gcode_id) {
        return a.gcode_id < gcode_id;
    };

    auto find_skip_block_end = [this](unsigned int gcode_id) -> unsigned int {
        auto it = std::upper_bound(
            m_skippable_blocks.begin(), m_skippable_blocks.end(), gcode_id,
            [](unsigned int id, const std::pair<unsigned int, unsigned int>& b) { return id < b.first; }
        );
        if (it != m_skippable_blocks.begin()) {
            auto candidate = std::prev(it);
            if (gcode_id >= candidate->first && gcode_id <= candidate->second)
                return candidate->second;
        }
        return 0;
    };

    auto find_skip_block_start = [this](unsigned int gcode_id) -> unsigned int {
        auto it = std::upper_bound(
            m_skippable_blocks.begin(), m_skippable_blocks.end(), gcode_id,
            [](unsigned int id, const std::pair<unsigned int, unsigned int>& b) { return id < b.first; }
        );
        if (it != m_skippable_blocks.begin()) {
            auto candidate = std::prev(it);
            if (gcode_id >= candidate->first && gcode_id <= candidate->second)
                return candidate->first;
        }
        return 0;
    };

    auto adjust_iter = [&](std::vector<Slic3r::GCodeProcessorResult::MoveVertex>::const_iterator iter,
                           const std::vector<Slic3r::GCodeProcessorResult::MoveVertex>::const_iterator& begin,
                           const std::vector<Slic3r::GCodeProcessorResult::MoveVertex>::const_iterator& end,
                           bool forward) -> std::vector<Slic3r::GCodeProcessorResult::MoveVertex>::const_iterator
    {
        if (forward) {
            while (iter != end) {
                unsigned current_id = iter->gcode_id;
                unsigned skip_block_end_val = find_skip_block_end(current_id);
                if (skip_block_end_val == 0)
                    break;
                iter = std::lower_bound(iter, end, skip_block_end_val + 1, gcode_move_comp);
            }
        }
        else {
            while (iter != begin) {
                unsigned current_id = iter->gcode_id;
                unsigned skip_block_start_val = find_skip_block_start(current_id);
                if (skip_block_start_val == 0)
                    break;
                auto new_iter = std::lower_bound(begin, iter, skip_block_start_val, gcode_move_comp);
                if (new_iter == begin)
                    break;
                iter = std::prev(new_iter);
            }
        }
        return iter;
    };

    int last_extruder_id = get_valid_extruder_id(block.last_nozzle_id);
    int next_extruder_id = get_valid_extruder_id(block.next_nozzle_id);
    float ext_heating_rate = m_heating_rate.size() > (size_t)next_extruder_id ? m_heating_rate[next_extruder_id] : 2.0f;
    float ext_cooling_rate = m_cooling_rate.size() > (size_t)last_extruder_id ? m_cooling_rate[last_extruder_id] : 0.5f;

    // Diagnostic: log extruder routing and physical_extruder_map
    {
        std::string pem_str;
        for (size_t i = 0; i < m_physical_extruder_map.size(); ++i)
            pem_str += (i ? "," : "") + std::to_string(m_physical_extruder_map[i]);
        int last_phys = (last_extruder_id >= 0 && last_extruder_id < (int)m_physical_extruder_map.size()) ? m_physical_extruder_map[last_extruder_id] : -1;
        int next_phys = (next_extruder_id >= 0 && next_extruder_id < (int)m_physical_extruder_map.size()) ? m_physical_extruder_map[next_extruder_id] : -1;
        VORTEK_LOG(warning, "inject_cooling_heating_command:"
            << " last_nozzle=" << block.last_nozzle_id << "->last_ext=" << last_extruder_id << "->phys_T" << last_phys
            << " | next_nozzle=" << block.next_nozzle_id << "->next_ext=" << next_extruder_id << "->phys_T" << next_phys
            << " | physical_extruder_map=[" << pem_str << "]"
            << " | heating_rate=" << ext_heating_rate << " cooling_rate=" << ext_cooling_rate);
    }

    auto add_M104_lines = [&](int gcode_id, int target_extruder, int target_temp, int target_filament, bool skippable, int next_filament_idx, int next_nozzle_id, int type, const std::string& comment = std::string()) {
        auto format_line_M104 = [&](int target_extruder_inner, int target_temp_inner, int target_filament_inner, bool skippable_inner, int next_filament_idx_inner, int next_nozzle_id_inner, const std::string& comment_inner) -> std::vector<std::string> {
            std::vector<std::string> buffer;
            if (skippable_inner) {
                const bool support_dynamic_nozzle_map = m_nozzle_group_result.is_support_dynamic_nozzle_map();
                std::string m632_line = "M632 S" + std::to_string(next_filament_idx_inner);
                if (support_dynamic_nozzle_map)
                    m632_line += " H" + std::to_string(next_nozzle_id_inner);
                if (m_extruder_max_nozzle_count.size() > (size_t)target_extruder_inner && m_extruder_max_nozzle_count[target_extruder_inner] > 1)
                    m632_line += " N R";
                m632_line += " W\n";
                buffer.emplace_back(std::move(m632_line));
            }
            buffer.emplace_back("M400\n");
            std::string M104_line = "M104";
            if (m_handle_hotend_as_extruder) {
                M104_line += (" I" + std::to_string(target_filament_inner == -1 ? next_filament_idx_inner : target_filament_inner));
            }
            else if (target_extruder_inner != -1) {
                M104_line += (" T" + std::to_string(m_physical_extruder_map[target_extruder_inner]));
            }

            M104_line += " S" + std::to_string(target_temp_inner);
            M104_line += " N0";

            if (!comment_inner.empty())
                M104_line += " ;" + comment_inner;
            M104_line += '\n';

            buffer.emplace_back(M104_line);

            if (skippable_inner)
                buffer.emplace_back("M633\n");

            return buffer;
        };

        std::vector<std::string> formatted = format_line_M104(target_extruder, target_temp, target_filament, skippable, next_filament_idx, next_nozzle_id, comment);
        for (auto& line : formatted) {
            std::string log_line = line;
            if (!log_line.empty() && log_line.back() == '\n') log_line.pop_back();
            VORTEK_LOG(warning, "inject_bbs: GCODE_ID=" << gcode_id << " LINE=" << log_line << " COMMENT=" << comment);
            inserted_operation_lines[gcode_id].emplace_back(line, type);
        }
    };

    // Reference to BBS: GCodeProcessor.cpp:6563-6564 — zero/inverted blocks are skipped.
    // With build_by_extruder_blocks, nozzle changes have proper non-zero free windows.
    if (!pre_cooling && !pre_heating && block.free_upper_gcode_id <= block.free_lower_gcode_id)
        return;

    auto move_iter_lower = std::lower_bound(m_moves.cbegin(), m_moves.cend(), block.free_lower_gcode_id, gcode_move_comp);
    auto move_iter_upper = std::lower_bound(m_moves.cbegin(), m_moves.cend(), block.free_upper_gcode_id, gcode_move_comp);

    if (move_iter_lower == m_moves.cend() || move_iter_upper == m_moves.cbegin())
        return;
    --move_iter_upper;

    float complete_free_time_gap = 0;
    if (move_iter_lower == m_moves.cbegin())
        complete_free_time_gap = get_cum_time(move_iter_upper);
    else
        complete_free_time_gap = get_cum_time(move_iter_upper) - get_cum_time(std::prev(move_iter_lower));

    auto partial_free_move_lower = std::lower_bound(m_moves.cbegin(), m_moves.cend(), block.partial_free_lower_id, gcode_move_comp);
    auto partial_free_move_upper = std::lower_bound(m_moves.cbegin(), m_moves.cend(), block.partial_free_upper_id, gcode_move_comp);
    if (partial_free_move_lower == m_moves.cend() || partial_free_move_upper == m_moves.cbegin())
        return;
    --partial_free_move_upper;

    float partial_free_time_gap = 0;
    if (partial_free_move_lower == m_moves.cbegin())
        partial_free_time_gap = get_cum_time(partial_free_move_upper);
    else
        partial_free_time_gap = get_cum_time(partial_free_move_upper) - get_cum_time(std::prev(partial_free_move_lower));

    if (move_iter_lower >= move_iter_upper) {
        // Reference to BBS: GCodeProcessor.cpp:6590 — complete free window is zero.
        // For intra-extruder nozzle changes, partial_free window IS available.
        // BBS generates M104 cooldown (M632/M633 skippable) BEFORE TC using partial_free,
        // and M104 reheat AFTER TC.
        bool is_nozzle_change = block.last_nozzle_id != block.next_nozzle_id
                                && block.last_nozzle_id >= 0 && block.next_nozzle_id >= 0;
        bool has_partial_free = partial_free_move_lower < partial_free_move_upper;

        if (is_nozzle_change && has_partial_free) {
            VORTEK_LOG(warning, "inject_cooling_heating: intra-extruder nozzle change ("
                << block.last_nozzle_id << "->" << block.next_nozzle_id
                << ") using partial_free [" << block.partial_free_lower_id << ".." << block.partial_free_upper_id << "]");

            // 1. Cooldown at partial_free_lower (BEFORE TC) — skippable with M632/M633
            int cooldown_temp_nc = 0;
            if (block.last_filament_id >= 0 && block.last_filament_id < (int)m_filament_pre_cooling_temps_nc.size())
                cooldown_temp_nc = m_filament_pre_cooling_temps_nc[block.last_filament_id];
            if (cooldown_temp_nc <= 0)
                cooldown_temp_nc = 180;  // fallback

            add_M104_lines(block.partial_free_lower_id, last_extruder_id, cooldown_temp_nc,
                           block.last_filament_id, true /*skippable*/,
                           block.next_filament_id, block.next_nozzle_id, 1,
                           "Multi extruder nozzle change cooldown");

            // 2. Reheat at free_upper (AFTER TC) — not skippable
            int reheat_temp = 220;
            if (block.next_filament_id >= 0 && block.next_filament_id < (int)m_filament_nozzle_temps.size())
                reheat_temp = m_filament_nozzle_temps[block.next_filament_id];

            add_M104_lines(block.free_upper_gcode_id, next_extruder_id, reheat_temp,
                           block.next_filament_id, false /*not skippable*/,
                           block.next_filament_id, block.next_nozzle_id, 2,
                           "Multi extruder nozzle change reheat");
        }
        return;
    }

    bool apply_cooling_when_partial_free = is_pre_cooling_valid(block.last_filament_id) && pre_cooling;

    if (apply_cooling_when_partial_free && partial_free_time_gap + complete_free_time_gap < m_inject_time_threshold)
        return;

    if (!apply_cooling_when_partial_free && complete_free_time_gap < m_inject_time_threshold)
        return;

    // extruder_id, ext_heating_rate, ext_cooling_rate, add_M104_lines already defined above zero-length check

    constexpr float room_temperature = 25.f;

    if (apply_cooling_when_partial_free) {
        float max_cooling_temp = std::min(curr_temp, std::min(get_partial_free_cooling_thres(block.last_filament_id), partial_free_time_gap * ext_cooling_rate));
        curr_temp = std::max(room_temperature, curr_temp - max_cooling_temp);
        if (!suppress_cooling_emission) {
            add_M104_lines(block.partial_free_lower_id, last_extruder_id, curr_temp, block.last_filament_id, false, block.next_filament_id, block.next_nozzle_id, 1, "Multi extruder pre cooling in post extrusion");
        } else {
            VORTEK_LOG(warning, "inject_cooling_heating: suppress partial cooling emission (sentinel block)");
        }
    }

    if (pre_cooling && !pre_heating) {
        if (target_temp >= curr_temp)
            return;
        int clamped_target = std::max((int)room_temperature, (int)target_temp);
        // Reference to BBS: GCodeProcessor.cpp — cooldown injected at partial_free_lower_id (post-extrusion, BEFORE TC)
        // NOT at free_lower_gcode_id which is AT the TC line itself.
        unsigned int cooldown_id = (block.partial_free_lower_id < block.free_lower_gcode_id)
                                    ? block.partial_free_lower_id
                                    : block.free_lower_gcode_id;
        VORTEK_LOG(warning, "inject_cooling_heating: cooldown S" << clamped_target
            << " at gcode_id=" << cooldown_id
            << " (partial_free_lower=" << block.partial_free_lower_id
            << " free_lower=" << block.free_lower_gcode_id << ")");
        add_M104_lines(cooldown_id, last_extruder_id, clamped_target, block.last_filament_id, false, block.next_filament_id, block.next_nozzle_id, 1, "Multi extruder pre cooling");
        return;
    }

    if (!pre_cooling && pre_heating) {
        if (target_temp <= curr_temp)
            return;
        float heating_start_time = get_cum_time(move_iter_upper) - (target_temp - curr_temp) / ext_heating_rate;
        auto heating_move_iter = std::upper_bound(move_iter_lower, move_iter_upper + 1, heating_start_time, [this](float time, const Slic3r::GCodeProcessorResult::MoveVertex& a) { return time < get_cum_time(m_moves.cbegin() + (&a - &m_moves[0])); });
        if (heating_move_iter == move_iter_lower) {
            add_M104_lines(block.free_lower_gcode_id, next_extruder_id, target_temp, block.next_filament_id, true, block.next_filament_id, block.next_nozzle_id, 2, "Multi extruder pre heating");
        }
        else {
            --heating_move_iter;
            heating_move_iter = adjust_iter(heating_move_iter, move_iter_lower, move_iter_upper, false);
            add_M104_lines(heating_move_iter->gcode_id, next_extruder_id, target_temp, block.next_filament_id, true, block.next_filament_id, block.next_nozzle_id, 2, "Multi extruder pre heating");
        }
        return;
    }

    // perform cooling first and then perform heating
    //
    // WHY no park_temp_nc floor on mid_temp here (vs Galantcev PR f58acc2a56):
    //   f58acc2a56 clamped mid_temp and cooling_temp at park_temp_nc (≈180°C) inside THIS function
    //   to prevent the carousel nozzle from cooling all the way to room temperature when the free
    //   window is long. That floor was correct in intent but implemented in the wrong place:
    //   injecting a per-extruder floor into a generic function makes the logic implicit and
    //   couples carousel semantics into a BBS-ported utility.
    //
    //   Our design moves the decision upstream: process_pre_cooling_and_heating() passes
    //   target_temp = park_temp_nc (not print_temp) for carousel blocks. With target_temp=180°C:
    //   - Short free window → formula gives mid_temp > 180 → cooling halts above park temp anyway.
    //   - Long free window  → formula gives mid_temp ≈ room_temp; nozzle cools to ~25°C, then
    //     preheats back to park_temp_nc. This is INTENTIONAL: the nozzle has time to fully cool
    //     and be reheated to park_temp_nc well before the toolchange; firmware NC then handles
    //     the short park_temp_nc → print_temp step from a known warm state.
    //   The final-idle block uses pre_heating=false → falls into the pre_cooling-only path above
    //   and legitimately cools to room temperature. No floor needed anywhere in this function.
    // Reference to BBS: BambuStudio commit 284ae6e2a5 — target_temp IS park_temp_nc.
    float mid_temp = std::max(room_temperature, (curr_temp * ext_heating_rate + target_temp * ext_cooling_rate - complete_free_time_gap * ext_cooling_rate * ext_heating_rate) / (ext_cooling_rate + ext_heating_rate));
    float heating_temp = target_temp - mid_temp;
    float heating_start_time = get_cum_time(move_iter_upper) - heating_temp / ext_heating_rate;
    auto heating_move_iter = std::upper_bound(move_iter_lower, move_iter_upper + 1, heating_start_time, [this](float time, const Slic3r::GCodeProcessorResult::MoveVertex& a) { return time < get_cum_time(m_moves.cbegin() + (&a - &m_moves[0])); });
    
    VORTEK_LOG(warning, "[DBG] mid_temp=" << mid_temp << " heating_temp=" << heating_temp
        << " upper_time=" << get_cum_time(move_iter_upper)
        << " heating_start_time=" << heating_start_time
        << " lower_time=" << get_cum_time(move_iter_lower)
        << " heating_iter_at_lower=" << (heating_move_iter == move_iter_lower));

    if (heating_move_iter == move_iter_lower)
        return;
    --heating_move_iter;
    heating_move_iter = adjust_iter(heating_move_iter, move_iter_lower, move_iter_upper, false);

    float real_cooling_time = get_cum_time(heating_move_iter) - get_cum_time(move_iter_lower);
    int real_delta_temp = std::min((int)(real_cooling_time * ext_cooling_rate), (int)curr_temp);
    VORTEK_LOG(warning, "[DBG] real_cooling_time=" << real_cooling_time << " real_delta_temp=" << real_delta_temp);
    if (real_delta_temp == 0)
        return;
    int cooling_temp = std::max((int)room_temperature, (int)curr_temp - real_delta_temp);
    if (!suppress_cooling_emission) {
        // Reference to BBS: cooldown injected at partial_free_lower_id (BEFORE TC), not AT TC line.
        unsigned int cooldown_id = (block.partial_free_lower_id < block.free_lower_gcode_id)
                                    ? block.partial_free_lower_id
                                    : block.free_lower_gcode_id;
        VORTEK_LOG(warning, "inject_cooling_heating: combined cooldown S" << cooling_temp
            << " at gcode_id=" << cooldown_id
            << " (partial_free_lower=" << block.partial_free_lower_id
            << " free_lower=" << block.free_lower_gcode_id << ")");
        add_M104_lines(cooldown_id, last_extruder_id, cooling_temp, block.last_filament_id, false, block.next_filament_id, block.next_nozzle_id, 1, "Multi extruder pre cooling");
    } else {
        VORTEK_LOG(warning, "inject_cooling_heating: suppress full cooling emission (sentinel), would be S" << cooling_temp);
    }
    add_M104_lines(heating_move_iter->gcode_id, next_extruder_id, target_temp, block.next_filament_id, true, block.next_filament_id, block.next_nozzle_id, 2, "Multi extruder pre heating");

    // Reference to BBS: BambuStudio/src/libslic3r/GCode/GCodeProcessor.cpp
    // When preheat_temperature_delta is active (target_temp < nozzle_temp), BBS injects
    // a second M104 at full nozzle_temp during the wipe tower/purge after the toolchange.
    // This allows the nozzle to warm from preheat (170-200°C) to print temp (220°C) during purge.
    if (block.next_filament_id >= 0 && block.next_filament_id < (int)m_filament_nozzle_temps.size()) {
        int nozzle_temp = m_filament_nozzle_temps[block.next_filament_id];
        if ((int)target_temp < nozzle_temp) {
            // Use post_tc_gcode_id (after NOZZLE_CHANGE_END) if available,
            // otherwise fall back to free_upper_gcode_id
            unsigned int reheat_gcode_id = block.post_tc_gcode_id > 0 ? block.post_tc_gcode_id : block.free_upper_gcode_id;
            VORTEK_LOG(warning, "inject_cooling_heating: adding post-TC reheat S" << nozzle_temp << " (preheat was S" << (int)target_temp << ") at gcode_id=" << reheat_gcode_id << " (post_tc=" << block.post_tc_gcode_id << ")");
            add_M104_lines(reheat_gcode_id, next_extruder_id, nozzle_temp, block.next_filament_id, false, block.next_filament_id, block.next_nozzle_id, 2, "Multi extruder post-TC reheat");
        }
    }
}

void PreCooling::inject_cooling_heating_command_bbs(
    InsertedLinesMap& inserted_operation_lines,
    const ExtruderFreeBlock& block,
    float curr_temp,
    float target_temp,
    bool pre_cooling,
    bool pre_heating
)
{
}

void PreCooling::inject_cooling_heating_command_orca(
    InsertedLinesMap& inserted_operation_lines,
    const ExtruderFreeBlock& block,
    float curr_temp,
    float target_temp,
    bool pre_cooling,
    bool pre_heating
)
{
}

PreCooling::InsertedLinesMap PreCooling::run_pre_scan(Slic3r::GCodeProcessor& processor, const std::string& filename)
{
    VORTEK_LOG(warning, "run_pre_scan started on file: " << filename);
    InsertedLinesMap inserted_operation_lines;
    
    // Hook isolation check for multi-nozzle configuration / H2C compatibility
    if (!processor.m_print || !is_h2c_printer(*processor.m_print) || !processor.m_print->get_layered_nozzle_group_result()) {
        VORTEK_LOG(warning, "run_pre_scan: hook bypass (not an H2C/multi-nozzle configuration)");
        return inserted_operation_lines;
    }
    VORTEK_LOG(warning, "run_pre_scan: H2C config detected, proceeding with pre-scan");

    const Slic3r::PrintConfig& print_config = processor.m_print->config();
    const auto& nozzle_group = *processor.m_print->get_layered_nozzle_group_result();

    std::vector<FilamentUsageBlock> filament_blocks;
    std::vector<ExtruderUsageBlock> extruder_blocks = { ExtruderUsageBlock() };
    std::vector<std::pair<unsigned int, unsigned int>> skippable_blocks;

    unsigned int machine_start_gcode_end_line_id = 0;
    unsigned int machine_end_gcode_start_line_id = std::numeric_limits<unsigned int>::max();

    int current_layer_id = 0;
    unsigned int line_id = 0;

    auto handle_nozzle_change_line = [&](const std::string& line, int& old_filament, int& next_filament, int& extruder_id, int& old_nozzle_id, int& new_nozzle_id) -> bool {
        std::regex re(R"(OF(\d+)\s+NF(\d+)\s+ON(\d+)\s+NN(\d+))");
        std::smatch match;
        if (!std::regex_search(line, match, re))
            return false;
        old_filament = std::stoi(match[1]);
        next_filament = std::stoi(match[2]);
        old_nozzle_id = std::stoi(match[3]);
        new_nozzle_id = std::stoi(match[4]);
        auto nozzle_opt = nozzle_group.get_nozzle_from_id(new_nozzle_id);
        extruder_id = nozzle_opt ? nozzle_opt->extruder_id : -1;
        return true;
    };

    // Fix 1: Track last filament state from start gcode, but don't create blocks for it.
    int start_gcode_last_filament = -1;
    int start_gcode_last_nozzle = -1;

    auto handle_filament_change = [&](int filament_id, int current_line_id, int nozzle_id = -1) {
        // Don't create filament blocks for T-commands in start gcode —
        // they are just initialization, not real filament usage.
        if (machine_start_gcode_end_line_id == 0) {
            start_gcode_last_filament = filament_id;
            start_gcode_last_nozzle = nozzle_id;
            VORTEK_LOG(warning, "handle_filament_change: SKIP (in start gcode) fil=" << filament_id << " line=" << current_line_id);
            return;
        }
        if (static_cast<unsigned int>(current_line_id) > machine_end_gcode_start_line_id) {
            return;
        }
        if (!filament_blocks.empty()) {
            VORTEK_LOG(warning, "handle_filament_change: closing block for fil=" << filament_blocks.back().filament_id << " upper=" << current_line_id);
            filament_blocks.back().upper_gcode_id = current_line_id;
        }
        if (nozzle_id == -1) {
            nozzle_id = nozzle_group.get_nozzle_id(filament_id, current_layer_id);
        }
        int extruder_id = 0;
        auto nozzle_ptr = nozzle_group.get_nozzle_from_id(nozzle_id);
        if (nozzle_ptr)
            extruder_id = nozzle_ptr->extruder_id;
        VORTEK_LOG(warning, "handle_filament_change: new block fil=" << filament_id << " ext=" << extruder_id << " nozzle=" << nozzle_id << " lower=" << current_line_id);
        filament_blocks.emplace_back(filament_id, extruder_id, nozzle_id, current_line_id, -1);
    };

    Slic3r::GCodeReader parser;
    parser.parse_file(filename, [&](Slic3r::GCodeReader& reader, const Slic3r::GCodeReader::GCodeLine& line) {
        ++line_id;
        const std::string& raw_line = line.raw();

        if (machine_start_gcode_end_line_id == 0 && 
            (raw_line.find("CHANGE_LAYER") != std::string::npos || raw_line.find("Z_HEIGHT") != std::string::npos)) {
            machine_start_gcode_end_line_id = line_id;
            VORTEK_LOG(warning, "run_pre_scan: machine_start_gcode_end at line " << line_id);
            // Create initial filament block from the last T-command seen in start gcode
            if (start_gcode_last_filament >= 0) {
                VORTEK_LOG(warning, "run_pre_scan: creating initial block from start gcode: fil=" << start_gcode_last_filament << " nozzle=" << start_gcode_last_nozzle);
                handle_filament_change(start_gcode_last_filament, line_id, start_gcode_last_nozzle);
            }
        }

        if (raw_line.find("machine:") != std::string::npos) {
            std::regex re_end(R"(machine:\s+\w+\s+end)");
            bool is_match = std::regex_search(raw_line, re_end);
            bool has_equals = (raw_line.find(" = ") != std::string::npos);
            if (machine_start_gcode_end_line_id > 0 && !has_equals && is_match) {
                machine_end_gcode_start_line_id = line_id;
                VORTEK_LOG(warning, "run_pre_scan: machine_end_gcode_start at line " << line_id);
            }
        }

        if (Slic3r::GCodeReader::GCodeLine::cmd_starts_with(raw_line, "T")) {
            int fid = -1;
            const char* p_space = raw_line.data();
            while (*p_space == ' ' || *p_space == '\t') ++p_space;
            int skips = p_space - raw_line.data();
            std::istringstream str(raw_line.substr(skips + 1));
            str >> fid;
            if (!str.fail() && fid >= 0 && fid < 255) {
                int nozzle_id = -1;
                char param;
                while (str >> param) {
                    if (param == 'H') {
                        str >> nozzle_id;
                        break;
                    }
                }
                handle_filament_change(fid, line_id, nozzle_id);
            }
        }
        else if (Slic3r::GCodeReader::GCodeLine::cmd_starts_with(raw_line, ";VT")) {
            int fid = -1;
            const char* p_space = raw_line.data();
            while (*p_space == ' ' || *p_space == '\t') ++p_space;
            int skips = p_space - raw_line.data();
            std::istringstream str(raw_line.substr(skips + 3));
            str >> fid;
            if (!str.fail() && fid >= 0 && fid < 255) {
                int nozzle_id = -1;
                char param;
                while (str >> param) {
                    if (param == 'H') {
                        str >> nozzle_id;
                        break;
                    }
                }
                handle_filament_change(fid, line_id, nozzle_id);
            }
        }
        else if (Slic3r::GCodeReader::GCodeLine::cmd_starts_with(raw_line, "M1020")) {
            size_t s_pos = raw_line.find('S');
            if (s_pos != std::string::npos) {
                std::istringstream str(raw_line.substr(s_pos + 1));
                int fid = -1;
                str >> fid;
                if (!str.fail() && fid >= 0 && fid < 255) {
                    int nozzle_id = -1;
                    char param;
                    while (str >> param) {
                        if (param == 'H') {
                            str >> nozzle_id;
                            break;
                        }
                    }
                    handle_filament_change(fid, line_id, nozzle_id);
                }
            }
        }
        else if (Slic3r::GCodeReader::GCodeLine::cmd_starts_with(raw_line, ";_NOZZLE_CHANGE_START")) {
            int prev_filament = -1, next_filament = -1, extruder_id = -1, prev_nozzle_id = -1, next_nozzle_id = -1;
            handle_nozzle_change_line(raw_line, prev_filament, next_filament, extruder_id, prev_nozzle_id, next_nozzle_id);
            VORTEK_LOG(warning, "run_pre_scan: NOZZLE_CHANGE_START at line " << line_id << " OF" << prev_filament << " NF" << next_filament << " ON" << prev_nozzle_id << " NN" << next_nozzle_id);
            if (!extruder_blocks.empty()) {
                extruder_blocks.back().initialize_step_2(line_id);
            }
        }
        else if (raw_line.find(";_NOZZLE_CHANGE_END") != std::string::npos) {
            VORTEK_LOG(warning, "run_pre_scan: NOZZLE_CHANGE_END found at line " << line_id << " raw=" << raw_line.substr(0, std::min(raw_line.size(), (size_t)80)));
            std::string marker_line = raw_line.substr(raw_line.find(";_NOZZLE_CHANGE_END"));
            int prev_filament = -1, next_filament = -1, extruder_id = -1, prev_nozzle_id = -1, next_nozzle_id = -1;
            handle_nozzle_change_line(marker_line, prev_filament, next_filament, extruder_id, prev_nozzle_id, next_nozzle_id);
            if (!extruder_blocks.empty()) {
                extruder_blocks.back().initialize_step_3(line_id, prev_filament, line_id, prev_nozzle_id);
            }
            ExtruderUsageBlock temp_construct_block;
            temp_construct_block.initialize_step_1(extruder_id, line_id, next_filament, next_nozzle_id);
            extruder_blocks.emplace_back(temp_construct_block);
        }
        else if (Slic3r::GCodeReader::GCodeLine::cmd_starts_with(raw_line, ";_CP_TOOLCHANGE_WIPE")) {
            std::regex re(R"(CT(\d)(?:\s+FL(\d))?)");
            std::smatch match;
            bool is_contact = false;
            bool is_first_layer = false;
            if (std::regex_search(raw_line, match, re)) {
                is_contact = std::stoi(match[1]);
                is_first_layer = match[2].matched ? std::stoi(match[2]) != 0 : false;
            }
            if (!extruder_blocks.empty()) {
                extruder_blocks.back().ignore_cooling_before_tower = is_contact || is_first_layer;
            }
        }
        else if (Slic3r::GCodeReader::GCodeLine::cmd_starts_with(raw_line, ";LAYER_CHANGE")) {
            ++current_layer_id;
        }
    });

    VORTEK_LOG(warning, "run_pre_scan: parse complete. total_lines=" << line_id
        << " filament_blocks=" << filament_blocks.size()
        << " extruder_blocks=" << extruder_blocks.size()
        << " start_end=" << machine_start_gcode_end_line_id
        << " end_start=" << machine_end_gcode_start_line_id);
    if (!filament_blocks.empty()) {
        filament_blocks.back().upper_gcode_id = machine_end_gcode_start_line_id;
    }

    if (!extruder_blocks.empty()) {
        int first_filament = 0;
        int last_filament = 0;
        if (!filament_blocks.empty()) {
            first_filament = filament_blocks.front().filament_id;
            last_filament = filament_blocks.back().filament_id;
        }
        int first_extruder_id = -1;
        auto nozzle_info = nozzle_group.get_first_nozzle_for_filament(first_filament);
        if (nozzle_info)
            first_extruder_id = nozzle_info->extruder_id;
        int start_nozzle_id = nozzle_info ? nozzle_info->group_id : -1;
        extruder_blocks.front().initialize_step_1(first_extruder_id, machine_start_gcode_end_line_id, first_filament, start_nozzle_id);

        extruder_blocks.back().initialize_step_2(machine_end_gcode_start_line_id);
        int last_nozzle_id = -1;
        if (!filament_blocks.empty())
            last_nozzle_id = filament_blocks.back().nozzle_id;
        extruder_blocks.back().initialize_step_3(machine_end_gcode_start_line_id, last_filament, machine_end_gcode_start_line_id, last_nozzle_id);
    }

    std::vector<int> filament_nozzle_temps(print_config.nozzle_temperature.values);
    std::vector<int> filament_nozzle_temps_initial_layer(print_config.nozzle_temperature_initial_layer.values);
    {
        std::string temps_str;
        for (size_t i = 0; i < filament_nozzle_temps.size(); ++i)
            temps_str += (i ? "," : "") + std::to_string(filament_nozzle_temps[i]);
        VORTEK_LOG(warning, "run_pre_scan: nozzle_temps=[" << temps_str << "]");
    }
    std::vector<int> physical_extruder_map(print_config.physical_extruder_map.values);
    
    int standby_temp_delta = print_config.standby_temperature_delta.value;

    // Per-filament pre-cooling target temperature:
    //   - filament_pre_cooling_temperature_nc > 0  →  NC-specific carousel park temp (H2C right extruder)
    //   - otherwise → filament_pre_cooling_temperature (BBS standard timing, left extruder / non-NC)
    // Reference to BBS: GCodeProcessor.cpp:1931 — m_filament_pre_cooling_temp = config.filament_pre_cooling_temperature
    size_t num_fil = print_config.filament_type.values.size();
    std::vector<int> pre_cooling_temp_nc(num_fil, 0);
    for (size_t i = 0; i < num_fil; ++i) {
        int val_nc = 0;
        if (!print_config.filament_pre_cooling_temperature_nc.values.empty())
            val_nc = print_config.filament_pre_cooling_temperature_nc.get_at(i);
        if (val_nc > 0) {
            pre_cooling_temp_nc[i] = val_nc;
        } else if (!print_config.filament_pre_cooling_temperature.values.empty()) {
            pre_cooling_temp_nc[i] = print_config.filament_pre_cooling_temperature.get_at(i);
        }
    }
    {
        std::string temps_str;
        for (size_t i = 0; i < pre_cooling_temp_nc.size(); ++i)
            temps_str += (i ? "," : "") + std::to_string(pre_cooling_temp_nc[i]);
        VORTEK_LOG(warning, "run_pre_scan: pre_cooling_temp_nc (nc→fallback)=[" << temps_str << "]");
    }

    std::vector<int> filament_idle_temps(print_config.idle_temperature.values.size());
    for (size_t i = 0; i < filament_idle_temps.size(); ++i) {
        filament_idle_temps[i] = print_config.idle_temperature.get_at(i);
    }

    std::vector<double> cooling_rate;
    if (print_config.hotend_cooling_rate.values.empty()) {
        cooling_rate.resize(print_config.nozzle_diameter.values.size(), 0.5);
    } else {
        for (size_t i = 0; i < print_config.nozzle_diameter.values.size(); ++i) {
            cooling_rate.push_back(print_config.hotend_cooling_rate.get_at(i));
        }
    }

    std::vector<double> heating_rate;
    if (print_config.hotend_heating_rate.values.empty()) {
        heating_rate.resize(print_config.nozzle_diameter.values.size(), 2.0);
    } else {
        for (size_t i = 0; i < print_config.nozzle_diameter.values.size(); ++i) {
            heating_rate.push_back(print_config.hotend_heating_rate.get_at(i));
        }
    }

    std::vector<int> extruder_max_nozzle_count;
    if (print_config.extruder_max_nozzle_count.values.empty()) {
        extruder_max_nozzle_count.resize(print_config.nozzle_diameter.values.size(), 1);
    } else {
        for (size_t i = 0; i < print_config.nozzle_diameter.values.size(); ++i) {
            extruder_max_nozzle_count.push_back(print_config.extruder_max_nozzle_count.get_at(i));
        }
    }

    // Reference to BBS: BambuStudio/src/libslic3r/GCode/GCodeProcessor.cpp line 2082-2084
    // Read filament_preheat_temperature_delta from config (BBS default=0)
    size_t num_filaments = print_config.filament_type.values.size();
    std::vector<double> filament_preheat_temperature_delta(num_filaments, 0.0);
    if (!print_config.filament_preheat_temperature_delta.values.empty()) {
        for (size_t i = 0; i < num_filaments; ++i) {
            double val = print_config.filament_preheat_temperature_delta.get_at(i);
            filament_preheat_temperature_delta[i] = (val != 0 && !std::isnan(val)) ? val : 0.0;
        }
    }
    VORTEK_LOG(warning, "run_pre_scan: filament_preheat_temperature_delta read from config: ["
        << [&]() { std::string s; for (size_t i = 0; i < filament_preheat_temperature_delta.size(); ++i) { if (i) s += ","; s += std::to_string((int)filament_preheat_temperature_delta[i]); } return s; }()
        << "]");

    // Reference to BBS: BambuStudio/src/libslic3r/GCode/GCodeProcessor.hpp TimeProcessContext
    // BBS default for filament_max_temperature_drop_when_ec is 0.0 (not 50.0)
    std::vector<double> filament_max_temperature_drop_when_ec(num_filaments, 0.0);

    // Reference to BBS: m_result.extruder_types — in OrcaSlicer read from print_config.extruder_type
    std::vector<Slic3r::ExtruderType> extruder_types;
    for (size_t i = 0; i < print_config.extruder_type.values.size(); ++i) {
        extruder_types.push_back(static_cast<Slic3r::ExtruderType>(print_config.extruder_type.values[i]));
    }
    std::vector<double> nozzle_diameter(print_config.nozzle_diameter.values);

    // Reference to BBS: m_enable_pre_heating from config
    bool enable_pre_heating = print_config.enable_pre_heating.value;
    // Reference to BBS: m_handle_hotend_as_extruder — for H2C always false (hotend != extruder)
    bool handle_hotend_as_extruder = false;

    VORTEK_LOG(warning, "run_pre_scan: enable_pre_heating=" << enable_pre_heating
        << " handle_hotend_as_extruder=" << handle_hotend_as_extruder
        << " extruder_types_count=" << extruder_types.size());

    int valid_machine_id = 0;
    for (size_t i = 0; i < static_cast<size_t>(Slic3r::PrintEstimatedStatistics::ETimeMode::Count); ++i) {
        if (processor.m_time_processor.machines[i].enabled) {
            valid_machine_id = i;
            break;
        }
    }

    PreCooling pre_cooling_processor(
        processor.m_result.moves,
        nozzle_group,
        filament_nozzle_temps,
        filament_nozzle_temps_initial_layer,
        physical_extruder_map,
        valid_machine_id,
        0.0f,                   // inject_time_threshold (same as BBS)
        handle_hotend_as_extruder,
        print_config.has_filament_switcher.value,
        standby_temp_delta,
        pre_cooling_temp_nc,
        filament_idle_temps,
        cooling_rate,
        heating_rate,
        skippable_blocks,
        extruder_max_nozzle_count,
        filament_preheat_temperature_delta,
        filament_max_temperature_drop_when_ec,
        machine_start_gcode_end_line_id,
        machine_end_gcode_start_line_id,
        extruder_types,
        nozzle_diameter
    );

    pre_cooling_processor.build_extruder_free_blocks(filament_blocks, extruder_blocks);

    // Populate post_tc_gcode_id for each free block.
    // Each extruder_block (after first) starts at NOZZLE_CHANGE_END gcode_id.
    // For each free block, find the NC_END that is closest to and >= free_upper_gcode_id.
    // Reference to BBS: BambuStudio/src/libslic3r/GCode/GCodeProcessor.cpp
    {
        std::vector<unsigned int> nc_end_positions;
        for (size_t i = 1; i < extruder_blocks.size(); ++i) {
            if (extruder_blocks[i].start_id > 0) {
                nc_end_positions.push_back(extruder_blocks[i].start_id);
                VORTEK_LOG(warning, "run_pre_scan: NC_END position=" << extruder_blocks[i].start_id);
            }
        }
        std::sort(nc_end_positions.begin(), nc_end_positions.end());

        for (auto& fb : pre_cooling_processor.m_extruder_free_blocks) {
            // Find first NC_END that is >= free_upper_gcode_id
            auto it = std::lower_bound(nc_end_positions.begin(), nc_end_positions.end(), fb.free_upper_gcode_id);
            if (it != nc_end_positions.end()) {
                fb.post_tc_gcode_id = *it;
                VORTEK_LOG(warning, "run_pre_scan: free_block upper=" << fb.free_upper_gcode_id << " → post_tc=" << fb.post_tc_gcode_id);
            }
        }
    }

    pre_cooling_processor.process_pre_cooling_and_heating(inserted_operation_lines);

    VORTEK_LOG(warning, "run_pre_scan: DONE. inserted_operation_lines=" << inserted_operation_lines.size());
    for (const auto& entry : inserted_operation_lines) {
        VORTEK_LOG(warning, "  inject at gcode_id=" << entry.first << " lines=" << entry.second.size());
    }
    return inserted_operation_lines;
}

void PreCooling::inject_lines(
    InsertedLinesMap::iterator& precooling_iter,
    const InsertedLinesMap& precooling_inserted_lines,
    bool enable_pre_heating,
    unsigned int line_id,
    std::function<void(const std::string&)> append_line_fn
)
{
    while (precooling_iter != precooling_inserted_lines.end() && line_id > precooling_iter->first) {
        VORTEK_LOG(warning, "inject_lines: SKIPPED gcode_id=" << precooling_iter->first << " (current line_id=" << line_id << ")");
        ++precooling_iter;
    }
    if (precooling_iter != precooling_inserted_lines.end() && line_id == precooling_iter->first) {
        VORTEK_LOG(warning, "inject_lines: INJECTING at line_id=" << line_id << " count=" << precooling_iter->second.size());
        for (const auto& elem : precooling_iter->second) {
            if (enable_pre_heating) {
                std::string log_line = elem.first;
                if (!log_line.empty() && log_line.back() == '\n') log_line.pop_back();
                VORTEK_LOG(warning, "inject_lines:   -> " << log_line);
                append_line_fn(elem.first);
            }
        }
        ++precooling_iter;
    }
}

} // namespace Vortek
