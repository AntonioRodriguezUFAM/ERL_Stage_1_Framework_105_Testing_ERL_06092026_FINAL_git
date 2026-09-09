

// Modules.hpp

// ========================================================================================
// Modules.hpp  (all modules implementing IModule)
// ========================================================================================
#pragma once

//#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO

#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <map>
#include <functional>
#include <stdexcept>
#include <unistd.h>                // access()
#include <spdlog/spdlog.h>

// -- TOP OF FILE (after #includes) -------------------------------------
#include <experimental/filesystem>   // <-- ADD
namespace fs = std::experimental::filesystem;   // <-- ADD


#include "../nlohmann/json.hpp"


#include "IModule.h"
#include "ModuleFactory.h"

#include "RuntimeControls.h"
#include "Scheduler.h"
#include "WorkloadMission.h"

class ThreadManager;  // forward-declare


//========================================================================================
// CORE: Context & Base Module Interface
//========================================================================================
// Context with ThreadManager and a global shutdown flag.
struct Context {
    std::atomic<bool> shutdown_flag{false};
    // [FIX] Add this flag
    std::atomic<bool> pipelineReady{false};
    std::shared_ptr<ThreadManager> tm;
};



// ----------------------------- includes from your project ------------------------------
#include "../Stage_01/SharedStructures/SharedQueue.h"

#include "../Stage_01/SharedStructures/ZeroCopyFrameData.h"

#include "../Stage_01/SharedStructures/CameraConfig.h"
#include "../Stage_01/SharedStructures/DisplayConfig.h"
#include "../Stage_01/SharedStructures/AlgorithmConfig.h"
#include "../Stage_01/SharedStructures/LynsynMonitorConfig.h"
#include "../Stage_01/SharedStructures/allModulesStatcs.h"

#include "../Stage_01/Concretes/DataConcrete_new.h"
#include "../Stage_01/Concretes/AlgorithmConcrete_new.h"
#include "../Stage_01/Concretes/SdlDisplayConcrete_new.h"
#include "../Stage_01/Concretes/SoCConcrete_new.h"
#include "../Stage_01/Concretes/LynsynMonitorConcrete_new.h"
#include "../Stage_01/Concretes/SystemMetricsAggregatorConcrete_v3_2.h"
// ---------------------------------------------------------------------------------------

using json = nlohmann::json;

// ========================================================================================
// Small helpers (string -> enums) as used in your v11 main
// ========================================================================================
inline PixelFormat stringToPixelFormat(const std::string& format) {
    // AUTO/RG10 are accepted here for CSI configuration compatibility.
    // DataConcrete performs the real backend negotiation by querying the V4L2 device.
    // For CSI IMX219, DataConcrete detects RG10 and converts it to YUYV-like grayscale frames.
    if (format == "YUYV") return PixelFormat::YUYV;
    if (format == "MJPG") return PixelFormat::MJPG;
    if (format == "AUTO" || format == "RG10" || format == "CSI_RG10") return PixelFormat::YUYV;
    throw std::invalid_argument("Unsupported pixel format: " + format);
}

//hrl::stringToAlgorithmType(algoType)

// inline AlgorithmType stringToAlgorithmType(const std::string& type) {
//     static const std::map<std::string, AlgorithmType> kMap = {
//         {"Invert", AlgorithmType::Invert},
//         {"Grayscale", AlgorithmType::Grayscale},
//         {"EdgeDetection", AlgorithmType::EdgeDetection},
//         {"PasswordHash", AlgorithmType::PasswordHash},
//         {"MultiPipeline", AlgorithmType::MultiPipeline},
//         {"MultiThreadedInvert", AlgorithmType::MultiThreadedInvert},
//         {"GaussianBlur", AlgorithmType::GaussianBlur},
//         {"OpticalFlow_LucasKanade", AlgorithmType::OpticalFlow_LucasKanade},
//         {"MatrixMultiply", AlgorithmType::MatrixMultiply},
//         {"Mandelbrot", AlgorithmType::Mandelbrot},
//         {"GPUMatrixMultiply", AlgorithmType::GPUMatrixMultiply},
//         {"SobelEdge", AlgorithmType::SobelEdge},
//         {"MedianFilter", AlgorithmType::MedianFilter},
//         {"HistogramEqualization", AlgorithmType::HistogramEqualization},
//         {"HeterogeneousGaussianBlur", AlgorithmType::HeterogeneousGaussianBlur}
//     };
//     auto it = kMap.find(type);
//     if (it == kMap.end()) throw std::invalid_argument("Unsupported algorithm type: " + type);
//     return it->second;
// }

// ===== BEGIN: disable placeholder registry =====
#if 0 
class ModuleRegistry {
public:
    static void registerModule(const std::string& name, std::function<std::unique_ptr<IModule>(json, Context&)> factory) {
        registry_[name] = factory;
    }
    static std::unique_ptr<IModule> create(const std::string& name, json cfg, Context& ctx) {
        auto it = registry_.find(name);
        if (it == registry_.end()) throw std::runtime_error("Unknown module: " + name);
        return it->second(cfg, ctx);
    }
private:
    static std::map<std::string, std::function<std::unique_ptr<IModule>(json, Context&)>> registry_;
};


//=========================================================================================
static bool cameraRegistered = (ModuleRegistry::registerModule("Camera", [](json cfg, Context& ctx) {
    return ModuleFactory::create<CameraModule>(cfg, ctx, ...);
}), true);
//=========================================================================================

#endif
// ===== END: disable placeholder registry =====


#include "Scheduler.h"
#include "RuntimeControls.h"

class SchedulerModule : public IModule {
public:
    // SchedulerModule(const json& cfg, 
    //                 Context& ctx, 
    //                 std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator,
    //                 std::shared_ptr<hrl::RuntimeControls> controls) // [FIX 2025-12-20] Added controls
    //     : cfg_(cfg), ctx_(ctx), aggregator_(aggregator), controls_(controls),
    //      scheduler_(hrl::Scheduler::Limits{}
    //     ) 
    // {}

