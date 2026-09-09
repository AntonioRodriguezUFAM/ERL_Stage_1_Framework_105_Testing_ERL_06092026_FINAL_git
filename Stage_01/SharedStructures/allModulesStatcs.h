//allModulesStatcs.h
#pragma once

#include <chrono>
#include <vector>
#include <cstdint>
#include <string>

#include <numeric>   // std::accumulate
#include <algorithm> // std::min, std::any_of

#include "../Includes/lynsyn.h" // Lynsyn library for power measurements
#include "../nlohmann/json.hpp" // JSON serialization
#include <spdlog/spdlog.h> // For logging
#include "../Others/utils.h"


using json = nlohmann::json;

// SAMPLING RATE = The frequency corresponding to a time period of \(500\text{\ milliseconds}\) is \(2\text{\ Hz}\).

// Custom definitions for LynsynSample flags (pending confirmation from usbprotocol.h)
#define SAMPLE_FLAG_VOLTAGE_VALID 0x0004 // Bit 2
#define SAMPLE_FLAG_CURRENT_VALID 0x0008 // Bit 3

// // // New structs for metrics (CameraStats, AlgorithmStats, DisplayStats, JetsonNanoInfo) would be defined here or in separate headers as needed.
// // New structs for metrics (CameraStats, AlgorithmStats, DisplayStats, JetsonNanoInfo) would be defined here or in separate headers as needed.
// //==================================================================================
// /**
//  * @struct PowerStats
//  * @brief Backward-compatible + always accurate power calculations.
//  *        Keeps all original members/setters for framework compatibility.
//  */
// struct PowerStats {
//     PowerStats() = default;
//     explicit PowerStats(std::chrono::system_clock::time_point ts) : timestamp(ts) {}

//     std::chrono::system_clock::time_point timestamp;

//     std::vector<double> voltages{4, 0.0};
//     std::vector<double> currents{4, 0.0};
//     std::vector<double> power{4, 0.0};

//     // Kept for full backward compatibility
//     double totalPower_ = 0.0;
//     double averagePower_ = 0.0;

//     double sensorPower(size_t i) const { 
//         return (i < power.size()) ? power[i] : 0.0; 
//     }

//     std::vector<double> powerPerSensor() const { return power; }

//     double totalPower() const { 
//         return std::accumulate(power.begin(), power.end(), 0.0); 
//     }

//     double averagePower() const { 
//         size_t n = sensorCount();
//         return n > 0 ? totalPower() / n : 0.0; 
//     }

//     size_t sensorCount() const { 
//         return std::min({voltages.size(), currents.size(), power.size()}); 
//     }

//     void setSensorData(size_t i, double voltage, double current) {
//         if (i >= voltages.size()) return;
//         voltages[i] = voltage;
//         currents[i] = current;
//         power[i]    = (voltage > 0.0 && current > 0.0) ? voltage * current : 0.0;
//         updateDerivedMetrics();   // Keep original behavior
//     }

//     void setTotalPower(double value) { totalPower_ = value; }
//     void setAveragePower(double value) { averagePower_ = value; }

//     void updateDerivedMetrics() {
//         totalPower_ = totalPower();           // Use dynamic version
//         averagePower_ = averagePower();       // Use dynamic version
//     }

//     bool isValid() const {
//         size_t count = sensorCount();
//         return timestamp != std::chrono::system_clock::time_point{} &&
//                count > 0 &&
//                std::any_of(power.begin(), power.begin() + count,
//                            [](double p){ return p > 0.01; });
//     }

//     json toJson() const {
//         json j;
//         j["timestamp"] = utils::formatTimestamp(timestamp);
//         j["total_w"] = totalPower();
//         j["average_w"] = averagePower();
//         for (size_t i = 0; i < sensorCount(); ++i) {
//             j["sensor" + std::to_string(i)] = {
//                 {"power_w", sensorPower(i)},
//                 {"voltage_v", voltages[i]},
//                 {"current_a", currents[i]}
//             };
//         }
//         return j;
//     }
// };


//==============================================================================
/**
 * @struct PowerStats
 * @brief Real Lynsyn power snapshot. Source of truth is power[i] = V[i] * I[i].
 */
struct PowerStats {
    PowerStats() = default;

    explicit PowerStats(std::chrono::system_clock::time_point ts)
        : timestamp(ts) {}

    std::chrono::system_clock::time_point timestamp{};

    // std::vector<double> voltages{4, 0.0};
    // std::vector<double> currents{4, 0.0};
    // std::vector<double> power{4, 0.0};

    // [MOD POWER_4W_FIX] Use size/value construction, not initializer_list.
    // {4, 0.0} creates values [4.0, 0.0], which made totalPower() = 4.0 W.
    std::vector<double> voltages = std::vector<double>(4, 0.0);
    std::vector<double> currents = std::vector<double>(4, 0.0);
    std::vector<double> power    = std::vector<double>(4, 0.0);

    // Kept for compatibility with existing code.
    // Backward compatibility members
    double totalPower_ = 0.0;
    double averagePower_ = 0.0;

