



//=========================================================================
// 20/03/2026
//========================================================================================
//==================================================================================================
// ConfigManager.hpp ? Null-safe config loader + pipeline builder
// - Ubuntu 18.04 compatible (std::experimental::filesystem)
// - Strict CONSUMER-FIRST ordering: Display ? Algorithm ? Camera ? Metrics ? Brain
// - Fixes: FPS drops, black screens, queue overflow, metrics invalid warnings
// - Updated: 20/03/2026 ? Cleaned & production-ready
//==================================================================================================

#pragma once

#include <memory>
#include <vector>
#include <string>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <experimental/filesystem>
#include <thread>

#include "MetricsSnapshot.h" // Ensures full hrl::MetricsSnapshot struct is loaded

#include <spdlog/spdlog.h>
#include "../nlohmann/json.hpp"

#include "IModule.h"
#include "RuntimeControls.h"  // must precede Modules/Scheduler
#include "Modules.h"                    // CameraModule, AlgorithmModule, DisplayModule, SoCModule, LynsynModule
//#include "MetricsSnapshot.h"            // hrl::MetricsSnapshot definition
#include "ModuleFactory.h"
#include "EvolutionarySelector_02.h"

#include "../Stage_01/SharedStructures/SharedQueue.h"
#include "../Stage_01/SharedStructures/ZeroCopyFrameData.h"
#include "../Stage_01/SharedStructures/ThreadManager.h"
#include "../Stage_01/Concretes/SystemMetricsAggregatorConcrete_v3_2.h"



namespace fs = std::experimental::filesystem;
using json = nlohmann::json;

// --------------------------------------------------------------
// Null-safe JSON helpers
// --------------------------------------------------------------
template<typename T>
inline T jvalue(const json& j, const char* key, const T& def) {
    if (j.is_object()) {
        auto it = j.find(key);
        if (it != j.end() && !it->is_null()) {
            try { return it->get<T>(); } catch (...) {}
        }
    }
    return def;
}

inline json jobject_or_empty(const json& parent, const char* key) {
    if (parent.is_object()) {
        auto it = parent.find(key);
        if (it != parent.end() && it->is_object()) return *it;
    }
    return json::object();
}

// --------------------------------------------------------------
// ConfigManager ? Main pipeline orchestrator
// --------------------------------------------------------------
class ConfigManager {
public:
    ConfigManager(const std::string& configPath, Context& ctx) : ctx_(ctx) {
        if (fs::exists(configPath)) {
            std::ifstream f(configPath);
            try {
                config_ = json::parse(f);
                spdlog::info("Loaded configuration from {}", configPath);
            } catch (const std::exception& e) {
                spdlog::warn("Failed parsing {} ({}). Using defaults.", configPath, e.what());
                config_ = json::object();
            }
        } else {
            spdlog::warn("Config file not found: {}. Using defaults.", configPath);
            config_ = json::object();
        }
        sanitizeDefaults();
    }

    ConfigManager(const json& cfg, Context& ctx) : ctx_(ctx), config_(cfg) {
        sanitizeDefaults();
    }

    // // --------------------------------------------------------------
    // // buildPipeline: Creates queues, aggregator, modules in CONSUMER-FIRST order
    // // --------------------------------------------------------------
    // void buildPipeline() {
    //     // 1. Queues + Runtime Controls
    //     buildQueues_();
    //     runtimeControls_ = std::make_shared<hrl::RuntimeControls>();

    //     // 2. Aggregator
    //     const json jCamera = jobject_or_empty(config_, "CameraConfig");
    //     const json jAgg    = jobject_or_empty(config_, "Aggregator");   // ? Important
    //     const json jLynsyn = jobject_or_empty(config_, "LynsynMonitorConfig");

    //     const json jEvo = jobject_or_empty(config_, "EvolutionarySelector");

    //     const int cameraFps = jvalue<int>(jCamera, "fps", 30);
    //     const int defaultMergeWait = std::max(10, std::min(250, (cameraFps > 0 ? (1000 / cameraFps) / 2 : 33)));
    //     const int mergeWaitMs = jvalue<int>(jAgg, "merge_wait_ms", defaultMergeWait);

    //     json aggCfg = {
    //         {"metrics_csv",  jvalue<std::string>(jAgg, "metrics_csv",  jvalue<std::string>(config_, "metrics_csv", "/tmp/realtime_metrics.csv"))},
    //         {"metrics_json", jvalue<std::string>(jAgg, "metrics_json", jvalue<std::string>(config_, "metrics_json", "/tmp/realtime_metrics.ndjson"))},
    //         {"retention_window_sec", jvalue<int>(jAgg, "retention_window_sec", 2000)},
    //         {"merge_wait_ms",        mergeWaitMs},
    //         {"flush_period_ms",      jvalue<int>(jAgg, "flush_period_ms", 1000)},
    //         {"json_flush_threshold", jvalue<int>(jAgg, "json_flush_threshold", 2000)},
    //         {"drop_empty_compat_rows", jvalue<bool>(jAgg, "drop_empty_compat_rows", true)},
    //         {"drop_empty_flush_rows",  jvalue<bool>(jAgg, "drop_empty_flush_rows", true)},
    //         {"expectsCamera", true},
    //         {"expectsAlgorithm", true},
    //         {"expectsDisplay", true},
    //         {"expectsSoC", true},
    //         {"expectsPower", jvalue<bool>(jLynsyn, "enabled", true)}
    //     };

    //     aggregator_ = std::make_shared<SystemMetricsAggregatorConcreteV3_2>(aggCfg);
    //     spdlog::info("[ConfigManager] Aggregator initialized with metrics_csv = {}", 
    //                 aggCfg.value("metrics_csv", std::string("N/A")));

    //     // // 2. Aggregator
    //     // const json jCamera = jobject_or_empty(config_, "CameraConfig");
    //     // const json jAgg    = jobject_or_empty(config_, "Aggregator");
    //     // const json jLynsyn = jobject_or_empty(config_, "LynsynMonitorConfig");