    /*
    Change SchedulerModule constructor and Scheduler usage
    What to do: Accept ThermalGovernor::Config in SchedulerModule and forward it into the Scheduler (which already has a constructor overload that accepts ThermalGovernor::Config).

    Patch (SchedulerModule constructor and member init):
    */

    SchedulerModule(const json& cfg, 
                    Context& ctx, 
                    std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator,
                    std::shared_ptr<hrl::RuntimeControls> controls,
                    const hrl::ThermalGovernor::Config& thermalCfg) // new param
        : cfg_(cfg), ctx_(ctx), aggregator_(aggregator), controls_(controls),
        scheduler_(hrl::Scheduler::Limits{}, thermalCfg) // forward thermalCfg 
    {}

    bool validate() override {
        // Validate scheduler-specific configs if needed
        return true;
    }

    void start() override {
        spdlog::info("[SchedulerModule] Starting adaptive control loop...");
        running_ = true;
        
        // Parse initial policy from config
        std::string modeStr = cfg_.value("policy", "BALANCED");
        if (modeStr == "MAX_PERFORMANCE") currentAction_.mode = hrl::PolicyMode::MAX_PERFORMANCE;
        else if (modeStr == "LOW_POWER")  currentAction_.mode = hrl::PolicyMode::LOW_POWER;
        else currentAction_.mode = hrl::PolicyMode::BALANCED;

        //currentAction_.target_fps = cfg_.value("targetFPS", 30.0);
        currentAction_.target_fps = hrl::MaybeDouble(cfg_.value("targetFPS", 30.0));  // [FIX 2025-12-20] Use MaybeDouble

        thread_ = std::thread([this]() {
            while (running_ && !ctx_.shutdown_flag) {
                step();
                std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // 1Hz Control Loop
            }
        });
    }

    void stop() override {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        spdlog::info("[SchedulerModule] Stopped.");
    }

private:
    void step() {
        if (!aggregator_) return;

        // 1. OBSERVE: Get metrics from the Aggregator
        // Note: You might need to add a public accessor to SystemMetricsAggregator to get the *latest* snapshot
        // For now, let's assume we can fetch the last set of mapped metrics.
        
        hrl::MetricsSnapshot snap;
        // Mocking the fetch - in real implementation, aggregator_->getLatest()
        // snap.fps = aggregator_->getLastFPS(); 
        // snap.cpu_util_avg = aggregator_->getLastCPU();
        // ... fill snap ...

        // 2. DECIDE & ACT: Apply HRL Scheduler
        scheduler_.apply(currentAction_, snap, *controls_);
    }

    json cfg_;
    Context& ctx_;
    std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator_;
    std::shared_ptr<hrl::RuntimeControls> controls_;
    hrl::Scheduler scheduler_;
    hrl::RLAction currentAction_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};


// ========================================================================================
// AggregatorModule
//  - Owns SystemMetricsAggregatorConcreteV3_2
//  - Provides accessor for other modules to share the same aggregator instance
// ========================================================================================
class AggregatorModule : public IModule {
public:
    explicit AggregatorModule(const json& cfg)
        : cfg_(cfg) {}

    bool validate() override {
        // Minimal sanity checks
        if (!cfg_.contains("metrics_csv") || !cfg_["metrics_csv"].is_string())
            cfg_["metrics_csv"] = "/tmp/build/realtime_metrics.csv";
        if (!cfg_.contains("metrics_json") || !cfg_["metrics_json"].is_string())
            cfg_["metrics_json"] = "/tmp/build/realtime_metrics.ndjson";
        if (!cfg_.contains("retention_window_sec") || !cfg_["retention_window_sec"].is_number_integer())
            cfg_["retention_window_sec"] = 2000;
        if (!cfg_.contains("merge_wait_ms") || !cfg_["merge_wait_ms"].is_number_integer())
            cfg_["merge_wait_ms"] = 50;
        if (!cfg_.contains("flush_period_ms") || !cfg_["flush_period_ms"].is_number_integer())
            cfg_["flush_period_ms"] = 1000;
        if (!cfg_.contains("json_flush_threshold") || !cfg_["json_flush_threshold"].is_number_integer())
            cfg_["json_flush_threshold"] = 2000;

        return true;
    }

    void start() override {
        spdlog::info("[AggregatorModule] Starting...");
        aggregator_ = std::make_shared<SystemMetricsAggregatorConcreteV3_2>(cfg_);
        spdlog::info("[AggregatorModule] Ready.");
    }

    void stop() override {
        spdlog::info("[AggregatorModule] Stopping...");
        if (aggregator_) {
            aggregator_->stop();
            aggregator_->forceFlushBatch();
        }
        aggregator_.reset();
        spdlog::info("[AggregatorModule] Stopped.");
    }

    std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> get() const { return aggregator_; }

private:
    json cfg_;
    std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator_;
};

// ========================================================================================
// CameraModule (wraps DataConcrete)
// ========================================================================================
class CameraModule : public IModule {
public:
    CameraModule(const json& cfg,
                 Context& ctx,
                 std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> cam2Alg,
                 std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> cam2Disp,
                 std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator)
        : cfg_(cfg), ctx_(ctx),
          cam2Alg_(std::move(cam2Alg)), cam2Disp_(std::move(cam2Disp)),
          aggregator_(std::move(aggregator)) {}