    size_t sensorCount() const {
        return std::min(std::min(voltages.size(), currents.size()), power.size());
    }

    double sensorPower(size_t i) const {
        return (i < power.size()) ? power[i] : 0.0;
    }

    std::vector<double> powerPerSensor() const {
        return power;
    }

    double totalPower() const {
        return std::accumulate(power.begin(), power.end(), 0.0);
    }

    double averagePower() const {
        const size_t count = sensorCount();
        return count > 0 ? totalPower() / static_cast<double>(count) : 0.0;
    }

    void setSensorData(size_t i, double voltage, double current) {
        if (i >= sensorCount()) {
            return;
        }

        voltages[i] = voltage;
        currents[i] = current;
        //power[i] = voltage * current;  // ? No noise guard: 0.0 × 0.0 = 0.0 is valid Lynsyn output when sensors are disconnected or powered off.
        power[i] = (voltage > 0.001 && current > 0.001) ? voltage * current : 0.0;

        updateDerivedMetrics();
    }

    // Compatibility only. Real Lynsyn power must use setSensorData().
    void setTotalPower(double value) {
        totalPower_ = value;
    }

    void setAveragePower(double value) {
        averagePower_ = value;
    }

    void updateDerivedMetrics() {
        totalPower_ = totalPower();
        averagePower_ = averagePower();
    }

    bool isValid() const {
        if (timestamp == std::chrono::system_clock::time_point{} ||
            sensorCount() == 0) {
            return false;
        }

        for (size_t i = 0; i < sensorCount(); ++i) {
            if (voltages[i] < 0.0 || currents[i] < 0.0 || power[i] < 0.0) {
                return false;
            }
        }

        // Reject empty/default power samples.
        return totalPower() > 0.01;
    }

    json toJson() const {
        json j;
        j["timestamp"] = utils::formatTimestamp(timestamp);
        j["total_w"] = totalPower();
        j["average_w"] = averagePower();

        for (size_t i = 0; i < sensorCount(); ++i) {
            j["sensor" + std::to_string(i)] = {
                {"power_w", sensorPower(i)},
                {"voltage_v", voltages[i]},
                {"current_a", currents[i]}
            };
        }

        return j;
    }
};

// //================================================================================
// // Old version before fallback logic was added (keep for reference or remove if unused):
// //===================================================================================
// /**
//  * @struct PowerStats
//  * @brief Represents power measurements across multiple sensors at a single point in time.
//  */
// struct PowerStats {
//     PowerStats() = default;
//     explicit PowerStats(std::chrono::system_clock::time_point ts) : timestamp(ts) {}
//     std::chrono::system_clock::time_point timestamp;
//     std::vector<double> voltages{4, 0.0};
//     std::vector<double> currents{4, 0.0};
//     std::vector<double> power{4, 0.0};

//     double totalPower_;  // Added as a member
//     double averagePower_; // Added as a member

//     double sensorPower(size_t i) const { return i < power.size() ? power[i] : 0.0; }
    
//     std::vector<double> powerPerSensor() const { return power; }
//     double totalPower() const { return std::accumulate(power.begin(), power.end(), 0.0); }
//     double averagePower() const { return sensorCount() > 0 ? totalPower() / sensorCount() : 0.0; }
//     size_t sensorCount() const { return std::min(voltages.size(), currents.size()); }
//     void setSensorData(size_t i, double voltage, double current) {
//         if (i < voltages.size()) {
//             voltages[i] = voltage;
//             currents[i] = current;
//             power[i] = voltage * current;
//             updateDerivedMetrics(); // Update total and average
//         }
//     }
//     void setTotalPower(double value) { totalPower_ = value; }
//     void setAveragePower(double value) { averagePower_ = value; }
    
//     void updateDerivedMetrics() {
//         totalPower_ = std::accumulate(power.begin(), power.end(), 0.0);
//         averagePower_ = sensorCount() > 0 ? totalPower_ / sensorCount() : 0.0;
//     }

//     /**
//      * @brief Checks if the PowerStats object is valid.
//      * @return True if valid, false otherwise.
//      */

//     bool isValid() const {
//             size_t count = std::min(voltages.size(), currents.size());
//             return timestamp != std::chrono::system_clock::time_point{} && count > 0 &&
//                 std::all_of(voltages.begin(), voltages.begin() + count, [](double v) { return v >= 0.0; }) &&
//                 std::all_of(currents.begin(), currents.begin() + count, [](double c) { return c >= 0.0; });
//         }
//         // ... (other methods unchanged)

//     /**
//      * @brief Converts to JSON for serialization.
//      * @return JSON object.
//      */
    
//     json toJson() const {
//         json j;
//         j["timestamp"] = utils::formatTimestamp(timestamp);
//         j["total_w"] = totalPower();
//         j["average_w"] = averagePower();
//         for (size_t i = 0; i < sensorCount(); ++i) {
//             j["sensor" + std::to_string(i)] = {
//                 {"power_w", sensorPower(i)},
//                 {"voltage_v", voltages[i]},
//                 {"current_a", currents[i]}
//             };
//         }
//         return j;
//     }
// };