    //     // const int cameraFps = jvalue<int>(jCamera, "fps", 30);
    //     // const int defaultMergeWait = std::max(10, std::min(250, (cameraFps > 0 ? (1000 / cameraFps) / 2 : 33)));
    //     // const int mergeWaitMs = jvalue<int>(jAgg, "merge_wait_ms", defaultMergeWait);

    
    //     // json aggCfg = {
    //     //     {"metrics_csv", jvalue<std::string>(config_, "metrics_csv", "/tmp/build/realtime_metrics_011.csv")},
    //     //     {"metrics_json", jvalue<std::string>(config_, "metrics_json", "/tmp/build/realtime_metrics011.ndjson")},
    //     //     {"retention_window_sec", jvalue<int>(config_, "retention_window_sec", 2000)},
    //     //     {"expectsPower", jvalue<bool>(jLynsyn, "enabled", true)},
    //     //     {"expectsCamera", true},
    //     //     {"expectsAlgorithm", true},
    //     //     {"expectsDisplay", true},
    //     //     {"expectsSoC", true},
    //     //     {"merge_wait_ms", mergeWaitMs},
    //     //     {"flush_period_ms", jvalue<int>(jAgg, "flush_period_ms", 1000)},
    //     //     {"json_flush_threshold", jvalue<int>(jAgg, "json_flush_threshold", 2000)},
    //     //     {"drop_empty_compat_rows", jvalue<bool>(jAgg, "drop_empty_compat_rows", true)},
    //     //     {"drop_empty_flush_rows", jvalue<bool>(jAgg, "drop_empty_flush_rows", true)}
    //     // };

    //     // aggregator_ = std::make_shared<SystemMetricsAggregatorConcreteV3_2>(aggCfg);
    //     // spdlog::info("[ConfigManager] Aggregator initialized.");

    //     // 3. Modules ? STRICT CONSUMER-FIRST ORDER
    //     const json jDisp  = jobject_or_empty(config_, "DisplayConfig");
    //     const json jAlg   = jobject_or_empty(config_, "AlgorithmConfig");
    //     const json jSoC   = jobject_or_empty(config_, "SoCConfig");
    //     const json jSched = jobject_or_empty(config_, "Scheduler");

    //     // Display FIRST (Consumer)
    //     modules_.push_back(ModuleFactory::create<DisplayModule>(jDisp, ctx_, cam2Disp_, alg2Disp_, aggregator_));
    //     displayModule_ = static_cast<DisplayModule*>(modules_.back().get());
    //     spdlog::info("[ConfigManager] [1/5] Display created (Consumer first)");

    //     // Algorithm SECOND
    //     modules_.push_back(ModuleFactory::create<AlgorithmModule>(jAlg, ctx_, cam2Alg_, alg2Disp_, *ctx_.tm, aggregator_, runtimeControls_));
    //     algorithmModule_ = static_cast<AlgorithmModule*>(modules_.back().get());
    //     spdlog::info("[ConfigManager] [2/5] Algorithm created");

    //     // Camera THIRD (Producer)
    //     modules_.push_back(ModuleFactory::create<CameraModule>(jCamera, ctx_, cam2Alg_, cam2Disp_, aggregator_));
    //     cameraModule_ = static_cast<CameraModule*>(modules_.back().get());
    //     spdlog::info("[ConfigManager] [3/5] Camera created (Producer last)");

    //     // SoC & Lynsyn (metrics)
    //     modules_.push_back(ModuleFactory::create<SoCModule>(jSoC, ctx_, aggregator_));
    //     socModule_ = static_cast<SoCModule*>(modules_.back().get());
    //     spdlog::info("[ConfigManager] [4/5] SoC created");

    //     if (jvalue<bool>(jLynsyn, "enabled", true)) {
    //         modules_.push_back(ModuleFactory::create<LynsynModule>(jLynsyn, ctx_, *ctx_.tm, aggregator_));
    //         lynsynModule_ = static_cast<LynsynModule*>(modules_.back().get());
    //         spdlog::info("[ConfigManager] [4/5] Lynsyn created");
    //     }

    //     // Brain LAST (Scheduler or ERL)
    //     // ERL may be requested either by Scheduler.use_erl or by EvolutionarySelector.enabled.
    //     const bool schedulerEnabled = jvalue<bool>(jSched, "enabled", config_.contains("Scheduler"));
    //     const bool evoEnabled       = jvalue<bool>(jEvo, "enabled", false);
    //     const bool useERL           = jvalue<bool>(jSched, "use_erl", evoEnabled);

    //     if (schedulerEnabled || evoEnabled) {
    //         if (useERL) {
    //             spdlog::info("[ConfigManager] [5/5] Initializing Evolutionary Selector (ERL)...");

    //             json jBrain = jSched;
    //             for (auto it = jEvo.begin(); it != jEvo.end(); ++it) {
    //                 jBrain[it.key()] = it.value();
    //             }

    //             modules_.push_back(ModuleFactory::create<EvolutionarySelector>(jBrain, ctx_, aggregator_, runtimeControls_));
    //             evolutionarySelector_ = static_cast<EvolutionarySelector*>(modules_.back().get());
    //         } else {
    //             spdlog::info("[ConfigManager] [5/5] Initializing Standard Scheduler...");
    //             modules_.push_back(ModuleFactory::create<SchedulerModule>(jSched, ctx_, aggregator_, runtimeControls_));
    //             schedulerModule_ = static_cast<SchedulerModule*>(modules_.back().get());
    //         }
    //     }

    //     spdlog::info("[ConfigManager] Pipeline built: {} modules (Display ? Algorithm ? Camera order).", modules_.size());
    // }

