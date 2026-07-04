#ifndef VORTEK_LOG_HPP
#define VORTEK_LOG_HPP

#include <boost/log/trivial.hpp>

/**
 * @brief Global compile-time switch for Vortek logging.
 * Set to 1 to enable logging, or 0 to completely compile out all Vortek logs.
 */
#define VORTEK_LOGGING_ENABLED 0

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

#endif // VORTEK_LOG_HPP