/**
 * @brief Convert raw LynsynSample to PowerStats structure.
 * @param s LynsynSample containing voltage, current, and time data.
 * @return PowerStats with populated fields.
 */

inline PowerStats convertToPowerStats(const LynsynSample& s) {
    PowerStats stats;
    double seconds = lynsyn_cyclesToSeconds(s.time);
    auto duration = std::chrono::duration<double>(seconds);
    stats.timestamp = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(duration));

    constexpr size_t max_sensors = std::min(static_cast<size_t>(LYNSYN_MAX_SENSORS), static_cast<size_t>(4));
    stats.voltages.resize(max_sensors, 0.0); // Pre-size with default
    stats.currents.resize(max_sensors, 0.0);

    for (size_t i = 0; i < max_sensors; ++i) {
        if (s.flags & SAMPLE_FLAG_VOLTAGE_VALID) {
            stats.voltages[i] = s.voltage[i];
        }
        if (s.flags & SAMPLE_FLAG_CURRENT_VALID) {
            stats.currents[i] = s.current[i];
        }
    }

    spdlog::debug("[convertToPowerStats] Populated {} voltages, {} currents (flags: {:04x})", 
                  stats.voltages.size(), stats.currents.size(), s.flags);
    return stats;
}

/**
 * @struct CameraStats
 * @brief Metrics for camera performance.
 */
struct CameraStats {
    std::chrono::system_clock::time_point timestamp;
    uint64_t frameNumber = 0;   // Frame sequence number
    double fps = 0.0;           // Measured capture FPS

    // Downstream/output frame delivered to AlgorithmConcrete/SdlDisplayConcrete.
    uint32_t frameWidth = 0;
    uint32_t frameHeight = 0;
    uint64_t frameSize = 0;

    // Raw V4L2 capture metadata, critical for CSI IMX219 validation.
    uint32_t rawCaptureWidth = 0;
    uint32_t rawCaptureHeight = 0;
    uint64_t rawBytesUsed = 0;
    uint32_t rawBytesPerLine = 0;
    uint32_t rawSizeImage = 0;
    std::string rawPixelFormat = "";
    std::string backend = "";

    // RG10 conversion/luma evidence. Helps prove that black screens are not being caused by empty frames.
    std::string rawBitAlignment = "";
    uint32_t rawSampleMin10 = 0;
    uint32_t rawSampleMax10 = 0;
    double rawSampleMean10 = 0.0;
    uint32_t lumaMin8 = 0;
    uint32_t lumaMax8 = 0;
    double lumaMean8 = 0.0;

    // Jetson CSI controls/mode evidence.
    int sensorMode = -1;
    int bypassMode = -1;
    bool csiRg10Mode = false;
    bool convertedToYuyvLike = false;

    // Capture health counters.
    uint64_t framesDequeued = 0;
    uint64_t framesQueued = 0;
    uint64_t droppedFrames = 0;

    CameraStats() = default;

    CameraStats(std::chrono::system_clock::time_point ts)
        : timestamp(ts), frameNumber(0), fps(0.0), frameWidth(0), frameHeight(0), frameSize(0) {}

    CameraStats(std::chrono::system_clock::time_point ts,
                uint64_t fn,
                double f,
                uint32_t w,
                uint32_t h,
                uint64_t s)
        : timestamp(ts), frameNumber(fn), fps(f), frameWidth(w), frameHeight(h), frameSize(s) {}

    bool isValid() const {
        return timestamp != std::chrono::system_clock::time_point{} &&
               frameWidth > 0 && frameHeight > 0 && frameSize > 0;
    }

    json toJson() const {
        return {
            {"timestamp", utils::formatTimestamp(timestamp)},
            {"frame_number", frameNumber},
            {"fps", fps},

            {"width", frameWidth},
            {"height", frameHeight},
            {"frame_size", frameSize},

            {"raw_capture_width", rawCaptureWidth},
            {"raw_capture_height", rawCaptureHeight},
            {"raw_pixel_format", rawPixelFormat},
            {"raw_bytes_used", rawBytesUsed},
            {"raw_bytes_per_line", rawBytesPerLine},
            {"raw_size_image", rawSizeImage},
            {"backend", backend},

            {"raw_bit_alignment", rawBitAlignment},
            {"raw_sample_min10", rawSampleMin10},
            {"raw_sample_max10", rawSampleMax10},
            {"raw_sample_mean10", rawSampleMean10},
            {"luma_min8", lumaMin8},
            {"luma_max8", lumaMax8},
            {"luma_mean8", lumaMean8},

            {"sensor_mode", sensorMode},
            {"bypass_mode", bypassMode},
            {"csi_rg10_mode", csiRg10Mode},
            {"converted_to_yuyv_like", convertedToYuyvLike},

            {"frames_dequeued", framesDequeued},
            {"frames_queued", framesQueued},
            {"dropped_frames", droppedFrames}
        };
    }
};