    //========================================================================================
    // --------------------------------------------------------------
    // buildPipeline: Creates queues, aggregator, modules in CONSUMER-FIRST order
    // --------------------------------------------------------------
    void buildPipeline() {
        // 1. Queues + Runtime Controls
        buildQueues_();
        runtimeControls_ = std::make_shared<hrl::RuntimeControls>();

        // 2. Aggregator
        const json jCamera = jobject_or_empty(config_, "CameraConfig");
        const json jAgg    = jobject_or_empty(config_, "Aggregator");   // ? Important
        const json jLynsyn = jobject_or_empty(config_, "LynsynMonitorConfig");

        const json jEvo = jobject_or_empty(config_, "EvolutionarySelector");

        const int cameraFps = jvalue<int>(jCamera, "fps", 30);
        const int defaultMergeWait = std::max(10, std::min(250, (cameraFps > 0 ? (1000 / cameraFps) / 2 : 33)));
        const int mergeWaitMs = jvalue<int>(jAgg, "merge_wait_ms", defaultMergeWait);

        json aggCfg = {
            {"metrics_csv",  jvalue<std::string>(jAgg, "metrics_csv",  jvalue<std::string>(config_, "metrics_csv", "/tmp/realtime_metrics.csv"))},
            {"metrics_json", jvalue<std::string>(jAgg, "metrics_json", jvalue<std::string>(config_, "metrics_json", "/tmp/realtime_metrics.ndjson"))},
            {"retention_window_sec", jvalue<int>(jAgg, "retention_window_sec", 2000)},
            {"merge_wait_ms",        mergeWaitMs},
            {"flush_period_ms",      jvalue<int>(jAgg, "flush_period_ms", 1000)},
            {"json_flush_threshold", jvalue<int>(jAgg, "json_flush_threshold", 2000)},
            {"drop_empty_compat_rows", jvalue<bool>(jAgg, "drop_empty_compat_rows", true)},
            {"drop_empty_flush_rows",  jvalue<bool>(jAgg, "drop_empty_flush_rows", true)},
            {"expectsCamera", true},
            {"expectsAlgorithm", true},
            {"expectsDisplay", true},
            {"expectsSoC", true},
            {"expectsPower", jvalue<bool>(jLynsyn, "enabled", true)}
        };

        aggregator_ = std::make_shared<SystemMetricsAggregatorConcreteV3_2>(aggCfg);
        spdlog::info("[ConfigManager] Aggregator initialized with metrics_csv = {}", 
                    aggCfg.value("metrics_csv", std::string("N/A")));

        // 3. Modules ? STRICT CONSUMER-FIRST ORDER
        const json jDisp  = jobject_or_empty(config_, "DisplayConfig");
        const json jAlg   = jobject_or_empty(config_, "AlgorithmConfig");
        const json jSoC   = jobject_or_empty(config_, "SoCConfig");
        const json jSched = jobject_or_empty(config_, "Scheduler");

        // Display FIRST (Consumer)
        modules_.push_back(ModuleFactory::create<DisplayModule>(jDisp, ctx_, cam2Disp_, alg2Disp_, aggregator_));
        displayModule_ = static_cast<DisplayModule*>(modules_.back().get());
        spdlog::info("[ConfigManager] [1/5] Display created (Consumer first)");

        // Algorithm SECOND
        modules_.push_back(ModuleFactory::create<AlgorithmModule>(jAlg, ctx_, cam2Alg_, alg2Disp_, *ctx_.tm, aggregator_, runtimeControls_));
        algorithmModule_ = static_cast<AlgorithmModule*>(modules_.back().get());
        spdlog::info("[ConfigManager] [2/5] Algorithm created");

        // Camera THIRD (Producer)
        modules_.push_back(ModuleFactory::create<CameraModule>(jCamera, ctx_, cam2Alg_, cam2Disp_, aggregator_));
        cameraModule_ = static_cast<CameraModule*>(modules_.back().get());
        spdlog::info("[ConfigManager] [3/5] Camera created (Producer last)");

        // SoC & Lynsyn (metrics)
        modules_.push_back(ModuleFactory::create<SoCModule>(jSoC, ctx_, aggregator_));
        socModule_ = static_cast<SoCModule*>(modules_.back().get());
        spdlog::info("[ConfigManager] [4/5] SoC created");

        if (jvalue<bool>(jLynsyn, "enabled", true)) {
            modules_.push_back(ModuleFactory::create<LynsynModule>(jLynsyn, ctx_, *ctx_.tm, aggregator_));
            lynsynModule_ = static_cast<LynsynModule*>(modules_.back().get());
            spdlog::info("[ConfigManager] [4/5] Lynsyn created");
        }

        // Brain LAST (Scheduler or ERL)
        const bool schedulerEnabled = jvalue<bool>(jSched, "enabled", config_.contains("Scheduler"));
        const bool evoEnabled       = jvalue<bool>(jEvo, "enabled", false);
        const bool useERL           = jvalue<bool>(jSched, "use_erl", evoEnabled);

        // --- PATCH 4 START: Build Configuration Structs ---
        const json jThermal = jobject_or_empty(config_, "ThermalGovernor");
        hrl::ThermalGovernor::Config thermalCfg;
        thermalCfg.temperature_offset_c = jvalue<double>(jThermal, "temperature_offset_c", thermalCfg.temperature_offset_c);
        thermalCfg.caution_c   = jvalue<double>(jThermal, "caution_c",   thermalCfg.caution_c);
        thermalCfg.warning_c   = jvalue<double>(jThermal, "warning_c",   thermalCfg.warning_c);
        thermalCfg.critical_c  = jvalue<double>(jThermal, "critical_c",  thermalCfg.critical_c);
        thermalCfg.emergency_c = jvalue<double>(jThermal, "emergency_c", thermalCfg.emergency_c);
        thermalCfg.shutdown_c  = jvalue<double>(jThermal, "shutdown_c",  thermalCfg.shutdown_c);

        const hrl::AdaptiveWeightManager::Config awmDefaults{}; 
        hrl::AdaptiveWeightManager::Config awmCfg;
        awmCfg.update_interval_gens   = jvalue<int>(jSched, "awm_update_interval_gens", awmDefaults.update_interval_gens);
        awmCfg.battery_critical_watts = jvalue<double>(jSched, "awm_battery_critical_watts", awmDefaults.battery_critical_watts);
        awmCfg.thermal_warn_cpu_c     = jvalue<double>(jSched, "awm_thermal_warn_cpu_c", awmDefaults.thermal_warn_cpu_c);
        awmCfg.thermal_warn_gpu_c     = jvalue<double>(jSched, "awm_thermal_warn_gpu_c", awmDefaults.thermal_warn_gpu_c);
        awmCfg.thermal_crit_cpu_c     = jvalue<double>(jSched, "awm_thermal_crit_cpu_c", awmDefaults.thermal_crit_cpu_c);
        awmCfg.thermal_crit_gpu_c     = jvalue<double>(jSched, "awm_thermal_crit_gpu_c", awmDefaults.thermal_crit_gpu_c);
        awmCfg.latency_sla_ms         = jvalue<double>(jSched, "awm_latency_sla_ms", awmDefaults.latency_sla_ms);
        awmCfg.fps_underperform_ratio = jvalue<double>(jSched, "awm_fps_underperform_ratio", awmDefaults.fps_underperform_ratio);
        awmCfg.transition_smoothing   = jvalue<double>(jSched, "awm_transition_smoothing", awmDefaults.transition_smoothing);
        // --- PATCH 4 END ---

        if (schedulerEnabled || evoEnabled) {
            if (useERL) {
                spdlog::info("[ConfigManager] [5/5] Initializing Evolutionary Selector (ERL)...");

                json jBrain = jSched;
                for (auto it = jEvo.begin(); it != jEvo.end(); ++it) {
                    jBrain[it.key()] = it.value();
                }

                // Forward thermalCfg and awmCfg to the ERL Constructor
                modules_.push_back(ModuleFactory::create<EvolutionarySelector>(
                    jBrain, ctx_, aggregator_, runtimeControls_, thermalCfg, awmCfg
                ));
                evolutionarySelector_ = static_cast<EvolutionarySelector*>(modules_.back().get());
            } else {
                spdlog::info("[ConfigManager] [5/5] Initializing Standard Scheduler...");
                
                // Forward thermalCfg to Standard Scheduler Constructor
                // modules_.push_back(ModuleFactory::create<SchedulerModule>(
                //     jSched, ctx_, aggregator_, runtimeControls_, thermalCfg
                // ));
                // When creating SchedulerModule in ConfigManager::buildPipeline():
                
                modules_.push_back(ModuleFactory::create<SchedulerModule>(
                    jSched, ctx_, aggregator_, runtimeControls_, thermalCfg
                ));
                schedulerModule_ = static_cast<SchedulerModule*>(modules_.back().get());
            }
        }

        spdlog::info("[ConfigManager] Pipeline built: {} modules (Display ? Algorithm ? Camera order).", modules_.size());
    }
    //=======================================================================================