    bool validate() override {
        spdlog::info("[CameraModule] Validating configuration...");

        if (!cfg_.contains("device") || !cfg_["device"].is_string())
            cfg_["device"] = "/dev/video0";   // CSI IMX219 default on Jetson Nano after camera installation.
        if (!cfg_.contains("width")  || !cfg_["width"].is_number_integer())  cfg_["width"]  = 320;
        if (!cfg_.contains("height") || !cfg_["height"].is_number_integer()) cfg_["height"] = 240;
        if (!cfg_.contains("fps")    || !cfg_["fps"].is_number_integer())    cfg_["fps"]    = 60;
        if (!cfg_.contains("pixelFormat") || !cfg_["pixelFormat"].is_string())
            cfg_["pixelFormat"] = "AUTO";
        if (!cfg_.contains("numBuffers") || !cfg_["numBuffers"].is_number_integer())
            cfg_["numBuffers"] = 8;

        int fps = cfg_.value("fps", 60);
        fps = std::max(1, std::min(fps, 120));
        cfg_["fps"] = fps;

        int numBuffers = cfg_.value("numBuffers", 8);
        numBuffers = std::max(2, std::min(numBuffers, 8));
        cfg_["numBuffers"] = numBuffers;

        const std::string devicePath = cfg_.value("device", std::string("/dev/video0"));
        if (access(devicePath.c_str(), F_OK) != 0) {
            spdlog::error("[CameraModule] Camera device not found: {}", devicePath);
            return false;
        }

        spdlog::info(
            "[CameraModule] Camera validated | device={} output={}x{} fps={} pixelFormat={} numBuffers={}",
            devicePath,
            cfg_.value("width", 320),
            cfg_.value("height", 240),
            cfg_.value("fps", 60),
            cfg_.value("pixelFormat", std::string("AUTO")),
            cfg_.value("numBuffers", 8)
        );

        return true;
    }

//================================================================
//================= CANERAMODULE START ===========================
// Changes:

// Added camCfg to pass the CameraConfig object.
// Added ctx_.tm to provide the ThreadManager instance (assuming Context holds a valid tm).
// Reordered arguments to match the constructor signature.
//================================================================
    void start() override {
        spdlog::info("[CameraModule] Starting...");

        const std::string devicePath = cfg_.value("device", std::string("/dev/video0"));
        const std::string pixelFormat = cfg_.value("pixelFormat", std::string("AUTO"));

        CameraConfig camCfg{
            cfg_.value("width", 320),
            cfg_.value("height", 240),
            cfg_.value("fps", 60),
            stringToPixelFormat(pixelFormat),
            cfg_.value("numBuffers", 8)
        };


        // Add these lines in CameraModule::start() after constructing CameraConfig camCfg
        // and before std::make_unique<DataConcrete>(...).

        const std::string rawPixelFormat = cfg_.value("rawCapturePixelFormat", std::string("RG10"));
        camCfg.device = devicePath;
        camCfg.rawCaptureWidth = cfg_.value("rawCaptureWidth", 1280);
        camCfg.rawCaptureHeight = cfg_.value("rawCaptureHeight", 720);
        camCfg.rawCaptureFps = cfg_.value("rawCaptureFps", cfg_.value("fps", 60));
        camCfg.rawCapturePixelFormat = stringToPixelFormat(rawPixelFormat);
        camCfg.sensorMode = cfg_.value("sensorMode", 4);
        camCfg.bypassMode = cfg_.value("bypassMode", 0);
        camCfg.lowLatencyMode = cfg_.value("lowLatencyMode", false);
        camCfg.forceSensorMode = cfg_.value("forceSensorMode", true);
        camCfg.strictCsiMode = cfg_.value("strictCsiMode", true);

        // New black-screen/luma diagnostic controls.
        camCfg.rawBitAlignment = cfg_.value("rawBitAlignment", std::string("AUTO"));
        camCfg.autoLumaStretch = cfg_.value("autoLumaStretch", true);
        camCfg.debugLumaMetrics = cfg_.value("debugLumaMetrics", true);
        camCfg.lumaBlackLevel = cfg_.value("lumaBlackLevel", -1);
        camCfg.lumaWhiteLevel = cfg_.value("lumaWhiteLevel", -1);

        spdlog::info(
            "[CameraModule] CSI/raw config: raw={}x{} {} rawFPS={} sensorMode={} bypassMode={} alignment={} autoLumaStretch={} debugLumaMetrics={} output={}x{}",
            camCfg.rawCaptureWidth,
            camCfg.rawCaptureHeight,
            pixelFormatToString(camCfg.rawCapturePixelFormat),
            camCfg.rawCaptureFps,
            camCfg.sensorMode,
            camCfg.bypassMode,
            camCfg.rawBitAlignment,
            camCfg.autoLumaStretch ? "ON" : "OFF",
            camCfg.debugLumaMetrics ? "ON" : "OFF",
            camCfg.width,
            camCfg.height
        );


        camera_ = std::make_unique<DataConcrete>(camCfg, ctx_.tm, cam2Alg_, cam2Disp_, aggregator_);

        spdlog::info(
            "[CameraModule] Opening camera device={} requestedOutput={}x{} requestedFPS={} pixelFormat={} numBuffers={}",
            devicePath, camCfg.width, camCfg.height, camCfg.fps, pixelFormat, camCfg.numBuffers
        );

        if (!camera_->openDevice(devicePath) || !camera_->configure(camCfg)) {
            throw std::runtime_error("[CameraModule] Failed to initialize camera: " + devicePath);
        }

        camera_->startCapture();
        spdlog::info("[CameraModule] Streaming started.");
    }


//================================================================
//================= CANERAMODULE END =============================
//================================================================
    void stop() override {
        spdlog::info("[CameraModule] Stopping...");
        if (camera_) {
            camera_->stopCapture();

            camera_.reset();
        }
        spdlog::info("[CameraModule] Stopped.");
    }

private:
    json cfg_;
    Context& ctx_;
    std::unique_ptr<DataConcrete> camera_;
    std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> cam2Alg_, cam2Disp_;
    std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator_;
};