/**
 * @struct AlgorithmStats
 * @brief Metrics for algorithm processing (e.g., inference).
 */
struct AlgorithmStats {

    std::chrono::system_clock::time_point timestamp; // This will represent the END time
    std::chrono::system_clock::time_point startTime; // ADD THIS FIELD
    uint64_t frameId = 0;             // Frame number for traceability
    double inferenceTimeMs = 0.0;     // Inference time per frame (ms)
    double confidenceScore = 0.0;     // Algorithm confidence (0-1)
    double fps = 0.0;                 // Algorithm processing FPS
    double avgProcTimeMs = 0.0;       // Average processing time per frame (ms)
    double totalProcTimeMs = 0.0;     // Total processing time (ms)
    uint64_t framesCount = 0;         // Number of processed frames
    uint64_t gpuFreeMemory = 0;       // GPU free memory (bytes)
    uint64_t gpuTotalMemory = 0;      // GPU total memory (bytes)
    double cudaKernelTimeMs = 0.0;    // CUDA kernel execution time (ms)
    uint32_t droppedFrames = 0;       // Number of dropped frames

    AlgorithmStats() = default;
    AlgorithmStats(std::chrono::system_clock::time_point ts)
        : timestamp(ts), inferenceTimeMs(0.0), confidenceScore(0.0), fps(0.0), avgProcTimeMs(0.0),
          totalProcTimeMs(0.0), framesCount(0), gpuFreeMemory(0), gpuTotalMemory(0), cudaKernelTimeMs(0.0) {}
    AlgorithmStats(std::chrono::system_clock::time_point ts, double inf, double conf, double f, double avg, double total, uint64_t fc)
        : timestamp(ts), inferenceTimeMs(inf), confidenceScore(conf), fps(f), avgProcTimeMs(avg), totalProcTimeMs(total), framesCount(fc),
          gpuFreeMemory(0), gpuTotalMemory(0), cudaKernelTimeMs(0.0) {}

    // // OLD CONSTRUCTOR (keep for compatibility or remove if unused)
    // explicit AlgorithmStats(std::chrono::system_clock::time_point ts)
    //     : timestamp(ts), startTime(ts) {} // Default startTime to endTime

    // --- ADD THIS NEW CONSTRUCTOR ---
    /**
     * @brief Construct AlgorithmStats with a defined start and end time.
     */
    AlgorithmStats(std::chrono::system_clock::time_point ts_start, 
                   std::chrono::system_clock::time_point ts_end)
        : timestamp(ts_end), startTime(ts_start) {}


    
    // If you want to allow first frame (framesCount = 0):
    bool isValid() const {
    return timestamp != std::chrono::system_clock::time_point{} &&
           inferenceTimeMs >= 0.0 &&
           fps >= 0.01 && fps < 1000.0;
        }


    // ... (other methods unchanged)

    /**
     * @brief Converts to JSON for serialization.
     * @return JSON object.
     */
    json toJson() const {
        return {
            {"timestamp", utils::formatTimestamp(timestamp)},
            {"inference_time_ms", inferenceTimeMs},
            {"confidence", confidenceScore},
            {"fps", fps},
            {"avg_proc_time_ms", avgProcTimeMs},
            {"total_proc_time_ms", totalProcTimeMs},
            {"frames_count", framesCount},
            {"gpu_free_memory_mb", gpuFreeMemory / (1024.0 * 1024.0)},
            {"gpu_total_memory_mb", gpuTotalMemory / (1024.0 * 1024.0)},
            {"cuda_kernel_time_ms", cudaKernelTimeMs}
        };
    }
};

/**
 * @struct DisplayStats
 * @brief Metrics for display rendering.
 */
struct DisplayStats {
    //std::chrono::system_clock::time_point timestamp;
    //std::chrono::steady_clock::time_point timestamp;  // ? CHANGE to steady_clock (monotonic, precise)
    std::chrono::steady_clock::time_point timestamp;  // ? CHANGED to steady_clock for precise timing (matches 'now')
    uint64_t frameId = 0;         // Frame number for traceability
    double latencyMs = 0.0;      // Display latency (ms)
    uint32_t droppedFrames = 0;  // Number of dropped frames
    double renderTimeMs = 0.0;   // Rendering time per frame (ms)
    double fps = 0.0;              // ? NEW: FPS from display loop (critical for validation)

     DisplayStats() = default;
    // DisplayStats(std::chrono::system_clock::time_point ts)
    //     : timestamp(ts), latencyMs(0.0), droppedFrames(0), renderTimeMs(0.0) {}
    // DisplayStats(std::chrono::system_clock::time_point ts, double lat, uint32_t df, double rt)
    //     : timestamp(ts), latencyMs(lat), droppedFrames(df), renderTimeMs(rt) {}
    
    explicit DisplayStats(std::chrono::steady_clock::time_point ts)
        : timestamp(ts), latencyMs(0.0), droppedFrames(0), renderTimeMs(0.0), fps(0.0) {}

