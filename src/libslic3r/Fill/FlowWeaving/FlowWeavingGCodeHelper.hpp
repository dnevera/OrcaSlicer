#pragma once
// ── FlowWeavingGCodeHelper.hpp ──────────────────────────────────────────────
//
// Minimal helper for GCode.cpp to access per-segment XY flow factors
// from ExtrusionPathContoured without leaking FW internals into base code.
//
// Usage in GCode.cpp:
//   #include "Fill/FlowWeaving/FlowWeavingGCodeHelper.hpp"
//   ...
//   extrusion_ratio *= FlowWeaving::segment_flow_factor(path, seg_idx);
//
// ─────────────────────────────────────────────────────────────────────────────

#include "../../ExtrusionEntity.hpp"

namespace Slic3r {
namespace FlowWeaving {

// Returns the XY flow multiplier for the given segment of a contoured path.
// If the path has no flow_factors (non-FW path, or legacy contoured path),
// returns 1.0 — a transparent no-op for the caller.
inline double segment_flow_factor(const ExtrusionPath &path, size_t seg_idx)
{
    const auto *contoured = dynamic_cast<const ExtrusionPathContoured *>(&path);
    if (contoured && seg_idx < contoured->flow_factors.size())
        return contoured->flow_factors[seg_idx];
    return 1.0;
}

} // namespace FlowWeaving
} // namespace Slic3r
