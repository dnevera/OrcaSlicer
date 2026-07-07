#ifndef VORTEK_LOG_HPP
#define VORTEK_LOG_HPP

#include <boost/log/trivial.hpp>

/**
 * @brief Global compile-time switch for Vortek logging.
 * Set to 1 to enable logging, or 0 to completely compile out all Vortek logs.
 */
#define VORTEK_LOGGING_ENABLED 1

#if VORTEK_LOGGING_ENABLED
    /**
     * @brief Core logging macro for the Vortek nozzle changer layer.
     * Prefixes all log outputs with "[Vortek] " and uses boost trivial log backend.
     * 
     * @param level Logging level (trace, debug, info, warning, error, fatal)
     * @param message Log message content stream expression
     */
    #define VORTEK_LOG(level, message) BOOST_LOG_TRIVIAL(warning) << "[Vortek] [" #level "] " << message
#else
    #define VORTEK_LOG(level, message) (void)0
#endif

// ---------------------------------------------------------------------------
// H2C Debug Flags
// Set to true/1 to enable debug overrides for testing the Hybrid slicing
// pipeline without physical HighFlow hardware.
// BOTH sides must be enabled simultaneously:
//   - VortekDeviceHooks: injects HighFlow into extruder_nozzle_stats (CAPACITY)
//   - VortekPrintHooks:  injects nvtHighFlow into filament_volume_map  (ASSIGNMENT)
// Reference to BBS: BambuStudio/src/libslic3r/PresetBundle.cpp – Hybrid nozzle slot assignment
// ---------------------------------------------------------------------------
static constexpr bool VORTEK_DEBUG_HF_NOZZLE_OVERRIDE       = true;  // [DEBUG] Enable for HF pipeline test, disable before release
static constexpr int  VORTEK_DEBUG_HF_NOZZLE_OVERRIDE_COUNT = 2;     // [DEBUG] HF nozzle slots to carve out of Right carousel.
                                                                       //   Total slots N is kept invariant: Std = N-COUNT, HF = COUNT.
                                                                       //   In debug mode: FIRST COUNT Right filaments → nvtHighFlow.
                                                                       //   In production (real HW): LAST COUNT Right filaments → nvtHighFlow.

#endif // VORTEK_LOG_HPP