// ========================================================================================
// AlgorithmModule (wraps AlgorithmConcrete)
// ========================================================================================
class AlgorithmModule : public IModule {
public:
    AlgorithmModule(const json& cfg,
                    Context& ctx,
                    std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> cam2Alg,
                    std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> alg2Disp,
                    ThreadManager& tm,
                    std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator,
                    std::shared_ptr<hrl::RuntimeControls> controls) // <--- Added this
        :   cfg_(cfg), 
            ctx_(ctx), 
            tm_(tm),
            cam2Alg_(std::move(cam2Alg)), 
            alg2Disp_(std::move(alg2Disp)),
            aggregator_(std::move(aggregator)),
            controls_(std::move(controls)),
            gpuPermission_(cfg.value("useGPU", false)) {}

//===========================================================================================================
bool validate() override {
    spdlog::info("[AlgorithmModule] Validating configuration...");

    if (!cfg_.contains("algorithmType") || !cfg_["algorithmType"].is_string())
        cfg_["algorithmType"] = "Invert";

    if (!cfg_.contains("concurrencyLevel") || !cfg_["concurrencyLevel"].is_number_integer())
        cfg_["concurrencyLevel"] = 4;

    if (!cfg_.contains("blurRadius") || !cfg_["blurRadius"].is_number_integer())
        cfg_["blurRadius"] = 5;

    if (!cfg_.contains("medianWindowSize") || !cfg_["medianWindowSize"].is_number_integer())
        cfg_["medianWindowSize"] = 5;

    if (!cfg_.contains("matrixSize") || !cfg_["matrixSize"].is_number_integer())
        cfg_["matrixSize"] = 512;

    if (!cfg_.contains("mandelbrotIter") || !cfg_["mandelbrotIter"].is_number_integer())
        cfg_["mandelbrotIter"] = 100;

    if (!cfg_.contains("useGPU") || !cfg_["useGPU"].is_boolean())
        cfg_["useGPU"] = false;

    try {
        const std::string algoType =
            cfg_.value("algorithmType", std::string("Invert"));

        (void)hrl::stringToAlgorithmType(algoType);

        spdlog::info("[AlgorithmModule] Validation successful: algorithmType={}", algoType);
        return true;

    } catch (const std::exception& e) {
        spdlog::error("[AlgorithmModule] Validation failed: {}", e.what());
        return false;
    }
}

//===============================================================================================================================
// Start the initial algorithm worker.  AlgorithmModule itself is persistent for
// the entire pipeline lifetime; only its internal AlgorithmConcrete is replaced
// during a workload mission transition.
void start() override {
    std::lock_guard<std::mutex> lock(workloadMutex_);
    const std::string algoType = cfg_.value("algorithmType", std::string("Invert"));

    spdlog::info("[AlgorithmModule] Starting persistent algorithm slot -> {}", algoType);
    algo_ = createConfiguredAlgorithm_(algoType, gpuPermission_);
    algo_->startAlgorithm();

    spdlog::info(
        "[AlgorithmModule] Started -> {} | configured GPU={} | concurrency={}",
        algoType,
        cfg_.value("useGPU", false) ? "ON" : "OFF",
        cfg_.value("concurrencyLevel", 4));
}

//===============================================================================================================================
// Hot-swap ONLY the algorithm workload.
//
// Transaction semantics:
//   * Parse and configure the replacement BEFORE stopping the current worker.
//   * Camera, queues, display, aggregator, Lynsyn, SoC telemetry, RuntimeControls
//     and ERL controller remain alive and retain object identity.
//   * cfg_["algorithmType"] is committed only after the new worker is running.
//
// This is the key pipeline seam used by the continuous dynamic-adaptation
// experiment.  A failed configure leaves the previous algorithm running.
bool switchWorkloadImpl_(const std::string& newAlgorithm, bool workloadGpuCapable)
{
    std::lock_guard<std::mutex> lock(workloadMutex_);

    const std::string oldAlgorithm =
        cfg_.value("algorithmType", std::string("UNKNOWN"));

    if (oldAlgorithm == newAlgorithm) {
        lastSwitchDowntimeMs_ = 0.0;
        lastSwitchInputDrainedFrames_ = 0U;
        lastSwitchOutputDrainedFrames_ = 0U;
        spdlog::info("[WORKLOAD] {} already active; no algorithm restart required",
                     newAlgorithm);
        return true;
    }

    // A stopped pipeline queue is an invariant violation. Never hide a global
    // shutdown/fatal producer state by automatically calling restart().
    if (!cam2Alg_ || !alg2Disp_ || cam2Alg_->isStopped() || alg2Disp_->isStopped()) {
        spdlog::critical(
            "[WORKLOAD] INVARIANT VIOLATION before swap {} -> {}: "
            "shared queue missing/stopped (cam2Alg_stopped={} alg2Disp_stopped={})",
            oldAlgorithm, newAlgorithm,
            cam2Alg_ ? cam2Alg_->isStopped() : true,
            alg2Disp_ ? alg2Disp_->isStopped() : true);
        return false;
    }

    spdlog::info("[WORKLOAD] Preparing hot swap {} -> {}", oldAlgorithm, newAlgorithm);

    // Configure first. A configuration failure must leave the current worker
    // completely untouched.
    std::unique_ptr<AlgorithmConcrete> replacement;
    try {
        replacement = createConfiguredAlgorithm_(
            newAlgorithm, gpuPermission_ && workloadGpuCapable);
    } catch (const std::exception& e) {
        spdlog::error(
            "[WORKLOAD] Replacement configure failed for {}: {}. Keeping {} active.",
            newAlgorithm, e.what(), oldAlgorithm);
        return false;
    }

    const uint64_t previousActiveEpoch = controls_
        ? controls_->active_workload_epoch.load(std::memory_order_acquire)
        : 0ULL;
    const uint64_t previousRequestedEpoch = controls_
        ? controls_->requested_workload_epoch.load(std::memory_order_acquire)
        : previousActiveEpoch;
    const uint64_t targetEpoch = controls_
        ? controls_->requested_workload_epoch.load(std::memory_order_acquire)
        : previousActiveEpoch;

    const auto switchStart = std::chrono::steady_clock::now();

    // Stop ONLY the old AlgorithmConcrete worker. Shared queues remain owned by
    // the persistent pipeline. pop_for() guarantees bounded quiesce latency.
    if (algo_) {
        algo_->stopAlgorithm(AlgorithmConcrete::StopMode::HotSwap);
    }

    // Verify the ownership invariant again after the old worker is fully joined.
    // If this ever trips, some legacy code still stopped a shared queue and the
    // experimental replicate must fail closed.
    if (cam2Alg_->isStopped() || alg2Disp_->isStopped()) {
        spdlog::critical(
            "[WORKLOAD] INVARIANT VIOLATION after old worker quiesce: "
            "shared queue was stopped during hot swap (cam2Alg={} alg2Disp={})",
            cam2Alg_->isStopped(), alg2Disp_->isStopped());
        return false;
    }

    // Clean phase boundary. The old worker is joined, so no more previous-
    // workload algorithm output can appear after these drains.
    lastSwitchInputDrainedFrames_ = cam2Alg_->drain();
    lastSwitchOutputDrainedFrames_ = alg2Disp_->drain();

    // Provenance protocol:
    //  1) clear processed frame;
    //  2) keep processed epoch on the OLD epoch (meaning "new workload has not
    //     proved a frame yet");
    //  3) activate target epoch;
    //  4) start replacement.
    // The replacement publishes {frame, epoch} only after real processing.
    if (controls_) {
        controls_->algorithm_processed_frame_id.store(
            0ULL, std::memory_order_relaxed);
        controls_->algorithm_processed_workload_epoch.store(
            previousActiveEpoch, std::memory_order_release);
        controls_->active_workload_epoch.store(
            targetEpoch, std::memory_order_release);
    }

    try {
        replacement->startAlgorithm();
    } catch (const std::exception& e) {
        spdlog::critical(
            "[WORKLOAD] Failed to start replacement {} after stopping {}: {}",
            newAlgorithm, oldAlgorithm, e.what());

        // Best-effort rollback to the previous controller/workload epoch. Queues
        // remain alive and have already been drained, so the old worker can safely
        // resume from fresh camera frames.
        if (controls_) {
            controls_->requested_workload_epoch.store(
                previousRequestedEpoch, std::memory_order_release);
            controls_->active_workload_epoch.store(
                previousActiveEpoch, std::memory_order_release);
            controls_->algorithm_processed_frame_id.store(
                0ULL, std::memory_order_relaxed);
            controls_->algorithm_processed_workload_epoch.store(
                previousActiveEpoch, std::memory_order_release);
        }

        if (algo_) {
            try {
                algo_->startAlgorithm();
                spdlog::warn(
                    "[WORKLOAD] Rolled back to {} after replacement start failure",
                    oldAlgorithm);
            } catch (...) {
                spdlog::critical(
                    "[WORKLOAD] Rollback restart also failed for {}", oldAlgorithm);
            }
        }
        return false;
    }

    algo_ = std::move(replacement);
    cfg_["algorithmType"] = newAlgorithm;
    cfg_["useGPU"] = (gpuPermission_ && workloadGpuCapable);

    const double switchMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - switchStart).count();
    lastSwitchDowntimeMs_ = switchMs;