    void startAll() {
        spdlog::info("[ConfigManager] Starting pipeline modules...");
        try {
            // 1. Aggregator first
            if (aggregator_) {
                spdlog::info("[ConfigManager] Starting Aggregator...");
                aggregator_->start();
            }

            // 2. Start modules in creation order (Display ? Algorithm ? Camera ? Metrics ? Brain)
            for (size_t i = 0; i < modules_.size(); ++i) {
                if (modules_[i]) {
                    spdlog::info("[ConfigManager] Starting Module [{}/{}]", i+1, modules_.size());
                    modules_[i]->start();
                    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Stabilize thread startup
                }
            }

            // 3. Signal pipeline is ready (releases EvolutionarySelector wait)
            ctx_.pipelineReady.store(true, std::memory_order_release);
            spdlog::info("[ConfigManager] Pipeline FULLY STARTED and READY.");
            spdlog::default_logger()->flush();
        }
        catch (const std::exception& e) {
            spdlog::critical("Startup failure: {}", e.what());
            spdlog::default_logger()->flush();
            flushMetrics();
            throw;
        }
    }

    bool validateAll() {
        for (auto& m : modules_) {
            if (m && !m->validate()) return false;
        }
        return true;
    }

    void stopAll() {
        spdlog::info("[ConfigManager] Stopping pipeline modules...");

        // Stop Display first.
        // SDL/EGL rendering on Jetson is sensitive to shutdown order.
        // Stopping display first avoids destroying queues/camera/algorithm before
        // the renderer has fully exited.
        IModule* displayPtr = displayModule_;

        if (displayModule_) {
            try {
                spdlog::info("[ConfigManager] Stopping Display first...");
                displayModule_->stop();
            } catch (const std::exception& e) {
                spdlog::error("[ConfigManager] Display stop failed: {}", e.what());
            }
        }

        // Stop the remaining modules in reverse order.
        for (auto it = modules_.rbegin(); it != modules_.rend(); ++it) {
            if (!(*it)) continue;

            // Skip display because it was already stopped.
            if (it->get() == displayPtr) continue;

            try {
                (*it)->stop();
            } catch (const std::exception& e) {
                spdlog::error("[ConfigManager] Module stop failed: {}", e.what());
            }
        }

        spdlog::info("[ConfigManager] All modules stopped.");
    }

    bool runLoop(double runSeconds) {
        auto startTime = std::chrono::steady_clock::now();
        while (!ctx_.shutdown_flag.load()) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - startTime).count() > runSeconds) {
                spdlog::info("[ConfigManager] Runtime limit reached ({} s).", runSeconds);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return true;
    }

    void flushMetrics() {
        if (aggregator_) {
            spdlog::info("[ConfigManager] Flushing metrics...");
            aggregator_->stop();
            aggregator_->forceFlushBatch();
            const json jProf = jobject_or_empty(config_, "Profiling");
            aggregator_->exportToCSV(jvalue<std::string>(jProf, "metricsOutputFile", "/tmp/PerformanceMetrics.csv"));
            spdlog::info("[ConfigManager] Metrics exported successfully.");
        }
    }

    const json& config() const { return config_; }

private:
    void sanitizeDefaults() {
        if (!config_.is_object()) config_ = json::object();
        // Add your existing sanitize logic here (width, height, fps, etc.)
    }

    void buildQueues_() {
        cam2Alg_   = std::make_shared<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>>(200);
        cam2Disp_  = std::make_shared<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>>(200);
        alg2Disp_  = std::make_shared<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>>(300);
        spdlog::info("[ConfigManager] Queues created: cam2Alg={}, cam2Disp={}, alg2Disp={}",
                     cam2Alg_->capacity(), cam2Disp_->capacity(), alg2Disp_->capacity());
    }

// public:
//     std::shared_ptr<hrl::RuntimeControls> getRuntimeControls() {
//             return runtimeControls_;
//         }

//         hrl::RuntimeControls& getRuntimeControlsRef() {
//             if (!runtimeControls_) throw std::runtime_error("RuntimeControls not initialized");
//             return *runtimeControls_;
//         }

//         hrl::MetricsSnapshot getLatestMetricsSnapshot() const {
//             if (aggregator_) {
//             //return aggregator_ ? aggregator_->getLatestSnapshot() : hrl::MetricsSnapshot{};
//             return aggregator_->getLatestSnapshot();   // Make sure this returns hrl::MetricsSnapshot
//         }
//         return hrl::MetricsSnapshot{};
//         }