    // NEW: Constructor for system_clock (approximate conversion for compatibility)
    explicit DisplayStats(std::chrono::system_clock::time_point sysTs)
        : timestamp(std::chrono::steady_clock::now() + (sysTs - std::chrono::system_clock::now())),  // Approximate cast
          latencyMs(0.0), droppedFrames(0), renderTimeMs(0.0), fps(0.0) {}

    DisplayStats(std::chrono::steady_clock::time_point ts, double lat, uint32_t df, double rt, double f)
        : timestamp(ts), latencyMs(lat), droppedFrames(df), renderTimeMs(rt), fps(f) {}
  

    /**
     * @brief Validates the display stats.
     * @return True if valid, false otherwise.
     */
    /**
     * @brief Validates the display stats.
     * @return True if we have a valid timestamp (relaxed for startup/fallback).
     *         We no longer require renderTimeMs > 0 because fallback pushes 0.
     */
    bool isValid() const {
        return timestamp != std::chrono::steady_clock::time_point{} &&
               (fps > 0.0 || renderTimeMs > 0.0 || latencyMs > 0.0 || frameId > 0);
    }

    /**
     * @brief Checks if the snapshot contains any meaningful data.
     */
    bool hasData() const {
        return fps > 0.0 || renderTimeMs > 0.0 || latencyMs > 0.0 || droppedFrames > 0;
    }

    /**
     * @brief Converts to JSON for serialization.
     * @return JSON object.
     */
    json toJson() const {
        // Convert steady to system for JSON (approximate, but fine for logs)
        auto sysTime = std::chrono::system_clock::now() + 
                       (timestamp - std::chrono::steady_clock::now());  // ? ADD: Approximate conversion
        return {
            {"timestamp", utils::formatTimestamp(sysTime)}, // Use converted system time for JSON
            {"frame_id", frameId},
            {"latency_ms", latencyMs},
            {"dropped_frames", droppedFrames},
            {"render_time_ms", renderTimeMs},
            {"fps", fps}                     // ? NEW
        };
    }
};

/**
 * @struct JetsonNanoInfo
 * @brief System-on-Chip metrics for Jetson Nano.
 */
struct JetsonNanoInfo {
    std::chrono::system_clock::time_point timestamp;
    // Memory metrics (MB)
    double RAM_In_Use_MB = 0.0;      // RAM currently in use
    double Total_RAM_MB = 0.0;       // Total available RAM
    double LFB_Size_MB = 0.0;        // Largest free block size
    double Block_Max_MB = 0.0;       // Maximum block size
    double SWAP_In_Use_MB = 0.0;     // SWAP currently in use
    double Total_SWAP_MB = 0.0;      // Total available SWAP
    double Cached_MB = 0.0;          // Cached memory
    // IRAM metrics (kB)
    double used_IRAM_kB = 0.0;       // Internal RAM used
    double total_IRAM_kB = 0.0;      // Total internal RAM
    double lfb_kB = 0.0;             // Largest free block
    // CPU metrics (4 cores)
    double CPU1_Utilization_Percent = 0.0;
    double CPU1_Frequency_MHz = 0.0;
    double CPU2_Utilization_Percent = 0.0;
    double CPU2_Frequency_MHz = 0.0;
    double CPU3_Utilization_Percent = 0.0;
    double CPU3_Frequency_MHz = 0.0;
    double CPU4_Utilization_Percent = 0.0;
    double CPU4_Frequency_MHz = 0.0;
    // Memory and GPU frequency
    double EMC_Frequency_Percent = 0.0; // External Memory Controller
    double GR3D_Frequency_Percent = 0.0; // GPU frequency
    // Temperature sensors (°C)
    double PLL_Temperature_C = 0.0;
    double CPU_Temperature_C = 0.0;
    double PMIC_Temperature_C = 0.0;
    double GPU_Temperature_C = 0.0;
    double AO_Temperature_C = 0.0; // Always-On sensor
    double Thermal_Temperature_C = 0.0;

    JetsonNanoInfo() = default;
    JetsonNanoInfo(std::chrono::system_clock::time_point ts)
        : timestamp(ts),
          RAM_In_Use_MB(0.0), Total_RAM_MB(0.0), LFB_Size_MB(0.0), Block_Max_MB(0.0),
          SWAP_In_Use_MB(0.0), Total_SWAP_MB(0.0), Cached_MB(0.0),
          used_IRAM_kB(0.0), total_IRAM_kB(0.0), lfb_kB(0.0),
          CPU1_Utilization_Percent(0.0), CPU1_Frequency_MHz(0.0),
          CPU2_Utilization_Percent(0.0), CPU2_Frequency_MHz(0.0),
          CPU3_Utilization_Percent(0.0), CPU3_Frequency_MHz(0.0),
          CPU4_Utilization_Percent(0.0), CPU4_Frequency_MHz(0.0),
          EMC_Frequency_Percent(0.0), GR3D_Frequency_Percent(0.0),
          PLL_Temperature_C(0.0), CPU_Temperature_C(0.0), PMIC_Temperature_C(0.0),
          GPU_Temperature_C(0.0), AO_Temperature_C(0.0), Thermal_Temperature_C(0.0) {}