    spdlog::info(
        "[WORKLOAD] COMMIT {} -> {} | algorithm-only downtime={:.3f} ms | "
        "drained input={} output={} | shared queues preserved",
        oldAlgorithm, newAlgorithm, switchMs,
        lastSwitchInputDrainedFrames_, lastSwitchOutputDrainedFrames_);
    return true;
}

public:
    // Preferred runtime API: capability travels with the mission profile.
    bool switchWorkload(const hrl::WorkloadProfile& target) {
        return switchWorkloadImpl_(target.label, target.gpuCapable);
    }

    // Backward-compatible API for existing callers. Unknown capability is
    // treated as GPU-capable; the original AlgorithmConfig permission still
    // gates actual CUDA use.
    bool switchWorkload(const std::string& newAlgorithm) {
        return switchWorkloadImpl_(newAlgorithm, true);
    }

std::string currentWorkload() const
{
    std::lock_guard<std::mutex> lock(workloadMutex_);
    return cfg_.value("algorithmType", std::string("UNKNOWN"));
}

double lastSwitchDowntimeMs() const
{
    std::lock_guard<std::mutex> lock(workloadMutex_);
    return lastSwitchDowntimeMs_;
}


size_t lastSwitchInputDrainedFrames() const
{
    std::lock_guard<std::mutex> lock(workloadMutex_);
    return lastSwitchInputDrainedFrames_;
}

size_t lastSwitchOutputDrainedFrames() const
{
    std::lock_guard<std::mutex> lock(workloadMutex_);
    return lastSwitchOutputDrainedFrames_;
}

bool algorithmStarved() const
{
    std::lock_guard<std::mutex> lock(workloadMutex_);
    return algo_ && algo_->isStarved();
}

//===============================================================================================================================
//===============================================================================================================================
    void stop() override {
        std::lock_guard<std::mutex> lock(workloadMutex_);
        spdlog::info("[AlgorithmModule] Stopping persistent algorithm slot...");
        if (algo_) {
            algo_->stopAlgorithm();
            algo_.reset();
        }
        spdlog::info("[AlgorithmModule] Stopped.");
    }

