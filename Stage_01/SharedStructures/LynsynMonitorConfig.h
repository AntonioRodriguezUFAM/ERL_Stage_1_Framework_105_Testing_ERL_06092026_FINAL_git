// LynsynMonitorConfig.h
// LynsynMonitorConfig.h

// Stage_01/SharedStructures/LynsynMonitorConfig.h
#ifndef LYNSYN_MONITOR_CONFIG_H
#define LYNSYN_MONITOR_CONFIG_H

//#pragma once
#include <string>
#include <cstdint>
#include <fstream>
#include <filesystem>

#include <experimental/filesystem>

namespace fs = std::experimental::filesystem;

/**
 * @struct LynsynMonitorConfig
 * @brief Configuration for Lynsyn power monitoring.
 * @brief Structure holding Lynsyn-related configuration settings,
 *        e.g. which sampling mode, duration, or output file, etc.
 */
struct LynsynMonitorConfig {
     /**
     * An optional output path if you want to do direct CSV logging from this module.
     * If empty, no direct logging is done here.
     */
    std::string outputCSV;    ///< Path to CSV file for logging power samples
      /**
     * If true, we do "period sampling" (time-based).
     * If false, do some other approach or BP-based sampling. 
     */
    bool periodSampling = true; ///< True for time-based sampling, false for breakpoint-based

    bool enabled = true;  ///< Enable or disable Lynsyn monitoring

    /**
     * If set > 0, we do a time-based sampling for `durationSec`.
     * Or if set to 0, it might wait for some breakpoint-based approach, etc.
     */
    double durationSec = 10.0;  ///<   aS,ALSPAs Sampling duration in seconds for period sampling
     /**
     * Which cores to sample for program counters
     * e.g. a bitmask: 0 => no PC sampling
     */
    uint32_t coreMask = 0xF;   ///< Bitmask for cores to monitor (e.g., 0xF for all 4 cores)

    //int coreMask = 15;  ///< Bitmask for cores to monitor (e.g., 0xF for all 4 cores)   
    
    /**
     * "sampleRate"
     * Sets a Default: If sampleRate is not found, it defaults to 1000 milliseconds (1 Hz) and logs a warning.
    */
    int sampleRateMs = 1000;  ///< Sampling rate in milliseconds (default 1000 ms) // <- default 1 Hz
    int period_ms = 1000; // Time period 1000
    /**
     * (Optional) If you want to read from a start/end BP addresses:
     */
    uint64_t startBreakpoint = 0; ///< Start address for breakpoint sampling
    uint64_t endBreakpoint = 0;   ///< End address for breakpoint sampling

    

    // Validation method
    bool validate() const {
        if (!outputCSV.empty()) {
            // Check if parent directory exists
            fs::path p(outputCSV);
            fs::path parent = p.parent_path();
            if (p.filename().empty()) return false;

            if (!parent.empty() && !fs::exists(parent)) {
             try {
                fs::create_directories(parent);
                spdlog::info("Created directory: {}", parent.string());
            } catch (const std::exception& e) {
                spdlog::error("Failed to create directory {}: {}", parent.string(), e.what());
                return false;
            }
        }
    }
    constexpr double MAX_DURATION = 3600.0; // 1 hour max

    if (periodSampling && (durationSec <= 0 || durationSec > MAX_DURATION)) return false;
    if (!periodSampling && (startBreakpoint == 0 || endBreakpoint == 0)) return false;
    if (coreMask == 0 || coreMask > 0xF) return false; // Limit to 4 cores
    if (sampleRateMs < 50) {
            spdlog::warn("sampleRateMs < 50ms may be unsupported by hardware");
        }
    return true;
    }
};

#endif // LYNSYN_MONITOR_CONFIG_H