    /**
     * @brief Validates the SoC stats.
     * @return True if valid, false otherwise.
     */
    bool isValid() const {
        return timestamp != std::chrono::system_clock::time_point{} &&
               //RAM_In_Use_MB >= 0.0 &&
               Total_RAM_MB > 0.0 ;//&&
               //CPU1_Utilization_Percent >= 0.0 && 
               //CPU1_Frequency_MHz >= 0.0;
    }

    /**
     * @brief Converts to JSON for serialization.
     * @return JSON object.
     */
    json toJson() const {
        return {
            {"timestamp", utils::formatTimestamp(timestamp)},
            {"ram_in_use_mb", RAM_In_Use_MB},
            {"total_ram_mb", Total_RAM_MB},
            {"lfb_size_mb", LFB_Size_MB},
            {"block_max_mb", Block_Max_MB},
            {"swap_in_use_mb", SWAP_In_Use_MB},
            {"total_swap_mb", Total_SWAP_MB},
            {"cached_mb", Cached_MB},
            {"used_iram_kb", used_IRAM_kB},
            {"total_iram_kb", total_IRAM_kB},
            {"lfb_kb", lfb_kB},
            {"cpu1_utilization_percent", CPU1_Utilization_Percent},
            {"cpu1_frequency_mhz", CPU1_Frequency_MHz},
            {"cpu2_utilization_percent", CPU2_Utilization_Percent},
            {"cpu2_frequency_mhz", CPU2_Frequency_MHz},
            {"cpu3_utilization_percent", CPU3_Utilization_Percent},
            {"cpu3_frequency_mhz", CPU3_Frequency_MHz},
            {"cpu4_utilization_percent", CPU4_Utilization_Percent},
            {"cpu4_frequency_mhz", CPU4_Frequency_MHz},
            {"emc_frequency_percent", EMC_Frequency_Percent},
            {"gr3d_frequency_percent", GR3D_Frequency_Percent},
            {"pll_temperature_c", PLL_Temperature_C},
            {"cpu_temperature_c", CPU_Temperature_C},
            {"pmic_temperature_c", PMIC_Temperature_C},
            {"gpu_temperature_c", GPU_Temperature_C},
            {"ao_temperature_c", AO_Temperature_C},
            {"thermal_temperature_c", Thermal_Temperature_C}
        };
    }
};



// /**
//  * @struct SystemMetricsSnapshot
//  * @brief Aggregated snapshot of all system metrics for a single frame at a point in time.
//  */
// struct SystemMetricsSnapshot {
//     std::chrono::system_clock::time_point timestamp;
//     uint64_t frameId = 0;                    // Frame number for traceability
//     CameraStats cameraStats;
//     AlgorithmStats algorithmStats;
//     DisplayStats displayStats;
//     JetsonNanoInfo socInfo;
//     PowerStats powerStats;
//     double joulesPerFrame = 0.0;             // Energy per frame (joules)
//     double endToEndLatencyMs = 0.0;          // End-to-end latency (ms)
//     double processingLatencyMs = 0.0;        // Algorithm processing latency (ms)
//     double displayLatencyMs = 0.0;           // Display rendering latency (ms)

//     SystemMetricsSnapshot() = default;
//     explicit SystemMetricsSnapshot(std::chrono::system_clock::time_point ts)
//         : timestamp(ts), frameId(0), cameraStats(ts), algorithmStats(ts), displayStats(ts), socInfo(ts), powerStats(ts) {}

//     /**
//      * @brief Validates the snapshot for completeness.
//      * @return True if all required fields are populated, false otherwise.
//      */
//     // ... (fields unchanged)
//     bool isValid() const {
//         return timestamp != std::chrono::system_clock::time_point{} &&
//                (cameraStats.isValid() || algorithmStats.isValid() ||
//                 displayStats.isValid() || socInfo.isValid() || powerStats.isValid());
//     }
//     // ... (other methods unchanged)
//     // In the header file where SystemMetricsSnapshot is defined
// // (likely Stage_01/Concretes/SystemMetricsAggregatorConcrete_v3_2.h),
// // add this method to the struct/class SystemMetricsSnapshot:

//     bool hasData() const {
//         // Replace 'metrics' with the actual member name that holds the data.
//         // For example, if it's a std::map<std::string, double> metrics;
//         // or std::vector<Metric> metrics;
//         // Adjust the check accordingly.
//         return !metrics.empty();  // Or check if key fields are set, e.g., (power > 0.0 || cpuUtil > 0.0)
//     }