private:
    // Build/configure an AlgorithmConcrete without starting it. Keeping this
    // logic in one function guarantees startup and runtime hot-swap use exactly
    // the same AlgorithmConfig semantics.
    std::unique_ptr<AlgorithmConcrete> createConfiguredAlgorithm_(const std::string& algorithmName, bool configuredUseGpu)
    {
        const hrl::AlgorithmType parsedAlgoType =
            hrl::stringToAlgorithmType(algorithmName);

        AlgorithmConfig algCfg{
            cfg_.value("concurrencyLevel", 4),
            parsedAlgoType,
            cfg_.value("modelPath", std::string("")),
            cfg_.value("matrixSize", 512),
            cfg_.value("mandelbrotIter", 100),
            cfg_.value("blurRadius", 5),
            cfg_.value("medianWindowSize", 5),
            // This is workload capability/configuration, not the current ERL
            // decision. RuntimeControls::enable_gpu remains authoritative at runtime.
            configuredUseGpu,
            OpticalFlowConfig{}
        };

        std::unique_ptr<AlgorithmConcrete> candidate(
            new AlgorithmConcrete(cam2Alg_, alg2Disp_, tm_, aggregator_, controls_));

        if (!candidate->configure(algCfg)) {
            throw std::runtime_error(
                "[AlgorithmModule] AlgorithmConcrete configure() failed for " + algorithmName);
        }
        return candidate;
    }

    json cfg_;
    Context& ctx_;
    ThreadManager& tm_;
    std::unique_ptr<AlgorithmConcrete> algo_;
    std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> cam2Alg_, alg2Disp_;
    std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator_;
    std::shared_ptr<hrl::RuntimeControls> controls_; // Stored here
    bool gpuPermission_; // Original config permission; survives workload changes.
    double lastSwitchDowntimeMs_ = 0.0;
    size_t lastSwitchInputDrainedFrames_ = 0U;
    size_t lastSwitchOutputDrainedFrames_ = 0U;
    mutable std::mutex workloadMutex_;
};

// ========================================================================================
// DisplayModule (wraps SdlDisplayConcrete)
//  - Manages a small internal thread that repeatedly calls renderAndPollEvents()
// ========================================================================================
//==============================================================================================================
// DisplayModule (FIXED - Uses ThreadManager for render thread)
//==============================================================================================================

class DisplayModule : public IModule {
public:
    DisplayModule(const json& cfg,
                  Context& ctx,
                  std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> cam2Disp,
                  std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> alg2Disp,
                  std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator)
        : cfg_(cfg), ctx_(ctx),
          cam2Disp_(std::move(cam2Disp)), alg2Disp_(std::move(alg2Disp)),
          aggregator_(std::move(aggregator)) {}

    bool validate() override {
        spdlog::info("[DisplayModule] Validating configuration...");
        if (!cfg_.contains("width")  || !cfg_["width"].is_number_integer())  cfg_["width"]  = 320;
        if (!cfg_.contains("height") || !cfg_["height"].is_number_integer()) cfg_["height"] = 240;
        if (!cfg_.contains("fullscreen") || !cfg_["fullscreen"].is_boolean()) cfg_["fullscreen"] = false;
        if (!cfg_.contains("vsync")      || !cfg_["vsync"].is_boolean())      cfg_["vsync"]      = true;
        if (!cfg_.contains("targetFPS")  || !cfg_["targetFPS"].is_number_integer()) cfg_["targetFPS"] = 60;
        return true;
    }

    void start() override {
        spdlog::info("[DisplayModule] Starting...");
        
        display_ = std::make_unique<SdlDisplayConcrete>(cam2Disp_, alg2Disp_, aggregator_);

        const int w = cfg_.value("width", 320);
        const int h = cfg_.value("height", 240);
        
        if (!display_->initializeDisplay(w, h)) {
            throw std::runtime_error("[DisplayModule] Failed to initialize display");
        }

        running_.store(true);
        
        // ? ADD RENDER THREAD VIA THREADMANAGER
        if (ctx_.tm) {
            ctx_.tm->addThread(
                Component::Custom,
                std::thread(&DisplayModule::renderLoopThread, this)
            );
            spdlog::info("[DisplayModule] Render thread added to ThreadManager");
        } else {
            spdlog::warn("[DisplayModule] No ThreadManager available; render thread not started");
            throw std::runtime_error("[DisplayModule] Context has no ThreadManager");
        }
    }

    void stop() override {
        spdlog::info("[DisplayModule] Stopping...");
        
        running_.store(false);
        
        // ? STOP RENDER THREAD VIA THREADMANAGER
        if (ctx_.tm) {
            ctx_.tm->joinThreadsFor(Component::Custom);
            spdlog::info("[DisplayModule] Render thread stopped via ThreadManager");
        }
        
        if (display_) {
            display_->closeDisplay();
            display_.reset();
        }
        
        spdlog::info("[DisplayModule] Stopped.");
    }

private:
    json cfg_;
    Context& ctx_;

    std::unique_ptr<SdlDisplayConcrete> display_;
    std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> cam2Disp_, alg2Disp_;
    std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator_;

    std::atomic<bool> running_{false};

    // ? RENDER LOOP THREAD METHOD (called via ThreadManager)
    void renderLoopThread() {
        spdlog::info("[DisplayModule] Render loop thread STARTED");
        
        int frameCount = 0;
        auto windowStart = std::chrono::steady_clock::now();
        const int targetFPS = cfg_.value("targetFPS", 60);
        const double targetFrameTimeMs = 1000.0 / targetFPS;
        
        while (running_.load() && display_ && display_->is_Running() && !ctx_.shutdown_flag.load()) {
            auto frame_start = std::chrono::high_resolution_clock::now();
            
            // ? RENDER ONE FRAME
            display_->renderAndPollEvents();
            
            auto frame_elapsed_ms = std::chrono::duration<double, std::milli>(
                std::chrono::high_resolution_clock::now() - frame_start).count();
            
            frameCount++;
            
            // ? METRICS EVERY 1 SECOND
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - windowStart).count();
            if (elapsed >= 1.0) {
                double fps = frameCount / elapsed;
                spdlog::info("[DisplayModule] RENDER METRICS [1s] | Frames: {} | FPS: {:.1f}",
                            frameCount, fps);
                
                windowStart = now;
                frameCount = 0;
            }
            
            // ? FRAME RATE CAP (if vsync disabled)
            if (!cfg_.value("vsync", true)) {
                if (frame_elapsed_ms < targetFrameTimeMs) {
                    std::this_thread::sleep_for(
                        std::chrono::duration<double, std::milli>(targetFrameTimeMs - frame_elapsed_ms)
                    );
                }
            }
        }
        
        spdlog::info("[DisplayModule] Render loop thread EXITED");
    }
};


