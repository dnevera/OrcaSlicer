// ── Flow Weaving Fade Envelope ────────────────────────────────────────────────
//
// Computes the Z-amplitude fade factor for Flow Weaving near the infill
// boundary (first/last N infill layers).
//
// The key difference from the global-layer clamp in GCode.cpp is that this
// check is XY-aware: it walks upper_layer / lower_layer and, for each step,
// tests whether the specific print POINT falls inside an stInternal polygon.
//
// This correctly handles:
//   • Sloped top/bottom surfaces  — different XY points on the same layer
//     may be at different distances from the infill boundary.
//   • Bridge floors (stInternalBridge) — treated as a hard boundary because
//     the point falls outside stInternal on that layer.
//   • stInternalSolid — treated as regular infill (modulation is safe inside
//     solid regions; they are not the model surface boundary).
//     Only actual top/bottom EXTERNAL shells stop the walk.
//
// Algorithm:
//   n_above = # consecutive layers above where pt ∈ stInternal  (≤ fade_n)
//   n_below = # consecutive layers below where pt ∈ stInternal  (≤ fade_n)
//   t       = min(n_above, n_below) / (float)fade_n
//   fade    = smoothstep(t) = t²·(3 − 2t)           ∈ [0, 1]
//
// Cache:
//   The result is cached per-path (path_id).  Since lines within a single
//   infill sub-path are short (< 1 nozzle diameter), adjacent points always
//   give the same result.
//
// Usage in GCode.cpp:
//
//   FlowWeavingFadeEnvelope fw_fade;          // member of GCode
//
//   // inside _extrude(), before compute_z():
//   const int  fade_n = m_config.flow_weaving_z_fade_layers.getInt();
//   const Point pt    = line.b.to_point();
//   const float fade  = fw_fade.compute(m_layer, pt, fade_n, path_id);
//   double effective_amp = m_config.flow_weaving_z_amplitude.value * fade;
//
// ─────────────────────────────────────────────────────────────────────────────

#ifndef slic3r_FlowWeavingFadeEnvelope_hpp_
#define slic3r_FlowWeavingFadeEnvelope_hpp_

#include "../../Layer.hpp"
#include "../../Point.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

namespace Slic3r {

class FlowWeavingFadeEnvelope
{
public:
    // ── Main entry point ─────────────────────────────────────────────────────
    //
    //  layer    — current Layer (must be non-null when fw_z_active is true)
    //  pt       — scaled Point (same coords as line.b.to_point())
    //  fade_n   — user-configured number of fade layers (0 = no fade → 1.0)
    //  path_id  — unique opaque identifier for the current path
    //             (use e.g. reinterpret_cast<uintptr_t>(&path) or a counter)
    //
    // Returns a value in [0, 1]:
    //   0.0  → at the infill boundary (first/last infill layer for this XY)
    //   1.0  → fully inside the infill volume
    //
    float compute(const Layer* layer, const Point& pt, int fade_n, uintptr_t path_id)
    {
        // Shortcut: fade disabled — full amplitude everywhere
        if (fade_n <= 0)
            return 1.0f;

        // Cache hit: same path, reuse last result
        if (path_id == m_cached_path_id && m_cache_valid)
            return m_cached_fade;

        // ── Count layers above where pt ∈ stInternal ─────────────────────
        int n_above = 0;
        for (const Layer* scan = layer->upper_layer;
             scan != nullptr && n_above < fade_n;
             scan = scan->upper_layer)
        {
            // stInternalSolid is INSIDE the model; only external top/bottom
            // shells are real boundaries — detected by point_in_infill() returning
            // false (the point is no longer in any stInternal polygon).
            if (!point_in_infill(scan, pt))
                break; // outside infill (top shell, bridge floor, void) → stop
            ++n_above;
        }

        // ── Count layers below where pt ∈ stInternal ─────────────────────
        int n_below = 0;
        for (const Layer* scan = layer->lower_layer;
             scan != nullptr && n_below < fade_n;
             scan = scan->lower_layer)
        {
            if (!point_in_infill(scan, pt))
                break; // outside infill (bottom shell, bridge, void) → stop
            ++n_below;
        }

        // ── Smoothstep fade ───────────────────────────────────────────────
        const int   layers_from_edge = std::min(n_above, n_below);
        const float t                = std::min(layers_from_edge, fade_n) / static_cast<float>(fade_n);
        const float fade             = t * t * (3.0f - 2.0f * t); // smoothstep

        // Store in cache
        m_cached_path_id = path_id;
        m_cached_fade    = fade;
        m_cache_valid    = true;

        return fade;
    }

    // Invalidate cache (call at start of each multi-path if needed,
    // though path_id mismatch handles this automatically).
    void invalidate() { m_cache_valid = false; }

private:
    // Returns true if pt lies inside an stInternal fill surface on layer l.
    // Only stInternal (sparse infill) counts — stInternalSolid, stInternalBridge
    // and all shell types are considered "outside" the active infill volume.
    static bool point_in_infill(const Layer* l, const Point& pt)
    {
        for (const LayerRegion* r : l->regions())
            for (const Surface& s : r->fill_surfaces.surfaces)
                if (s.surface_type == stInternal && s.expolygon.contains(pt))
                    return true;
        return false;
    }

    // ── Per-path cache ────────────────────────────────────────────────────────
    uintptr_t m_cached_path_id = 0;
    float     m_cached_fade    = 1.0f;
    bool      m_cache_valid    = false;
};

} // namespace Slic3r

#endif // slic3r_FlowWeavingFadeEnvelope_hpp_