//     /**
//      * @brief Converts the snapshot to JSON for serialization.
//      * @return JSON object representing the snapshot.
//      */
//     json toJson() const {
//         json j;
//         j["timestamp"] = utils::formatTimestamp(timestamp);
//         j["frame_id"] = frameId;
//         j["camera"] = cameraStats.toJson();
//         j["algorithm"] = algorithmStats.toJson();
//         j["display"] = displayStats.toJson();
//         j["soc"] = socInfo.toJson();
//         j["power"] = powerStats.toJson();
//         j["derived"] = {
//             {"end_to_end_latency_ms", endToEndLatencyMs},
//             {"processing_latency_ms", processingLatencyMs},
//             {"display_latency_ms", displayLatencyMs},
//             {"joules_per_frame", joulesPerFrame}
//         };
//         return j;
//     }
// };


// /**
//  * @struct SystemMetricsSnapshot
//  * @brief Aggregated snapshot of all system metrics for a single frame at a point in time.
//  */
// struct SystemMetricsSnapshot {
//     std::chrono::system_clock::time_point timestamp;
//     //std::chrono::steady_clock::time_point timestamp;  // ? CHANGED to steady_clock for consistency with FPS/latency
//     uint64_t frameId = 0; // Frame number for traceability
//     CameraStats cameraStats;
//     AlgorithmStats algorithmStats;
//     DisplayStats displayStats;
//     JetsonNanoInfo socInfo;
//     PowerStats powerStats;

//     double joulesPerFrame = 0.0; // Energy per frame (joules)
//     double endToEndLatencyMs = 0.0; // End-to-end latency (ms)
//     double processingLatencyMs = 0.0; // Algorithm processing latency (ms)
//     double displayLatencyMs = 0.0; // Display rendering latency (ms)

//     bool valid = false;  // ? ADD THIS: Public member for quick checks (set in constructors/methods)
//     SystemMetricsSnapshot() = default;
//     explicit SystemMetricsSnapshot(std::chrono::system_clock::time_point ts)
//         : timestamp(ts), frameId(0), cameraStats(ts), algorithmStats(ts), displayStats(ts), socInfo(ts), powerStats(ts) {}


//       /**
//      * @brief Validates the snapshot for completeness.
//      * @return True if all required fields are populated, false otherwise.
//      */
//     // ... (fields unchanged)
//     bool isValid() const {
//         return timestamp != std::chrono::system_clock::time_point{} &&
//                (cameraStats.isValid() || algorithmStats.isValid() ||
//                 displayStats.isValid() || socInfo.isValid() || powerStats.isValid());
//     }
//     // ... (other methods unchanged)
//     /**
//      * @brief Checks if the snapshot contains any meaningful data.
//      * @return True if at least one sub-stat has data or derived metrics are non-zero, false otherwise.
//      * @note This can be used in conjunction with isValid() for stricter checks.
//      */
//     bool hasData() const {
//         // Check sub-stats for data (assuming their isValid() implies data presence)
//         // and derived fields for non-default values.
//         // Adjust as needed if sub-stats have a separate hasData() or specific fields.
//         return cameraStats.isValid() ||
//                algorithmStats.isValid() ||
//                displayStats.isValid() ||
//                socInfo.isValid() ||
//                powerStats.isValid() ||
//                (joulesPerFrame != 0.0) ||
//                (endToEndLatencyMs != 0.0) ||
//                (processingLatencyMs != 0.0) ||
//                (displayLatencyMs != 0.0);
//     }
//     /**
//      * @brief Converts the snapshot to JSON for serialization.
//      * @return JSON object representing the snapshot.
//      */
//     json toJson() const {
//         json j;
//         j["timestamp"] = utils::formatTimestamp(timestamp);
//         j["frame_id"] = frameId;
//         j["camera"] = cameraStats.toJson();
//         j["algorithm"] = algorithmStats.toJson();
//         j["display"] = displayStats.toJson();
//         j["soc"] = socInfo.toJson();
//         j["power"] = powerStats.toJson();
//         j["derived"] = {
//             {"end_to_end_latency_ms", endToEndLatencyMs},
//             {"processing_latency_ms", processingLatencyMs},
//             {"display_latency_ms", displayLatencyMs},
//             {"joules_per_frame", joulesPerFrame}
//         };
//         // Optional: Add validity flags for debugging/serialization
//         j["valid"] = isValid();
//         j["has_data"] = hasData();
//         return j;
//     }
// };



//==============================================================================================================================================
/**
 * @struct SystemMetricsSnapshot
 * @brief Aggregated snapshot of all system metrics for a single frame at a point in time.
 * SystemMetricsSnapshot is the single source of truth passed from 
 * SystemMetricsAggregatorConcrete_v3 --> EvolutionarySelector --> GeneticAlgorithm. 
 * Adding the field here keeps the architecture clean, zero-copy friendly, 
 * and consistent with the other derived fields (fps, avg_power_w_alg, cpu_util_avg, etc.) you already added.
 */
struct SystemMetricsSnapshot {
    std::chrono::system_clock::time_point timestamp;

    uint64_t frameId = 0; // Frame number for traceability

    CameraStats cameraStats;
    AlgorithmStats algorithmStats;
    DisplayStats displayStats;
    JetsonNanoInfo socInfo;
    PowerStats powerStats;