//         hrl::RLAction getLastRLAction() const {
//             if (evolutionarySelector_) {
//                 //return evolutionarySelector_->getLastAction();
//                 return evolutionarySelector_->getBestAction();   // Use existing method
//             }
//             return hrl::RLAction{};
//         }
public:
    std::shared_ptr<hrl::RuntimeControls> getRuntimeControls() {
        return runtimeControls_;
    }

    hrl::RuntimeControls& getRuntimeControlsRef() {
        if (!runtimeControls_) {
            throw std::runtime_error("RuntimeControls not initialized");
        }
        return *runtimeControls_;
    }

    // FIXED VERSION
    // hrl::MetricsSnapshot getLatestMetricsSnapshot() const {
    //     if (!aggregator_) {
    //         return hrl::MetricsSnapshot{};
    //     }

    //     SystemMetricsSnapshot sys = aggregator_->getLatestSnapshot();

    //     hrl::MetricsSnapshot snap{};
    //     snap.fps                = sys.fps;
    //     snap.avg_inference_ms   = sys.AlgoInferenceMs;      // adjust field name if needed
    //     snap.avg_power_w_alg    = sys.PowerSensor0_Power;   // or whatever your field is
    //     snap.cpu_util_avg       = (sys.SoC_CPU1_Util + sys.SoC_CPU2_Util + 
    //                                sys.SoC_CPU3_Util + sys.SoC_CPU4_Util) / 4.0;

    //     return snap;
    // }

    // Inside ConfigManager class (public section)

    // hrl::MetricsSnapshot getLatestMetricsSnapshot() const {
    //     if (!aggregator_) {
    //         return hrl::MetricsSnapshot{};
    //     }

    //     SystemMetricsSnapshot sys = aggregator_->getLatestSnapshot();
    //     hrl::MetricsSnapshot snap{};

    //     snap.timestamp                = sys.timestamp;
    //     snap.frameId                  = sys.frameId;

    //     snap.fps                      = sys.algorithmStats.fps > 0 ? sys.algorithmStats.fps : sys.cameraStats.fps;
    //     snap.avg_inference_ms         = sys.algorithmStats.inferenceTimeMs;
    //     snap.avg_power_w_alg          = sys.powerStats.totalPower();
    //     snap.cpu_util_avg             = sys.socInfo.CPU1_Utilization_Percent; // or average all cores
    //     snap.gpu_util_avg             = sys.socInfo.GR3D_Frequency_Percent;   // approximate

    //     snap.end_to_end_latency_ms    = sys.endToEndLatencyMs;
    //     snap.processing_latency_ms    = sys.processingLatencyMs;
    //     snap.display_latency_ms       = sys.displayLatencyMs;
    //     snap.joules_per_frame         = sys.joulesPerFrame;

    //     snap.cpu_temp_c               = sys.socInfo.CPU_Temperature_C;
    //     snap.gpu_temp_c               = sys.socInfo.GPU_Temperature_C;

    //     snap.computeAggregatedMetrics();

    //     return snap;
    // }

        hrl::MetricsSnapshot getLatestMetricsSnapshot() const {
        if (!aggregator_) {
            return hrl::MetricsSnapshot{};
        }

        SystemMetricsSnapshot sys = aggregator_->getLatestSnapshot();
        hrl::MetricsSnapshot snap{};

        //snap.timestamp = sys.timestamp;
        snap.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            sys.timestamp.time_since_epoch()).count();

        snap.frameId   = sys.frameId;

        // Core metrics
        snap.fps = sys.algorithmStats.fps > 0 ? sys.algorithmStats.fps : sys.cameraStats.fps;
        snap.avg_inference_ms = sys.algorithmStats.inferenceTimeMs;
        snap.avg_power_w_alg  = sys.powerStats.totalPower();

        // CPU Util - better average
        snap.cpu_util_avg = (sys.socInfo.CPU1_Utilization_Percent +
                             sys.socInfo.CPU2_Utilization_Percent +
                             sys.socInfo.CPU3_Utilization_Percent +
                             sys.socInfo.CPU4_Utilization_Percent) / 4.0;

        snap.gpu_util_avg = sys.socInfo.GR3D_Frequency_Percent;

        // Latencies
        snap.end_to_end_latency_ms = sys.endToEndLatencyMs;
        snap.processing_latency_ms = sys.processingLatencyMs;
        snap.display_latency_ms    = sys.displayLatencyMs;
        snap.joules_per_frame      = sys.joulesPerFrame;

        // Temperatures
        snap.cpu_temp_c = sys.socInfo.CPU_Temperature_C;
        snap.gpu_temp_c = sys.socInfo.GPU_Temperature_C;

        snap.computeAggregatedMetrics();

        return snap;
    }

    hrl::RLAction getLastRLAction() const {
        if (evolutionarySelector_) {
            return evolutionarySelector_->getBestAction();
        }
        return hrl::RLAction{};
    }


private:
    Context& ctx_;
    json config_;
    std::vector<std::unique_ptr<IModule>> modules_;
    std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator_;
    std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> cam2Alg_;
    std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> cam2Disp_;
    std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> alg2Disp_;
    std::shared_ptr<hrl::RuntimeControls> runtimeControls_;

    // Named (non-owning) pointers ? modules_ vector owns the objects
    SoCModule*           socModule_           = nullptr;
    SchedulerModule*     schedulerModule_     = nullptr;
    EvolutionarySelector* evolutionarySelector_ = nullptr;
    LynsynModule*        lynsynModule_        = nullptr;
    CameraModule*        cameraModule_        = nullptr;
    AlgorithmModule*     algorithmModule_     = nullptr;
    DisplayModule*       displayModule_       = nullptr;
};


// //=========================================================================
// // 20/03/2026
// //========================================================================================
// //==================================================================================================
// // ConfigManager.hpp ? Null-safe config loader + pipeline builder
// // - Ubuntu 18.04 compatible (std::experimental::filesystem)
// // - Strict CONSUMER-FIRST ordering: Display ? Algorithm ? Camera ? Metrics ? Brain
// // - Fixes: FPS drops, black screens, queue overflow, metrics invalid warnings
// // - Updated: 20/03/2026 ? Cleaned & production-ready
// //==================================================================================================

// #pragma once

// #include <memory>
// #include <vector>
// #include <string>
// #include <fstream>
// #include <stdexcept>
// #include <algorithm>
// #include <experimental/filesystem>
// #include <thread>

// #include <spdlog/spdlog.h>
// #include "../nlohmann/json.hpp"

// #include "IModule.h"
// #include "Modules.h"                    // CameraModule, AlgorithmModule, DisplayModule, SoCModule, LynsynModule
// #include "MetricsSnapshot.h"            // hrl::MetricsSnapshot definition
// #include "ModuleFactory.h"
// #include "EvolutionarySelector_02.h"

// #include "../Stage_01/SharedStructures/SharedQueue.h"
// #include "../Stage_01/SharedStructures/ZeroCopyFrameData.h"
// #include "../Stage_01/SharedStructures/ThreadManager.h"
// #include "../Stage_01/Concretes/SystemMetricsAggregatorConcrete_v3_2.h"

// namespace fs = std::experimental::filesystem;
// using json = nlohmann::json;

// // --------------------------------------------------------------
// // Null-safe JSON helpers
// // --------------------------------------------------------------
// template<typename T>
// inline T jvalue(const json& j, const char* key, const T& def) {
//     if (j.is_object()) {
//         auto it = j.find(key);
//         if (it != j.end() && !it->is_null()) {
//             try { return it->get<T>(); } catch (...) {}
//         }
//     }
//     return def;
// }

// inline json jobject_or_empty(const json& parent, const char* key) {
//     if (parent.is_object()) {
//         auto it = parent.find(key);
//         if (it != parent.end() && it->is_object()) return *it;
//     }
//     return json::object();
// }

