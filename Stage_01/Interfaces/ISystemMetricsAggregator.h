// ISystemMetricsAggregator.h
// // File 1: ISystemMetricsAggregator.h (Interface)

// #pragma once

// #include <chrono>
// #include <functional>
// #include <memory>
// #include <string>
// #include <vector>
// #include <mutex>
// #include "../SharedStructures/allModulesStatcs.h"

// /**
//  * @interface ISystemMetricsAggregator
//  * @brief Interface for a system metrics aggregator that collects and manages metrics from various system components.
//  * This interface defines the contract for metric collection, allowing different implementations to provide specific aggregation logic.
//  */
// class ISystemMetricsAggregator {
// public:
//     virtual ~ISystemMetricsAggregator() = default;

//     /**
//      * @brief Pushes a timestamped snapshot of the current system metrics.
//      * @param timestamp The time at which this snapshot is taken.
//      * @param updateFn A lambda that modifies the relevant part of the snapshot.
//      */
//     virtual void pushMetrics(
//         const std::chrono::system_clock::time_point& timestamp,
//         std::function<void(SystemMetricsSnapshot&)> updateFn
//     ) = 0;

//     /**
//      * @brief Retrieves the most recent metrics snapshot.
//      * @return The latest SystemMetricsSnapshot or a default-constructed one if none exist.
//      */
//     virtual SystemMetricsSnapshot getLatestSnapshot() const = 0;

//     /**
//      * @brief Exports all collected metrics to a CSV file.
//      * @param filePath The path to the output CSV file.
//      */
//     virtual void exportToCSV(const std::string& filePath) = 0;

//     /**
//      * @brief Exports all collected metrics to a JSON file.
//      * @param filePath The path to the output JSON file.
//      */
//   virtual void exportToJSON(const std::string& filePath) = 0;

//     /**
//      * @brief Retrieves all collected metrics snapshots.
//      * @return A vector of all SystemMetricsSnapshot instances.
//      */
//     virtual std::vector<SystemMetricsSnapshot> getAllSnapshots() const = 0;

//     /**
//      * @brief Pushes camera metrics into the aggregator.
//      * @param stats The camera metrics to be pushed.
//      */
//     virtual void pushCameraStats(const CameraStats& stats) = 0;

//     /**
//      * @brief Pushes algorithm metrics into the aggregator.
//      * @param stats The algorithm metrics to be pushed.
//      */
//     virtual void pushAlgorithmStats(const AlgorithmStats& stats) = 0;

//     /**
//      * @brief Pushes display metrics into the aggregator.
//      * @param stats The display metrics to be pushed.
//      */
//     virtual void pushDisplayStats(const DisplayStats& stats) = 0;

//     /**
//      * @brief Pushes SoC (System on Chip) metrics into the aggregator.
//      * @param stats The SoC metrics to be pushed.
//      */
//     virtual void pushSoCStats(const JetsonNanoInfo& stats) = 0;

//     /**
//      * @brief Pushes power metrics into the aggregator.
//      * @param stats The power metrics to be pushed.
//      */
//     virtual void pushPowerStats(const PowerStats& stats) = 0;

//     /**
//      * @brief Retrieves aggregated metrics at a specific timestamp.
//      * @param ts The timestamp for which to retrieve metrics.
//      * @return The aggregated metrics snapshot at the specified timestamp.
//      */
//     virtual SystemMetricsSnapshot getAggregatedAt(std::chrono::system_clock::time_point ts) const = 0;

//     /**
//      * @brief Retrieves the configuration settings for the aggregator.
//      * @return The AggregatorConfig structure containing configuration settings.
//      */     
    
//     //std::shared_ptr<ISystemMetricsAggregator> agg = std::make_shared<SystemMetricsAggregatorConcreteV2>();


//     //------------------------------------------------------------------
//     //  **NEW** frame-centric API  (used by *_v2 modules)
//     //------------------------------------------------------------------
//     virtual void beginFrame     (uint64_t /*frameId*/, const CameraStats&      )  {};
//     virtual void mergeAlgorithm (uint64_t /*frameId*/, const AlgorithmStats&   )  {};
//     virtual void mergeDisplay   (uint64_t /*frameId*/, const DisplayStats&     )  {};
//     virtual void mergeSoC       (uint64_t /*frameId*/, const JetsonNanoInfo&   )  {};
//     virtual void mergePower     (uint64_t /*frameId*/, const PowerStats&       )  {};

