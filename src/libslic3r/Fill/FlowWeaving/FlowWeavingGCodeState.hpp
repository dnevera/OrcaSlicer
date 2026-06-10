// ── FlowWeavingGCodeState.hpp ────────────────────────────────────────────────
//
// Aggregator header for GCode-side Flow Weaving state.
//
// GCode.hpp includes ONLY this file; all internal FlowWeaving detail headers
// are kept inside Fill/FlowWeaving/ and referenced from here.
// This follows the same isolation pattern used by GCode/AdaptivePAProcessor.hpp
// and keeps GCode.hpp free of Fill-implementation details.
//
// ─────────────────────────────────────────────────────────────────────────────
#ifndef slic3r_FlowWeavingGCodeState_hpp_
#define slic3r_FlowWeavingGCodeState_hpp_

#include "FlowWeavingContext.hpp"
#include "FlowWeavingZModulator.hpp"
#include "FlowWeavingFadeEnvelope.hpp"
#include "FlowWeavingZClamp.hpp"

#endif // slic3r_FlowWeavingGCodeState_hpp_