// // --------------------------------------------------------------
// // ConfigManager ? Main pipeline orchestrator
// // --------------------------------------------------------------
// class ConfigManager {
// public:
//     ConfigManager(const std::string& configPath, Context& ctx) : ctx_(ctx) {
//         if (fs::exists(configPath)) {
//             std::ifstream f(configPath);
//             try {
//                 config_ = json::parse(f);
//                 spdlog::info("Loaded configuration from {}", configPath);
//             } catch (const std::exception& e) {
//                 spdlog::warn("Failed parsing {} ({}). Using defaults.", configPath, e.what());
//                 config_ = json::object();
//             }
//         } else {
//             spdlog::warn("Config file not found: {}. Using defaults.", configPath);
//             config_ = json::object();
//         }
//         sanitizeDefaults();
//     }

//     ConfigManager(const json& cfg, Context& ctx) : ctx_(ctx), config_(cfg) {
//         sanitizeDefaults();
//     }

//     // --------------------------------------------------------------
//     // buildPipeline: Creates queues, aggregator, modules in CONSUMER-FIRST order
//     // --------------------------------------------------------------
//     void buildPipeline() {
//         // 1. Queues + Runtime Controls
//         buildQueues_();
//         runtimeControls_ = std::make_shared<hrl::RuntimeControls>();

//         // 2. Aggregator
//         const json jCamera = jobject_or_empty(config_, "CameraConfig");
//         const json jAgg    = jobject_or_empty(config_, "Aggregator");   // ? Important
//         const json jLynsyn = jobject_or_empty(config_, "LynsynMonitorConfig");

//         const json jEvo = jobject_or_empty(config_, "EvolutionarySelector");

//         const int cameraFps = jvalue<int>(jCamera, "fps", 30);
//         const int defaultMergeWait = std::max(10, std::min(250, (cameraFps > 0 ? (1000 / cameraFps) / 2 : 33)));
//         const int mergeWaitMs = jvalue<int>(jAgg, "merge_wait_ms", defaultMergeWait);

//         json aggCfg = {
//             {"metrics_csv",  jvalue<std::string>(jAgg, "metrics_csv",  jvalue<std::string>(config_, "metrics_csv", "/tmp/realtime_metrics.csv"))},
//             {"metrics_json", jvalue<std::string>(jAgg, "metrics_json", jvalue<std::string>(config_, "metrics_json", "/tmp/realtime_metrics.ndjson"))},
//             {"retention_window_sec", jvalue<int>(jAgg, "retention_window_sec", 2000)},
//             {"merge_wait_ms",        mergeWaitMs},
//             {"flush_period_ms",      jvalue<int>(jAgg, "flush_period_ms", 1000)},
//             {"json_flush_threshold", jvalue<int>(jAgg, "json_flush_threshold", 2000)},
//             {"drop_empty_compat_rows", jvalue<bool>(jAgg, "drop_empty_compat_rows", true)},
//             {"drop_empty_flush_rows",  jvalue<bool>(jAgg, "drop_empty_flush_rows", true)},
//             {"expectsCamera", true},
//             {"expectsAlgorithm", true},
//             {"expectsDisplay", true},
//             {"expectsSoC", true},
//             {"expectsPower", jvalue<bool>(jLynsyn, "enabled", true)}
//         };

//         aggregator_ = std::make_shared<SystemMetricsAggregatorConcreteV3_2>(aggCfg);
//         spdlog::info("[ConfigManager] Aggregator initialized with metrics_csv = {}", 
//                     aggCfg.value("metrics_csv", std::string("N/A")));

//         // // 2. Aggregator
//         // const json jCamera = jobject_or_empty(config_, "CameraConfig");
//         // const json jAgg    = jobject_or_empty(config_, "Aggregator");
//         // const json jLynsyn = jobject_or_empty(config_, "LynsynMonitorConfig");

//         // const int cameraFps = jvalue<int>(jCamera, "fps", 30);
//         // const int defaultMergeWait = std::max(10, std::min(250, (cameraFps > 0 ? (1000 / cameraFps) / 2 : 33)));
//         // const int mergeWaitMs = jvalue<int>(jAgg, "merge_wait_ms", defaultMergeWait);

    
//         // json aggCfg = {
//         //     {"metrics_csv", jvalue<std::string>(config_, "metrics_csv", "/tmp/build/realtime_metrics_011.csv")},
//         //     {"metrics_json", jvalue<std::string>(config_, "metrics_json", "/tmp/build/realtime_metrics011.ndjson")},
//         //     {"retention_window_sec", jvalue<int>(config_, "retention_window_sec", 2000)},
//         //     {"expectsPower", jvalue<bool>(jLynsyn, "enabled", true)},
//         //     {"expectsCamera", true},
//         //     {"expectsAlgorithm", true},
//         //     {"expectsDisplay", true},
//         //     {"expectsSoC", true},
//         //     {"merge_wait_ms", mergeWaitMs},
//         //     {"flush_period_ms", jvalue<int>(jAgg, "flush_period_ms", 1000)},
//         //     {"json_flush_threshold", jvalue<int>(jAgg, "json_flush_threshold", 2000)},
//         //     {"drop_empty_compat_rows", jvalue<bool>(jAgg, "drop_empty_compat_rows", true)},
//         //     {"drop_empty_flush_rows", jvalue<bool>(jAgg, "drop_empty_flush_rows", true)}
//         // };

//         // aggregator_ = std::make_shared<SystemMetricsAggregatorConcreteV3_2>(aggCfg);
//         // spdlog::info("[ConfigManager] Aggregator initialized.");

//         // 3. Modules ? STRICT CONSUMER-FIRST ORDER
//         const json jDisp  = jobject_or_empty(config_, "DisplayConfig");
//         const json jAlg   = jobject_or_empty(config_, "AlgorithmConfig");
//         const json jSoC   = jobject_or_empty(config_, "SoCConfig");
//         const json jSched = jobject_or_empty(config_, "Scheduler");

//         // Display FIRST (Consumer)
//         modules_.push_back(ModuleFactory::create<DisplayModule>(jDisp, ctx_, cam2Disp_, alg2Disp_, aggregator_));
//         displayModule_ = static_cast<DisplayModule*>(modules_.back().get());
//         spdlog::info("[ConfigManager] [1/5] Display created (Consumer first)");

//         // Algorithm SECOND
//         modules_.push_back(ModuleFactory::create<AlgorithmModule>(jAlg, ctx_, cam2Alg_, alg2Disp_, *ctx_.tm, aggregator_, runtimeControls_));
//         algorithmModule_ = static_cast<AlgorithmModule*>(modules_.back().get());
//         spdlog::info("[ConfigManager] [2/5] Algorithm created");

//         // Camera THIRD (Producer)
//         modules_.push_back(ModuleFactory::create<CameraModule>(jCamera, ctx_, cam2Alg_, cam2Disp_, aggregator_));
//         cameraModule_ = static_cast<CameraModule*>(modules_.back().get());
//         spdlog::info("[ConfigManager] [3/5] Camera created (Producer last)");

