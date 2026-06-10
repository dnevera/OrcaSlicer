// ── Flow Weaving G-code Context ───────────────────────────────────────────────
//
// Central state container for the Flow Weaving G-code pass.
//
// FlowWeavingGCodePass updates this struct at every stage of computation,
// giving full diagnostic visibility without relying on OrcaSlicer's internal
// context objects.  All intermediate results (fade, effective amplitude, raw Z,
// clamped Z, boundary distances) are stored here for use by debug comments
// and post-hoc analysis.
//
// Lifecycle:
//   • begin_multipath() — fills cfg + layer state, resets path state
//   • process_line()    — fills dbg_* fields for every extrusion line
//
// ─────────────────────────────────────────────────────────────────────────────

#ifndef slic3r_FlowWeavingContext_hpp_
#define slic3r_FlowWeavingContext_hpp_

#include "../../PrintConfig.hpp"    // InfillPattern
#include "../../Layer.hpp"          // Layer

#include <cstdint>

namespace Slic3r {

// ── Config snapshot (captured once at begin_multipath) ────────────────────────
// Decouples FlowWeavingGCodePass from FullPrintConfig / GCode internals so that
// the entire FlowWeaving subsystem can be unit-tested without a live GCode instance.
struct FlowWeavingGCodeConfig {
    InfillPattern pattern;       // sparse_infill_pattern value
    double  z_amplitude;         // flow_weaving_z_amplitude  (% of layer_height)
    double  period;              // flow_weaving_period       (mm)
    int     fade_n;              // flow_weaving_z_fade_layers
    int     z_tol_layers;        // flow_weaving_z_flow_tolerance (layer count)
    double  first_layer_z;       // initial_layer_print_height (mm)
    double  overlap_degree;      // flow_weaving_overlap_degree [0, 1]
};

// ── Full runtime context ───────────────────────────────────────────────────────
struct FlowWeavingContext {
    // ── Config snapshot ─────────────────────────────────────────────────────
    FlowWeavingGCodeConfig cfg {};

    // ── Layer state (set at begin_multipath) ─────────────────────────────────
    const Layer* layer       = nullptr; // current Layer pointer (non-owning)
    double  nominal_z        = 0.;     // m_nominal_z from GCode
    double  layer_height     = 0.2;    // m_layer->height (single layer, not combined)
    int     phase_idx        = 0;      // sine wave phase alternation index
    int     combine_step     = 1;      // infill combination ratio (1 = no combination)

    // ── Multi-path state (reset at begin_multipath) ──────────────────────────
    double  path_offset      = 0.;     // accumulated distance across sub-paths (mm)
    double  path_total_len   = 0.;     // total multipath length used for wall-taper (mm)

    // ── Per-line diagnostic state (updated by process_line for every line) ───
    //
    // These fields represent the intermediate calculation results for the LAST
    // processed extrusion line.  Read them after process_line() returns to
    // inspect how the final Z was derived.
    uintptr_t dbg_path_id    = 0;      // opaque path ID fed to FadeEnvelope cache
    float   dbg_fade         = 1.f;   // fade factor [0, 1] — from FadeEnvelope
    double  dbg_eff_amp      = 0.;    // effective amplitude in mm (cfg.z_amplitude × fade)
    double  dbg_z_raw        = 0.;    // Z from ZModulator BEFORE boundary clamping
    double  dbg_z_final      = 0.;    // Z AFTER boundary clamping (value sent to printer)
    double  dbg_dist_up      = -1.;   // distance to upper boundary (mm), -1 = no boundary
    double  dbg_dist_dn      = -1.;   // distance to lower boundary (mm), -1 = no boundary
    bool    dbg_clamped_up   = false; // true if z was clipped to upper boundary
    bool    dbg_clamped_dn   = false; // true if z was clipped to lower boundary

    // ── Previous-layer interlock diagnostics ─────────────────────────────────
    //
    // These fields characterise the gap (or overlap) with the previous layer.
    // Computed mathematically from the phase-alternated wave without storing
    // the actual path — the previous layer's Z at this position is the exact
    // mirror image of the current layer (opposite phase).
    //
    // prev_layer_z:  nozzle Z of the previous FW layer at the same path distance
    //   = (nominal_z - layer_height) - amp × layer_height × sin(dist, phase)
    //   which equals  nominal_z - layer_height - (z_final - nominal_z)
    //   (the previous layer had the opposite phase, so its sine = -t)
    //
    // gap_to_prev:   vertical gap between current nozzle Z and previous bead top
    //   gap = z_final - dbg_prev_layer_z
    //   Positive → nozzle is above previous bead (normal).
    //   Near-zero or negative → mechanical interlock zone (overlap_degree active).
    double  dbg_prev_layer_z = 0.;    // computed Z of prev FW layer at same distance
    double  dbg_gap_to_prev  = 0.;    // gap to prev layer bead top (mm)

    // ── Convenience accessors ────────────────────────────────────────────────
    int layer_id() const {
        return layer ? static_cast<int>(layer->id()) : -1;
    }
    double deflection() const { return dbg_z_final - nominal_z; }

    // Fill diagnostic previous-layer fields after z_final is known.
    // Call this once per line after dbg_z_final, nominal_z and layer_height are set.
    void update_prev_layer_diag() {
        // The previous FW layer printed with opposite phase, so its sine was -t.
        // current_t = (dbg_z_final - nominal_z) / (amp_eff × layer_height)
        // prev nozzle Z = (nominal_z - layer_height) + amp × layer_height × (-t)
        //               = (nominal_z - layer_height) - (dbg_z_final - nominal_z)
        const double deflect = dbg_z_final - nominal_z;
        dbg_prev_layer_z = (nominal_z - layer_height) - deflect;
        // The bead top of the previous layer ≈ prev nozzle Z (nozzle tip = bead top in FDM)
        dbg_gap_to_prev = dbg_z_final - dbg_prev_layer_z;
    }
};

} // namespace Slic3r

#endif // slic3r_FlowWeavingContext_hpp_