    double joulesPerFrame = 0.0; // Energy per frame (joules)
    double endToEndLatencyMs = 0.0; // End-to-end latency (ms)
    double processingLatencyMs = 0.0; // Algorithm processing latency (ms)
    double displayLatencyMs = 0.0; // Display rendering latency (ms)
    
    // NEW: Add the missing top-level fields from your query
    // These are typically derived/aggregated, but add them as members for direct access
    // === EXISTING DERIVED FIELDS (already present) ===
    double fps = 0.0;             // Aggregated FPS (e.g., from camera/algo)
    double avg_power_w_alg = 0.0; // Average power during algorithm processing
    double cpu_util_avg = 0.0;    // Average CPU utilization
    double gpu_util_avg = 0.0;    // Average GPU utilization
    double cpu_temp_c = 0.0;      // CPU temperature (°C)
    double gpu_temp_c = 0.0;      // GPU temperature (°C)

    // === NEW: Aggregated latency for Pareto objective (PhD ERL requirement) ===
    double avg_latency_ms = 0.0;          // ? ADD THIS LINE
    // Optional weighted variant for finer control
    double weighted_end_to_end_latency_ms = 0.0;

    
    bool valid = false; // ADD THIS: Public member for quick checks (set in constructors/methods)
    
    SystemMetricsSnapshot() = default;
    
    explicit SystemMetricsSnapshot(std::chrono::system_clock::time_point ts)
        : timestamp(ts), frameId(0), cameraStats(ts), algorithmStats(ts), displayStats(ts), socInfo(ts), powerStats(ts) {
        valid = isValid();  // Set based on validation
    }
    
    /**
     * @brief Validates the snapshot for completeness.
     * @return True if all required fields are populated, false otherwise.
     */
    bool isValid() const {
        return timestamp != std::chrono::system_clock::time_point{} &&
               (cameraStats.isValid() || algorithmStats.isValid() ||
                displayStats.isValid() || socInfo.isValid() || powerStats.isValid());
    }
    
    /**
     * @brief Checks if the snapshot contains any meaningful data.
     * @return True if at least one sub-stat has data or derived metrics are non-zero, false otherwise.
     * @note This can be used in conjunction with isValid() for stricter checks.
     */
    bool hasData() const {
        // Check sub-stats for data (assuming their isValid() implies data presence)
        // and derived fields for non-default values.
        // Adjust as needed if sub-stats have a separate hasData() or specific fields.
        return cameraStats.isValid() ||
               algorithmStats.isValid() ||
               displayStats.isValid() ||
               socInfo.isValid() ||
               powerStats.isValid() ||
               (joulesPerFrame != 0.0) ||
               (endToEndLatencyMs != 0.0) ||
               (processingLatencyMs != 0.0) ||
               (displayLatencyMs != 0.0) ||
               (fps != 0.0) ||             // NEW: Check added fields
               (avg_power_w_alg != 0.0) ||
               (cpu_util_avg != 0.0) ||
               (gpu_util_avg != 0.0) ||
               (cpu_temp_c != 0.0) ||
               (gpu_temp_c != 0.0);
    }
    
    /**
     * @brief Converts the snapshot to JSON for serialization.
     * @return JSON object representing the snapshot.
     */
    json toJson() const {
        json j;
        j["timestamp"] = utils::formatTimestamp(timestamp);
        j["frame_id"] = frameId;
        j["camera"] = cameraStats.toJson();
        j["algorithm"] = algorithmStats.toJson();
        j["display"] = displayStats.toJson();
        j["soc"] = socInfo.toJson();
        j["power"] = powerStats.toJson();
        j["derived"] = {
            {"end_to_end_latency_ms", endToEndLatencyMs},
            {"processing_latency_ms", processingLatencyMs},
            {"display_latency_ms", displayLatencyMs},
            {"joules_per_frame", joulesPerFrame},
            {"fps", fps},                           // NEW
            {"avg_power_w_alg", avg_power_w_alg},   // NEW
            {"cpu_util_avg", cpu_util_avg},         // NEW
            {"gpu_util_avg", gpu_util_avg},         // NEW
            {"cpu_temp_c", cpu_temp_c},             // NEW
            {"gpu_temp_c", gpu_temp_c}              // NEW
        };
        // Optional: Add validity flags for debugging/serialization
        j["valid"] = isValid();
        j["has_data"] = hasData();
        return j;
    }

/**
     * @brief Computes aggregated latency from sub-components (call this in aggregator after filling sub-stats)
     */
    void computeAggregatedLatency() {
        // Weighted average: 60% end-to-end, 30% processing, 10% display (tune for your pipeline)
        avg_latency_ms = 0.6 * endToEndLatencyMs +
                         0.3 * processingLatencyMs +
                         0.1 * displayLatencyMs;

        // Alternative: simple max for worst-case real-time guarantee
        // avg_latency_ms = std::max({endToEndLatencyMs, processingLatencyMs, displayLatencyMs});

        weighted_end_to_end_latency_ms = endToEndLatencyMs; // keep raw if needed
    }
    
};
//==============================================================================================================================================
//#endif // ALL_MODULES_STATCS_H