//         // SoC & Lynsyn (metrics)
//         modules_.push_back(ModuleFactory::create<SoCModule>(jSoC, ctx_, aggregator_));
//         socModule_ = static_cast<SoCModule*>(modules_.back().get());
//         spdlog::info("[ConfigManager] [4/5] SoC created");

//         if (jvalue<bool>(jLynsyn, "enabled", true)) {
//             modules_.push_back(ModuleFactory::create<LynsynModule>(jLynsyn, ctx_, *ctx_.tm, aggregator_));
//             lynsynModule_ = static_cast<LynsynModule*>(modules_.back().get());
//             spdlog::info("[ConfigManager] [4/5] Lynsyn created");
//         }

//         // Brain LAST (Scheduler or ERL)
//         bool schedEnabled = jvalue<bool>(jSched, "enabled", config_.contains("Scheduler"));
//         if (schedEnabled) {
//             bool useERL = jvalue<bool>(jSched, "use_erl", false);
//             if (useERL) {
//                 spdlog::info("[ConfigManager] [5/5] Initializing Evolutionary Selector (ERL)...");
//                 //modules_.push_back(ModuleFactory::create<EvolutionarySelector>(jSched, ctx_, aggregator_, runtimeControls_));
//                 json jBrain = jSched;

//                 for (auto it = jEvo.begin(); it != jEvo.end(); ++it) {
//                     jBrain[it.key()] = it.value();
//                 }

//                 modules_.push_back(ModuleFactory::create<EvolutionarySelector>(jBrain, ctx_, aggregator_, runtimeControls_));

//                 evolutionarySelector_ = static_cast<EvolutionarySelector*>(modules_.back().get());
//             } else {
//                 spdlog::info("[ConfigManager] [5/5] Initializing Standard Scheduler...");
//                 //modules_.push_back(ModuleFactory::create<SchedulerModule>(jSched, ctx_, aggregator_, runtimeControls_));
//                 json jBrain = jSched;

//                 for (auto it = jEvo.begin(); it != jEvo.end(); ++it) {
//                     jBrain[it.key()] = it.value();
//                 }

//                 modules_.push_back(ModuleFactory::create<EvolutionarySelector>(jBrain, ctx_, aggregator_, runtimeControls_));
//                 schedulerModule_ = static_cast<SchedulerModule*>(modules_.back().get());
//             }
//         }

//         spdlog::info("[ConfigManager] Pipeline built: {} modules (Display ? Algorithm ? Camera order).", modules_.size());
//     }

//     void startAll() {
//         spdlog::info("[ConfigManager] Starting pipeline modules...");
//         try {
//             // 1. Aggregator first
//             if (aggregator_) {
//                 spdlog::info("[ConfigManager] Starting Aggregator...");
//                 aggregator_->start();
//             }

//             // 2. Start modules in creation order (Display ? Algorithm ? Camera ? Metrics ? Brain)
//             for (size_t i = 0; i < modules_.size(); ++i) {
//                 if (modules_[i]) {
//                     spdlog::info("[ConfigManager] Starting Module [{}/{}]", i+1, modules_.size());
//                     modules_[i]->start();
//                     std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Stabilize thread startup
//                 }
//             }

//             // 3. Signal pipeline is ready (releases EvolutionarySelector wait)
//             ctx_.pipelineReady.store(true, std::memory_order_release);
//             spdlog::info("[ConfigManager] Pipeline FULLY STARTED and READY.");
//             spdlog::default_logger()->flush();
//         }
//         catch (const std::exception& e) {
//             spdlog::critical("Startup failure: {}", e.what());
//             spdlog::default_logger()->flush();
//             flushMetrics();
//             throw;
//         }
//     }

//     bool validateAll() {
//         for (auto& m : modules_) {
//             if (m && !m->validate()) return false;
//         }
//         return true;
//     }

//     void stopAll() {
//         spdlog::info("[ConfigManager] Stopping pipeline modules...");

//         // Stop Display first.
//         // SDL/EGL rendering on Jetson is sensitive to shutdown order.
//         // Stopping display first avoids destroying queues/camera/algorithm before
//         // the renderer has fully exited.
//         IModule* displayPtr = displayModule_;

//         if (displayModule_) {
//             try {
//                 spdlog::info("[ConfigManager] Stopping Display first...");
//                 displayModule_->stop();
//             } catch (const std::exception& e) {
//                 spdlog::error("[ConfigManager] Display stop failed: {}", e.what());
//             }
//         }

//         // Stop the remaining modules in reverse order.
//         for (auto it = modules_.rbegin(); it != modules_.rend(); ++it) {
//             if (!(*it)) continue;

//             // Skip display because it was already stopped.
//             if (it->get() == displayPtr) continue;

//             try {
//                 (*it)->stop();
//             } catch (const std::exception& e) {
//                 spdlog::error("[ConfigManager] Module stop failed: {}", e.what());
//             }
//         }

//         spdlog::info("[ConfigManager] All modules stopped.");
//     }

//     bool runLoop(double runSeconds) {
//         auto startTime = std::chrono::steady_clock::now();
//         while (!ctx_.shutdown_flag.load()) {
//             auto now = std::chrono::steady_clock::now();
//             if (std::chrono::duration<double>(now - startTime).count() > runSeconds) {
//                 spdlog::info("[ConfigManager] Runtime limit reached ({} s).", runSeconds);
//                 break;
//             }
//             std::this_thread::sleep_for(std::chrono::milliseconds(50));
//         }
//         return true;
//     }

//     void flushMetrics() {
//         if (aggregator_) {
//             spdlog::info("[ConfigManager] Flushing metrics...");
//             aggregator_->stop();
//             aggregator_->forceFlushBatch();
//             const json jProf = jobject_or_empty(config_, "Profiling");
//             aggregator_->exportToCSV(jvalue<std::string>(jProf, "metricsOutputFile", "/tmp/PerformanceMetrics.csv"));
//             spdlog::info("[ConfigManager] Metrics exported successfully.");
//         }
//     }

//     const json& config() const { return config_; }

// private:
//     void sanitizeDefaults() {
//         if (!config_.is_object()) config_ = json::object();
//         // Add your existing sanitize logic here (width, height, fps, etc.)
//     }

//     void buildQueues_() {
//         cam2Alg_   = std::make_shared<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>>(200);
//         cam2Disp_  = std::make_shared<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>>(200);
//         alg2Disp_  = std::make_shared<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>>(300);
//         spdlog::info("[ConfigManager] Queues created: cam2Alg={}, cam2Disp={}, alg2Disp={}",
//                      cam2Alg_->capacity(), cam2Disp_->capacity(), alg2Disp_->capacity());
//     }