// //=================================================================================
// class DisplayModule : public IModule {
// public:
//     DisplayModule(const json& cfg,
//                   Context& ctx,
//                   std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> cam2Disp,
//                   std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> alg2Disp,
//                   std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator)
//         : cfg_(cfg), ctx_(ctx),
//           cam2Disp_(std::move(cam2Disp)), alg2Disp_(std::move(alg2Disp)),
//           aggregator_(std::move(aggregator)) {}

//     bool validate() override {
//         spdlog::info("[DisplayModule] Validating configuration...");
//         if (!cfg_.contains("width")  || !cfg_["width"].is_number_integer())  cfg_["width"]  = 640;
//         if (!cfg_.contains("height") || !cfg_["height"].is_number_integer()) cfg_["height"] = 480;
//         if (!cfg_.contains("fullscreen") || !cfg_["fullscreen"].is_boolean()) cfg_["fullscreen"] = false;
//         if (!cfg_.contains("vsync")      || !cfg_["vsync"].is_boolean())      cfg_["vsync"]      = true;
//         return true;
//     }

//     void start() override {
//         spdlog::info("[DisplayModule] Starting...");
//         display_ = std::make_unique<SdlDisplayConcrete>(cam2Disp_, alg2Disp_, aggregator_);

//         const int w = cfg_.value("width", 640);
//         const int h = cfg_.value("height", 480);
//         if (!display_->initializeDisplay(w, h)) {
//             throw std::runtime_error("[DisplayModule] Failed to initialize display");
//         }

//         running_.store(true);
//         loopThread_ = std::thread([this]() {
//             spdlog::info("[DisplayModule] Render loop started.");
//             // while (running_.load() && display_ && display_->is_Running() && !ctx_.shutdown_flag.load()) {
//             //     display_->renderAndPollEvents();
//             //     std::this_thread::sleep_for(std::chrono::milliseconds(1));
//             // }
//             while (running_.load() && display_ && display_->is_Running() && !ctx_.shutdown_flag.load()) {
//                 const auto frame_start = std::chrono::steady_clock::now();
//                 display_->renderAndPollEvents();

//                 // Target 60 FPS if vsync is off
//                 if (!cfg_.value("vsync", true)) {
//                     const auto target = std::chrono::milliseconds(16); // ~60 Hz
//                     const auto elapsed = std::chrono::steady_clock::now() - frame_start;
//                     if (elapsed < target)
//                         std::this_thread::sleep_for(target - elapsed);
//                 }
//             }


//             spdlog::info("[DisplayModule] Render loop finished.");
//         });
//     }

//     void stop() override {
//         spdlog::info("[DisplayModule] Stopping...");
//         running_.store(false);
//         if (loopThread_.joinable()) loopThread_.join();
//         if (display_) {
//             display_->closeDisplay();
//             display_.reset();
//         }
//         spdlog::info("[DisplayModule] Stopped.");
//     }

// private:
//     json cfg_;
//     Context& ctx_;

//     std::unique_ptr<SdlDisplayConcrete> display_;
//     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> cam2Disp_, alg2Disp_;
//     std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator_;

//     std::atomic<bool> running_{false};
//     std::thread loopThread_;
// };

// ========================================================================================
// SoCModule (wraps SoCConcrete)
//  - Your SoCConcrete_new in v11 used initializeSoC(); monitoring ran internally
// ========================================================================================
class SoCModule : public IModule {
public:
    SoCModule(const json& /*cfg*/,
              Context& /*ctx*/,
              std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator)
        : aggregator_(std::move(aggregator)) {}

    bool validate() override {
        // Nothing special here (you can add cfg validation if needed)
        return true;
    }

    void start() override {
        spdlog::info("[SoCModule] Starting...");
        soc_ = std::make_unique<SoCConcrete>(aggregator_);
        if (!soc_->initializeSoC()) {
            throw std::runtime_error("[SoCModule] initializeSoC() failed");
        }
        spdlog::info("[SoCModule] Started.");
    }

    void stop() override {
        spdlog::info("[SoCModule] Stopping...");
        soc_.reset();
        spdlog::info("[SoCModule] Stopped.");
    }

private:
    std::unique_ptr<SoCConcrete> soc_;
    std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator_;
};

// ========================================================================================
// LynsynModule (wraps LynsynMonitorConcrete)
// ========================================================================================

