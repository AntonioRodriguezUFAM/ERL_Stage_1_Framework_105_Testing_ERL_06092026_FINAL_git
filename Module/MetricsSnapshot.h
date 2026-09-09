//MetricsSnapshot.h
#pragma once

#include <cstdint>

namespace hrl {

struct MetricsSnapshot {
    // --- Identifiers & Timestamps ---
    uint64_t timestamp{0};
    uint64_t frameId{0};

    // --- Performance & Latency Metrics ---
    double fps{0.0};
    double latency_ms{0.0};
    double avg_latency_ms{0.0};
    double avg_inference_ms{0.0};
    double processing_latency_ms{0.0};
    double display_latency_ms{0.0};
    double end_to_end_latency_ms{0.0};

    // --- Power & Energy Metrics ---
    double energy_j_alg{0.0};
    double avg_power_w_alg{0.0};
    double joulesPerFrame{0.0};
    double joules_per_frame{0.0}; // Alias for joulesPerFrame
    double powerPerFrameW{0.0};

    // --- System Telemetry ---
    double cpu_util_avg{0.0};
    double gpu_util_avg{0.0};
    double cpu_temp_c{0.0};
    double gpu_temp_c{0.0};

    // --- Validity Flag ---
    bool valid{false};

    /**
     * @brief Checks if any telemetry data has been populated.
     */
    bool hasData() const {
        return (fps > 0.0) || (latency_ms > 0.0) || (avg_latency_ms > 0.0) ||
               (end_to_end_latency_ms > 0.0) || (energy_j_alg > 0.0) ||
               (avg_power_w_alg > 0.0) || (cpu_util_avg > 0.0) ||
               (gpu_util_avg > 0.0) || (cpu_temp_c > 0.0) || (gpu_temp_c > 0.0);
    }

    /**
     * @brief Aggregates weighted latency metrics, syncs alias fields,
     *        and sets the snapshot validity state.
     */
    void computeAggregatedMetrics() {
        // 1. Compute aggregated latency if not explicitly set
        if (avg_latency_ms <= 0.0) {
            avg_latency_ms = 0.6 * end_to_end_latency_ms +
                             0.3 * processing_latency_ms +
                             0.1 * display_latency_ms;
        }

        // 2. Maintain backward compatibility for latency_ms
        if (latency_ms <= 0.0) {
            latency_ms = (avg_latency_ms > 0.0) ? avg_latency_ms : end_to_end_latency_ms;
        }

        // 3. Synchronize Joules-per-frame naming conventions
        if (joulesPerFrame <= 0.0 && joules_per_frame > 0.0) {
            joulesPerFrame = joules_per_frame;
        }
        if (joules_per_frame <= 0.0 && joulesPerFrame > 0.0) {
            joules_per_frame = joulesPerFrame;
        }

        // 4. Update validity status
        valid = hasData();
    }
};

} // namespace hrl

//=============================================================================================
// #pragma once
// #include <chrono>
// #include <string>

// namespace hrl {

// /**
//  * @struct MetricsSnapshot
//  * @brief Lightweight snapshot used by ERL (GeneticAlgorithm + EvolutionarySelector)
//  *        This is the main structure passed to the optimization loop.
//  *     it contains core real-time metrics, latency breakdowns, power consumption, and temperatures.
//  *       Flat snapshot for the RL Agent's decision making
//  */
// struct MetricsSnapshot {
//     std::chrono::system_clock::time_point timestamp{};

//     uint64_t frameId = 0;

//     // === Core Real-time Metrics ===
//     double fps = 0.0;
//     double avg_inference_ms = 0.0;

//     // === Power & Energy ===
//     double avg_power_w_alg = 0.0;
//     double joules_per_frame = 0.0;      // Preferred name
//     double joulesPerFrame = 0.0;        // Compatibility alias

//     // === Resource Utilization ===
//     double cpu_util_avg = 0.0;
//     double gpu_util_avg = 0.0;

//     // === Latency (detailed + aggregated) ===
//     double end_to_end_latency_ms = 0.0;
//     double processing_latency_ms = 0.0;
//     double display_latency_ms = 0.0;
//     double avg_latency_ms = 0.0;        // Weighted for Pareto objectives
//     double latency_ms = 0.0;            // Compatibility alias (old code)

//     // === Thermal Awareness ===
//     double cpu_temp_c = 0.0;
//     double gpu_temp_c = 0.0;

//     bool valid = false;

//     MetricsSnapshot() = default;

//     explicit MetricsSnapshot(std::chrono::system_clock::time_point ts)
//         : timestamp(ts) {}

//     bool hasData() const {
//         return fps > 0.01 ||
//                avg_inference_ms > 0.0 ||
//                avg_power_w_alg > 0.0 ||
//                joules_per_frame > 0.0 ||
//                cpu_util_avg > 0.0 ||
//                gpu_util_avg > 0.0 ||
//                end_to_end_latency_ms > 0.0;
//     }

//     bool isValid() const {
//         return valid && timestamp != std::chrono::system_clock::time_point{} && hasData();
//     }

//     // void computeAggregatedMetrics() {
//     //     // Compute aggregated latency if not already set
//     //     if (avg_latency_ms <= 0.0) {
//     //         avg_latency_ms = 0.6 * end_to_end_latency_ms +
//     //                          0.3 * processing_latency_ms +
//     //                          0.1 * display_latency_ms;
//     //     }

//     //     // Maintain backward compatibility
//     //     if (latency_ms <= 0.0) {
//     //         latency_ms = avg_latency_ms > 0.0 ? avg_latency_ms : end_to_end_latency_ms;
//     //     }

//     //     if (joulesPerFrame <= 0.0 && joules_per_frame > 0.0) {
//     //         joulesPerFrame = joules_per_frame;
//     //     }
//     //     if (joules_per_frame <= 0.0 && joulesPerFrame > 0.0) {
//     //         joules_per_frame = joulesPerFrame;
//     //     }

//     //     valid = hasData();
//     // }
// };

// } // namespace hrl