// // public:
// //     std::shared_ptr<hrl::RuntimeControls> getRuntimeControls() {
// //             return runtimeControls_;
// //         }

// //         hrl::RuntimeControls& getRuntimeControlsRef() {
// //             if (!runtimeControls_) throw std::runtime_error("RuntimeControls not initialized");
// //             return *runtimeControls_;
// //         }

// //         hrl::MetricsSnapshot getLatestMetricsSnapshot() const {
// //             if (aggregator_) {
// //             //return aggregator_ ? aggregator_->getLatestSnapshot() : hrl::MetricsSnapshot{};
// //             return aggregator_->getLatestSnapshot();   // Make sure this returns hrl::MetricsSnapshot
// //         }
// //         return hrl::MetricsSnapshot{};
// //         }

// //         hrl::RLAction getLastRLAction() const {
// //             if (evolutionarySelector_) {
// //                 //return evolutionarySelector_->getLastAction();
// //                 return evolutionarySelector_->getBestAction();   // Use existing method
// //             }
// //             return hrl::RLAction{};
// //         }
// public:
//     std::shared_ptr<hrl::RuntimeControls> getRuntimeControls() {
//         return runtimeControls_;
//     }

//     hrl::RuntimeControls& getRuntimeControlsRef() {
//         if (!runtimeControls_) {
//             throw std::runtime_error("RuntimeControls not initialized");
//         }
//         return *runtimeControls_;
//     }

//     // FIXED VERSION
//     // hrl::MetricsSnapshot getLatestMetricsSnapshot() const {
//     //     if (!aggregator_) {
//     //         return hrl::MetricsSnapshot{};
//     //     }

//     //     SystemMetricsSnapshot sys = aggregator_->getLatestSnapshot();

//     //     hrl::MetricsSnapshot snap{};
//     //     snap.fps                = sys.fps;
//     //     snap.avg_inference_ms   = sys.AlgoInferenceMs;      // adjust field name if needed
//     //     snap.avg_power_w_alg    = sys.PowerSensor0_Power;   // or whatever your field is
//     //     snap.cpu_util_avg       = (sys.SoC_CPU1_Util + sys.SoC_CPU2_Util + 
//     //                                sys.SoC_CPU3_Util + sys.SoC_CPU4_Util) / 4.0;

//     //     return snap;
//     // }

//     // Inside ConfigManager class (public section)

//     // hrl::MetricsSnapshot getLatestMetricsSnapshot() const {
//     //     if (!aggregator_) {
//     //         return hrl::MetricsSnapshot{};
//     //     }

//     //     SystemMetricsSnapshot sys = aggregator_->getLatestSnapshot();
//     //     hrl::MetricsSnapshot snap{};

//     //     snap.timestamp                = sys.timestamp;
//     //     snap.frameId                  = sys.frameId;

//     //     snap.fps                      = sys.algorithmStats.fps > 0 ? sys.algorithmStats.fps : sys.cameraStats.fps;
//     //     snap.avg_inference_ms         = sys.algorithmStats.inferenceTimeMs;
//     //     snap.avg_power_w_alg          = sys.powerStats.totalPower();
//     //     snap.cpu_util_avg             = sys.socInfo.CPU1_Utilization_Percent; // or average all cores
//     //     snap.gpu_util_avg             = sys.socInfo.GR3D_Frequency_Percent;   // approximate

//     //     snap.end_to_end_latency_ms    = sys.endToEndLatencyMs;
//     //     snap.processing_latency_ms    = sys.processingLatencyMs;
//     //     snap.display_latency_ms       = sys.displayLatencyMs;
//     //     snap.joules_per_frame         = sys.joulesPerFrame;

//     //     snap.cpu_temp_c               = sys.socInfo.CPU_Temperature_C;
//     //     snap.gpu_temp_c               = sys.socInfo.GPU_Temperature_C;

//     //     snap.computeAggregatedMetrics();

//     //     return snap;
//     // }

//         hrl::MetricsSnapshot getLatestMetricsSnapshot() const {
//         if (!aggregator_) {
//             return hrl::MetricsSnapshot{};
//         }

//         SystemMetricsSnapshot sys = aggregator_->getLatestSnapshot();
//         hrl::MetricsSnapshot snap{};

//         snap.timestamp = sys.timestamp;
//         snap.frameId   = sys.frameId;

//         // Core metrics
//         snap.fps = sys.algorithmStats.fps > 0 ? sys.algorithmStats.fps : sys.cameraStats.fps;
//         snap.avg_inference_ms = sys.algorithmStats.inferenceTimeMs;
//         snap.avg_power_w_alg  = sys.powerStats.totalPower();

//         // CPU Util - better average
//         snap.cpu_util_avg = (sys.socInfo.CPU1_Utilization_Percent +
//                              sys.socInfo.CPU2_Utilization_Percent +
//                              sys.socInfo.CPU3_Utilization_Percent +
//                              sys.socInfo.CPU4_Utilization_Percent) / 4.0;

//         snap.gpu_util_avg = sys.socInfo.GR3D_Frequency_Percent;

//         // Latencies
//         snap.end_to_end_latency_ms = sys.endToEndLatencyMs;
//         snap.processing_latency_ms = sys.processingLatencyMs;
//         snap.display_latency_ms    = sys.displayLatencyMs;
//         snap.joules_per_frame      = sys.joulesPerFrame;

//         // Temperatures
//         snap.cpu_temp_c = sys.socInfo.CPU_Temperature_C;
//         snap.gpu_temp_c = sys.socInfo.GPU_Temperature_C;

//         snap.computeAggregatedMetrics();

//         return snap;
//     }

//     hrl::RLAction getLastRLAction() const {
//         if (evolutionarySelector_) {
//             return evolutionarySelector_->getBestAction();
//         }
//         return hrl::RLAction{};
//     }


// private:
//     Context& ctx_;
//     json config_;
//     std::vector<std::unique_ptr<IModule>> modules_;
//     std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator_;
//     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> cam2Alg_;
//     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> cam2Disp_;
//     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> alg2Disp_;
//     std::shared_ptr<hrl::RuntimeControls> runtimeControls_;

//     // Named (non-owning) pointers ? modules_ vector owns the objects
//     SoCModule*           socModule_           = nullptr;
//     SchedulerModule*     schedulerModule_     = nullptr;
//     EvolutionarySelector* evolutionarySelector_ = nullptr;
//     LynsynModule*        lynsynModule_        = nullptr;
//     CameraModule*        cameraModule_        = nullptr;
//     AlgorithmModule*     algorithmModule_     = nullptr;
//     DisplayModule*       displayModule_       = nullptr;
// };