// ========================================================================================
// LynsynModule (wraps LynsynMonitorConcrete)
// ========================================================================================
class LynsynModule : public IModule {
public:
    LynsynModule(const json& cfg,
                 Context& ctx,
                 ThreadManager& tm,
                 std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator)
        : cfg_(cfg), ctx_(ctx), tm_(tm), aggregator_(std::move(aggregator)) {}

//====================================================================================================================

// REPLACE THE ENTIRE validate() WITH THIS:
bool validate() override {
    spdlog::info("[LynsynModule] Validating configuration...");

    // [MOD] Sanitize outputCSV early to prevent segfault in std::filesystem::path
    std::string raw_output = cfg_.value("outputCSV", "/tmp/lynsyn_output.csv");
    std::string sanitized_output;

    auto sanitize_path = [](const std::string& in) -> std::string {
        if (in.empty()) {
            spdlog::warn("[LynsynModule] outputCSV is empty, using default: /tmp/lynsyn_output.csv");
            return "/tmp/lynsyn_output.csv";
        }
        // [MOD] Reject control characters to prevent invalid path crashes
        for (char c : in) {
            if (static_cast<unsigned char>(c) < 32) {
                spdlog::warn("[LynsynModule] outputCSV '{}' contains control characters, using default", in);
                return "/tmp/lynsyn_output.csv";
            }
        }
        try {
            fs::path p(in);
            // [MOD] Convert relative to absolute paths
            if (p.is_relative()) {
                std::string abs_path = fs::absolute(p).string();
                spdlog::debug("[LynsynModule] Converted relative path '{}' to absolute: '{}'", in, abs_path);
                return abs_path;
            }
            return in;
        } catch (const fs::filesystem_error& e) {
            spdlog::warn("[LynsynModule] Invalid outputCSV '{}': {}. Using default", in, e.what());
            return "/tmp/lynsyn_output.csv";
        } catch (...) {
            spdlog::warn("[LynsynModule] Unexpected error processing outputCSV '{}'. Using default", in);
            return "/tmp/lynsyn_output.csv";
        }
    };

    sanitized_output = sanitize_path(raw_output);
    cfg_["outputCSV"] = sanitized_output; // [MOD] Update config with sanitized path

    // [MOD] Test file writability
    try {
        std::ofstream test(sanitized_output, std::ios::out | std::ios::app);
        if (!test) {
            spdlog::error("[LynsynModule] Cannot write to '{}', falling back to /tmp/lynsyn_output.csv", sanitized_output);
            sanitized_output = "/tmp/lynsyn_output.csv";
            cfg_["outputCSV"] = sanitized_output;
        }
    } catch (const std::exception& e) {
        spdlog::warn("[LynsynModule] Failed to test writability for '{}': {}. Using default", sanitized_output, e.what());
        sanitized_output = "/tmp/lynsyn_output.csv";
        cfg_["outputCSV"] = sanitized_output;
    }

    // [MOD] Fill other configuration defaults
    if (!cfg_.contains("enabled") || !cfg_["enabled"].is_boolean()) cfg_["enabled"] = true;
    if (!cfg_.contains("coreMask") || !cfg_["coreMask"].is_number_integer()) cfg_["coreMask"] = 15;
    if (!cfg_.contains("periodSampling") || !cfg_["periodSampling"].is_boolean()) cfg_["periodSampling"] = true;
    if (!cfg_.contains("durationSec") || !cfg_["durationSec"].is_number()) cfg_["durationSec"] = 5.0;
    if (!cfg_.contains("sampleRateMs") || !cfg_["sampleRateMs"].is_number_integer()) cfg_["sampleRateMs"] = 1000;
    if (!cfg_.contains("startBreakpoint") || !cfg_["startBreakpoint"].is_number_unsigned()) cfg_["startBreakpoint"] = 0ULL;
    if (!cfg_.contains("endBreakpoint") || !cfg_["endBreakpoint"].is_number_unsigned()) cfg_["endBreakpoint"] = 0ULL;

    // Early exit if module disabled
    if (!cfg_.value("enabled", true)) {
        spdlog::info("[LynsynModule] Disabled by config, validation passed");
        return true;
    }

    // [MOD] Create parent directory for CSV, avoid perm_options
    try {
        fs::path p(sanitized_output);
        fs::path parent = p.parent_path();
        if (!parent.empty()) {
            fs::create_directories(parent);
            // [MOD] Removed fs::perm_options for compatibility with std::experimental::filesystem
            // Default permissions (rwxr-xr-x) set by create_directories are sufficient
            spdlog::info("[LynsynModule] Ensured directory: {}", parent.string());
        }
    } catch (const std::exception& e) {
        spdlog::warn("[LynsynModule] Failed to create CSV directory: {}", e.what());
    }

    // [MOD] Log final resolved path
    spdlog::info("[LynsynModule] Using outputCSV: '{}'", sanitized_output);

    return true;
}
//====================================================================================================================
    void start() override {
        if (!cfg_.value("enabled", true)) {
            spdlog::info("[LynsynModule] Disabled by config; skipping start.");
            return;
        }

        spdlog::info("[LynsynModule] Starting...");
        power_ = std::make_unique<LynsynMonitorConcrete>(aggregator_, tm_);

        // --- Named, type-safe config population ---
        LynsynMonitorConfig lcfg{};
        lcfg.outputCSV       = cfg_.value("outputCSV", std::string("/tmp/lynsyn_output.csv"));
        lcfg.periodSampling  = cfg_.value("periodSampling", true);
        lcfg.durationSec     = cfg_.value("durationSec", 5.0);            // double
        lcfg.coreMask        = static_cast<uint32_t>(cfg_.value("coreMask", 15));
        lcfg.startBreakpoint = cfg_.value("startBreakpoint", 0ULL);
        lcfg.sampleRateMs    = cfg_.value("sampleRateMs", 1000);          // int
        lcfg.endBreakpoint   = cfg_.value("endBreakpoint", 0ULL);

        // Some parts of your monitor reference config_.period_ms; keep it aligned with sampleRateMs.
        // If LynsynMonitorConfig already has period_ms, set it; if not, you can add it to the struct.
        lcfg.period_ms       = lcfg.sampleRateMs;                          // keep device period == host rate

        // Sane clamp for safety (your monitor loop clamps too, but keep it consistent)
        if (lcfg.sampleRateMs < 100)  lcfg.sampleRateMs = 100;
        if (lcfg.period_ms   < 100)   lcfg.period_ms    = 100;

        // --- Configure + Initialize ---
        if (!power_->configure(lcfg) || !power_->initialize()) {
            spdlog::error("[LynsynModule] configure/initialize failed; disabling power expectations.");
            // Avoid aggregator waiting for power rows
            if (aggregator_) {
                auto cfgA = aggregator_->getConfig();
                cfgA.expectsPower = false;
                aggregator_->updateConfig(cfgA);
            }
            power_.reset();
            // Throw if you want to fail the whole pipeline; otherwise return to continue without power.
            // throw std::runtime_error("[LynsynModule] configure/initialize failed");
            return;
        }

        power_->setErrorCallback([](const std::string& msg){
            spdlog::error("[Lynsyn Error] {}", msg);
        });

        power_->startMonitoring();
        spdlog::info("[LynsynModule] Started.");
    }

    void stop() override {
        spdlog::info("[LynsynModule] Stopping...");
        if (power_) {
            power_->stop();
            power_.reset();
        }
        spdlog::info("[LynsynModule] Stopped.");
    }

private:
    json cfg_;
    Context& ctx_;
    ThreadManager& tm_;
    std::unique_ptr<LynsynMonitorConcrete> power_;
    std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator_;
};