//     //virtual void mergeSoC (uint64_t id, const JetsonNanoInfo& s) {};
//     //virtual void mergePower (uint64_t id, const PowerStats& s){};

//     virtual void overlayStats(const std::string& module,
//                       const std::unordered_map<std::string, double>& kv,
//                       const std::chrono::system_clock::time_point& ts) = 0;


// };
// // End of ISystemMetricsAggregator.h
// //================================================================

// File 2: ISystemMetricsAggregator.h (Implementation)
//================================================================
// Interfaces/ISystemMetricsAggregator.h

//================================================================================
// ISystemMetricsAggregator.h  FIXED VERSION (Suppress unused params)
//================================================================================

#pragma once
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include "../SharedStructures/allModulesStatcs.h"
#include "../SharedStructures/AggregatorConfig.h"

//using time_point = std::chrono::system_clock::time_point;

/**
 * @interface ISystemMetricsAggregator
 * @brief Interface for a system metrics aggregator that collects and manages metrics from various system components.
 */
class ISystemMetricsAggregator:public IModule { // Inherit from IModule here
public:
    virtual ~ISystemMetricsAggregator() = default;

    // In ISystemMetricsAggregator.h
    virtual void forceFlushBatch() = 0;

    //virtual void pushMetrics(const system_clock::time_point& timestamp, std::function<void(SystemMetricsSnapshot&)> updateFn) = 0;
    virtual SystemMetricsSnapshot getLatestSnapshot() const = 0;
    virtual void exportToCSV(const std::string& filePath) = 0;
    virtual void exportToJSON(const std::string& filePath) = 0;
    virtual std::vector<SystemMetricsSnapshot> getAllSnapshots() const = 0;
    virtual void pushCameraStats(const CameraStats& stats) = 0;
    virtual void pushAlgorithmStats(const AlgorithmStats& stats) = 0;
    virtual void pushDisplayStats(const DisplayStats& stats) = 0;
    virtual void pushSoCStats(const JetsonNanoInfo& stats) = 0;
    virtual void pushPowerStats(const PowerStats& stats) = 0;
    //virtual SystemMetricsSnapshot getAggregatedAt(const system_clock::time_point& ts) const = 0;
    virtual const AggregatorConfig& getConfig() const = 0;

    // NEW (Fixed)
    virtual void pushMetrics(const std::chrono::system_clock::time_point& timestamp, std::function<void(SystemMetricsSnapshot&)> updateFn) = 0;
    // NEW (Fixed)
    virtual SystemMetricsSnapshot getAggregatedAt(const std::chrono::system_clock::time_point& ts) const = 0;

    //------------------------------------------------------------------

    // Frame-centric API
    virtual void beginFrame(uint64_t frameId, const CameraStats& stats) {
        //(void)frameId; (void)stats;  // [FIX] Suppress unused warnings
        (void)frameId; (void)stats;  // [FIX 2025-12-19] Suppress unused param warnings
    }
    //virtual void beginFrame(uint64_t frameId, const CameraStats& stats) {};
    virtual void mergeAlgorithm(uint64_t frameId, const AlgorithmStats& stats) {
       // (void)frameId; (void)stats;
        (void)frameId; (void)stats;  // [FIX 2025-12-19] Suppress unused param warnings
    };
    virtual void mergeDisplay(uint64_t frameId, const DisplayStats& stats) {
        //(void)frameId; (void)stats;
        (void)frameId; (void)stats;  // [FIX 2025-12-19] Suppress unused param warnings
    };
    virtual void mergeSoC(uint64_t frameId, const JetsonNanoInfo& stats) =0;
    virtual void mergePower(uint64_t frameId, const PowerStats& stats) =0;

    // virtual void overlayStats(const std::string& module,
    //                          const std::unordered_map<std::string, double>& kv,
    //                          const time_point& ts) = 0;

    virtual void overlayStats(const std::string& module,
                              const std::unordered_map<std::string, double>& values,
                              std::chrono::system_clock::time_point ts) {
        (void)module; (void)values; (void)ts;  // [FIX 2025-12-19] Suppress unused param warnings
    }

    // IModule methods are now inherited and must be implemented in concretes
};
// End of ISystemMetricsAggregator.h
//================================================================



