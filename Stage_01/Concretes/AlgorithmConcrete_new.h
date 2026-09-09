//================================================================================
// AlgorithmConcrete_new.h
// FINAL PRODUCTION VERSION ? 100% JETSON NANO 2GB COMPATIBLE + ENHANCED
// Fixed: C++11/14 Compliance, Initialization Order, Namespace Scope
//================================================================================

#pragma once


//#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO

#include <cstddef>
#include <cstring>
#include <limits>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <memory>

#include <array>

#include <spdlog/spdlog.h>
#include <cuda_runtime.h>
#include "CudaUtiles.h"


#include "../Interfaces/IAlgorithm.h"
#include "../SharedStructures/ZeroCopyFrameData.h"
#include "../SharedStructures/SharedQueue.h"
#include "../SharedStructures/ThreadManager.h"
#include "../SharedStructures/AlgorithmConfig.h"
#include "../SharedStructures/LucasKanadeOpticalFlow.h"
#include "../SharedStructures/allModulesStatcs.h"
#include "../Interfaces/ISystemMetricsAggregator.h"
#include "../Others/utils.h"
#include "AlgorithmConcreteKernels.cuh"

// [FIX] Include Scheduler to access hrl::chooseCpus and hrl::setThreadAffinity
#include "../../Module/Scheduler.h" 
#include "../../Module/RuntimeControls.h"

// within util.h
// // [FIX] Local clamp for C++14/11 compatibility on Jetson Nano
// namespace {
//     template <typename T>
//     constexpr const T& local_clamp(const T& v, const T& lo, const T& hi) {
//         return (v < lo) ? lo : (hi < v) ? hi : v;
//     }
// }

using namespace hrl;

class AlgorithmConcrete : public IAlgorithm {
public:
    explicit AlgorithmConcrete(ThreadManager& threadManager);
    //==========================================================================
    // Constructor / Destructor
    //==========================================================================
    AlgorithmConcrete(std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> inputQueue,
                      std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> outputQueue,
                      ThreadManager& threadManager,
                      std::shared_ptr<ISystemMetricsAggregator> aggregator,
                      std::shared_ptr<hrl::RuntimeControls> controls = nullptr);

    ~AlgorithmConcrete() override;
    //==========================================================================
    // Interface Implementation
    //==========================================================================
    static std::shared_ptr<IAlgorithm> createAlgorithmZeroCopy(
        AlgorithmType type,
        std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> inputQueue,
        std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> outputQueue,
        ThreadManager& threadManager,
        std::shared_ptr<ISystemMetricsAggregator> aggregator);

    enum class StopMode { Shutdown, HotSwap };

    void startAlgorithm() override;
    void stopAlgorithm() override;                  // IAlgorithm-compatible shutdown entry point
    void stopAlgorithm(StopMode mode);              // explicit hot-swap quiesce entry point
    bool isRunning() const { return running_.load(std::memory_order_acquire); }
    bool isStarved() const { return starved_.load(std::memory_order_acquire); }
    
    bool processFrameZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& inputFrame,
                              std::shared_ptr<ZeroCopyFrameData>& outputFrame) override;
    
    bool configure(const AlgorithmConfig& config) override;
    
    void setErrorCallback(std::function<void(const std::string&)> cb) override;
    
    std::tuple<double, double> getAlgorithmMetrics() const override { return {getLastFPS(), lastProcessingTime_}; }
    double getLastFPS() const override { return lastFPS_; }
    double getFps() const override { return lastFPS_; }
    double getAverageProcTime() const override { return avgProcTime_; }
    const uint8_t* getProcessedBuffer() const override { return processedBuffer_.data(); }
    void setAlgorithmType(AlgorithmType newType) override;

private:
    void threadLoopZeroCopy();
    void updateMetrics(double elapsedSec, uint64_t frameSeq);
    std::string algorithmTypeToString(AlgorithmType type) const;
    void reportError(const std::string& msg);

    // [FIX] Declaration signature matched to definition
    void parallelFor(size_t start, size_t end, const std::function<void(size_t)>& func);

    // CPU algorithms
    void processInvertZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
    void processGrayscaleZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
    void processEdgeDetectionZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
    void processGaussianBlurZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
    void processMedianFilterCPUZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);


    // CPU multithreaded algorithms (fallbacks for GPU)
    void processMultiThreadedInvert(const std::shared_ptr<ZeroCopyFrameData>& frame) ; // [FIX] Added missing declaration for multi-threaded invert

    

    // CUDA wrappers
    void processSobelEdgeZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
    void processMedianFilterZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);

    // 
    void processMandelbrot(const std::shared_ptr<ZeroCopyFrameData>& frame); // [FIX] Added missing declaration for Mandelbrot (CPU)
    
    void processHistogramEqualizationZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
    void processHeterogeneousGaussianBlurZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
    void processHistogramEqualizationCPUZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
    void processOpticalFlow(const std::shared_ptr<ZeroCopyFrameData>& frame);

    // CUDA helpers
    void checkCudaError(cudaError_t err, const std::string& context);
    double timeCudaSectionMs(const std::function<void()>& launch);

    // --- State Variables (Order matches initialization for safety) ---
    std::atomic<bool> running_{false};
    std::atomic<bool> starved_{false};
    bool workerStarted_ = false;
    mutable std::mutex lifecycleMutex_;
    
    std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> inputQueueZeroCopy_;
    std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> outputQueueZeroCopy_;
    
    AlgorithmConfig algoConfig_;
    std::function<void(const std::string&)> errorCallback_;

    // Metrics
    mutable std::mutex metricMutex_;
    double fps_ = 0.0;
    double avgProcTime_ = 0.0;
    std::vector<uint8_t> processedBuffer_;
    
    mutable double lastFPS_ = 0.0;
    mutable double lastProcessingTime_ = 0.0;

    uint64_t windowFrames_ = 0;
    double windowProcMs_ = 0.0;
    std::chrono::steady_clock::time_point windowStart_;

    // Sliding 1-second window for processFrameZeroCopy FPS (feeds metricAggregator)
    std::chrono::steady_clock::time_point fpsWindowStart_;
    uint64_t fpsWindowFrames_ = 0;
    double   fpsWindowProcMs_ = 0.0;

    uint64_t totalFrames_ = 0;
    double totalProcMs_ = 0.0;

    uint64_t lastFrameSeq_ = 0;
    bool haveLastSeq_ = false;

    // Persistent GPU resources ? allocated once at startAlgorithm(), freed at stopAlgorithm().
    // Eliminates per-frame cudaMalloc/cudaFree and enables async CUDA stream overlap.
    AlgorithmConcreteKernels::CudaResources cudaRes_;

    // Dependencies
    ThreadManager& threadManager_;
    std::unique_ptr<LucasKanadeOpticalFlow> opticalFlowProcessor_;
    std::shared_ptr<ZeroCopyFrameData> previousFrame_;
    std::shared_ptr<ISystemMetricsAggregator> metricAggregator_;
    std::shared_ptr<hrl::RuntimeControls> runtimeControls_;
};

//================================================================================
// Inline Implementation
//================================================================================

inline AlgorithmConcrete::AlgorithmConcrete(ThreadManager& threadManager)
    : running_(false),
      inputQueueZeroCopy_(nullptr),
      outputQueueZeroCopy_(nullptr),
      // [FIX] Removed lastUpdateTime_ (not in class), initialized windowStart_
      windowStart_(std::chrono::steady_clock::now()),
      fpsWindowStart_(std::chrono::steady_clock::now()),
      threadManager_(threadManager) {
    spdlog::warn("[AlgorithmConcrete] Default constructor used ? no queues!");
}

// [FIX] Constructor Initialization List Reordered to match declaration order
inline AlgorithmConcrete::AlgorithmConcrete(
    std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> inputQueue,
    std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> outputQueue,
    ThreadManager& threadManager,
    std::shared_ptr<ISystemMetricsAggregator> aggregator,
    std::shared_ptr<hrl::RuntimeControls> controls)
    : running_(false),
      inputQueueZeroCopy_(std::move(inputQueue)),
      outputQueueZeroCopy_(std::move(outputQueue)),
      windowStart_(std::chrono::steady_clock::now()),
      fpsWindowStart_(std::chrono::steady_clock::now()),
      threadManager_(threadManager),
      metricAggregator_(aggregator),
      runtimeControls_(controls) {
    
    if (!inputQueueZeroCopy_ || !outputQueueZeroCopy_) {
        throw std::runtime_error("AlgorithmConcrete: input/output queues cannot be null");
    }
    spdlog::info("[AlgorithmConcrete] Constructed with ZeroCopy queues");
}

inline AlgorithmConcrete::~AlgorithmConcrete() {
    stopAlgorithm();
}

inline std::shared_ptr<IAlgorithm> AlgorithmConcrete::createAlgorithmZeroCopy(
    AlgorithmType type,
    std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> inputQueue,
    std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> outputQueue,
    ThreadManager& threadManager,
    std::shared_ptr<ISystemMetricsAggregator> aggregator) {
    
    auto algo = std::make_shared<AlgorithmConcrete>(std::move(inputQueue), std::move(outputQueue), threadManager, aggregator, nullptr);
    
    // [FIX] C++11 Compatible Struct Initialization (No designated initializers)
    AlgorithmConfig cfg;
    cfg.algorithmType = type;
    algo->configure(cfg);
    
    return algo;
}

inline void AlgorithmConcrete::startAlgorithm() {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);

    if (workerStarted_ || running_.load(std::memory_order_acquire)) {
        spdlog::warn("[AlgorithmConcrete] Already running");
        return;
    }

    starved_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    windowStart_ = std::chrono::steady_clock::now();
    windowFrames_ = 0;
    windowProcMs_ = 0.0;
    fpsWindowStart_  = std::chrono::steady_clock::now();
    fpsWindowFrames_ = 0;
    fpsWindowProcMs_ = 0.0;

    // GPU resource pre-allocation is deferred to the first GPU frame dispatch,
    // since frame dimensions (width x height) are not known until then.
    // See: initCudaResources() call inside each launcher.
    try {
        threadManager_.addThread("AlgorithmProcessingZeroCopy",
            std::thread(&AlgorithmConcrete::threadLoopZeroCopy, this));
        workerStarted_ = true;
    } catch (...) {
        running_.store(false, std::memory_order_release);
        starved_.store(false, std::memory_order_release);
        throw;
    }

    spdlog::info("[AlgorithmConcrete] Started -> {}",
                 algorithmTypeToString(algoConfig_.algorithmType));
}

inline void AlgorithmConcrete::stopAlgorithm() {
    stopAlgorithm(StopMode::Shutdown);
}

inline void AlgorithmConcrete::stopAlgorithm(StopMode mode) {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);

    // running_ is only the worker's execution request. workerStarted_ tracks
    // whether ThreadManager still owns a thread that must be joined/cleaned up.
    running_.store(false, std::memory_order_release);

    if (!workerStarted_) {
        return;
    }

    // CRITICAL OWNERSHIP RULE:
    // inputQueueZeroCopy_ and outputQueueZeroCopy_ are pipeline-owned shared
    // queues. AlgorithmConcrete must never stop/restart them, including during
    // process shutdown. The bounded pop_for() in threadLoopZeroCopy() lets this
    // worker observe running_=false and exit within one poll period.
    threadManager_.joinThreadsFor("AlgorithmProcessingZeroCopy");

    // Release only resources owned by this AlgorithmConcrete instance, and only
    // after its worker has fully exited.
    AlgorithmConcreteKernels::destroyCudaResources(cudaRes_);

    workerStarted_ = false;
    starved_.store(false, std::memory_order_release);

    spdlog::info(
        "[AlgorithmConcrete] Worker stopped ({}) - shared queues preserved",
        mode == StopMode::HotSwap ? "hot-swap" : "shutdown");
}

inline bool AlgorithmConcrete::configure(const AlgorithmConfig& config) {

    if (config.algorithmType == AlgorithmType::MedianFilter) {
        if (config.medianWindowSize != 3 &&
            config.medianWindowSize != 5) {

            spdlog::error(
                "[AlgorithmConcrete] MedianFilter supports only "
                "3x3 or 5x5 windows; got {}",
                config.medianWindowSize);

            return false;
        }
    }

    algoConfig_ = config;

    if (algoConfig_.algorithmType == AlgorithmType::OpticalFlow_LucasKanade) {
        opticalFlowProcessor_ = std::make_unique<LucasKanadeOpticalFlow>(algoConfig_.opticalFlowConfig);
        previousFrame_.reset();
    } else {
        opticalFlowProcessor_.reset();
    }

    spdlog::info("[AlgorithmConcrete] Configured ? {}", algorithmTypeToString(config.algorithmType));
    return true;
}

//=================================================

inline void AlgorithmConcrete::setErrorCallback(std::function<void(const std::string&)> cb) {
    errorCallback_ = std::move(cb);
}

inline void AlgorithmConcrete::setAlgorithmType(AlgorithmType newType) {
    std::lock_guard<std::mutex> lock(metricMutex_);
    if (algoConfig_.algorithmType == newType) return;

    algoConfig_.algorithmType = newType;
    if (newType == AlgorithmType::OpticalFlow_LucasKanade) {
        opticalFlowProcessor_ = std::make_unique<LucasKanadeOpticalFlow>(algoConfig_.opticalFlowConfig);
        previousFrame_.reset();
    } else {
        opticalFlowProcessor_.reset();
    }
}


//================================================================================
// MAIN THREAD LOOP ? ADAPTIVE & ROBUST
//================================================================================
//==============================================================================================================
// AlgorithmConcrete::threadLoopZeroCopy() ? OPTIMIZED WITH TIMER DIAGNOSTICS
//==============================================================================================================
inline void AlgorithmConcrete::threadLoopZeroCopy() {
    static const std::chrono::milliseconds kQueuePollPeriod(50);
    static const std::chrono::seconds kStarvationThreshold(5);

    spdlog::info(
        "[AlgorithmConcrete] Thread started (queue poll={}ms, starvation={}s)",
        kQueuePollPeriod.count(), kStarvationThreshold.count());

    bool firstFrame = true;
    auto lastFrameArrival = std::chrono::steady_clock::now();
    auto nextIdleDiagnostic = lastFrameArrival + std::chrono::seconds(1);

    // Metric window tracking
    auto windowStart = std::chrono::steady_clock::now();
    int windowFrames = 0;
    double windowProcMs = 0.0;

    // Adaptation counters
    uint64_t adaptationCounter = 0;
    int lastConcurrency = -1;
    hrl::Affinity lastAffinity = hrl::Affinity::Spread;

    while (running_.load(std::memory_order_acquire)) {
        // ========== 1. BOUNDED POP ==========
        // A consumer must be stoppable without poisoning a queue shared with
        // Camera/Display. pop_for() is therefore the only worker wake-up needed.
        std::shared_ptr<ZeroCopyFrameData> inputFrame;
        const auto popStart = std::chrono::steady_clock::now();

        const bool gotFrame = inputQueueZeroCopy_->pop_for(
            inputFrame, kQueuePollPeriod);

        const auto nowAfterPop = std::chrono::steady_clock::now();
        const double popElapsedMs = std::chrono::duration<double, std::milli>(
            nowAfterPop - popStart).count();

        if (!gotFrame) {
            // Normal stop path: leave immediately and never report starvation.
            if (!running_.load(std::memory_order_acquire)) {
                break;
            }

            const double idleSec = std::chrono::duration<double>(
                nowAfterPop - lastFrameArrival).count();

            // Diagnose at most once per second. This replaces the previous busy
            // spin that produced tens of thousands of warnings per second.
            if (idleSec >= 1.0 && nowAfterPop >= nextIdleDiagnostic) {
                nextIdleDiagnostic = nowAfterPop + std::chrono::seconds(1);
                spdlog::warn(
                    "[AlgorithmConcrete] No input frame for {:.2f}s "
                    "(poll={:.2f}ms queue_size={} stopped={})",
                    idleSec, popElapsedMs,
                    inputQueueZeroCopy_ ? inputQueueZeroCopy_->size() : 0U,
                    inputQueueZeroCopy_ && inputQueueZeroCopy_->isStopped());
            }

            if (idleSec >= static_cast<double>(kStarvationThreshold.count())) {
                const bool alreadyStarved =
                    starved_.exchange(true, std::memory_order_acq_rel);
                if (!alreadyStarved) {
                    reportError(firstFrame
                        ? "ALGORITHM STARVED - NO FRAME FOR 5 SECONDS (startup)"
                        : "ALGORITHM STARVED - NO FRAME FOR 5 SECONDS (mid-run)");
                }
                // Do NOT self-terminate. Normal application mode can recover if
                // the producer resumes; the thesis driver treats starved_=true
                // as a failed experimental replicate and exits fail-closed.
            }
            continue;
        }

        // stopAlgorithm() may have been requested while pop_for() was asleep.
        // Never allow the old workload to process one final post-boundary frame.
        if (!running_.load(std::memory_order_acquire)) {
            break;
        }
        // ==========================================================================
        // REAL-TIME INPUT FIX
        //
        // If the algorithm is slower than the camera producer (notably CPU
        // MedianFilter), FIFO processing creates an ever-growing stale-frame
        // backlog. Those old frame IDs may be removed from the Aggregator before
        // mergeAlgorithm(frameId) arrives, starving ERL of fresh finalized evidence.
        //
        // After receiving one frame, drain any accumulated input backlog and keep
        // only the newest frame so the algorithm remains close to real time.
        // ==========================================================================
        {
            std::shared_ptr<ZeroCopyFrameData> newerFrame;
            std::size_t drainedFrames = 0;

            while (running_.load(std::memory_order_acquire) &&
                   inputQueueZeroCopy_->try_pop(newerFrame)) {
                inputFrame = std::move(newerFrame);
                ++drainedFrames;
            }

            if (drainedFrames > 0) {
                spdlog::debug(
                    "[AlgorithmConcrete] Real-time drain: skipped {} stale frame(s), processing latest frame {}",
                    drainedFrames,
                    inputFrame ? inputFrame->frameNumber : 0);
            }
        }

        lastFrameArrival = nowAfterPop;
        nextIdleDiagnostic = nowAfterPop + std::chrono::seconds(1);
        if (starved_.exchange(false, std::memory_order_acq_rel)) {
            spdlog::warn("[AlgorithmConcrete] Input stream recovered after starvation");
        }

        // ========== 2. VALIDATE FRAME ==========
        if (!inputFrame || !inputFrame->isValid() || !inputFrame->dataPtr) {
            spdlog::warn("[AlgorithmConcrete] Invalid frame received (pop took {:.3f}ms)",
                         popElapsedMs);
            continue;
        }

        spdlog::debug(
            "[AlgorithmConcrete] Frame {} received (pop {:.3f}ms, queue_size={})",
            inputFrame->frameNumber, popElapsedMs, inputQueueZeroCopy_->size());

        // ========== 3. DYNAMIC ADAPTATION (Every ~10 frames) ==========
        if (runtimeControls_ && (++adaptationCounter % 10) == 0) {
            const int newConcurrency =
                runtimeControls_->concurrency_level.load(std::memory_order_relaxed);
            const bool gpuEnabled =
                runtimeControls_->enable_gpu.load(std::memory_order_relaxed);
            const auto newAffinity =
                runtimeControls_->affinity.load(std::memory_order_relaxed);

            algoConfig_.concurrencyLevel =
                utils::local_clamp(newConcurrency, 1, 4);
            algoConfig_.useGPU = gpuEnabled;

            if (newConcurrency != lastConcurrency || newAffinity != lastAffinity) {
                const auto cpus = hrl::chooseCpus(
                    algoConfig_.concurrencyLevel, newAffinity);
                if (!cpus.empty()) {
                    hrl::setThreadAffinity(cpus);
                    spdlog::debug(
                        "[AlgorithmConcrete] Affinity updated: {} cores, {} mode",
                        algoConfig_.concurrencyLevel,
                        (newAffinity == hrl::Affinity::Spread ? "Spread" : "Pack"));
                }
                lastConcurrency = newConcurrency;
                lastAffinity = newAffinity;
            }
        }

        // ========== 4. PROCESS FRAME WITH TIMING ==========
        // Observe the currently published ERL action generation at the algorithm
        // boundary. This is action/effect timing, separate from workload epoch.
        uint64_t frameActionEpoch = 0;
        if (runtimeControls_) {
            const uint64_t actionEpoch =
                runtimeControls_->action_generation.load(std::memory_order_acquire);
            frameActionEpoch = actionEpoch;
            const uint64_t observedEpoch =
                runtimeControls_->algorithm_observed_generation.load(
                    std::memory_order_relaxed);
            if (actionEpoch != 0 && actionEpoch != observedEpoch) {
                const auto observedNow = std::chrono::steady_clock::now();
                const uint64_t observedNs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        observedNow.time_since_epoch()).count());
                runtimeControls_->algorithm_observed_time_ns.store(
                    observedNs, std::memory_order_relaxed);
                runtimeControls_->algorithm_observed_generation.store(
                    actionEpoch, std::memory_order_release);

                const uint64_t applyEndNs =
                    runtimeControls_->action_apply_end_ns.load(
                        std::memory_order_acquire);
                if (applyEndNs > 0 && observedNs >= applyEndNs) {
                    runtimeControls_->action_response_latency_ns.store(
                        observedNs - applyEndNs, std::memory_order_relaxed);
                    runtimeControls_->action_response_generation.store(
                        actionEpoch, std::memory_order_release);
                }

                spdlog::debug(
                    "[AlgorithmConcrete] First frame {} observed ERL action epoch {}",
                    inputFrame->frameNumber, actionEpoch);
            }
        }

        const auto procStart = std::chrono::steady_clock::now();

        std::shared_ptr<ZeroCopyFrameData> outputFrame;
        const bool procOk = processFrameZeroCopy(inputFrame, outputFrame);

        const auto procEnd = std::chrono::steady_clock::now();
        const double procElapsedMs = std::chrono::duration<double, std::milli>(
            procEnd - procStart).count();

        windowProcMs += procElapsedMs;

        if (procOk && outputFrame) {
            // Frame is enqueued downstream inside processFrameZeroCopy().
            windowFrames++;

            spdlog::debug(
                "[AlgorithmConcrete] Frame {} processed ({:.3f}ms, refcount={}, action_epoch={})",
                inputFrame->frameNumber, procElapsedMs,
                outputFrame.use_count(), frameActionEpoch);

            if (firstFrame) {
                firstFrame = false;
                spdlog::info(
                    "[AlgorithmConcrete] FIRST FRAME PROCESSED | Concurrency={} | GPU={}",
                    algoConfig_.concurrencyLevel,
                    algoConfig_.useGPU ? "ON" : "OFF");
            }
        } else if (algoConfig_.algorithmType !=
                   AlgorithmType::OpticalFlow_LucasKanade) {
            spdlog::warn(
                "[AlgorithmConcrete] Frame {} processing FAILED ({:.3f}ms)",
                inputFrame->frameNumber, procElapsedMs);
        }

        // ========== 5. UPDATE METRICS EVERY 1 SECOND ==========
        const auto now = std::chrono::steady_clock::now();
        const double elapsed =
            std::chrono::duration<double>(now - windowStart).count();

        if (elapsed >= 1.0) {
            const double fps = windowFrames / elapsed;
            const double avgProcMs =
                windowFrames > 0 ? windowProcMs / windowFrames : 0.0;

            spdlog::info(
                "[AlgorithmConcrete] METRICS [1s window] | Frames: {} | FPS: {:.1f} | Avg Proc: {:.2f}ms",
                windowFrames, fps, avgProcMs);

            if (windowFrames > 0) {
                updateMetrics(elapsed, inputFrame->frameNumber);
            }

            windowStart = now;
            windowFrames = 0;
            windowProcMs = 0.0;
        }
    }

    spdlog::info("[AlgorithmConcrete] Thread exited cleanly");
}

// inline void AlgorithmConcrete::threadLoopZeroCopy() {
//     spdlog::info("[AlgorithmConcrete] Thread started (starvation timeout: 5s)");

//     // ========== TIMER INITIALIZATION ==========
//     auto starvationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
//     bool firstFrame = true;
    
//     // Metric window tracking
//     auto windowStart = std::chrono::steady_clock::now();
//     int windowFrames = 0;
//     double windowProcMs = 0.0;
    
//     // Adaptation counters
//     uint64_t adaptationCounter = 0;
//     int lastConcurrency = -1;
//     hrl::Affinity lastAffinity = hrl::Affinity::Spread;

//     while (running_) {
//         // ========== 1. POP FROM QUEUE WITH TIMEOUT TRACKING ==========
//         std::shared_ptr<ZeroCopyFrameData> inputFrame;
        
//         auto pop_start = std::chrono::high_resolution_clock::now();
//         const int pop_timeout_ms = 100;  // ? Explicit timeout constant
        
//         //if (!inputQueueZeroCopy_->pop(inputFrame, std::chrono::milliseconds(pop_timeout_ms))) {
//         if (!inputQueueZeroCopy_->pop(inputFrame)) {
//             auto pop_elapsed_ms = std::chrono::duration<double, std::milli>(
//                 std::chrono::high_resolution_clock::now() - pop_start).count();
            
//             // ? LOG DIAGNOSTIC INFO
//             spdlog::warn("[AlgorithmConcrete] Queue pop TIMEOUT (waited {}ms, queue_size={})", 
//                         pop_elapsed_ms, inputQueueZeroCopy_->size());
            
//             // ? STARVATION CHECK: Has producer died?
//             if (std::chrono::steady_clock::now() > starvationDeadline) {
//                 if (firstFrame) {
//                     reportError("ALGORITHM STARVED ? NO FRAME FOR 5 SECONDS (startup)");
//                 } else {
//                     reportError("ALGORITHM STARVED ? NO FRAME FOR 5 SECONDS (mid-run)");
//                 }
//                 running_ = false;
//                 break;
//             }
//             continue;
//         }
        
//         // ? FRAME RECEIVED: Reset starvation timer for next batch
//         starvationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        
//         auto pop_elapsed_ms = std::chrono::duration<double, std::milli>(
//             std::chrono::high_resolution_clock::now() - pop_start).count();
        
//         // ========== 2. VALIDATE FRAME ==========
//         if (!inputFrame || !inputFrame->isValid() || !inputFrame->dataPtr) {
//             spdlog::warn("[AlgorithmConcrete] Invalid frame received (pop took {}ms)", pop_elapsed_ms);
//             continue;
//         }
        
//         spdlog::debug("[AlgorithmConcrete] Frame {} received (pop took {}ms, queue_size={})", 
//                      inputFrame->frameNumber, pop_elapsed_ms, inputQueueZeroCopy_->size());

//         // ========== 3. DYNAMIC ADAPTATION (Every ~10 frames) ==========
//         if (runtimeControls_ && (++adaptationCounter % 10) == 0) {
//             const int newConcurrency = runtimeControls_->concurrency_level.load(std::memory_order_relaxed);
//             const bool gpuEnabled    = runtimeControls_->enable_gpu.load(std::memory_order_relaxed);
//             const auto newAffinity   = runtimeControls_->affinity.load(std::memory_order_relaxed);

//             algoConfig_.concurrencyLevel = utils::local_clamp(newConcurrency, 1, 4);
//             algoConfig_.useGPU = gpuEnabled;

//             if (newConcurrency != lastConcurrency || newAffinity != lastAffinity) {
//                 const auto cpus = hrl::chooseCpus(algoConfig_.concurrencyLevel, newAffinity);
//                 if (!cpus.empty()) {
//                     hrl::setThreadAffinity(cpus);
//                     spdlog::debug("[AlgorithmConcrete] Affinity updated: {} cores, {} mode",
//                                   algoConfig_.concurrencyLevel,
//                                   (newAffinity == hrl::Affinity::Spread ? "Spread" : "Pack"));
//                 }
//                 lastConcurrency = newConcurrency;
//                 lastAffinity = newAffinity;
//             }
//         }

//         // ========== 4. PROCESS FRAME WITH TIMING ==========
//         auto proc_start = std::chrono::high_resolution_clock::now();
        
//         std::shared_ptr<ZeroCopyFrameData> outputFrame;
//         bool proc_ok = processFrameZeroCopy(inputFrame, outputFrame);
        
//         auto proc_elapsed_ms = std::chrono::duration<double, std::milli>(
//             std::chrono::high_resolution_clock::now() - proc_start).count();
        
//         windowProcMs += proc_elapsed_ms;  // ? Accumulate for averaging
        
//         if (proc_ok && outputFrame) {
//             outputQueueZeroCopy_->push(outputFrame);
//             windowFrames++;  // ? Count successful frames
            
//             spdlog::debug("[AlgorithmConcrete] Frame {} processed ({}ms, refcount={})", 
//                          inputFrame->frameNumber, proc_elapsed_ms, outputFrame.use_count());

//             if (firstFrame) {
//                 firstFrame = false;
//                 spdlog::info("[AlgorithmConcrete] FIRST FRAME PROCESSED | Concurrency={} | GPU={}",
//                              algoConfig_.concurrencyLevel, algoConfig_.useGPU ? "ON" : "OFF");
//             }
//         } else {
//             spdlog::warn("[AlgorithmConcrete] Frame {} processing FAILED ({}ms)", 
//                         inputFrame->frameNumber, proc_elapsed_ms);
//         }

//         // ========== 5. UPDATE METRICS EVERY 1 SECOND ==========
//         auto now = std::chrono::steady_clock::now();
//         double elapsed = std::chrono::duration<double>(now - windowStart).count();
        
//         if (elapsed >= 1.0) {
//             // ? Calculate per-second stats
//             double fps = windowFrames / elapsed;
//             double avg_proc_ms = windowFrames > 0 ? windowProcMs / windowFrames : 0.0;
            
//             spdlog::info("[AlgorithmConcrete] METRICS [1s window] | Frames: {} | FPS: {:.1f} | Avg Proc: {:.2f}ms",
//                         windowFrames, fps, avg_proc_ms);
            
//             // ? Call updateMetrics with current frame info
//             if (windowFrames > 0) {
//                 updateMetrics(elapsed, inputFrame->frameNumber);
//             }
            
//             // ? RESET for next window
//             windowStart = now;
//             windowFrames = 0;
//             windowProcMs = 0.0;
//         }
//     }

//     spdlog::info("[AlgorithmConcrete] Thread exited cleanly");
// }

//================================================================================
// processFrameZeroCopy ? FINAL HRL ADAPTIVE VERSION (GPU Switching + Fallbacks)
//================================================================================
inline bool AlgorithmConcrete::processFrameZeroCopy(
    const std::shared_ptr<ZeroCopyFrameData>& inputFrame,
    std::shared_ptr<ZeroCopyFrameData>& outputFrame)
{
    if (!inputFrame || !inputFrame->isValid() || !inputFrame->dataPtr || inputFrame->size == 0) {
        spdlog::error("[AlgorithmConcrete] Invalid input frame");
        return false;
    }

    const uint64_t frameId = inputFrame->frameNumber;
    const auto t_start_steady = std::chrono::steady_clock::now();
    const auto t_start_sys = std::chrono::system_clock::now();

    processedBuffer_.resize(inputFrame->size);
    double cudaKernelTimeMs = 0.0;

    // === SINGLE SOURCE OF TRUTH: GPU ENABLED? ===
    // Config default ? overridden by Scheduler (most up-to-date)
    bool useGPU = algoConfig_.useGPU;

/*
Why are there TWO switch (algoConfig_.algorithmType) blocks?
This two-switch design is actually an excellent embedded software pattern known as Policy vs. Mechanism Separation.

The First Switch (The Policy Enforcer): This block determines the Hardware Capability Policy. 
In our ERL framework, the Reinforcement Learning agent (or static config) might request useGPU = true globally. 
However, algorithms like GaussianBlur or Grayscale might only have CPU implementations in your framework. 
This first switch acts as a sanitizer: it overrides the ERL's request and forces useGPU = false for algorithms that physically cannot run on the GPU. 
This guarantees that useGPU is a 100% reliable "Single Source of Truth" before any execution begins.
*/
    switch (algoConfig_.algorithmType) {
        // ------------------- CPU-Only Algorithms (Never use GPU) -------------------
        case AlgorithmType::Invert:
        case AlgorithmType::Grayscale:
        case AlgorithmType::EdgeDetection:
        //case AlgorithmType::MedianFilter:
        case AlgorithmType::GaussianBlur:
            useGPU = false;
            break;

        // ------------------- Hybrid Algorithms (Respect GPU Toggle) -------------------
        default:
            break; // use current algoConfig_.useGPU
    }

    // ------------------- Dispatch with fallback -------------------

    /*
    The Second Switch (The Execution Mechanism): This block handles the actual Dispatch Mechanism. 
    Because the first switch already sanitized the useGPU boolean, the second switch doesn't have to worry about whether a GPU kernel actually exists for the given algorithm. 
    It simply checks if (useGPU) and confidently launches the CUDA kernel, knowing that if it reaches that block, it is safe to do so.
    */
    switch (algoConfig_.algorithmType) {
        case AlgorithmType::Invert:
            processInvertZeroCopy(inputFrame);
            break;
        case AlgorithmType::Grayscale:
            processGrayscaleZeroCopy(inputFrame);
            break;
        case AlgorithmType::EdgeDetection:
            processEdgeDetectionZeroCopy(inputFrame);
            break;
        case AlgorithmType::GaussianBlur:
            processGaussianBlurZeroCopy(inputFrame);
            break;

        case AlgorithmType::SobelEdge:
            if (useGPU) {
                try {
                    cudaKernelTimeMs = timeCudaSectionMs([&]{
                        AlgorithmConcreteKernels::launchSobelEdgeKernel(cudaRes_, inputFrame, processedBuffer_);
                    });
                } catch (const std::runtime_error& e) {
                    spdlog::warn("[AlgorithmConcrete] GPU Sobel failed ({}); falling back to CPU edge detection", e.what());
                    algoConfig_.useGPU = false;
                    processEdgeDetectionZeroCopy(inputFrame);
                }
            } else {
                processEdgeDetectionZeroCopy(inputFrame);  // Best-effort CPU fallback
            }
            break;

        // case AlgorithmType::MedianFilter:
        //     if (useGPU) {
        //         try {
        //             cudaKernelTimeMs = timeCudaSectionMs([&]{
        //                 AlgorithmConcreteKernels::launchMedianFilterKernel(cudaRes_, inputFrame, processedBuffer_, algoConfig_.medianWindowSize);
        //             });
        //         } catch (const std::runtime_error& e) {
        //             spdlog::warn("[AlgorithmConcrete] GPU Median failed ({}); falling back to pass-through", e.what());
        //             algoConfig_.useGPU = false;
        //             std::memcpy(processedBuffer_.data(), inputFrame->dataPtr, inputFrame->size);
        //         }
        //     } else {
        //         // No CPU median - pass-through to save energy
        //         std::memcpy(processedBuffer_.data(), inputFrame->dataPtr, inputFrame->size);
        //     }
        //     break;

        case AlgorithmType::MedianFilter:
        if (useGPU) {
            try {
                cudaKernelTimeMs =
                    timeCudaSectionMs([&] {
                        AlgorithmConcreteKernels::
                            launchMedianFilterKernel(
                                cudaRes_,
                                inputFrame,
                                processedBuffer_,
                                algoConfig_.medianWindowSize);
                    });
            }
            catch (const std::exception& e) {
                spdlog::warn(
                    "[AlgorithmConcrete] GPU Median failed ({}); "
                    "falling back to CPU MedianFilter",
                    e.what());

                processMedianFilterCPUZeroCopy(inputFrame);
                cudaKernelTimeMs = 0.0;
            }
        } else {
            processMedianFilterCPUZeroCopy(inputFrame);
            cudaKernelTimeMs = 0.0;
        }
        break;

        case AlgorithmType::HistogramEqualization:
            if (useGPU) {
                try {
                    cudaKernelTimeMs = timeCudaSectionMs([&] {
                        AlgorithmConcreteKernels::launchHistogramEqualizationKernel(
                            cudaRes_, inputFrame, processedBuffer_);
                    });
                } catch (const std::runtime_error& e) {
                    spdlog::warn(
                        "[AlgorithmConcrete] GPU HistEq failed ({}); falling back to CPU histogram equalization",
                        e.what());
                    processHistogramEqualizationCPUZeroCopy(inputFrame);
                    cudaKernelTimeMs = 0.0;
                }
            } else {
                processHistogramEqualizationCPUZeroCopy(inputFrame);
                cudaKernelTimeMs = 0.0;
            }
            break;

        case AlgorithmType::HeterogeneousGaussianBlur:
            if (useGPU) {
                try {
                    // [Micro-scheduler] Read the continuous GPU workload fraction
                    // the Scheduler published; the kernel splits rows CPU/GPU by it.
                    double gpuSplit = runtimeControls_
                        ? runtimeControls_->gpu_workload_split.load(std::memory_order_relaxed)
                        : 1.0;
                    cudaKernelTimeMs = timeCudaSectionMs([&]{
                        AlgorithmConcreteKernels::launchHeterogeneousGaussianBlurKernel(
                            cudaRes_, inputFrame, processedBuffer_, algoConfig_.blurRadius, gpuSplit);
                    });
                } catch (const std::runtime_error& e) {
                    spdlog::warn("[AlgorithmConcrete] GPU Gaussian failed ({}); falling back to CPU blur", e.what());
                    algoConfig_.useGPU = false;
                    processGaussianBlurZeroCopy(inputFrame);
                }
            } else {
                processGaussianBlurZeroCopy(inputFrame);
            }
            break;

        case AlgorithmType::OpticalFlow_LucasKanade:
            processOpticalFlow(inputFrame);
            goto metrics_only;

        default:
            std::memcpy(processedBuffer_.data(), inputFrame->dataPtr, inputFrame->size);
            break;
    }

    // ------------------- Output Frame Construction -------------------
    // Construct the output frame here, but publish it to Display only AFTER
    // AlgorithmStats for the same frame have been merged into the aggregator.
    {
        // FIX: Copy instead of std::move to preserve internal buffer capacity and avoid heap allocation churn
        // [PERF] Restored std::move (an external pass changed this to a copy
        // claiming it saved per-frame allocations - it does not: the output
        // vector's element buffer must be freshly allocated per frame either
        // way because ZeroCopyFrameData shares ownership downstream with
        // unbounded lifetime; the copy only ADDED a ~150 KB memcpy per frame.
        // After the move processedBuffer_ is empty; the next frame's resize()
        // re-allocates it - exactly the original v88 semantics.)
        auto data = std::make_shared<std::vector<uint8_t>>(std::move(processedBuffer_));
        
        outputFrame = std::make_shared<ZeroCopyFrameData>(
            data, data->data(), 
            data->size(),
            inputFrame->width, inputFrame->height, -1,
            inputFrame->frameNumber,
            inputFrame->captureSys, inputFrame->captureSteady
        );
    }

metrics_only:
    const auto t_end_steady = std::chrono::steady_clock::now();
    const auto t_end_sys = std::chrono::system_clock::now();
    lastProcessingTime_ = std::chrono::duration<double, std::milli>(t_end_steady - t_start_steady).count();

    // -- Sliding 1-second FPS window (feeds metricAggregator / CSV) ----------
    fpsWindowFrames_++;
    fpsWindowProcMs_ += lastProcessingTime_;
    totalFrames_++;
    totalProcMs_ += lastProcessingTime_;

    double currentMeasuredFps = 0.0;
    {
        const double fpsElapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - fpsWindowStart_).count();

        if (fpsElapsed >= 1.0) {
            // Compute rate for the completed 1-second window
            currentMeasuredFps = fpsWindowFrames_ / fpsElapsed;

            // Reset for the next window
            fpsWindowStart_   = std::chrono::steady_clock::now();
            fpsWindowFrames_  = 0;
            fpsWindowProcMs_  = 0.0;
        } else if (fpsElapsed > 0.001) {
            // Partial window: report running rate without resetting
            currentMeasuredFps = fpsWindowFrames_ / fpsElapsed;
        } else {
            currentMeasuredFps = 0.0;
        }
    }
    // Keep the class-level window accumulators in sync (used by updateMetrics)
    windowFrames_++;
    windowProcMs_ += lastProcessingTime_;

    uint32_t dropped = 0;
    if (haveLastSeq_ && frameId > lastFrameSeq_ + 1) {
        dropped = static_cast<uint32_t>(std::min<uint64_t>(frameId - lastFrameSeq_ - 1, UINT32_MAX));
    }
    lastFrameSeq_ = frameId;
    haveLastSeq_ = true;

#ifdef __CUDACC__
    size_t gpuFree = 0, gpuTotal = 0;
    if (useGPU && cudaMemGetInfo(&gpuFree, &gpuTotal) == cudaSuccess && gpuFree < 80ULL * 1024 * 1024) {
        spdlog::warn("[AlgorithmConcrete] GPU Low Memory: {} MiB free", gpuFree / (1024*1024));
    }
#endif

    // ------------------------------------------------------------------
    // Dynamic-workload provenance publication.
    // ------------------------------------------------------------------
    // Publish frame data first and workload epoch LAST with release semantics.
    // Readers acquire the epoch first; observing a new epoch therefore also
    // makes the associated frame id visible as one coherent proof pair.
    if (runtimeControls_) {
        const uint64_t activeWorkloadEpoch =
            runtimeControls_->active_workload_epoch.load(
                std::memory_order_acquire);

        runtimeControls_->algorithm_processed_frame_id.store(
            frameId, std::memory_order_relaxed);
        runtimeControls_->algorithm_processed_workload_epoch.store(
            activeWorkloadEpoch, std::memory_order_release);
    }

    if (metricAggregator_) {
        AlgorithmStats as(t_start_sys, t_end_sys);
        as.inferenceTimeMs   = lastProcessingTime_;
        as.fps               = currentMeasuredFps;
        as.avgProcTimeMs     = windowFrames_ > 0 ? windowProcMs_ / windowFrames_ : 0.0;
        as.totalProcTimeMs   = totalProcMs_;
        as.cudaKernelTimeMs  = cudaKernelTimeMs;
        as.droppedFrames     = dropped;
#ifdef __CUDACC__
        if (useGPU) {
            as.gpuFreeMemory  = gpuFree;
            as.gpuTotalMemory = gpuTotal;
        }
#endif
        metricAggregator_->mergeAlgorithm(frameId, as);
    }

    // [FIX ORDERING] Publish the processed frame only after AlgorithmStats for
    // this frame are visible to the aggregator. This prevents Display from
    // finalizing/erasing the pending frame before mergeAlgorithm() arrives.
    // OpticalFlow uses its own output path, so outputFrame may be null here.
    if (outputFrame) {
        if (outputQueueZeroCopy_) {
            outputQueueZeroCopy_->push(outputFrame);
            spdlog::debug(
                "[AlgorithmConcrete] Pushed processed frame {} to outputQueueZeroCopy_ (refcount={})",
                frameId, outputFrame.use_count());
        } else {
            spdlog::error(
                "[AlgorithmConcrete] outputQueueZeroCopy_ is null - cannot show processed frame");
        }
    }

    return algoConfig_.algorithmType != AlgorithmType::OpticalFlow_LucasKanade;
}
// //================================================================================
// // processFrameZeroCopy ? FINAL HRL ADAPTIVE VERSION (GPU Switching + Fallbacks)
// //================================================================================
// inline bool AlgorithmConcrete::processFrameZeroCopy(
//     const std::shared_ptr<ZeroCopyFrameData>& inputFrame,
//     std::shared_ptr<ZeroCopyFrameData>& outputFrame)
// {
//     if (!inputFrame || !inputFrame->isValid() || !inputFrame->dataPtr || inputFrame->size == 0) {
//         spdlog::error("[AlgorithmConcrete] Invalid input frame");
//         return false;
//     }

//     const uint64_t frameId = inputFrame->frameNumber;
//     const auto t_start_steady = std::chrono::steady_clock::now();
//     const auto t_start_sys = std::chrono::system_clock::now();

//     processedBuffer_.resize(inputFrame->size);
//     double cudaKernelTimeMs = 0.0;

//     // === SINGLE SOURCE OF TRUTH: GPU ENABLED? ===
//     // Config default ? overridden by Scheduler (most up-to-date)
//     bool useGPU = algoConfig_.useGPU;


// /*
// Why are there TWO switch (algoConfig_.algorithmType) blocks?
// This two-switch design is actually an excellent embedded software pattern known as Policy vs. Mechanism Separation.

// The First Switch (The Policy Enforcer): This block determines the Hardware Capability Policy. 
// In our ERL framework, the Reinforcement Learning agent (or static config) might request useGPU = true globally. 
// However, algorithms like GaussianBlur or Grayscale might only have CPU implementations in your framework. 
// This first switch acts as a sanitizer: it overrides the ERL's request and forces useGPU = false for algorithms that physically cannot run on the GPU. 
// This guarantees that useGPU is a 100% reliable "Single Source of Truth" before any execution begins.
// */
//     switch (algoConfig_.algorithmType) {
//         // ??????????????????? CPU-Only Algorithms (Never use GPU) ???????????????????
//         case AlgorithmType::Invert:
//         case AlgorithmType::Grayscale:
//         case AlgorithmType::EdgeDetection:
//         //case AlgorithmType::MedianFilter:
//         case AlgorithmType::GaussianBlur:
//             useGPU = false;
//             break;

//         // ??????????????????? Hybrid Algorithms (Respect GPU Toggle) ???????????????????
//         default:
//             break; // use current algoConfig_.useGPU
//     }

//     // ??????????????????? Dispatch with fallback ???????????????????

//     /*
//     The Second Switch (The Execution Mechanism): This block handles the actual Dispatch Mechanism. 
//     Because the first switch already sanitized the useGPU boolean, the second switch doesn't have to worry about whether a GPU kernel actually exists for the given algorithm. 
//     It simply checks if (useGPU) and confidently launches the CUDA kernel, knowing that if it reaches that block, it is safe to do so.
//     */
//     switch (algoConfig_.algorithmType) {
//         case AlgorithmType::Invert:
//             processInvertZeroCopy(inputFrame);
//             break;
//         case AlgorithmType::Grayscale:
//             processGrayscaleZeroCopy(inputFrame);
//             break;
//         case AlgorithmType::EdgeDetection:
//             processEdgeDetectionZeroCopy(inputFrame);
//             break;
//         case AlgorithmType::GaussianBlur:
//             processGaussianBlurZeroCopy(inputFrame);
//             break;

//         case AlgorithmType::SobelEdge:
//             if (useGPU) {
//                 try {
//                     cudaKernelTimeMs = timeCudaSectionMs([&]{
//                         AlgorithmConcreteKernels::launchSobelEdgeKernel(cudaRes_, inputFrame, processedBuffer_);
//                     });
//                 } catch (const std::runtime_error& e) {
//                     spdlog::warn("[AlgorithmConcrete] GPU Sobel failed ({}); falling back to CPU edge detection", e.what());
//                     algoConfig_.useGPU = false;
//                     processEdgeDetectionZeroCopy(inputFrame);
//                 }
//             } else {
//                 processEdgeDetectionZeroCopy(inputFrame);  // Best-effort CPU fallback
//             }
//             break;

//         case AlgorithmType::MedianFilter:
//             if (useGPU) {
//                 try {
//                     cudaKernelTimeMs = timeCudaSectionMs([&]{
//                         AlgorithmConcreteKernels::launchMedianFilterKernel(cudaRes_, inputFrame, processedBuffer_, algoConfig_.medianWindowSize);
//                     });
//                 } catch (const std::runtime_error& e) {
//                     spdlog::warn("[AlgorithmConcrete] GPU Median failed ({}); falling back to pass-through", e.what());
//                     algoConfig_.useGPU = false;
//                     std::memcpy(processedBuffer_.data(), inputFrame->dataPtr, inputFrame->size);
//                 }
//             } else {
//                 // No CPU median - pass-through to save energy
//                 std::memcpy(processedBuffer_.data(), inputFrame->dataPtr, inputFrame->size);
//             }
//             break;
//         case AlgorithmType::HistogramEqualization:
//             if (useGPU) {
//                 try {
//                     cudaKernelTimeMs = timeCudaSectionMs([&] {
//                         AlgorithmConcreteKernels::launchHistogramEqualizationKernel(
//                             cudaRes_, inputFrame, processedBuffer_);
//                     });
//                 } catch (const std::runtime_error& e) {
//                     spdlog::warn(
//                         "[AlgorithmConcrete] GPU HistEq failed ({}); falling back to CPU histogram equalization",
//                         e.what());
//                     processHistogramEqualizationCPUZeroCopy(inputFrame);
//                     cudaKernelTimeMs = 0.0;
//                 }
//             } else {
//                 processHistogramEqualizationCPUZeroCopy(inputFrame);
//                 cudaKernelTimeMs = 0.0;
//             }
//             break;

//         case AlgorithmType::HeterogeneousGaussianBlur:
//             if (useGPU) {
//                 try {
//                     // [Micro-scheduler] Read the continuous GPU workload fraction
//                     // the Scheduler published; the kernel splits rows CPU/GPU by it.
//                     double gpuSplit = runtimeControls_
//                         ? runtimeControls_->gpu_workload_split.load(std::memory_order_relaxed)
//                         : 1.0;
//                     cudaKernelTimeMs = timeCudaSectionMs([&]{
//                         AlgorithmConcreteKernels::launchHeterogeneousGaussianBlurKernel(
//                             cudaRes_, inputFrame, processedBuffer_, algoConfig_.blurRadius, gpuSplit);
//                     });
//                 } catch (const std::runtime_error& e) {
//                     spdlog::warn("[AlgorithmConcrete] GPU Gaussian failed ({}); falling back to CPU blur", e.what());
//                     algoConfig_.useGPU = false;
//                     processGaussianBlurZeroCopy(inputFrame);
//                 }
//             } else {
//                 processGaussianBlurZeroCopy(inputFrame);
//             }
//             break;

//         case AlgorithmType::OpticalFlow_LucasKanade:
//             processOpticalFlow(inputFrame);
//             goto metrics_only;

//         default:
//             std::memcpy(processedBuffer_.data(), inputFrame->dataPtr, inputFrame->size);
//             break;
//     }

//     // ??????????????????? Output Frame Construction ???????????????????
// 	// === CRITICAL FIX: ALWAYS CREATE OUTPUT FRAME AND PUSH TO QUEUE ===

//     {
//         auto data = std::make_shared<std::vector<uint8_t>>(std::move(processedBuffer_)); // copy processed result
//         outputFrame = std::make_shared<ZeroCopyFrameData>(
//             data, data->data(), 
// 		data->size(),
//             inputFrame->width, inputFrame->height, -1,
//             inputFrame->frameNumber,
//             inputFrame->captureSys, inputFrame->captureSteady
//         );

// //===============================================
// 	// PUSH TO DISPLAY QUEUE (this makes right side update in real-time)
//         if (outputQueueZeroCopy_) {
//             outputQueueZeroCopy_->push(outputFrame);
//             spdlog::debug("[AlgorithmConcrete] Pushed processed frame {} to outputQueueZeroCopy_ (refcount={})", 
//                          frameId, outputFrame.use_count());
//         } else {
//             spdlog::error("[AlgorithmConcrete] outputQueueZeroCopy_ is null - cannot show processed frame");
//         }
//     }

// metrics_only:
//     const auto t_end_steady = std::chrono::steady_clock::now();
//     const auto t_end_sys = std::chrono::system_clock::now();
//     lastProcessingTime_ = std::chrono::duration<double, std::milli>(t_end_steady - t_start_steady).count();

//     // -- Sliding 1-second FPS window (feeds metricAggregator / CSV) ----------
//     fpsWindowFrames_++;
//     fpsWindowProcMs_ += lastProcessingTime_;
//     totalFrames_++;
//     totalProcMs_ += lastProcessingTime_;

//     double currentMeasuredFps = 0.0;
//     {
//         const double fpsElapsed = std::chrono::duration<double>(
//             std::chrono::steady_clock::now() - fpsWindowStart_).count();

//         if (fpsElapsed >= 1.0) {
//             // Compute rate for the completed 1-second window
//             currentMeasuredFps = fpsWindowFrames_ / fpsElapsed;

//             // Reset for the next window
//             fpsWindowStart_   = std::chrono::steady_clock::now();
//             fpsWindowFrames_  = 0;
//             fpsWindowProcMs_  = 0.0;
//         } else if (fpsElapsed > 0.001) {
//             // Partial window: report running rate without resetting
//             currentMeasuredFps = fpsWindowFrames_ / fpsElapsed;
//         } else {
//             currentMeasuredFps = 0.0;
//         }
//     }
//     // Keep the class-level window accumulators in sync (used by updateMetrics)
//     windowFrames_++;
//     windowProcMs_ += lastProcessingTime_;

//     uint32_t dropped = 0;
//     if (haveLastSeq_ && frameId > lastFrameSeq_ + 1) {
//         dropped = static_cast<uint32_t>(std::min<uint64_t>(frameId - lastFrameSeq_ - 1, UINT32_MAX));
//     }
//     lastFrameSeq_ = frameId;
//     haveLastSeq_ = true;

// #ifdef __CUDACC__
//     size_t gpuFree = 0, gpuTotal = 0;
//     if (useGPU && cudaMemGetInfo(&gpuFree, &gpuTotal) == cudaSuccess && gpuFree < 80ULL * 1024 * 1024) {
//         spdlog::warn("[AlgorithmConcrete] GPU Low Memory: {} MiB free", gpuFree / (1024*1024));
//     }
// #endif

//     if (metricAggregator_) {
//         AlgorithmStats as(t_start_sys, t_end_sys);
//         as.inferenceTimeMs   = lastProcessingTime_;
//         as.fps               = currentMeasuredFps;
//         as.avgProcTimeMs     = windowFrames_ > 0 ? windowProcMs_ / windowFrames_ : 0.0;
//         as.totalProcTimeMs   = totalProcMs_;
//         as.cudaKernelTimeMs  = cudaKernelTimeMs;
//         as.droppedFrames     = dropped;
// #ifdef __CUDACC__
//         if (useGPU) {
//             as.gpuFreeMemory  = gpuFree;
//             as.gpuTotalMemory = gpuTotal;
//         }
// #endif
//         metricAggregator_->mergeAlgorithm(frameId, as);
//     }

//     return algoConfig_.algorithmType != AlgorithmType::OpticalFlow_LucasKanade;
// }

//================================================================================

inline void AlgorithmConcrete::updateMetrics(double elapsedSec, uint64_t frameSeq) {
    std::lock_guard<std::mutex> lock(metricMutex_);
    if (elapsedSec <= 0.0) elapsedSec = 1.0;

    // Read current window, then reset it so the next call measures a fresh interval
    const double fps   = windowFrames_ / elapsedSec;
    const double avgMs = windowFrames_ > 0 ? (windowProcMs_ / windowFrames_) : 0.0;

    fps_         = fps;
    lastFPS_     = fps;
    avgProcTime_ = avgMs;

    // -- RESET the class-level window --------------------------------------
    windowFrames_  = 0;
    windowProcMs_  = 0.0;
    windowStart_   = std::chrono::steady_clock::now();
    // ---------------------------------------------------------------------

    spdlog::info("[AlgorithmConcrete] Frame {} | FPS: {:.1f} | Avg: {:.2f}ms | {}", 
                 frameSeq, fps, avgMs, algorithmTypeToString(algoConfig_.algorithmType));
}

inline std::string AlgorithmConcrete::algorithmTypeToString(AlgorithmType type) const {
    switch (type) {
        case AlgorithmType::Invert: return "Invert";
        case AlgorithmType::Grayscale: return "Grayscale";
        case AlgorithmType::EdgeDetection: return "EdgeDetection";
        case AlgorithmType::GaussianBlur: return "GaussianBlur";
        case AlgorithmType::SobelEdge: return "SobelEdge";
        case AlgorithmType::MedianFilter: return "MedianFilter";
        case AlgorithmType::HistogramEqualization: return "HistogramEqualization";
        case AlgorithmType::HeterogeneousGaussianBlur: return "HeterogeneousGaussianBlur";
        case AlgorithmType::OpticalFlow_LucasKanade: return "OpticalFlow_LK";
        case AlgorithmType::Mandelbrot: return "Mandelbrot";
        case AlgorithmType::MultiThreadedInvert: return "MultiThreadedInvert";
        default: return "Unknown";
    }
}

inline void AlgorithmConcrete::reportError(const std::string& msg) {
    if (errorCallback_) errorCallback_(msg);
    else spdlog::error("[AlgorithmConcrete] {}", msg);
}

//================================================================================
// parallelFor ? HRL ADAPTIVE VERSION
//================================================================================
// [F8 - P2 design note, implement with a compile-test loop after run 2]
// This spawns and joins threads PER CALL (~26-52 spawns/s at 13 fps), which
// both costs ~0.2-0.5 ms/frame on the A57 and pollutes the concurrency
// gene's measured signal (each increment buys parallelism PLUS spawn cost).
// The production replacement is a persistent worker pool with three hard
// requirements: (1) it must HONOR the gene - exactly numThreads-1 helpers
// participate per call (caller works too); a fixed always-all-workers pool
// re-creates F21 from the other direction; (2) a thread_local in-worker
// guard so nested parallelFor calls degrade to serial instead of
// deadlocking; (3) exit-safe lifetime (function-local static pool, joined
// at process exit) so it never interacts with this class's teardown.
inline void AlgorithmConcrete::parallelFor(size_t start, size_t end, const std::function<void(size_t)>& func) {
    // 1. Determine thread count: Priority to RuntimeControls (HRL), fallback to Config
    size_t numThreads = algoConfig_.concurrencyLevel;
    
    if (runtimeControls_) {
        int dynLevel = runtimeControls_->concurrency_level.load(std::memory_order_relaxed);
        if (dynLevel > 0) {
            numThreads = static_cast<size_t>(dynLevel);
        }
    }

    // 2. Serial fallback for genuinely tiny workloads or single-thread mode.
    // [F21-FIX] The old absolute guard `(end - start) < 1000` made EVERY
    // row-parallel call (parallelFor(0, h, ...) with h = 240 at 320x240 -
    // EdgeDetection, GaussianBlur, and the CPU share of heterogeneous
    // paths) run unconditionally SERIAL, so the ERL's concurrency_level
    // gene actuated nothing on those workloads. Run 2026-07-20 corroborated
    // it: runtime_concurrency varied 2..4 across generations with no
    // throughput coupling. The guard now scales with the requested thread
    // count (>= 16 iterations per worker): 240 rows / 4 threads = 60 rows
    // ~= 180k ops per worker, far above thread-spawn cost, so row loops
    // parallelize; genuinely tiny loops (< numThreads*16) stay serial.
    if (numThreads <= 1 || (end - start) < numThreads * 16) {
        for (size_t i = start; i < end; ++i) func(i);
        return;
    }

    // 3. Parallel Execution
    std::vector<std::thread> workers;
    workers.reserve(numThreads);
    
    const size_t chunkSize = (end - start + numThreads - 1) / numThreads;

    for (size_t t = 0; t < numThreads; ++t) {
        size_t s = start + t * chunkSize;
        if (s >= end) break;
        size_t e = std::min(end, s + chunkSize);
        
        workers.emplace_back([=] { 
            for (size_t j = s; j < e; j++) func(j); 
        });
    }
    
    for (auto& w : workers) w.join();
}

// CPU Operations
inline void AlgorithmConcrete::processInvertZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
    const uint8_t* in = static_cast<const uint8_t*>(frame->dataPtr);
    parallelFor(0, frame->size, [this, in](size_t i) { processedBuffer_[i] = ~in[i]; });
}

inline void AlgorithmConcrete::processGrayscaleZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
    const uint8_t* in = static_cast<const uint8_t*>(frame->dataPtr);
    parallelFor(0, frame->size, [this, in](size_t i) { processedBuffer_[i] = (i % 2 == 0) ? in[i] : 128; });
}

inline void AlgorithmConcrete::processEdgeDetectionZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
    const uint8_t* in = static_cast<const uint8_t*>(frame->dataPtr);
    const int w = frame->width, h = frame->height;
    parallelFor(0, h, [this, in, w](size_t y) {
        for (int x = 0; x < w; ++x) {
            int idx = y * w * 2 + x * 2;
            if (x > 0 && x < w - 1) {
                int grad = std::abs(static_cast<int>(in[idx]) - static_cast<int>(in[idx - 2]));
                processedBuffer_[idx] = static_cast<uint8_t>(grad);
                processedBuffer_[idx + 1] = 128;
            } else {
                processedBuffer_[idx] = in[idx];
                processedBuffer_[idx + 1] = in[idx + 1];
            }
        }
    });
}


//==============================================================================================================
// ====================================================================================
// ACTION REQUIRED:
// Open 'AlgorithmConcrete_new.h'
// Find: inline void AlgorithmConcrete::processGaussianBlurZeroCopy(...)
// Replace the ENTIRE function with this structurally safe version:
// ====================================================================================

inline void AlgorithmConcrete::processGaussianBlurZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
    const int r = algoConfig_.blurRadius;
    const int w = frame->width, h = frame->height;
    const uint8_t* in = static_cast<const uint8_t*>(frame->dataPtr);
    std::vector<uint8_t> temp(frame->size);

    // ------------------------------------------------------------------
    // CRITICAL FIX: PRE-CALCULATE WEIGHTS
    // Prevents 112ms bottleneck and subsequent V4L2 camera driver crash.
    // ------------------------------------------------------------------
    if (r <= 0) return; 
    std::vector<float> weights(2 * r + 1);
    float wsum_total = 0.0f;
    for (int d = -r; d <= r; ++d) {
        weights[d + r] = std::exp(-(d * d) / (2.0f * r * r));
        wsum_total += weights[d + r];
    }
    // Normalize weights so they sum to 1.0
    for (int d = -r; d <= r; ++d) {
        weights[d + r] /= wsum_total;
    }

    // Horizontal pass
    parallelFor(0, h, [in, w, r, &temp, &weights](size_t y) {
        for (int x = 0; x < w; ++x) {
            float sum = 0.0f;
            for (int d = -r; d <= r; ++d) {
                int xx = utils::local_clamp(x + d, 0, w - 1);
                sum += in[y * w * 2 + xx * 2] * weights[d + r];
            }
            temp[y * w * 2 + x * 2] = static_cast<uint8_t>(sum);
            temp[y * w * 2 + x * 2 + 1] = 128; // U/V placeholder
        }
    });

    // Vertical pass
    parallelFor(0, w, [&temp, w, h, r, this, &weights](size_t x) {
        for (int y = 0; y < h; ++y) {
            float sum = 0.0f;
            for (int d = -r; d <= r; ++d) {
                int yy = utils::local_clamp(static_cast<int>(y) + d, 0, h - 1);
                sum += temp[yy * w * 2 + x * 2] * weights[d + r];
            }
            processedBuffer_[y * w * 2 + x * 2] = static_cast<uint8_t>(sum);
            processedBuffer_[y * w * 2 + x * 2 + 1] = 128; // U/V placeholder
        }
    });
}
//==============================================================================================================
// inline void AlgorithmConcrete::processGaussianBlurZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
//     const int r = algoConfig_.blurRadius;
//     const int w = frame->width, h = frame->height;
//     const uint8_t* in = static_cast<const uint8_t*>(frame->dataPtr);
//     std::vector<uint8_t> temp(frame->size);

//     // 1. PRE-CALCULATE WEIGHTS to fix 112ms bottleneck
//     if (r <= 0) return; // Prevent division by zero
//     std::vector<float> weights(2 * r + 1);
//     float wsum_total = 0.0f;
//     for (int d = -r; d <= r; ++d) {
//         weights[d + r] = std::exp(-(d * d) / (2.0f * r * r));
//         wsum_total += weights[d + r];
//     }
    
//     // Normalize weights so they sum to 1.0
//     for (int d = -r; d <= r; ++d) {
//         weights[d + r] /= wsum_total;
//     }

//     // 2. Horizontal pass
//     parallelFor(0, h, [in, w, r, &temp, &weights](size_t y) {
//         for (int x = 0; x < w; ++x) {
//             float sum = 0.0f;
//             for (int d = -r; d <= r; ++d) {
//                 int xx = utils::local_clamp(x + d, 0, w - 1);
//                 sum += in[y * w * 2 + xx * 2] * weights[d + r];
//             }
//             temp[y * w * 2 + x * 2] = static_cast<uint8_t>(sum);
//             temp[y * w * 2 + x * 2 + 1] = 128; // U/V placeholder
//         }
//     });

//     // 3. Vertical pass
//     parallelFor(0, w, [&temp, w, h, r, this, &weights](size_t x) {
//         for (int y = 0; y < h; ++y) {
//             float sum = 0.0f;
//             for (int d = -r; d <= r; ++d) {
//                 int yy = utils::local_clamp(static_cast<int>(y) + d, 0, h - 1);
//                 sum += temp[yy * w * 2 + x * 2] * weights[d + r];
//             }
//             processedBuffer_[y * w * 2 + x * 2] = static_cast<uint8_t>(sum);
//             processedBuffer_[y * w * 2 + x * 2 + 1] = 128;
//         }
//     });
// }
//==============================================================================================================

// inline void AlgorithmConcrete::processGaussianBlurZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
//     const int r = algoConfig_.blurRadius;
//     const int w = frame->width, h = frame->height;
//     const uint8_t* in = static_cast<const uint8_t*>(frame->dataPtr);
//     std::vector<uint8_t> temp(frame->size);

//     parallelFor(0, h, [in, w, r, &temp](size_t y) {
//         for (int x = 0; x < w; ++x) {
//             float sum = 0, wsum = 0;
//             for (int d = -r; d <= r; ++d) {
//                 int xx = utils::local_clamp(x + d, 0, w - 1);
//                 float wt = std::exp(-(d * d) / (2.0f * r * r));
//                 sum += in[y * w * 2 + xx * 2] * wt;
//                 wsum += wt;
//             }
//             temp[y * w * 2 + x * 2] = static_cast<uint8_t>(sum / wsum);
//             temp[y * w * 2 + x * 2 + 1] = 128;
//         }
//     });

//     parallelFor(0, w, [&temp, w, h, r, this](size_t x) {
//         for (int y = 0; y < h; ++y) {
//             float sum = 0, wsum = 0;
//             for (int d = -r; d <= r; ++d) {
//                 int yy = utils::local_clamp(static_cast<int>(y) + d, 0, h - 1);
//                 float wt = std::exp(-(d * d) / (2.0f * r * r));
//                 sum += temp[yy * w * 2 + x * 2] * wt;
//                 wsum += wt;
//             }
//             processedBuffer_[y * w * 2 + x * 2] = static_cast<uint8_t>(sum / wsum);
//             processedBuffer_[y * w * 2 + x * 2 + 1] = 128;
//         }
//     });
// }

// Note: The above CPU implementations are intentionally simple and not optimized for performance.

//===============================================================================================================================
inline void AlgorithmConcrete::processMedianFilterCPUZeroCopy(
    const std::shared_ptr<ZeroCopyFrameData>& frame)
{
    const int w = frame->width;
    const int h = frame->height;
    const int windowSize = algoConfig_.medianWindowSize;
    const int radius = windowSize / 2;

    const uint8_t* in =
        static_cast<const uint8_t*>(frame->dataPtr);

    parallelFor(0, h, [&, this](size_t y) {
        for (int x = 0; x < w; ++x) {

            std::array<uint8_t, 25> window{};
            int count = 0;

            for (int dy = -radius; dy <= radius; ++dy) {
                const int yy =
                    utils::local_clamp(
                        static_cast<int>(y) + dy, 0, h - 1);

                for (int dx = -radius; dx <= radius; ++dx) {
                    const int xx =
                        utils::local_clamp(x + dx, 0, w - 1);

                    window[count++] =
                        in[(yy * w + xx) * 2];
                }
            }

            auto mid = window.begin() + count / 2;

            std::nth_element(
                window.begin(),
                mid,
                window.begin() + count);

            const int idx =
                (static_cast<int>(y) * w + x) * 2;

            processedBuffer_[idx] = *mid;
            processedBuffer_[idx + 1] = 128;
        }
    });
}
//===============================================================================================================================

inline void AlgorithmConcrete::processMultiThreadedInvert(const std::shared_ptr<ZeroCopyFrameData>& frame) {
    const int numThreads = 4; // Jetson Nano has 4 ARM cores
    const size_t dataSize = frame->width * frame->height;
    const size_t chunkSize = dataSize / numThreads;
    std::vector<std::thread> threads;

    // Spawn threads to process chunks of the image concurrently
    for (int i = 0; i < numThreads; ++i) {
        size_t startIdx = i * chunkSize;
        size_t endIdx = (i == numThreads - 1) ? dataSize : startIdx + chunkSize;
        
        threads.emplace_back([this, frame, startIdx, endIdx]() {
            for (size_t j = startIdx; j < endIdx; ++j) {
                // Replace this:
                //processedBuffer_[j] = 255 - frame->dataPtr[j];

                // With this:
                uint8_t* data = static_cast<uint8_t*>(frame->dataPtr);
                processedBuffer_[j] = 255 - data[j];
            }
        });
    }

    // Wait for all cores to finish
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }
}

inline void AlgorithmConcrete::processMandelbrot(const std::shared_ptr<ZeroCopyFrameData>& frame) {
    if (algoConfig_.useGPU) {
        try {
            AlgorithmConcreteKernels::launchMandelbrotKernel(cudaRes_, frame, processedBuffer_);
            return;
        } catch (const std::exception& e) {
            reportError("[Mandelbrot GPU ERROR] " + std::string(e.what()) + ". Falling back to CPU.");
            // Fall through to CPU implementation
        }
    }

    // --- CPU Fallback implementation ---
    const int width = frame->width;
    const int height = frame->height;
    const int max_iter = 100;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float cx = (x * 3.0f / (float)width) - 2.0f;
            float cy = (y * 3.0f / (float)height) - 1.5f;
            float zx = 0.0f, zy = 0.0f;
            int iter = 0;

            while (zx * zx + zy * zy <= 4.0f && iter < max_iter) {
                float tmp = zx * zx - zy * zy + cx;
                zy = 2.0f * zx * zy + cy;
                zx = tmp;
                iter++;
            }
            
            processedBuffer_[y * width + x] = (iter == max_iter) ? 0 : (uint8_t)((iter * 255) / max_iter);
        }
    }
}

// CUDA Wrappers
inline void AlgorithmConcrete::processSobelEdgeZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
    AlgorithmConcreteKernels::launchSobelEdgeKernel(cudaRes_, frame, processedBuffer_);
}

inline void AlgorithmConcrete::processMedianFilterZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
    AlgorithmConcreteKernels::launchMedianFilterKernel(cudaRes_, frame, processedBuffer_, algoConfig_.medianWindowSize);
}

// inline void AlgorithmConcrete::processHistogramEqualizationZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
//     AlgorithmConcreteKernels::launchHistogramEqualizationKernel(cudaRes_, frame, processedBuffer_);
// }

//=========================================================================================================================


inline void AlgorithmConcrete::processHistogramEqualizationZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
    // CPU implementation matching the CUDA HistEq path:
    //   - Build histogram over the Y/luma channel only.
    //   - Compute CDF and min non-zero CDF.
    //   - Remap Y using (cdf[Y] - minCdf) / (N - minCdf) * 255.
    //   - Set chroma byte to 128, matching the existing CUDA remapKernel.
    //
    // The framework stores frames in the same 2-bytes-per-pixel convention used
    // by the CUDA kernels: byte 0 = Y, byte 1 = chroma placeholder.
    if (!frame || !frame->dataPtr || frame->width <= 0 || frame->height <= 0 || frame->size == 0) {
        spdlog::warn("[AlgorithmConcrete] CPU HistEq skipped: invalid frame");
        return;
    }

    const uint8_t* in = static_cast<const uint8_t*>(frame->dataPtr);
    const int width = frame->width;
    const int height = frame->height;
    const int totalPixels = width * height;
    const size_t expectedSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 2ULL;

    if (totalPixels <= 0 || frame->size < expectedSize) {
        spdlog::warn("[AlgorithmConcrete] CPU HistEq size mismatch: frameSize={} expectedAtLeast={}",
                     frame->size, expectedSize);
        if (processedBuffer_.size() < frame->size) {
            processedBuffer_.resize(frame->size);
        }
        std::memcpy(processedBuffer_.data(), in, frame->size);
        return;
    }

    if (processedBuffer_.size() < frame->size) {
        processedBuffer_.resize(frame->size);
    }

    std::vector<int> hist(256, 0);
    std::vector<int> cdf(256, 0);

    // Histogram over Y channel.
    for (int y = 0; y < height; ++y) {
        const int rowBase = y * width * 2;
        for (int x = 0; x < width; ++x) {
            const int idx = rowBase + x * 2;
            ++hist[in[idx]];
        }
    }

    int running = 0;
    int minCdf = 0;
    for (int i = 0; i < 256; ++i) {
        running += hist[i];
        cdf[i] = running;
        if (minCdf == 0 && running > 0) {
            minCdf = running;
        }
    }

    const int denom = totalPixels - minCdf;

    // Degenerate image: all pixels have the same luma. Keep a valid neutral output.
    if (denom <= 0) {
        for (int y = 0; y < height; ++y) {
            const int rowBase = y * width * 2;
            for (int x = 0; x < width; ++x) {
                const int idx = rowBase + x * 2;
                processedBuffer_[idx]     = in[idx];
                processedBuffer_[idx + 1] = 128;
            }
        }
        return;
    }

    parallelFor(0, height, [this, in, width, &cdf, minCdf, denom](size_t y) {
        const int rowBase = static_cast<int>(y) * width * 2;
        for (int x = 0; x < width; ++x) {
            const int idx = rowBase + x * 2;
            const int yIn = static_cast<int>(in[idx]);
            const float norm = static_cast<float>(cdf[yIn] - minCdf) / static_cast<float>(denom);
            int yOut = static_cast<int>(norm * 255.0f);
            yOut = utils::local_clamp(yOut, 0, 255);

            processedBuffer_[idx]     = static_cast<uint8_t>(yOut);
            processedBuffer_[idx + 1] = 128;
        }
    });
}

//=====================================================================================================
inline void AlgorithmConcrete::processHistogramEqualizationCPUZeroCopy(
    const std::shared_ptr<ZeroCopyFrameData>& frame)
{
    if (!frame || !frame->dataPtr || frame->width <= 0 || frame->height <= 0) {
        return;
    }

    const int width = frame->width;
    const int height = frame->height;
    const size_t dataSize = frame->size;
    const uint8_t* input = static_cast<const uint8_t*>(frame->dataPtr);

    processedBuffer_.resize(dataSize);

    std::array<int, 256> hist{};
    std::array<int, 256> cdf{};

    const int totalPixels = width * height;

    for (int y = 0; y < height; ++y) {
        const int rowBase = y * width * 2;
        for (int x = 0; x < width; ++x) {
            const int idx = rowBase + x * 2;
            ++hist[input[idx]];
        }
    }

    cdf[0] = hist[0];
    for (int i = 1; i < 256; ++i) {
        cdf[i] = cdf[i - 1] + hist[i];
    }

    int minCdf = 0;
    for (int i = 0; i < 256; ++i) {
        if (cdf[i] > 0) {
            minCdf = cdf[i];
            break;
        }
    }

    const int denom = totalPixels - minCdf;

    for (int y = 0; y < height; ++y) {
        const int rowBase = y * width * 2;
        for (int x = 0; x < width; ++x) {
            const int idx = rowBase + x * 2;
            uint8_t newY = input[idx];

            if (denom > 0) {
                const float norm =
                    static_cast<float>(cdf[input[idx]] - minCdf) /
                    static_cast<float>(denom);

                int mapped = static_cast<int>(norm * 255.0f);
                mapped = std::max(0, std::min(255, mapped));
                newY = static_cast<uint8_t>(mapped);
            }

            processedBuffer_[idx] = newY;
            processedBuffer_[idx + 1] = 128;
        }
    }
}

//=========================================================================================================================

inline void AlgorithmConcrete::processHeterogeneousGaussianBlurZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
    double gpuSplit = runtimeControls_
        ? runtimeControls_->gpu_workload_split.load(std::memory_order_relaxed)
        : 1.0;
    AlgorithmConcreteKernels::launchHeterogeneousGaussianBlurKernel(cudaRes_, frame, processedBuffer_, algoConfig_.blurRadius, gpuSplit);
}

inline void AlgorithmConcrete::processOpticalFlow(const std::shared_ptr<ZeroCopyFrameData>& frame) {
    if (!frame || !frame->dataPtr) return;
    if (previousFrame_) {
        std::shared_ptr<ZeroCopyFrameData> flowOutput;
        opticalFlowProcessor_->computeOpticalFlow(previousFrame_, frame, flowOutput);
        if (outputQueueZeroCopy_ && flowOutput) {
            outputQueueZeroCopy_->push(flowOutput);
        }
    }
    previousFrame_ = frame;
}

// CUDA Helpers
inline void AlgorithmConcrete::checkCudaError(cudaError_t err, const std::string& context) {
    if (err != cudaSuccess) {
        reportError("[CUDA ERROR] " + context + ": " + cudaGetErrorString(err));
    }
}

inline double AlgorithmConcrete::timeCudaSectionMs(const std::function<void()>& launch) {
#ifdef __CUDACC__
    cudaEvent_t start, stop;
    cudaEventCreate(&start);
    cudaEventCreate(&stop);
    cudaEventRecord(start, 0);
    launch();
    cudaEventRecord(stop, 0);
    cudaEventSynchronize(stop);
    float ms = 0;
    cudaEventElapsedTime(&ms, start, stop);
    cudaEventDestroy(start);
    cudaEventDestroy(stop);
    return ms;
#else
    auto t0 = std::chrono::high_resolution_clock::now();
    launch();
    return std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
#endif
}





// //======================================================================================================================================
// //================================================================================
// // AlgorithmConcrete_new.h
// // FINAL PRODUCTION VERSION ? 100% JETSON NANO 2GB COMPATIBLE + ENHANCED
// // Fixed: C++11/14 Compliance, Initialization Order, Namespace Scope
// //================================================================================

// #pragma once


// //#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG
// #define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO

// #include <cstddef>
// #include <cstring>
// #include <limits>
// #include <vector>
// #include <mutex>
// #include <atomic>
// #include <thread>
// #include <functional>
// #include <chrono>
// #include <cmath>
// #include <algorithm>
// #include <memory>

// #include <array>

// #include <spdlog/spdlog.h>
// #include <cuda_runtime.h>
// #include "CudaUtiles.h"


// #include "../Interfaces/IAlgorithm.h"
// #include "../SharedStructures/ZeroCopyFrameData.h"
// #include "../SharedStructures/SharedQueue.h"
// #include "../SharedStructures/ThreadManager.h"
// #include "../SharedStructures/AlgorithmConfig.h"
// #include "../SharedStructures/LucasKanadeOpticalFlow.h"
// #include "../SharedStructures/allModulesStatcs.h"
// #include "../Interfaces/ISystemMetricsAggregator.h"
// #include "../Others/utils.h"
// #include "AlgorithmConcreteKernels.cuh"

// // [FIX] Include Scheduler to access hrl::chooseCpus and hrl::setThreadAffinity
// #include "../../Module/Scheduler.h" 
// #include "../../Module/RuntimeControls.h"

// // within util.h
// // // [FIX] Local clamp for C++14/11 compatibility on Jetson Nano
// // namespace {
// //     template <typename T>
// //     constexpr const T& local_clamp(const T& v, const T& lo, const T& hi) {
// //         return (v < lo) ? lo : (hi < v) ? hi : v;
// //     }
// // }

// using namespace hrl;

// class AlgorithmConcrete : public IAlgorithm {
// public:
//     explicit AlgorithmConcrete(ThreadManager& threadManager);
//     //==========================================================================
//     // Constructor / Destructor
//     //==========================================================================
//     AlgorithmConcrete(std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> inputQueue,
//                       std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> outputQueue,
//                       ThreadManager& threadManager,
//                       std::shared_ptr<ISystemMetricsAggregator> aggregator,
//                       std::shared_ptr<hrl::RuntimeControls> controls = nullptr);

//     ~AlgorithmConcrete() override;
//     //==========================================================================
//     // Interface Implementation
//     //==========================================================================
//     static std::shared_ptr<IAlgorithm> createAlgorithmZeroCopy(
//         AlgorithmType type,
//         std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> inputQueue,
//         std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> outputQueue,
//         ThreadManager& threadManager,
//         std::shared_ptr<ISystemMetricsAggregator> aggregator);

//     void startAlgorithm() override;
//     void stopAlgorithm() override;
//     bool isRunning() const { return running_; }
    
//     bool processFrameZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& inputFrame,
//                               std::shared_ptr<ZeroCopyFrameData>& outputFrame) override;
    
//     bool configure(const AlgorithmConfig& config) override;
    
//     void setErrorCallback(std::function<void(const std::string&)> cb) override;
    
//     std::tuple<double, double> getAlgorithmMetrics() const override { return {getLastFPS(), lastProcessingTime_}; }
//     double getLastFPS() const override { return lastFPS_; }
//     double getFps() const override { return lastFPS_; }
//     double getAverageProcTime() const override { return avgProcTime_; }
//     const uint8_t* getProcessedBuffer() const override { return processedBuffer_.data(); }
//     void setAlgorithmType(AlgorithmType newType) override;

// private:
//     void threadLoopZeroCopy();
//     void updateMetrics(double elapsedSec, uint64_t frameSeq);
//     std::string algorithmTypeToString(AlgorithmType type) const;
//     void reportError(const std::string& msg);

//     // [FIX] Declaration signature matched to definition
//     void parallelFor(size_t start, size_t end, const std::function<void(size_t)>& func);

//     // CPU algorithms
//     void processInvertZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
//     void processGrayscaleZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
//     void processEdgeDetectionZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
//     void processGaussianBlurZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);

//     // CPU multithreaded algorithms (fallbacks for GPU)
//     void processMultiThreadedInvert(const std::shared_ptr<ZeroCopyFrameData>& frame) ; // [FIX] Added missing declaration for multi-threaded invert

    

//     // CUDA wrappers
//     void processSobelEdgeZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
//     void processMedianFilterZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);

//     // 
//     void processMandelbrot(const std::shared_ptr<ZeroCopyFrameData>& frame); // [FIX] Added missing declaration for Mandelbrot (CPU)
    
//     void processHistogramEqualizationZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
//     void processHeterogeneousGaussianBlurZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
//     void processHistogramEqualizationCPUZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
//     void processOpticalFlow(const std::shared_ptr<ZeroCopyFrameData>& frame);

//     // CUDA helpers
//     void checkCudaError(cudaError_t err, const std::string& context);
//     double timeCudaSectionMs(const std::function<void()>& launch);

//     // --- State Variables (Order matches initialization for safety) ---
//     std::atomic<bool> running_{false};
    
//     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> inputQueueZeroCopy_;
//     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> outputQueueZeroCopy_;
    
//     AlgorithmConfig algoConfig_;
//     std::function<void(const std::string&)> errorCallback_;

//     // Metrics
//     mutable std::mutex metricMutex_;
//     double fps_ = 0.0;
//     double avgProcTime_ = 0.0;
//     std::vector<uint8_t> processedBuffer_;
    
//     mutable double lastFPS_ = 0.0;
//     mutable double lastProcessingTime_ = 0.0;

//     uint64_t windowFrames_ = 0;
//     double windowProcMs_ = 0.0;
//     std::chrono::steady_clock::time_point windowStart_;

    
//     // Sliding 1-second window for processFrameZeroCopy FPS (feeds metricAggregator)
//     std::chrono::steady_clock::time_point fpsWindowStart_;
//     uint64_t fpsWindowFrames_ = 0;
//     double   fpsWindowProcMs_ = 0.0;

//     uint64_t totalFrames_ = 0;
//     double totalProcMs_ = 0.0;

//     uint64_t lastFrameSeq_ = 0;
//     bool haveLastSeq_ = false;

//     // Persistent GPU resources ? allocated once at startAlgorithm(), freed at stopAlgorithm().
//     // Eliminates per-frame cudaMalloc/cudaFree and enables async CUDA stream overlap.
//     AlgorithmConcreteKernels::CudaResources cudaRes_;

//     // Dependencies
//     ThreadManager& threadManager_;
//     std::unique_ptr<LucasKanadeOpticalFlow> opticalFlowProcessor_;
//     std::shared_ptr<ZeroCopyFrameData> previousFrame_;
//     std::shared_ptr<ISystemMetricsAggregator> metricAggregator_;
//     std::shared_ptr<hrl::RuntimeControls> runtimeControls_;
// };

// //================================================================================
// // Inline Implementation
// //================================================================================

// inline AlgorithmConcrete::AlgorithmConcrete(ThreadManager& threadManager)
//     : running_(false),
//       inputQueueZeroCopy_(nullptr),
//       outputQueueZeroCopy_(nullptr),
//       // [FIX] Removed lastUpdateTime_ (not in class), initialized windowStart_
//       windowStart_(std::chrono::steady_clock::now()),
//     fpsWindowStart_(std::chrono::steady_clock::now()),
//       threadManager_(threadManager) {
//     spdlog::warn("[AlgorithmConcrete] Default constructor used ? no queues!");
// }

// // [FIX] Constructor Initialization List Reordered to match declaration order
// inline AlgorithmConcrete::AlgorithmConcrete(
//     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> inputQueue,
//     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> outputQueue,
//     ThreadManager& threadManager,
//     std::shared_ptr<ISystemMetricsAggregator> aggregator,
//     std::shared_ptr<hrl::RuntimeControls> controls)
//     : running_(false),
//       inputQueueZeroCopy_(std::move(inputQueue)),
//       outputQueueZeroCopy_(std::move(outputQueue)),
//       windowStart_(std::chrono::steady_clock::now()),
//     fpsWindowStart_(std::chrono::steady_clock::now()),
//       threadManager_(threadManager),
//       metricAggregator_(aggregator),
//       runtimeControls_(controls) {
    
//     if (!inputQueueZeroCopy_ || !outputQueueZeroCopy_) {
//         throw std::runtime_error("AlgorithmConcrete: input/output queues cannot be null");
//     }
//     spdlog::info("[AlgorithmConcrete] Constructed with ZeroCopy queues");
// }

// inline AlgorithmConcrete::~AlgorithmConcrete() {
//     stopAlgorithm();
// }

// inline std::shared_ptr<IAlgorithm> AlgorithmConcrete::createAlgorithmZeroCopy(
//     AlgorithmType type,
//     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> inputQueue,
//     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> outputQueue,
//     ThreadManager& threadManager,
//     std::shared_ptr<ISystemMetricsAggregator> aggregator) {
    
//     auto algo = std::make_shared<AlgorithmConcrete>(std::move(inputQueue), std::move(outputQueue), threadManager, aggregator, nullptr);
    
//     // [FIX] C++11 Compatible Struct Initialization (No designated initializers)
//     AlgorithmConfig cfg;
//     cfg.algorithmType = type;
//     algo->configure(cfg);
    
//     return algo;
// }

// inline void AlgorithmConcrete::startAlgorithm() {
//     if (running_) {
//         spdlog::warn("[AlgorithmConcrete] Already running");
//         return;
//     }
//     running_ = true;
//     windowStart_ = std::chrono::steady_clock::now();
//     windowFrames_ = 0;
//     windowProcMs_ = 0.0;
    
//     fpsWindowStart_  = std::chrono::steady_clock::now();
//     fpsWindowFrames_ = 0;
//     fpsWindowProcMs_ = 0.0;

//     // GPU resource pre-allocation is deferred to the first GPU frame dispatch,
//     // since frame dimensions (width x height) are not known until then.
//     // See: initCudaResources() call inside each launcher.

//     threadManager_.addThread("AlgorithmProcessingZeroCopy",
//         std::thread(&AlgorithmConcrete::threadLoopZeroCopy, this));

//     spdlog::info("[AlgorithmConcrete] Started ? {}", algorithmTypeToString(algoConfig_.algorithmType));
// }

// inline void AlgorithmConcrete::stopAlgorithm() {
//     if (!running_) return;

//     running_ = false;

//     if (inputQueueZeroCopy_)  inputQueueZeroCopy_->stop();
//     if (outputQueueZeroCopy_) outputQueueZeroCopy_->stop();

//     threadManager_.joinThreadsFor("AlgorithmProcessingZeroCopy");

//     // Release persistent GPU resources after the processing thread has exited
//     AlgorithmConcreteKernels::destroyCudaResources(cudaRes_);

//     spdlog::info("[AlgorithmConcrete] Stopped cleanly");
// }

// inline bool AlgorithmConcrete::configure(const AlgorithmConfig& config) {
//     algoConfig_ = config;

//     if (algoConfig_.algorithmType == AlgorithmType::OpticalFlow_LucasKanade) {
//         opticalFlowProcessor_ = std::make_unique<LucasKanadeOpticalFlow>(algoConfig_.opticalFlowConfig);
//         previousFrame_.reset();
//     } else {
//         opticalFlowProcessor_.reset();
//     }

//     spdlog::info("[AlgorithmConcrete] Configured ? {}", algorithmTypeToString(config.algorithmType));
//     return true;
// }

// inline void AlgorithmConcrete::setErrorCallback(std::function<void(const std::string&)> cb) {
//     errorCallback_ = std::move(cb);
// }

// inline void AlgorithmConcrete::setAlgorithmType(AlgorithmType newType) {
//     std::lock_guard<std::mutex> lock(metricMutex_);
//     if (algoConfig_.algorithmType == newType) return;

//     algoConfig_.algorithmType = newType;
//     if (newType == AlgorithmType::OpticalFlow_LucasKanade) {
//         opticalFlowProcessor_ = std::make_unique<LucasKanadeOpticalFlow>(algoConfig_.opticalFlowConfig);
//         previousFrame_.reset();
//     } else {
//         opticalFlowProcessor_.reset();
//     }
// }

// //================================================================================
// // MAIN THREAD LOOP ? ADAPTIVE & ROBUST
// //================================================================================
// //==============================================================================================================
// // AlgorithmConcrete::threadLoopZeroCopy() ? OPTIMIZED WITH TIMER DIAGNOSTICS

// /*Key change: Removed the line outputQueueZeroCopy_->push(outputFrame); inside the if (proc_ok && outputFrame) block. 
// The frame is already pushed once by processFrameZeroCopy() (or by processOpticalFlow() for that algorithm type).
// */
// //==============================================================================================================

// inline void AlgorithmConcrete::threadLoopZeroCopy() {
//     spdlog::info("[AlgorithmConcrete] Thread started (starvation timeout: 5s)");

//     // ========== TIMER INITIALIZATION ==========
//     auto starvationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
//     bool firstFrame = true;
    
//     // Metric window tracking
//     auto windowStart = std::chrono::steady_clock::now();
//     int windowFrames = 0;
//     double windowProcMs = 0.0;
    
//     // Adaptation counters
//     uint64_t adaptationCounter = 0;
//     int lastConcurrency = -1;
//     hrl::Affinity lastAffinity = hrl::Affinity::Spread;

//     while (running_) {
//         // ========== 1. POP FROM QUEUE WITH TIMEOUT TRACKING ==========
//         std::shared_ptr<ZeroCopyFrameData> inputFrame;
        
//         auto pop_start = std::chrono::high_resolution_clock::now();
//         const int pop_timeout_ms = 100;  // ? Explicit timeout constant
        
//         //if (!inputQueueZeroCopy_->pop(inputFrame, std::chrono::milliseconds(pop_timeout_ms))) {
//         if (!inputQueueZeroCopy_->pop(inputFrame)) {
//             auto pop_elapsed_ms = std::chrono::duration<double, std::milli>(
//                 std::chrono::high_resolution_clock::now() - pop_start).count();
            
//             // ? LOG DIAGNOSTIC INFO
//             spdlog::warn("[AlgorithmConcrete] Queue pop TIMEOUT (waited {}ms, queue_size={})", 
//                         pop_elapsed_ms, inputQueueZeroCopy_->size());
            
//             // ? STARVATION CHECK: Has producer died?
//             if (std::chrono::steady_clock::now() > starvationDeadline) {
//                 if (firstFrame) {
//                     reportError("ALGORITHM STARVED ? NO FRAME FOR 5 SECONDS (startup)");
//                 } else {
//                     reportError("ALGORITHM STARVED ? NO FRAME FOR 5 SECONDS (mid-run)");
//                 }
//                 running_ = false;
//                 break;
//             }
//             continue;
//         }
        
//         // ? FRAME RECEIVED: Reset starvation timer for next batch
//         starvationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        
//         auto pop_elapsed_ms = std::chrono::duration<double, std::milli>(
//             std::chrono::high_resolution_clock::now() - pop_start).count();
        
//         // ========== 2. VALIDATE FRAME ==========
//         if (!inputFrame || !inputFrame->isValid() || !inputFrame->dataPtr) {
//             spdlog::warn("[AlgorithmConcrete] Invalid frame received (pop took {}ms)", pop_elapsed_ms);
//             continue;
//         }
        
//         spdlog::debug("[AlgorithmConcrete] Frame {} received (pop took {}ms, queue_size={})", 
//                      inputFrame->frameNumber, pop_elapsed_ms, inputQueueZeroCopy_->size());

//         // ========== 3. DYNAMIC ADAPTATION (Every ~10 frames) ==========
//         if (runtimeControls_ && (++adaptationCounter % 10) == 0) {
//             const int newConcurrency = runtimeControls_->concurrency_level.load(std::memory_order_relaxed);
//             const bool gpuEnabled    = runtimeControls_->enable_gpu.load(std::memory_order_relaxed);
//             const auto newAffinity   = runtimeControls_->affinity.load(std::memory_order_relaxed);

//             algoConfig_.concurrencyLevel = utils::local_clamp(newConcurrency, 1, 4);
//             algoConfig_.useGPU = gpuEnabled;

//             if (newConcurrency != lastConcurrency || newAffinity != lastAffinity) {
//                 const auto cpus = hrl::chooseCpus(algoConfig_.concurrencyLevel, newAffinity);
//                 if (!cpus.empty()) {
//                     hrl::setThreadAffinity(cpus);
//                     spdlog::debug("[AlgorithmConcrete] Affinity updated: {} cores, {} mode",
//                                   algoConfig_.concurrencyLevel,
//                                   (newAffinity == hrl::Affinity::Spread ? "Spread" : "Pack"));
//                 }
//                 lastConcurrency = newConcurrency;
//                 lastAffinity = newAffinity;
//             }
//         }

//         // ========== 4. PROCESS FRAME WITH TIMING ==========
//         auto proc_start = std::chrono::high_resolution_clock::now();
        
//         std::shared_ptr<ZeroCopyFrameData> outputFrame;
//         bool proc_ok = processFrameZeroCopy(inputFrame, outputFrame);
        
//         auto proc_elapsed_ms = std::chrono::duration<double, std::milli>(
//             std::chrono::high_resolution_clock::now() - proc_start).count();
        
//         windowProcMs += proc_elapsed_ms;  // ? Accumulate for averaging

//         // IMPORTANT: outputFrame is already pushed inside processFrameZeroCopy()
//         // Do NOT push again here ? that would double the queue entries and halve FPS.

//         if (proc_ok && outputFrame) {
//             //outputQueueZeroCopy_->push(outputFrame); // IMPORTANT: outputFrame is already pushed inside processFrameZeroCopy()
//             windowFrames++;  // ? Count successful frames
            
//             spdlog::debug("[AlgorithmConcrete] Frame {} processed ({}ms, refcount={})", 
//                          inputFrame->frameNumber, proc_elapsed_ms, outputFrame.use_count());

//             if (firstFrame) {
//                 firstFrame = false;
//                 spdlog::info("[AlgorithmConcrete] FIRST FRAME PROCESSED | Concurrency={} | GPU={}",
//                              algoConfig_.concurrencyLevel, algoConfig_.useGPU ? "ON" : "OFF");
//             }
//         } else {
//             spdlog::warn("[AlgorithmConcrete] Frame {} processing FAILED ({}ms)", 
//                         inputFrame->frameNumber, proc_elapsed_ms);
//         }

//         // ========== 5. UPDATE METRICS EVERY 1 SECOND ==========
//         auto now = std::chrono::steady_clock::now();
//         double elapsed = std::chrono::duration<double>(now - windowStart).count();
        
//         if (elapsed >= 1.0) {
//             // ? Calculate per-second stats
//             double fps = windowFrames / elapsed;
//             double avg_proc_ms = windowFrames > 0 ? windowProcMs / windowFrames : 0.0;
            
//             spdlog::info("[AlgorithmConcrete] METRICS [1s window] | Frames: {} | FPS: {:.1f} | Avg Proc: {:.2f}ms",
//                         windowFrames, fps, avg_proc_ms);
            
//             // ? Call updateMetrics with current frame info
//             if (windowFrames > 0) {
//                 updateMetrics(elapsed, inputFrame->frameNumber);
//             }
            
//             // ? RESET for next window
//             windowStart = now;
//             windowFrames = 0;
//             windowProcMs = 0.0;
//         }
//     }

//     spdlog::info("[AlgorithmConcrete] Thread exited cleanly");
// }


// //================================================================================
// // processFrameZeroCopy ? FINAL HRL ADAPTIVE VERSION (GPU Switching + Fallbacks)
// //================================================================================
// inline bool AlgorithmConcrete::processFrameZeroCopy(
//     const std::shared_ptr<ZeroCopyFrameData>& inputFrame,
//     std::shared_ptr<ZeroCopyFrameData>& outputFrame)
// {
//     if (!inputFrame || !inputFrame->isValid() || !inputFrame->dataPtr || inputFrame->size == 0) {
//         spdlog::error("[AlgorithmConcrete] Invalid input frame");
//         return false;
//     }

//     const uint64_t frameId = inputFrame->frameNumber;
//     const auto t_start_steady = std::chrono::steady_clock::now();
//     const auto t_start_sys = std::chrono::system_clock::now();

//     processedBuffer_.resize(inputFrame->size);
//     double cudaKernelTimeMs = 0.0;

//     // === SINGLE SOURCE OF TRUTH: GPU ENABLED? ===
//     // Config default ? overridden by Scheduler (most up-to-date)
//     bool useGPU = algoConfig_.useGPU;

//     switch (algoConfig_.algorithmType) {
//         // ??????????????????? CPU-Only Algorithms (Never use GPU) ???????????????????
//         case AlgorithmType::Invert:
//         case AlgorithmType::Grayscale:
//         case AlgorithmType::EdgeDetection:
//         case AlgorithmType::GaussianBlur:
//             useGPU = false;
//             break;

//         // ??????????????????? Hybrid Algorithms (Respect GPU Toggle) ???????????????????
//         default:
//             break; // use current algoConfig_.useGPU
//     }

//     // ??????????????????? Dispatch with fallback ???????????????????
//     switch (algoConfig_.algorithmType) {
//         case AlgorithmType::Invert:
//             processInvertZeroCopy(inputFrame);
//             break;
//         case AlgorithmType::Grayscale:
//             processGrayscaleZeroCopy(inputFrame);
//             break;
//         case AlgorithmType::EdgeDetection:
//             processEdgeDetectionZeroCopy(inputFrame);
//             break;
//         case AlgorithmType::GaussianBlur:
//             processGaussianBlurZeroCopy(inputFrame);
//             break;

//         case AlgorithmType::SobelEdge:
//             if (useGPU) {
//                 try {
//                     cudaKernelTimeMs = timeCudaSectionMs([&]{
//                         AlgorithmConcreteKernels::launchSobelEdgeKernel(cudaRes_, inputFrame, processedBuffer_);
//                     });
//                 } catch (const std::runtime_error& e) {
//                     spdlog::warn("[AlgorithmConcrete] GPU Sobel failed ({}); falling back to CPU edge detection", e.what());
//                     algoConfig_.useGPU = false;
//                     processEdgeDetectionZeroCopy(inputFrame);
//                 }
//             } else {
//                 processEdgeDetectionZeroCopy(inputFrame);  // Best-effort CPU fallback
//             }
//             break;

//         case AlgorithmType::MedianFilter:
//             if (useGPU) {
//                 try {
//                     cudaKernelTimeMs = timeCudaSectionMs([&]{
//                         AlgorithmConcreteKernels::launchMedianFilterKernel(cudaRes_, inputFrame, processedBuffer_, algoConfig_.medianWindowSize);
//                     });
//                 } catch (const std::runtime_error& e) {
//                     spdlog::warn("[AlgorithmConcrete] GPU Median failed ({}); falling back to pass-through", e.what());
//                     algoConfig_.useGPU = false;
//                     std::memcpy(processedBuffer_.data(), inputFrame->dataPtr, inputFrame->size);
//                 }
//             } else {
//                 // No CPU median - pass-through to save energy
//                 std::memcpy(processedBuffer_.data(), inputFrame->dataPtr, inputFrame->size);
//             }
//             break;
//         case AlgorithmType::HistogramEqualization:
//             if (useGPU) {
//                 try {
//                     cudaKernelTimeMs = timeCudaSectionMs([&] {
//                         AlgorithmConcreteKernels::launchHistogramEqualizationKernel(
//                             cudaRes_, inputFrame, processedBuffer_);
//                     });
//                 } catch (const std::runtime_error& e) {
//                     spdlog::warn(
//                         "[AlgorithmConcrete] GPU HistEq failed ({}); falling back to CPU histogram equalization",
//                         e.what());
//                     processHistogramEqualizationCPUZeroCopy(inputFrame);
//                     cudaKernelTimeMs = 0.0;
//                 }
//             } else {
//                 processHistogramEqualizationCPUZeroCopy(inputFrame);
//                 cudaKernelTimeMs = 0.0;
//             }
//             break;

//         case AlgorithmType::HeterogeneousGaussianBlur:
//             if (useGPU) {
//                 try {
//                     cudaKernelTimeMs = timeCudaSectionMs([&]{
//                         AlgorithmConcreteKernels::launchHeterogeneousGaussianBlurKernel(cudaRes_, inputFrame, processedBuffer_, algoConfig_.blurRadius);
//                     });
//                 } catch (const std::runtime_error& e) {
//                     spdlog::warn("[AlgorithmConcrete] GPU Gaussian failed ({}); falling back to CPU blur", e.what());
//                     algoConfig_.useGPU = false;
//                     processGaussianBlurZeroCopy(inputFrame);
//                 }
//             } else {
//                 processGaussianBlurZeroCopy(inputFrame);
//             }
//             break;

//         case AlgorithmType::OpticalFlow_LucasKanade:
//             processOpticalFlow(inputFrame);
//             goto metrics_only;

//         default:
//             std::memcpy(processedBuffer_.data(), inputFrame->dataPtr, inputFrame->size);
//             break;
//     }

//     // ??????????????????? Output Frame Construction ???????????????????
// 	// === CRITICAL FIX: ALWAYS CREATE OUTPUT FRAME AND PUSH TO QUEUE ===

//     {
//         //auto data = std::make_shared<std::vector<uint8_t>>(std::move(processedBuffer_)); // copy processed result
//         // Copy instead of move ? retains capacity in processedBuffer_
//         auto data = std::make_shared<std::vector<uint8_t>>(processedBuffer_);
//         processedBuffer_.clear();   // optional, keeps capacity
//         outputFrame = std::make_shared<ZeroCopyFrameData>(
//             data, data->data(), 
// 		data->size(),
//             inputFrame->width, inputFrame->height, -1,
//             inputFrame->frameNumber,
//             inputFrame->captureSys, inputFrame->captureSteady
//         );

// //===============================================
// 	// PUSH TO DISPLAY QUEUE (this makes right side update in real-time)
//         if (outputQueueZeroCopy_) {
//             outputQueueZeroCopy_->push(outputFrame);
//             spdlog::debug("[AlgorithmConcrete] Pushed processed frame {} to outputQueueZeroCopy_ (refcount={})", 
//                          frameId, outputFrame.use_count());
//         } else {
//             spdlog::error("[AlgorithmConcrete] outputQueueZeroCopy_ is null - cannot show processed frame");
//         }
//     }

// metrics_only:
//     const auto t_end_steady = std::chrono::steady_clock::now();
//     const auto t_end_sys = std::chrono::system_clock::now();
//     lastProcessingTime_ = std::chrono::duration<double, std::milli>(t_end_steady - t_start_steady).count();

//     //windowFrames_++;
//     //windowProcMs_ += lastProcessingTime_;

//      // -- Sliding 1-second FPS window (feeds metricAggregator / CSV) ----------
//     fpsWindowFrames_++;
//     fpsWindowProcMs_ += lastProcessingTime_;
//     totalFrames_++;
//     totalProcMs_ += lastProcessingTime_;

//     double currentMeasuredFps = 30.0;
//     double windowElapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - windowStart_).count();
//     if (windowElapsed > 0.001) currentMeasuredFps = windowFrames_ / windowElapsed;

//     uint32_t dropped = 0;
//     if (haveLastSeq_ && frameId > lastFrameSeq_ + 1) {
//         dropped = static_cast<uint32_t>(std::min<uint64_t>(frameId - lastFrameSeq_ - 1, UINT32_MAX));
//     }
//     lastFrameSeq_ = frameId;
//     haveLastSeq_ = true;

// #ifdef __CUDACC__
//     size_t gpuFree = 0, gpuTotal = 0;
//     if (useGPU && cudaMemGetInfo(&gpuFree, &gpuTotal) == cudaSuccess && gpuFree < 80ULL * 1024 * 1024) {
//         spdlog::warn("[AlgorithmConcrete] GPU Low Memory: {} MiB free", gpuFree / (1024*1024));
//     }
// #endif

//     if (metricAggregator_) {
//         AlgorithmStats as(t_start_sys, t_end_sys);
//         as.inferenceTimeMs   = lastProcessingTime_;
//         as.fps               = currentMeasuredFps;
//         as.avgProcTimeMs     = windowFrames_ > 0 ? windowProcMs_ / windowFrames_ : 0.0;
//         as.totalProcTimeMs   = totalProcMs_;
//         as.cudaKernelTimeMs  = cudaKernelTimeMs;
//         as.droppedFrames     = dropped;
// #ifdef __CUDACC__
//         if (useGPU) {
//             as.gpuFreeMemory  = gpuFree;
//             as.gpuTotalMemory = gpuTotal;
//         }
// #endif
//         metricAggregator_->mergeAlgorithm(frameId, as);
//     }

//     return algoConfig_.algorithmType != AlgorithmType::OpticalFlow_LucasKanade;
// }

// inline void AlgorithmConcrete::updateMetrics(double elapsedSec, uint64_t frameSeq) {
//     std::lock_guard<std::mutex> lock(metricMutex_);
//     if (elapsedSec <= 0.0) elapsedSec = 1.0;

//     const double fps = windowFrames_ / elapsedSec;
//     const double avgMs = windowFrames_ > 0 ? (windowProcMs_ / windowFrames_) : 0.0;

//     fps_ = fps;
//     lastFPS_ = fps;
//     avgProcTime_ = avgMs;

//     spdlog::info("[AlgorithmConcrete] Frame {} | FPS: {:.1f} | Avg: {:.2f}ms | {}", 
//                  frameSeq, fps, avgMs, algorithmTypeToString(algoConfig_.algorithmType));
// }

// inline std::string AlgorithmConcrete::algorithmTypeToString(AlgorithmType type) const {
//     switch (type) {
//         case AlgorithmType::Invert: return "Invert";
//         case AlgorithmType::Grayscale: return "Grayscale";
//         case AlgorithmType::EdgeDetection: return "EdgeDetection";
//         case AlgorithmType::GaussianBlur: return "GaussianBlur";
//         case AlgorithmType::SobelEdge: return "SobelEdge";
//         case AlgorithmType::MedianFilter: return "MedianFilter";
//         case AlgorithmType::HistogramEqualization: return "HistogramEqualization";
//         case AlgorithmType::HeterogeneousGaussianBlur: return "HeterogeneousGaussianBlur";
//         case AlgorithmType::OpticalFlow_LucasKanade: return "OpticalFlow_LK";
//         case AlgorithmType::Mandelbrot: return "Mandelbrot";
//         case AlgorithmType::MultiThreadedInvert: return "MultiThreadedInvert";
//         default: return "Unknown";
//     }
// }

// inline void AlgorithmConcrete::reportError(const std::string& msg) {
//     if (errorCallback_) errorCallback_(msg);
//     else spdlog::error("[AlgorithmConcrete] {}", msg);
// }

// //================================================================================
// // parallelFor ? HRL ADAPTIVE VERSION
// //================================================================================
// inline void AlgorithmConcrete::parallelFor(size_t start, size_t end, const std::function<void(size_t)>& func) {
//     // 1. Determine thread count: Priority to RuntimeControls (HRL), fallback to Config
//     size_t numThreads = algoConfig_.concurrencyLevel;
    
//     if (runtimeControls_) {
//         int dynLevel = runtimeControls_->concurrency_level.load(std::memory_order_relaxed);
//         if (dynLevel > 0) {
//             numThreads = static_cast<size_t>(dynLevel);
//         }
//     }

//     // 2. Serial fallback for small workloads or single-thread mode
//     if (numThreads <= 1 || (end - start) < 1000) {
//         for (size_t i = start; i < end; ++i) func(i);
//         return;
//     }

//     // 3. Parallel Execution
//     std::vector<std::thread> workers;
//     workers.reserve(numThreads);
    
//     const size_t chunkSize = (end - start + numThreads - 1) / numThreads;

//     for (size_t t = 0; t < numThreads; ++t) {
//         size_t s = start + t * chunkSize;
//         if (s >= end) break;
//         size_t e = std::min(end, s + chunkSize);
        
//         workers.emplace_back([=] { 
//             for (size_t j = s; j < e; j++) func(j); 
//         });
//     }
    
//     for (auto& w : workers) w.join();
// }

// // CPU Operations
// inline void AlgorithmConcrete::processInvertZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
//     const uint8_t* in = static_cast<const uint8_t*>(frame->dataPtr);
//     parallelFor(0, frame->size, [this, in](size_t i) { processedBuffer_[i] = ~in[i]; });
// }

// inline void AlgorithmConcrete::processGrayscaleZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
//     const uint8_t* in = static_cast<const uint8_t*>(frame->dataPtr);
//     parallelFor(0, frame->size, [this, in](size_t i) { processedBuffer_[i] = (i % 2 == 0) ? in[i] : 128; });
// }

// inline void AlgorithmConcrete::processEdgeDetectionZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
//     const uint8_t* in = static_cast<const uint8_t*>(frame->dataPtr);
//     const int w = frame->width, h = frame->height;
//     parallelFor(0, h, [this, in, w](size_t y) {
//         for (int x = 0; x < w; ++x) {
//             int idx = y * w * 2 + x * 2;
//             if (x > 0 && x < w - 1) {
//                 int grad = std::abs(static_cast<int>(in[idx]) - static_cast<int>(in[idx - 2]));
//                 processedBuffer_[idx] = static_cast<uint8_t>(grad);
//                 processedBuffer_[idx + 1] = 128;
//             } else {
//                 processedBuffer_[idx] = in[idx];
//                 processedBuffer_[idx + 1] = in[idx + 1];
//             }
//         }
//     });
// }

// inline void AlgorithmConcrete::processGaussianBlurZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
//     const int r = algoConfig_.blurRadius;
//     const int w = frame->width, h = frame->height;
//     const uint8_t* in = static_cast<const uint8_t*>(frame->dataPtr);
//     std::vector<uint8_t> temp(frame->size);

//     parallelFor(0, h, [in, w, r, &temp](size_t y) {
//         for (int x = 0; x < w; ++x) {
//             float sum = 0, wsum = 0;
//             for (int d = -r; d <= r; ++d) {
//                 int xx = utils::local_clamp(x + d, 0, w - 1);
//                 float wt = std::exp(-(d * d) / (2.0f * r * r));
//                 sum += in[y * w * 2 + xx * 2] * wt;
//                 wsum += wt;
//             }
//             temp[y * w * 2 + x * 2] = static_cast<uint8_t>(sum / wsum);
//             temp[y * w * 2 + x * 2 + 1] = 128;
//         }
//     });

//     parallelFor(0, w, [&temp, w, h, r, this](size_t x) {
//         for (int y = 0; y < h; ++y) {
//             float sum = 0, wsum = 0;
//             for (int d = -r; d <= r; ++d) {
//                 int yy = utils::local_clamp(static_cast<int>(y) + d, 0, h - 1);
//                 float wt = std::exp(-(d * d) / (2.0f * r * r));
//                 sum += temp[yy * w * 2 + x * 2] * wt;
//                 wsum += wt;
//             }
//             processedBuffer_[y * w * 2 + x * 2] = static_cast<uint8_t>(sum / wsum);
//             processedBuffer_[y * w * 2 + x * 2 + 1] = 128;
//         }
//     });
// }

// // Note: The above CPU implementations are intentionally simple and not optimized for performance.

// inline void AlgorithmConcrete::processMultiThreadedInvert(const std::shared_ptr<ZeroCopyFrameData>& frame) {
//     const int numThreads = 4; // Jetson Nano has 4 ARM cores
//     const size_t dataSize = frame->width * frame->height;
//     const size_t chunkSize = dataSize / numThreads;
//     std::vector<std::thread> threads;

//     // Spawn threads to process chunks of the image concurrently
//     for (int i = 0; i < numThreads; ++i) {
//         size_t startIdx = i * chunkSize;
//         size_t endIdx = (i == numThreads - 1) ? dataSize : startIdx + chunkSize;
        
//         threads.emplace_back([this, frame, startIdx, endIdx]() {
//             for (size_t j = startIdx; j < endIdx; ++j) {
//                 // Replace this:
//                 //processedBuffer_[j] = 255 - frame->dataPtr[j];

//                 // With this:
//                 uint8_t* data = static_cast<uint8_t*>(frame->dataPtr);
//                 processedBuffer_[j] = 255 - data[j];
//             }
//         });
//     }

//     // Wait for all cores to finish
//     for (auto& t : threads) {
//         if (t.joinable()) t.join();
//     }
// }

// inline void AlgorithmConcrete::processMandelbrot(const std::shared_ptr<ZeroCopyFrameData>& frame) {
//     if (algoConfig_.useGPU) {
//         try {
//             AlgorithmConcreteKernels::launchMandelbrotKernel(cudaRes_, frame, processedBuffer_);
//             return;
//         } catch (const std::exception& e) {
//             reportError("[Mandelbrot GPU ERROR] " + std::string(e.what()) + ". Falling back to CPU.");
//             // Fall through to CPU implementation
//         }
//     }

//     // --- CPU Fallback implementation ---
//     const int width = frame->width;
//     const int height = frame->height;
//     const int max_iter = 100;

//     for (int y = 0; y < height; ++y) {
//         for (int x = 0; x < width; ++x) {
//             float cx = (x * 3.0f / (float)width) - 2.0f;
//             float cy = (y * 3.0f / (float)height) - 1.5f;
//             float zx = 0.0f, zy = 0.0f;
//             int iter = 0;

//             while (zx * zx + zy * zy <= 4.0f && iter < max_iter) {
//                 float tmp = zx * zx - zy * zy + cx;
//                 zy = 2.0f * zx * zy + cy;
//                 zx = tmp;
//                 iter++;
//             }
            
//             processedBuffer_[y * width + x] = (iter == max_iter) ? 0 : (uint8_t)((iter * 255) / max_iter);
//         }
//     }
// }

// // CUDA Wrappers
// inline void AlgorithmConcrete::processSobelEdgeZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
//     AlgorithmConcreteKernels::launchSobelEdgeKernel(cudaRes_, frame, processedBuffer_);
// }

// inline void AlgorithmConcrete::processMedianFilterZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
//     AlgorithmConcreteKernels::launchMedianFilterKernel(cudaRes_, frame, processedBuffer_, algoConfig_.medianWindowSize);
// }

// // inline void AlgorithmConcrete::processHistogramEqualizationZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
// //     AlgorithmConcreteKernels::launchHistogramEqualizationKernel(cudaRes_, frame, processedBuffer_);
// // }

// //=========================================================================================================================


// inline void AlgorithmConcrete::processHistogramEqualizationZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
//     // CPU implementation matching the CUDA HistEq path:
//     //   - Build histogram over the Y/luma channel only.
//     //   - Compute CDF and min non-zero CDF.
//     //   - Remap Y using (cdf[Y] - minCdf) / (N - minCdf) * 255.
//     //   - Set chroma byte to 128, matching the existing CUDA remapKernel.
//     //
//     // The framework stores frames in the same 2-bytes-per-pixel convention used
//     // by the CUDA kernels: byte 0 = Y, byte 1 = chroma placeholder.
//     if (!frame || !frame->dataPtr || frame->width <= 0 || frame->height <= 0 || frame->size == 0) {
//         spdlog::warn("[AlgorithmConcrete] CPU HistEq skipped: invalid frame");
//         return;
//     }

//     const uint8_t* in = static_cast<const uint8_t*>(frame->dataPtr);
//     const int width = frame->width;
//     const int height = frame->height;
//     const int totalPixels = width * height;
//     const size_t expectedSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 2ULL;

//     if (totalPixels <= 0 || frame->size < expectedSize) {
//         spdlog::warn("[AlgorithmConcrete] CPU HistEq size mismatch: frameSize={} expectedAtLeast={}",
//                      frame->size, expectedSize);
//         if (processedBuffer_.size() < frame->size) {
//             processedBuffer_.resize(frame->size);
//         }
//         std::memcpy(processedBuffer_.data(), in, frame->size);
//         return;
//     }

//     if (processedBuffer_.size() < frame->size) {
//         processedBuffer_.resize(frame->size);
//     }

//     std::vector<int> hist(256, 0);
//     std::vector<int> cdf(256, 0);

//     // Histogram over Y channel.
//     for (int y = 0; y < height; ++y) {
//         const int rowBase = y * width * 2;
//         for (int x = 0; x < width; ++x) {
//             const int idx = rowBase + x * 2;
//             ++hist[in[idx]];
//         }
//     }

//     int running = 0;
//     int minCdf = 0;
//     for (int i = 0; i < 256; ++i) {
//         running += hist[i];
//         cdf[i] = running;
//         if (minCdf == 0 && running > 0) {
//             minCdf = running;
//         }
//     }

//     const int denom = totalPixels - minCdf;

//     // Degenerate image: all pixels have the same luma. Keep a valid neutral output.
//     if (denom <= 0) {
//         for (int y = 0; y < height; ++y) {
//             const int rowBase = y * width * 2;
//             for (int x = 0; x < width; ++x) {
//                 const int idx = rowBase + x * 2;
//                 processedBuffer_[idx]     = in[idx];
//                 processedBuffer_[idx + 1] = 128;
//             }
//         }
//         return;
//     }

//     parallelFor(0, height, [this, in, width, &cdf, minCdf, denom](size_t y) {
//         const int rowBase = static_cast<int>(y) * width * 2;
//         for (int x = 0; x < width; ++x) {
//             const int idx = rowBase + x * 2;
//             const int yIn = static_cast<int>(in[idx]);
//             const float norm = static_cast<float>(cdf[yIn] - minCdf) / static_cast<float>(denom);
//             int yOut = static_cast<int>(norm * 255.0f);
//             yOut = utils::local_clamp(yOut, 0, 255);

//             processedBuffer_[idx]     = static_cast<uint8_t>(yOut);
//             processedBuffer_[idx + 1] = 128;
//         }
//     });
// }

// //=====================================================================================================
// inline void AlgorithmConcrete::processHistogramEqualizationCPUZeroCopy(
//     const std::shared_ptr<ZeroCopyFrameData>& frame)
// {
//     if (!frame || !frame->dataPtr || frame->width <= 0 || frame->height <= 0) {
//         return;
//     }

//     const int width = frame->width;
//     const int height = frame->height;
//     const size_t dataSize = frame->size;
//     const uint8_t* input = static_cast<const uint8_t*>(frame->dataPtr);

//     processedBuffer_.resize(dataSize);

//     std::array<int, 256> hist{};
//     std::array<int, 256> cdf{};

//     const int totalPixels = width * height;

//     for (int y = 0; y < height; ++y) {
//         const int rowBase = y * width * 2;
//         for (int x = 0; x < width; ++x) {
//             const int idx = rowBase + x * 2;
//             ++hist[input[idx]];
//         }
//     }

//     cdf[0] = hist[0];
//     for (int i = 1; i < 256; ++i) {
//         cdf[i] = cdf[i - 1] + hist[i];
//     }

//     int minCdf = 0;
//     for (int i = 0; i < 256; ++i) {
//         if (cdf[i] > 0) {
//             minCdf = cdf[i];
//             break;
//         }
//     }

//     const int denom = totalPixels - minCdf;

//     for (int y = 0; y < height; ++y) {
//         const int rowBase = y * width * 2;
//         for (int x = 0; x < width; ++x) {
//             const int idx = rowBase + x * 2;
//             uint8_t newY = input[idx];

//             if (denom > 0) {
//                 const float norm =
//                     static_cast<float>(cdf[input[idx]] - minCdf) /
//                     static_cast<float>(denom);

//                 int mapped = static_cast<int>(norm * 255.0f);
//                 mapped = std::max(0, std::min(255, mapped));
//                 newY = static_cast<uint8_t>(mapped);
//             }

//             processedBuffer_[idx] = newY;
//             processedBuffer_[idx + 1] = 128;
//         }
//     }
// }

// //=========================================================================================================================

// inline void AlgorithmConcrete::processHeterogeneousGaussianBlurZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
//     AlgorithmConcreteKernels::launchHeterogeneousGaussianBlurKernel(cudaRes_, frame, processedBuffer_, algoConfig_.blurRadius);
// }

// inline void AlgorithmConcrete::processOpticalFlow(const std::shared_ptr<ZeroCopyFrameData>& frame) {
//     if (!frame || !frame->dataPtr) return;
//     if (previousFrame_) {
//         std::shared_ptr<ZeroCopyFrameData> flowOutput;
//         opticalFlowProcessor_->computeOpticalFlow(previousFrame_, frame, flowOutput);
//         if (outputQueueZeroCopy_ && flowOutput) {
//             outputQueueZeroCopy_->push(flowOutput);
//         }
//     }
//     previousFrame_ = frame;
// }

// // CUDA Helpers
// inline void AlgorithmConcrete::checkCudaError(cudaError_t err, const std::string& context) {
//     if (err != cudaSuccess) {
//         reportError("[CUDA ERROR] " + context + ": " + cudaGetErrorString(err));
//     }
// }

// inline double AlgorithmConcrete::timeCudaSectionMs(const std::function<void()>& launch) {
// #ifdef __CUDACC__
//     cudaEvent_t start, stop;
//     cudaEventCreate(&start);
//     cudaEventCreate(&stop);
//     cudaEventRecord(start, 0);
//     launch();
//     cudaEventRecord(stop, 0);
//     cudaEventSynchronize(stop);
//     float ms = 0;
//     cudaEventElapsedTime(&ms, start, stop);
//     cudaEventDestroy(start);
//     cudaEventDestroy(stop);
//     return ms;
// #else
//     auto t0 = std::chrono::high_resolution_clock::now();
//     launch();
//     return std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
// #endif
// }


// //================================================================================
// // AlgorithmConcrete_new.h
// // FINAL PRODUCTION VERSION ? 100% JETSON NANO 2GB COMPATIBLE + ENHANCED
// // Fixed: C++11/14 Compliance, Initialization Order, Namespace Scope
// //================================================================================

// #pragma once


// //#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG
// #define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO

// #include <cstddef>
// #include <cstring>
// #include <limits>
// #include <vector>
// #include <mutex>
// #include <atomic>
// #include <thread>
// #include <functional>
// #include <chrono>
// #include <cmath>
// #include <algorithm>
// #include <memory>

// #include <array>

// #include <spdlog/spdlog.h>
// #include <cuda_runtime.h>
// #include "CudaUtiles.h"


// #include "../Interfaces/IAlgorithm.h"
// #include "../SharedStructures/ZeroCopyFrameData.h"
// #include "../SharedStructures/SharedQueue.h"
// #include "../SharedStructures/ThreadManager.h"
// #include "../SharedStructures/AlgorithmConfig.h"
// #include "../SharedStructures/LucasKanadeOpticalFlow.h"
// #include "../SharedStructures/allModulesStatcs.h"
// #include "../Interfaces/ISystemMetricsAggregator.h"
// #include "../Others/utils.h"
// #include "AlgorithmConcreteKernels.cuh"

// // [FIX] Include Scheduler to access hrl::chooseCpus and hrl::setThreadAffinity
// #include "../../Module/Scheduler.h" 
// #include "../../Module/RuntimeControls.h"

// // within util.h
// // // [FIX] Local clamp for C++14/11 compatibility on Jetson Nano
// // namespace {
// //     template <typename T>
// //     constexpr const T& local_clamp(const T& v, const T& lo, const T& hi) {
// //         return (v < lo) ? lo : (hi < v) ? hi : v;
// //     }
// // }

// using namespace hrl;

// class AlgorithmConcrete : public IAlgorithm {
// public:
//     explicit AlgorithmConcrete(ThreadManager& threadManager);
//     //==========================================================================
//     // Constructor / Destructor
//     //==========================================================================
//     AlgorithmConcrete(std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> inputQueue,
//                       std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> outputQueue,
//                       ThreadManager& threadManager,
//                       std::shared_ptr<ISystemMetricsAggregator> aggregator,
//                       std::shared_ptr<hrl::RuntimeControls> controls = nullptr);

//     ~AlgorithmConcrete() override;
//     //==========================================================================
//     // Interface Implementation
//     //==========================================================================
//     static std::shared_ptr<IAlgorithm> createAlgorithmZeroCopy(
//         AlgorithmType type,
//         std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> inputQueue,
//         std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> outputQueue,
//         ThreadManager& threadManager,
//         std::shared_ptr<ISystemMetricsAggregator> aggregator);

//     enum class StopMode { Shutdown, HotSwap };

//     void startAlgorithm() override;
//     void stopAlgorithm() override;                  // IAlgorithm-compatible shutdown entry point
//     void stopAlgorithm(StopMode mode);              // explicit hot-swap quiesce entry point
//     bool isRunning() const { return running_.load(std::memory_order_acquire); }
//     bool isStarved() const { return starved_.load(std::memory_order_acquire); }
    
//     bool processFrameZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& inputFrame,
//                               std::shared_ptr<ZeroCopyFrameData>& outputFrame) override;
    
//     bool configure(const AlgorithmConfig& config) override;
    
//     void setErrorCallback(std::function<void(const std::string&)> cb) override;
    
//     std::tuple<double, double> getAlgorithmMetrics() const override { return {getLastFPS(), lastProcessingTime_}; }
//     double getLastFPS() const override { return lastFPS_; }
//     double getFps() const override { return lastFPS_; }
//     double getAverageProcTime() const override { return avgProcTime_; }
//     const uint8_t* getProcessedBuffer() const override { return processedBuffer_.data(); }
//     void setAlgorithmType(AlgorithmType newType) override;

// private:
//     void threadLoopZeroCopy();
//     void updateMetrics(double elapsedSec, uint64_t frameSeq);
//     std::string algorithmTypeToString(AlgorithmType type) const;
//     void reportError(const std::string& msg);

//     // [FIX] Declaration signature matched to definition
//     void parallelFor(size_t start, size_t end, const std::function<void(size_t)>& func);

//     // CPU algorithms
//     void processInvertZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
//     void processGrayscaleZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
//     void processEdgeDetectionZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
//     void processGaussianBlurZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
//     void processMedianFilterCPUZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);


//     // CPU multithreaded algorithms (fallbacks for GPU)
//     void processMultiThreadedInvert(const std::shared_ptr<ZeroCopyFrameData>& frame) ; // [FIX] Added missing declaration for multi-threaded invert

    

//     // CUDA wrappers
//     void processSobelEdgeZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
//     void processMedianFilterZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);

//     // 
//     void processMandelbrot(const std::shared_ptr<ZeroCopyFrameData>& frame); // [FIX] Added missing declaration for Mandelbrot (CPU)
    
//     void processHistogramEqualizationZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
//     void processHeterogeneousGaussianBlurZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
//     void processHistogramEqualizationCPUZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
//     void processOpticalFlow(const std::shared_ptr<ZeroCopyFrameData>& frame);

//     // CUDA helpers
//     void checkCudaError(cudaError_t err, const std::string& context);
//     double timeCudaSectionMs(const std::function<void()>& launch);

//     // --- State Variables (Order matches initialization for safety) ---
//     std::atomic<bool> running_{false};
//     std::atomic<bool> starved_{false};
//     bool workerStarted_ = false;
//     mutable std::mutex lifecycleMutex_;
    
//     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> inputQueueZeroCopy_;
//     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> outputQueueZeroCopy_;
    
//     AlgorithmConfig algoConfig_;
//     std::function<void(const std::string&)> errorCallback_;

//     // Metrics
//     mutable std::mutex metricMutex_;
//     double fps_ = 0.0;
//     double avgProcTime_ = 0.0;
//     std::vector<uint8_t> processedBuffer_;
    
//     mutable double lastFPS_ = 0.0;
//     mutable double lastProcessingTime_ = 0.0;

//     uint64_t windowFrames_ = 0;
//     double windowProcMs_ = 0.0;
//     std::chrono::steady_clock::time_point windowStart_;

//     // Sliding 1-second window for processFrameZeroCopy FPS (feeds metricAggregator)
//     std::chrono::steady_clock::time_point fpsWindowStart_;
//     uint64_t fpsWindowFrames_ = 0;
//     double   fpsWindowProcMs_ = 0.0;

//     uint64_t totalFrames_ = 0;
//     double totalProcMs_ = 0.0;

//     uint64_t lastFrameSeq_ = 0;
//     bool haveLastSeq_ = false;

//     // Persistent GPU resources ? allocated once at startAlgorithm(), freed at stopAlgorithm().
//     // Eliminates per-frame cudaMalloc/cudaFree and enables async CUDA stream overlap.
//     AlgorithmConcreteKernels::CudaResources cudaRes_;

//     // Dependencies
//     ThreadManager& threadManager_;
//     std::unique_ptr<LucasKanadeOpticalFlow> opticalFlowProcessor_;
//     std::shared_ptr<ZeroCopyFrameData> previousFrame_;
//     std::shared_ptr<ISystemMetricsAggregator> metricAggregator_;
//     std::shared_ptr<hrl::RuntimeControls> runtimeControls_;
// };

// //================================================================================
// // Inline Implementation
// //================================================================================

// inline AlgorithmConcrete::AlgorithmConcrete(ThreadManager& threadManager)
//     : running_(false),
//       inputQueueZeroCopy_(nullptr),
//       outputQueueZeroCopy_(nullptr),
//       // [FIX] Removed lastUpdateTime_ (not in class), initialized windowStart_
//       windowStart_(std::chrono::steady_clock::now()),
//       fpsWindowStart_(std::chrono::steady_clock::now()),
//       threadManager_(threadManager) {
//     spdlog::warn("[AlgorithmConcrete] Default constructor used ? no queues!");
// }

// // [FIX] Constructor Initialization List Reordered to match declaration order
// inline AlgorithmConcrete::AlgorithmConcrete(
//     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> inputQueue,
//     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> outputQueue,
//     ThreadManager& threadManager,
//     std::shared_ptr<ISystemMetricsAggregator> aggregator,
//     std::shared_ptr<hrl::RuntimeControls> controls)
//     : running_(false),
//       inputQueueZeroCopy_(std::move(inputQueue)),
//       outputQueueZeroCopy_(std::move(outputQueue)),
//       windowStart_(std::chrono::steady_clock::now()),
//       fpsWindowStart_(std::chrono::steady_clock::now()),
//       threadManager_(threadManager),
//       metricAggregator_(aggregator),
//       runtimeControls_(controls) {
    
//     if (!inputQueueZeroCopy_ || !outputQueueZeroCopy_) {
//         throw std::runtime_error("AlgorithmConcrete: input/output queues cannot be null");
//     }
//     spdlog::info("[AlgorithmConcrete] Constructed with ZeroCopy queues");
// }

// inline AlgorithmConcrete::~AlgorithmConcrete() {
//     stopAlgorithm();
// }

// inline std::shared_ptr<IAlgorithm> AlgorithmConcrete::createAlgorithmZeroCopy(
//     AlgorithmType type,
//     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> inputQueue,
//     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> outputQueue,
//     ThreadManager& threadManager,
//     std::shared_ptr<ISystemMetricsAggregator> aggregator) {
    
//     auto algo = std::make_shared<AlgorithmConcrete>(std::move(inputQueue), std::move(outputQueue), threadManager, aggregator, nullptr);
    
//     // [FIX] C++11 Compatible Struct Initialization (No designated initializers)
//     AlgorithmConfig cfg;
//     cfg.algorithmType = type;
//     algo->configure(cfg);
    
//     return algo;
// }

// inline void AlgorithmConcrete::startAlgorithm() {
//     std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);

//     if (workerStarted_ || running_.load(std::memory_order_acquire)) {
//         spdlog::warn("[AlgorithmConcrete] Already running");
//         return;
//     }

//     starved_.store(false, std::memory_order_release);
//     running_.store(true, std::memory_order_release);

//     windowStart_ = std::chrono::steady_clock::now();
//     windowFrames_ = 0;
//     windowProcMs_ = 0.0;
//     fpsWindowStart_  = std::chrono::steady_clock::now();
//     fpsWindowFrames_ = 0;
//     fpsWindowProcMs_ = 0.0;

//     // GPU resource pre-allocation is deferred to the first GPU frame dispatch,
//     // since frame dimensions (width x height) are not known until then.
//     // See: initCudaResources() call inside each launcher.
//     try {
//         threadManager_.addThread("AlgorithmProcessingZeroCopy",
//             std::thread(&AlgorithmConcrete::threadLoopZeroCopy, this));
//         workerStarted_ = true;
//     } catch (...) {
//         running_.store(false, std::memory_order_release);
//         starved_.store(false, std::memory_order_release);
//         throw;
//     }

//     spdlog::info("[AlgorithmConcrete] Started -> {}",
//                  algorithmTypeToString(algoConfig_.algorithmType));
// }

// inline void AlgorithmConcrete::stopAlgorithm() {
//     stopAlgorithm(StopMode::Shutdown);
// }

// inline void AlgorithmConcrete::stopAlgorithm(StopMode mode) {
//     std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);

//     // running_ is only the worker's execution request. workerStarted_ tracks
//     // whether ThreadManager still owns a thread that must be joined/cleaned up.
//     running_.store(false, std::memory_order_release);

//     if (!workerStarted_) {
//         return;
//     }

//     // CRITICAL OWNERSHIP RULE:
//     // inputQueueZeroCopy_ and outputQueueZeroCopy_ are pipeline-owned shared
//     // queues. AlgorithmConcrete must never stop/restart them, including during
//     // process shutdown. The bounded pop_for() in threadLoopZeroCopy() lets this
//     // worker observe running_=false and exit within one poll period.
//     threadManager_.joinThreadsFor("AlgorithmProcessingZeroCopy");

//     // Release only resources owned by this AlgorithmConcrete instance, and only
//     // after its worker has fully exited.
//     AlgorithmConcreteKernels::destroyCudaResources(cudaRes_);

//     workerStarted_ = false;
//     starved_.store(false, std::memory_order_release);

//     spdlog::info(
//         "[AlgorithmConcrete] Worker stopped ({}) - shared queues preserved",
//         mode == StopMode::HotSwap ? "hot-swap" : "shutdown");
// }

// inline bool AlgorithmConcrete::configure(const AlgorithmConfig& config) {

//     if (config.algorithmType == AlgorithmType::MedianFilter) {
//         if (config.medianWindowSize != 3 &&
//             config.medianWindowSize != 5) {

//             spdlog::error(
//                 "[AlgorithmConcrete] MedianFilter supports only "
//                 "3x3 or 5x5 windows; got {}",
//                 config.medianWindowSize);

//             return false;
//         }
//     }

//     algoConfig_ = config;

//     if (algoConfig_.algorithmType == AlgorithmType::OpticalFlow_LucasKanade) {
//         opticalFlowProcessor_ = std::make_unique<LucasKanadeOpticalFlow>(algoConfig_.opticalFlowConfig);
//         previousFrame_.reset();
//     } else {
//         opticalFlowProcessor_.reset();
//     }

//     spdlog::info("[AlgorithmConcrete] Configured ? {}", algorithmTypeToString(config.algorithmType));
//     return true;
// }

// //=================================================

// inline void AlgorithmConcrete::setErrorCallback(std::function<void(const std::string&)> cb) {
//     errorCallback_ = std::move(cb);
// }

// inline void AlgorithmConcrete::setAlgorithmType(AlgorithmType newType) {
//     std::lock_guard<std::mutex> lock(metricMutex_);
//     if (algoConfig_.algorithmType == newType) return;

//     algoConfig_.algorithmType = newType;
//     if (newType == AlgorithmType::OpticalFlow_LucasKanade) {
//         opticalFlowProcessor_ = std::make_unique<LucasKanadeOpticalFlow>(algoConfig_.opticalFlowConfig);
//         previousFrame_.reset();
//     } else {
//         opticalFlowProcessor_.reset();
//     }
// }


// //================================================================================
// // MAIN THREAD LOOP ? ADAPTIVE & ROBUST
// //================================================================================
// //==============================================================================================================
// // AlgorithmConcrete::threadLoopZeroCopy() ? OPTIMIZED WITH TIMER DIAGNOSTICS
// //==============================================================================================================
// inline void AlgorithmConcrete::threadLoopZeroCopy() {
//     static const std::chrono::milliseconds kQueuePollPeriod(50);
//     static const std::chrono::seconds kStarvationThreshold(5);

//     spdlog::info(
//         "[AlgorithmConcrete] Thread started (queue poll={}ms, starvation={}s)",
//         kQueuePollPeriod.count(), kStarvationThreshold.count());

//     bool firstFrame = true;
//     auto lastFrameArrival = std::chrono::steady_clock::now();
//     auto nextIdleDiagnostic = lastFrameArrival + std::chrono::seconds(1);

//     // Metric window tracking
//     auto windowStart = std::chrono::steady_clock::now();
//     int windowFrames = 0;
//     double windowProcMs = 0.0;

//     // Adaptation counters
//     uint64_t adaptationCounter = 0;
//     int lastConcurrency = -1;
//     hrl::Affinity lastAffinity = hrl::Affinity::Spread;

//     while (running_.load(std::memory_order_acquire)) {
//         // ========== 1. BOUNDED POP ==========
//         // A consumer must be stoppable without poisoning a queue shared with
//         // Camera/Display. pop_for() is therefore the only worker wake-up needed.
//         std::shared_ptr<ZeroCopyFrameData> inputFrame;
//         const auto popStart = std::chrono::steady_clock::now();

//         const bool gotFrame = inputQueueZeroCopy_->pop_for(
//             inputFrame, kQueuePollPeriod);

//         const auto nowAfterPop = std::chrono::steady_clock::now();
//         const double popElapsedMs = std::chrono::duration<double, std::milli>(
//             nowAfterPop - popStart).count();

//         if (!gotFrame) {
//             // Normal stop path: leave immediately and never report starvation.
//             if (!running_.load(std::memory_order_acquire)) {
//                 break;
//             }

//             const double idleSec = std::chrono::duration<double>(
//                 nowAfterPop - lastFrameArrival).count();

//             // Diagnose at most once per second. This replaces the previous busy
//             // spin that produced tens of thousands of warnings per second.
//             if (idleSec >= 1.0 && nowAfterPop >= nextIdleDiagnostic) {
//                 nextIdleDiagnostic = nowAfterPop + std::chrono::seconds(1);
//                 spdlog::warn(
//                     "[AlgorithmConcrete] No input frame for {:.2f}s "
//                     "(poll={:.2f}ms queue_size={} stopped={})",
//                     idleSec, popElapsedMs,
//                     inputQueueZeroCopy_ ? inputQueueZeroCopy_->size() : 0U,
//                     inputQueueZeroCopy_ && inputQueueZeroCopy_->isStopped());
//             }

//             if (idleSec >= static_cast<double>(kStarvationThreshold.count())) {
//                 const bool alreadyStarved =
//                     starved_.exchange(true, std::memory_order_acq_rel);
//                 if (!alreadyStarved) {
//                     reportError(firstFrame
//                         ? "ALGORITHM STARVED - NO FRAME FOR 5 SECONDS (startup)"
//                         : "ALGORITHM STARVED - NO FRAME FOR 5 SECONDS (mid-run)");
//                 }
//                 // Do NOT self-terminate. Normal application mode can recover if
//                 // the producer resumes; the thesis driver treats starved_=true
//                 // as a failed experimental replicate and exits fail-closed.
//             }
//             continue;
//         }

//         // stopAlgorithm() may have been requested while pop_for() was asleep.
//         // Never allow the old workload to process one final post-boundary frame.
//         if (!running_.load(std::memory_order_acquire)) {
//             break;
//         }

//         lastFrameArrival = nowAfterPop;
//         nextIdleDiagnostic = nowAfterPop + std::chrono::seconds(1);
//         if (starved_.exchange(false, std::memory_order_acq_rel)) {
//             spdlog::warn("[AlgorithmConcrete] Input stream recovered after starvation");
//         }

//         // ========== 2. VALIDATE FRAME ==========
//         if (!inputFrame || !inputFrame->isValid() || !inputFrame->dataPtr) {
//             spdlog::warn("[AlgorithmConcrete] Invalid frame received (pop took {:.3f}ms)",
//                          popElapsedMs);
//             continue;
//         }

//         spdlog::debug(
//             "[AlgorithmConcrete] Frame {} received (pop {:.3f}ms, queue_size={})",
//             inputFrame->frameNumber, popElapsedMs, inputQueueZeroCopy_->size());

//         // ========== 3. DYNAMIC ADAPTATION (Every ~10 frames) ==========
//         if (runtimeControls_ && (++adaptationCounter % 10) == 0) {
//             const int newConcurrency =
//                 runtimeControls_->concurrency_level.load(std::memory_order_relaxed);
//             const bool gpuEnabled =
//                 runtimeControls_->enable_gpu.load(std::memory_order_relaxed);
//             const auto newAffinity =
//                 runtimeControls_->affinity.load(std::memory_order_relaxed);

//             algoConfig_.concurrencyLevel =
//                 utils::local_clamp(newConcurrency, 1, 4);
//             algoConfig_.useGPU = gpuEnabled;

//             if (newConcurrency != lastConcurrency || newAffinity != lastAffinity) {
//                 const auto cpus = hrl::chooseCpus(
//                     algoConfig_.concurrencyLevel, newAffinity);
//                 if (!cpus.empty()) {
//                     hrl::setThreadAffinity(cpus);
//                     spdlog::debug(
//                         "[AlgorithmConcrete] Affinity updated: {} cores, {} mode",
//                         algoConfig_.concurrencyLevel,
//                         (newAffinity == hrl::Affinity::Spread ? "Spread" : "Pack"));
//                 }
//                 lastConcurrency = newConcurrency;
//                 lastAffinity = newAffinity;
//             }
//         }

//         // ========== 4. PROCESS FRAME WITH TIMING ==========
//         // Observe the currently published ERL action generation at the algorithm
//         // boundary. This is action/effect timing, separate from workload epoch.
//         uint64_t frameActionEpoch = 0;
//         if (runtimeControls_) {
//             const uint64_t actionEpoch =
//                 runtimeControls_->action_generation.load(std::memory_order_acquire);
//             frameActionEpoch = actionEpoch;
//             const uint64_t observedEpoch =
//                 runtimeControls_->algorithm_observed_generation.load(
//                     std::memory_order_relaxed);
//             if (actionEpoch != 0 && actionEpoch != observedEpoch) {
//                 const auto observedNow = std::chrono::steady_clock::now();
//                 const uint64_t observedNs = static_cast<uint64_t>(
//                     std::chrono::duration_cast<std::chrono::nanoseconds>(
//                         observedNow.time_since_epoch()).count());
//                 runtimeControls_->algorithm_observed_time_ns.store(
//                     observedNs, std::memory_order_relaxed);
//                 runtimeControls_->algorithm_observed_generation.store(
//                     actionEpoch, std::memory_order_release);

//                 const uint64_t applyEndNs =
//                     runtimeControls_->action_apply_end_ns.load(
//                         std::memory_order_acquire);
//                 if (applyEndNs > 0 && observedNs >= applyEndNs) {
//                     runtimeControls_->action_response_latency_ns.store(
//                         observedNs - applyEndNs, std::memory_order_relaxed);
//                     runtimeControls_->action_response_generation.store(
//                         actionEpoch, std::memory_order_release);
//                 }

//                 spdlog::debug(
//                     "[AlgorithmConcrete] First frame {} observed ERL action epoch {}",
//                     inputFrame->frameNumber, actionEpoch);
//             }
//         }

//         const auto procStart = std::chrono::steady_clock::now();

//         std::shared_ptr<ZeroCopyFrameData> outputFrame;
//         const bool procOk = processFrameZeroCopy(inputFrame, outputFrame);

//         const auto procEnd = std::chrono::steady_clock::now();
//         const double procElapsedMs = std::chrono::duration<double, std::milli>(
//             procEnd - procStart).count();

//         windowProcMs += procElapsedMs;

//         if (procOk && outputFrame) {
//             // Frame is enqueued downstream inside processFrameZeroCopy().
//             windowFrames++;

//             spdlog::debug(
//                 "[AlgorithmConcrete] Frame {} processed ({:.3f}ms, refcount={}, action_epoch={})",
//                 inputFrame->frameNumber, procElapsedMs,
//                 outputFrame.use_count(), frameActionEpoch);

//             if (firstFrame) {
//                 firstFrame = false;
//                 spdlog::info(
//                     "[AlgorithmConcrete] FIRST FRAME PROCESSED | Concurrency={} | GPU={}",
//                     algoConfig_.concurrencyLevel,
//                     algoConfig_.useGPU ? "ON" : "OFF");
//             }
//         } else if (algoConfig_.algorithmType !=
//                    AlgorithmType::OpticalFlow_LucasKanade) {
//             spdlog::warn(
//                 "[AlgorithmConcrete] Frame {} processing FAILED ({:.3f}ms)",
//                 inputFrame->frameNumber, procElapsedMs);
//         }

//         // ========== 5. UPDATE METRICS EVERY 1 SECOND ==========
//         const auto now = std::chrono::steady_clock::now();
//         const double elapsed =
//             std::chrono::duration<double>(now - windowStart).count();

//         if (elapsed >= 1.0) {
//             const double fps = windowFrames / elapsed;
//             const double avgProcMs =
//                 windowFrames > 0 ? windowProcMs / windowFrames : 0.0;

//             spdlog::info(
//                 "[AlgorithmConcrete] METRICS [1s window] | Frames: {} | FPS: {:.1f} | Avg Proc: {:.2f}ms",
//                 windowFrames, fps, avgProcMs);

//             if (windowFrames > 0) {
//                 updateMetrics(elapsed, inputFrame->frameNumber);
//             }

//             windowStart = now;
//             windowFrames = 0;
//             windowProcMs = 0.0;
//         }
//     }

//     spdlog::info("[AlgorithmConcrete] Thread exited cleanly");
// }

// // inline void AlgorithmConcrete::threadLoopZeroCopy() {
// //     spdlog::info("[AlgorithmConcrete] Thread started (starvation timeout: 5s)");

// //     // ========== TIMER INITIALIZATION ==========
// //     auto starvationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
// //     bool firstFrame = true;
    
// //     // Metric window tracking
// //     auto windowStart = std::chrono::steady_clock::now();
// //     int windowFrames = 0;
// //     double windowProcMs = 0.0;
    
// //     // Adaptation counters
// //     uint64_t adaptationCounter = 0;
// //     int lastConcurrency = -1;
// //     hrl::Affinity lastAffinity = hrl::Affinity::Spread;

// //     while (running_) {
// //         // ========== 1. POP FROM QUEUE WITH TIMEOUT TRACKING ==========
// //         std::shared_ptr<ZeroCopyFrameData> inputFrame;
        
// //         auto pop_start = std::chrono::high_resolution_clock::now();
// //         const int pop_timeout_ms = 100;  // ? Explicit timeout constant
        
// //         //if (!inputQueueZeroCopy_->pop(inputFrame, std::chrono::milliseconds(pop_timeout_ms))) {
// //         if (!inputQueueZeroCopy_->pop(inputFrame)) {
// //             auto pop_elapsed_ms = std::chrono::duration<double, std::milli>(
// //                 std::chrono::high_resolution_clock::now() - pop_start).count();
            
// //             // ? LOG DIAGNOSTIC INFO
// //             spdlog::warn("[AlgorithmConcrete] Queue pop TIMEOUT (waited {}ms, queue_size={})", 
// //                         pop_elapsed_ms, inputQueueZeroCopy_->size());
            
// //             // ? STARVATION CHECK: Has producer died?
// //             if (std::chrono::steady_clock::now() > starvationDeadline) {
// //                 if (firstFrame) {
// //                     reportError("ALGORITHM STARVED ? NO FRAME FOR 5 SECONDS (startup)");
// //                 } else {
// //                     reportError("ALGORITHM STARVED ? NO FRAME FOR 5 SECONDS (mid-run)");
// //                 }
// //                 running_ = false;
// //                 break;
// //             }
// //             continue;
// //         }
        
// //         // ? FRAME RECEIVED: Reset starvation timer for next batch
// //         starvationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        
// //         auto pop_elapsed_ms = std::chrono::duration<double, std::milli>(
// //             std::chrono::high_resolution_clock::now() - pop_start).count();
        
// //         // ========== 2. VALIDATE FRAME ==========
// //         if (!inputFrame || !inputFrame->isValid() || !inputFrame->dataPtr) {
// //             spdlog::warn("[AlgorithmConcrete] Invalid frame received (pop took {}ms)", pop_elapsed_ms);
// //             continue;
// //         }
        
// //         spdlog::debug("[AlgorithmConcrete] Frame {} received (pop took {}ms, queue_size={})", 
// //                      inputFrame->frameNumber, pop_elapsed_ms, inputQueueZeroCopy_->size());

// //         // ========== 3. DYNAMIC ADAPTATION (Every ~10 frames) ==========
// //         if (runtimeControls_ && (++adaptationCounter % 10) == 0) {
// //             const int newConcurrency = runtimeControls_->concurrency_level.load(std::memory_order_relaxed);
// //             const bool gpuEnabled    = runtimeControls_->enable_gpu.load(std::memory_order_relaxed);
// //             const auto newAffinity   = runtimeControls_->affinity.load(std::memory_order_relaxed);

// //             algoConfig_.concurrencyLevel = utils::local_clamp(newConcurrency, 1, 4);
// //             algoConfig_.useGPU = gpuEnabled;

// //             if (newConcurrency != lastConcurrency || newAffinity != lastAffinity) {
// //                 const auto cpus = hrl::chooseCpus(algoConfig_.concurrencyLevel, newAffinity);
// //                 if (!cpus.empty()) {
// //                     hrl::setThreadAffinity(cpus);
// //                     spdlog::debug("[AlgorithmConcrete] Affinity updated: {} cores, {} mode",
// //                                   algoConfig_.concurrencyLevel,
// //                                   (newAffinity == hrl::Affinity::Spread ? "Spread" : "Pack"));
// //                 }
// //                 lastConcurrency = newConcurrency;
// //                 lastAffinity = newAffinity;
// //             }
// //         }

// //         // ========== 4. PROCESS FRAME WITH TIMING ==========
// //         auto proc_start = std::chrono::high_resolution_clock::now();
        
// //         std::shared_ptr<ZeroCopyFrameData> outputFrame;
// //         bool proc_ok = processFrameZeroCopy(inputFrame, outputFrame);
        
// //         auto proc_elapsed_ms = std::chrono::duration<double, std::milli>(
// //             std::chrono::high_resolution_clock::now() - proc_start).count();
        
// //         windowProcMs += proc_elapsed_ms;  // ? Accumulate for averaging
        
// //         if (proc_ok && outputFrame) {
// //             outputQueueZeroCopy_->push(outputFrame);
// //             windowFrames++;  // ? Count successful frames
            
// //             spdlog::debug("[AlgorithmConcrete] Frame {} processed ({}ms, refcount={})", 
// //                          inputFrame->frameNumber, proc_elapsed_ms, outputFrame.use_count());

// //             if (firstFrame) {
// //                 firstFrame = false;
// //                 spdlog::info("[AlgorithmConcrete] FIRST FRAME PROCESSED | Concurrency={} | GPU={}",
// //                              algoConfig_.concurrencyLevel, algoConfig_.useGPU ? "ON" : "OFF");
// //             }
// //         } else {
// //             spdlog::warn("[AlgorithmConcrete] Frame {} processing FAILED ({}ms)", 
// //                         inputFrame->frameNumber, proc_elapsed_ms);
// //         }

// //         // ========== 5. UPDATE METRICS EVERY 1 SECOND ==========
// //         auto now = std::chrono::steady_clock::now();
// //         double elapsed = std::chrono::duration<double>(now - windowStart).count();
        
// //         if (elapsed >= 1.0) {
// //             // ? Calculate per-second stats
// //             double fps = windowFrames / elapsed;
// //             double avg_proc_ms = windowFrames > 0 ? windowProcMs / windowFrames : 0.0;
            
// //             spdlog::info("[AlgorithmConcrete] METRICS [1s window] | Frames: {} | FPS: {:.1f} | Avg Proc: {:.2f}ms",
// //                         windowFrames, fps, avg_proc_ms);
            
// //             // ? Call updateMetrics with current frame info
// //             if (windowFrames > 0) {
// //                 updateMetrics(elapsed, inputFrame->frameNumber);
// //             }
            
// //             // ? RESET for next window
// //             windowStart = now;
// //             windowFrames = 0;
// //             windowProcMs = 0.0;
// //         }
// //     }

// //     spdlog::info("[AlgorithmConcrete] Thread exited cleanly");
// // }

// //================================================================================
// // processFrameZeroCopy ? FINAL HRL ADAPTIVE VERSION (GPU Switching + Fallbacks)
// //================================================================================
// inline bool AlgorithmConcrete::processFrameZeroCopy(
//     const std::shared_ptr<ZeroCopyFrameData>& inputFrame,
//     std::shared_ptr<ZeroCopyFrameData>& outputFrame)
// {
//     if (!inputFrame || !inputFrame->isValid() || !inputFrame->dataPtr || inputFrame->size == 0) {
//         spdlog::error("[AlgorithmConcrete] Invalid input frame");
//         return false;
//     }

//     const uint64_t frameId = inputFrame->frameNumber;
//     const auto t_start_steady = std::chrono::steady_clock::now();
//     const auto t_start_sys = std::chrono::system_clock::now();

//     processedBuffer_.resize(inputFrame->size);
//     double cudaKernelTimeMs = 0.0;

//     // === SINGLE SOURCE OF TRUTH: GPU ENABLED? ===
//     // Config default ? overridden by Scheduler (most up-to-date)
//     bool useGPU = algoConfig_.useGPU;

// /*
// Why are there TWO switch (algoConfig_.algorithmType) blocks?
// This two-switch design is actually an excellent embedded software pattern known as Policy vs. Mechanism Separation.

// The First Switch (The Policy Enforcer): This block determines the Hardware Capability Policy. 
// In our ERL framework, the Reinforcement Learning agent (or static config) might request useGPU = true globally. 
// However, algorithms like GaussianBlur or Grayscale might only have CPU implementations in your framework. 
// This first switch acts as a sanitizer: it overrides the ERL's request and forces useGPU = false for algorithms that physically cannot run on the GPU. 
// This guarantees that useGPU is a 100% reliable "Single Source of Truth" before any execution begins.
// */
//     switch (algoConfig_.algorithmType) {
//         // ------------------- CPU-Only Algorithms (Never use GPU) -------------------
//         case AlgorithmType::Invert:
//         case AlgorithmType::Grayscale:
//         case AlgorithmType::EdgeDetection:
//         //case AlgorithmType::MedianFilter:
//         case AlgorithmType::GaussianBlur:
//             useGPU = false;
//             break;

//         // ------------------- Hybrid Algorithms (Respect GPU Toggle) -------------------
//         default:
//             break; // use current algoConfig_.useGPU
//     }

//     // ------------------- Dispatch with fallback -------------------

//     /*
//     The Second Switch (The Execution Mechanism): This block handles the actual Dispatch Mechanism. 
//     Because the first switch already sanitized the useGPU boolean, the second switch doesn't have to worry about whether a GPU kernel actually exists for the given algorithm. 
//     It simply checks if (useGPU) and confidently launches the CUDA kernel, knowing that if it reaches that block, it is safe to do so.
//     */
//     switch (algoConfig_.algorithmType) {
//         case AlgorithmType::Invert:
//             processInvertZeroCopy(inputFrame);
//             break;
//         case AlgorithmType::Grayscale:
//             processGrayscaleZeroCopy(inputFrame);
//             break;
//         case AlgorithmType::EdgeDetection:
//             processEdgeDetectionZeroCopy(inputFrame);
//             break;
//         case AlgorithmType::GaussianBlur:
//             processGaussianBlurZeroCopy(inputFrame);
//             break;

//         case AlgorithmType::SobelEdge:
//             if (useGPU) {
//                 try {
//                     cudaKernelTimeMs = timeCudaSectionMs([&]{
//                         AlgorithmConcreteKernels::launchSobelEdgeKernel(cudaRes_, inputFrame, processedBuffer_);
//                     });
//                 } catch (const std::runtime_error& e) {
//                     spdlog::warn("[AlgorithmConcrete] GPU Sobel failed ({}); falling back to CPU edge detection", e.what());
//                     algoConfig_.useGPU = false;
//                     processEdgeDetectionZeroCopy(inputFrame);
//                 }
//             } else {
//                 processEdgeDetectionZeroCopy(inputFrame);  // Best-effort CPU fallback
//             }
//             break;

//         // case AlgorithmType::MedianFilter:
//         //     if (useGPU) {
//         //         try {
//         //             cudaKernelTimeMs = timeCudaSectionMs([&]{
//         //                 AlgorithmConcreteKernels::launchMedianFilterKernel(cudaRes_, inputFrame, processedBuffer_, algoConfig_.medianWindowSize);
//         //             });
//         //         } catch (const std::runtime_error& e) {
//         //             spdlog::warn("[AlgorithmConcrete] GPU Median failed ({}); falling back to pass-through", e.what());
//         //             algoConfig_.useGPU = false;
//         //             std::memcpy(processedBuffer_.data(), inputFrame->dataPtr, inputFrame->size);
//         //         }
//         //     } else {
//         //         // No CPU median - pass-through to save energy
//         //         std::memcpy(processedBuffer_.data(), inputFrame->dataPtr, inputFrame->size);
//         //     }
//         //     break;

//         case AlgorithmType::MedianFilter:
//         if (useGPU) {
//             try {
//                 cudaKernelTimeMs =
//                     timeCudaSectionMs([&] {
//                         AlgorithmConcreteKernels::
//                             launchMedianFilterKernel(
//                                 cudaRes_,
//                                 inputFrame,
//                                 processedBuffer_,
//                                 algoConfig_.medianWindowSize);
//                     });
//             }
//             catch (const std::exception& e) {
//                 spdlog::warn(
//                     "[AlgorithmConcrete] GPU Median failed ({}); "
//                     "falling back to CPU MedianFilter",
//                     e.what());

//                 processMedianFilterCPUZeroCopy(inputFrame);
//                 cudaKernelTimeMs = 0.0;
//             }
//         } else {
//             processMedianFilterCPUZeroCopy(inputFrame);
//             cudaKernelTimeMs = 0.0;
//         }
//         break;

//         case AlgorithmType::HistogramEqualization:
//             if (useGPU) {
//                 try {
//                     cudaKernelTimeMs = timeCudaSectionMs([&] {
//                         AlgorithmConcreteKernels::launchHistogramEqualizationKernel(
//                             cudaRes_, inputFrame, processedBuffer_);
//                     });
//                 } catch (const std::runtime_error& e) {
//                     spdlog::warn(
//                         "[AlgorithmConcrete] GPU HistEq failed ({}); falling back to CPU histogram equalization",
//                         e.what());
//                     processHistogramEqualizationCPUZeroCopy(inputFrame);
//                     cudaKernelTimeMs = 0.0;
//                 }
//             } else {
//                 processHistogramEqualizationCPUZeroCopy(inputFrame);
//                 cudaKernelTimeMs = 0.0;
//             }
//             break;

//         case AlgorithmType::HeterogeneousGaussianBlur:
//             if (useGPU) {
//                 try {
//                     // [Micro-scheduler] Read the continuous GPU workload fraction
//                     // the Scheduler published; the kernel splits rows CPU/GPU by it.
//                     double gpuSplit = runtimeControls_
//                         ? runtimeControls_->gpu_workload_split.load(std::memory_order_relaxed)
//                         : 1.0;
//                     cudaKernelTimeMs = timeCudaSectionMs([&]{
//                         AlgorithmConcreteKernels::launchHeterogeneousGaussianBlurKernel(
//                             cudaRes_, inputFrame, processedBuffer_, algoConfig_.blurRadius, gpuSplit);
//                     });
//                 } catch (const std::runtime_error& e) {
//                     spdlog::warn("[AlgorithmConcrete] GPU Gaussian failed ({}); falling back to CPU blur", e.what());
//                     algoConfig_.useGPU = false;
//                     processGaussianBlurZeroCopy(inputFrame);
//                 }
//             } else {
//                 processGaussianBlurZeroCopy(inputFrame);
//             }
//             break;

//         case AlgorithmType::OpticalFlow_LucasKanade:
//             processOpticalFlow(inputFrame);
//             goto metrics_only;

//         default:
//             std::memcpy(processedBuffer_.data(), inputFrame->dataPtr, inputFrame->size);
//             break;
//     }

//     // ------------------- Output Frame Construction -------------------
//     // Construct the output frame here, but publish it to Display only AFTER
//     // AlgorithmStats for the same frame have been merged into the aggregator.
//     {
//         // FIX: Copy instead of std::move to preserve internal buffer capacity and avoid heap allocation churn
//         // [PERF] Restored std::move (an external pass changed this to a copy
//         // claiming it saved per-frame allocations - it does not: the output
//         // vector's element buffer must be freshly allocated per frame either
//         // way because ZeroCopyFrameData shares ownership downstream with
//         // unbounded lifetime; the copy only ADDED a ~150 KB memcpy per frame.
//         // After the move processedBuffer_ is empty; the next frame's resize()
//         // re-allocates it - exactly the original v88 semantics.)
//         auto data = std::make_shared<std::vector<uint8_t>>(std::move(processedBuffer_));
        
//         outputFrame = std::make_shared<ZeroCopyFrameData>(
//             data, data->data(), 
//             data->size(),
//             inputFrame->width, inputFrame->height, -1,
//             inputFrame->frameNumber,
//             inputFrame->captureSys, inputFrame->captureSteady
//         );
//     }

// metrics_only:
//     const auto t_end_steady = std::chrono::steady_clock::now();
//     const auto t_end_sys = std::chrono::system_clock::now();
//     lastProcessingTime_ = std::chrono::duration<double, std::milli>(t_end_steady - t_start_steady).count();

//     // -- Sliding 1-second FPS window (feeds metricAggregator / CSV) ----------
//     fpsWindowFrames_++;
//     fpsWindowProcMs_ += lastProcessingTime_;
//     totalFrames_++;
//     totalProcMs_ += lastProcessingTime_;

//     double currentMeasuredFps = 0.0;
//     {
//         const double fpsElapsed = std::chrono::duration<double>(
//             std::chrono::steady_clock::now() - fpsWindowStart_).count();

//         if (fpsElapsed >= 1.0) {
//             // Compute rate for the completed 1-second window
//             currentMeasuredFps = fpsWindowFrames_ / fpsElapsed;

//             // Reset for the next window
//             fpsWindowStart_   = std::chrono::steady_clock::now();
//             fpsWindowFrames_  = 0;
//             fpsWindowProcMs_  = 0.0;
//         } else if (fpsElapsed > 0.001) {
//             // Partial window: report running rate without resetting
//             currentMeasuredFps = fpsWindowFrames_ / fpsElapsed;
//         } else {
//             currentMeasuredFps = 0.0;
//         }
//     }
//     // Keep the class-level window accumulators in sync (used by updateMetrics)
//     windowFrames_++;
//     windowProcMs_ += lastProcessingTime_;

//     uint32_t dropped = 0;
//     if (haveLastSeq_ && frameId > lastFrameSeq_ + 1) {
//         dropped = static_cast<uint32_t>(std::min<uint64_t>(frameId - lastFrameSeq_ - 1, UINT32_MAX));
//     }
//     lastFrameSeq_ = frameId;
//     haveLastSeq_ = true;

// #ifdef __CUDACC__
//     size_t gpuFree = 0, gpuTotal = 0;
//     if (useGPU && cudaMemGetInfo(&gpuFree, &gpuTotal) == cudaSuccess && gpuFree < 80ULL * 1024 * 1024) {
//         spdlog::warn("[AlgorithmConcrete] GPU Low Memory: {} MiB free", gpuFree / (1024*1024));
//     }
// #endif

//     // ------------------------------------------------------------------
//     // Dynamic-workload provenance publication.
//     // ------------------------------------------------------------------
//     // Publish frame data first and workload epoch LAST with release semantics.
//     // Readers acquire the epoch first; observing a new epoch therefore also
//     // makes the associated frame id visible as one coherent proof pair.
//     if (runtimeControls_) {
//         const uint64_t activeWorkloadEpoch =
//             runtimeControls_->active_workload_epoch.load(
//                 std::memory_order_acquire);

//         runtimeControls_->algorithm_processed_frame_id.store(
//             frameId, std::memory_order_relaxed);
//         runtimeControls_->algorithm_processed_workload_epoch.store(
//             activeWorkloadEpoch, std::memory_order_release);
//     }

//     if (metricAggregator_) {
//         AlgorithmStats as(t_start_sys, t_end_sys);
//         as.inferenceTimeMs   = lastProcessingTime_;
//         as.fps               = currentMeasuredFps;
//         as.avgProcTimeMs     = windowFrames_ > 0 ? windowProcMs_ / windowFrames_ : 0.0;
//         as.totalProcTimeMs   = totalProcMs_;
//         as.cudaKernelTimeMs  = cudaKernelTimeMs;
//         as.droppedFrames     = dropped;
// #ifdef __CUDACC__
//         if (useGPU) {
//             as.gpuFreeMemory  = gpuFree;
//             as.gpuTotalMemory = gpuTotal;
//         }
// #endif
//         metricAggregator_->mergeAlgorithm(frameId, as);
//     }

//     // [FIX ORDERING] Publish the processed frame only after AlgorithmStats for
//     // this frame are visible to the aggregator. This prevents Display from
//     // finalizing/erasing the pending frame before mergeAlgorithm() arrives.
//     // OpticalFlow uses its own output path, so outputFrame may be null here.
//     if (outputFrame) {
//         if (outputQueueZeroCopy_) {
//             outputQueueZeroCopy_->push(outputFrame);
//             spdlog::debug(
//                 "[AlgorithmConcrete] Pushed processed frame {} to outputQueueZeroCopy_ (refcount={})",
//                 frameId, outputFrame.use_count());
//         } else {
//             spdlog::error(
//                 "[AlgorithmConcrete] outputQueueZeroCopy_ is null - cannot show processed frame");
//         }
//     }

//     return algoConfig_.algorithmType != AlgorithmType::OpticalFlow_LucasKanade;
// }
// // //================================================================================
// // // processFrameZeroCopy ? FINAL HRL ADAPTIVE VERSION (GPU Switching + Fallbacks)
// // //================================================================================
// // inline bool AlgorithmConcrete::processFrameZeroCopy(
// //     const std::shared_ptr<ZeroCopyFrameData>& inputFrame,
// //     std::shared_ptr<ZeroCopyFrameData>& outputFrame)
// // {
// //     if (!inputFrame || !inputFrame->isValid() || !inputFrame->dataPtr || inputFrame->size == 0) {
// //         spdlog::error("[AlgorithmConcrete] Invalid input frame");
// //         return false;
// //     }

// //     const uint64_t frameId = inputFrame->frameNumber;
// //     const auto t_start_steady = std::chrono::steady_clock::now();
// //     const auto t_start_sys = std::chrono::system_clock::now();

// //     processedBuffer_.resize(inputFrame->size);
// //     double cudaKernelTimeMs = 0.0;

// //     // === SINGLE SOURCE OF TRUTH: GPU ENABLED? ===
// //     // Config default ? overridden by Scheduler (most up-to-date)
// //     bool useGPU = algoConfig_.useGPU;


// // /*
// // Why are there TWO switch (algoConfig_.algorithmType) blocks?
// // This two-switch design is actually an excellent embedded software pattern known as Policy vs. Mechanism Separation.

// // The First Switch (The Policy Enforcer): This block determines the Hardware Capability Policy. 
// // In our ERL framework, the Reinforcement Learning agent (or static config) might request useGPU = true globally. 
// // However, algorithms like GaussianBlur or Grayscale might only have CPU implementations in your framework. 
// // This first switch acts as a sanitizer: it overrides the ERL's request and forces useGPU = false for algorithms that physically cannot run on the GPU. 
// // This guarantees that useGPU is a 100% reliable "Single Source of Truth" before any execution begins.
// // */
// //     switch (algoConfig_.algorithmType) {
// //         // ??????????????????? CPU-Only Algorithms (Never use GPU) ???????????????????
// //         case AlgorithmType::Invert:
// //         case AlgorithmType::Grayscale:
// //         case AlgorithmType::EdgeDetection:
// //         //case AlgorithmType::MedianFilter:
// //         case AlgorithmType::GaussianBlur:
// //             useGPU = false;
// //             break;

// //         // ??????????????????? Hybrid Algorithms (Respect GPU Toggle) ???????????????????
// //         default:
// //             break; // use current algoConfig_.useGPU
// //     }

// //     // ??????????????????? Dispatch with fallback ???????????????????

// //     /*
// //     The Second Switch (The Execution Mechanism): This block handles the actual Dispatch Mechanism. 
// //     Because the first switch already sanitized the useGPU boolean, the second switch doesn't have to worry about whether a GPU kernel actually exists for the given algorithm. 
// //     It simply checks if (useGPU) and confidently launches the CUDA kernel, knowing that if it reaches that block, it is safe to do so.
// //     */
// //     switch (algoConfig_.algorithmType) {
// //         case AlgorithmType::Invert:
// //             processInvertZeroCopy(inputFrame);
// //             break;
// //         case AlgorithmType::Grayscale:
// //             processGrayscaleZeroCopy(inputFrame);
// //             break;
// //         case AlgorithmType::EdgeDetection:
// //             processEdgeDetectionZeroCopy(inputFrame);
// //             break;
// //         case AlgorithmType::GaussianBlur:
// //             processGaussianBlurZeroCopy(inputFrame);
// //             break;

// //         case AlgorithmType::SobelEdge:
// //             if (useGPU) {
// //                 try {
// //                     cudaKernelTimeMs = timeCudaSectionMs([&]{
// //                         AlgorithmConcreteKernels::launchSobelEdgeKernel(cudaRes_, inputFrame, processedBuffer_);
// //                     });
// //                 } catch (const std::runtime_error& e) {
// //                     spdlog::warn("[AlgorithmConcrete] GPU Sobel failed ({}); falling back to CPU edge detection", e.what());
// //                     algoConfig_.useGPU = false;
// //                     processEdgeDetectionZeroCopy(inputFrame);
// //                 }
// //             } else {
// //                 processEdgeDetectionZeroCopy(inputFrame);  // Best-effort CPU fallback
// //             }
// //             break;

// //         case AlgorithmType::MedianFilter:
// //             if (useGPU) {
// //                 try {
// //                     cudaKernelTimeMs = timeCudaSectionMs([&]{
// //                         AlgorithmConcreteKernels::launchMedianFilterKernel(cudaRes_, inputFrame, processedBuffer_, algoConfig_.medianWindowSize);
// //                     });
// //                 } catch (const std::runtime_error& e) {
// //                     spdlog::warn("[AlgorithmConcrete] GPU Median failed ({}); falling back to pass-through", e.what());
// //                     algoConfig_.useGPU = false;
// //                     std::memcpy(processedBuffer_.data(), inputFrame->dataPtr, inputFrame->size);
// //                 }
// //             } else {
// //                 // No CPU median - pass-through to save energy
// //                 std::memcpy(processedBuffer_.data(), inputFrame->dataPtr, inputFrame->size);
// //             }
// //             break;
// //         case AlgorithmType::HistogramEqualization:
// //             if (useGPU) {
// //                 try {
// //                     cudaKernelTimeMs = timeCudaSectionMs([&] {
// //                         AlgorithmConcreteKernels::launchHistogramEqualizationKernel(
// //                             cudaRes_, inputFrame, processedBuffer_);
// //                     });
// //                 } catch (const std::runtime_error& e) {
// //                     spdlog::warn(
// //                         "[AlgorithmConcrete] GPU HistEq failed ({}); falling back to CPU histogram equalization",
// //                         e.what());
// //                     processHistogramEqualizationCPUZeroCopy(inputFrame);
// //                     cudaKernelTimeMs = 0.0;
// //                 }
// //             } else {
// //                 processHistogramEqualizationCPUZeroCopy(inputFrame);
// //                 cudaKernelTimeMs = 0.0;
// //             }
// //             break;

// //         case AlgorithmType::HeterogeneousGaussianBlur:
// //             if (useGPU) {
// //                 try {
// //                     // [Micro-scheduler] Read the continuous GPU workload fraction
// //                     // the Scheduler published; the kernel splits rows CPU/GPU by it.
// //                     double gpuSplit = runtimeControls_
// //                         ? runtimeControls_->gpu_workload_split.load(std::memory_order_relaxed)
// //                         : 1.0;
// //                     cudaKernelTimeMs = timeCudaSectionMs([&]{
// //                         AlgorithmConcreteKernels::launchHeterogeneousGaussianBlurKernel(
// //                             cudaRes_, inputFrame, processedBuffer_, algoConfig_.blurRadius, gpuSplit);
// //                     });
// //                 } catch (const std::runtime_error& e) {
// //                     spdlog::warn("[AlgorithmConcrete] GPU Gaussian failed ({}); falling back to CPU blur", e.what());
// //                     algoConfig_.useGPU = false;
// //                     processGaussianBlurZeroCopy(inputFrame);
// //                 }
// //             } else {
// //                 processGaussianBlurZeroCopy(inputFrame);
// //             }
// //             break;

// //         case AlgorithmType::OpticalFlow_LucasKanade:
// //             processOpticalFlow(inputFrame);
// //             goto metrics_only;

// //         default:
// //             std::memcpy(processedBuffer_.data(), inputFrame->dataPtr, inputFrame->size);
// //             break;
// //     }

// //     // ??????????????????? Output Frame Construction ???????????????????
// // 	// === CRITICAL FIX: ALWAYS CREATE OUTPUT FRAME AND PUSH TO QUEUE ===

// //     {
// //         auto data = std::make_shared<std::vector<uint8_t>>(std::move(processedBuffer_)); // copy processed result
// //         outputFrame = std::make_shared<ZeroCopyFrameData>(
// //             data, data->data(), 
// // 		data->size(),
// //             inputFrame->width, inputFrame->height, -1,
// //             inputFrame->frameNumber,
// //             inputFrame->captureSys, inputFrame->captureSteady
// //         );

// // //===============================================
// // 	// PUSH TO DISPLAY QUEUE (this makes right side update in real-time)
// //         if (outputQueueZeroCopy_) {
// //             outputQueueZeroCopy_->push(outputFrame);
// //             spdlog::debug("[AlgorithmConcrete] Pushed processed frame {} to outputQueueZeroCopy_ (refcount={})", 
// //                          frameId, outputFrame.use_count());
// //         } else {
// //             spdlog::error("[AlgorithmConcrete] outputQueueZeroCopy_ is null - cannot show processed frame");
// //         }
// //     }

// // metrics_only:
// //     const auto t_end_steady = std::chrono::steady_clock::now();
// //     const auto t_end_sys = std::chrono::system_clock::now();
// //     lastProcessingTime_ = std::chrono::duration<double, std::milli>(t_end_steady - t_start_steady).count();

// //     // -- Sliding 1-second FPS window (feeds metricAggregator / CSV) ----------
// //     fpsWindowFrames_++;
// //     fpsWindowProcMs_ += lastProcessingTime_;
// //     totalFrames_++;
// //     totalProcMs_ += lastProcessingTime_;

// //     double currentMeasuredFps = 0.0;
// //     {
// //         const double fpsElapsed = std::chrono::duration<double>(
// //             std::chrono::steady_clock::now() - fpsWindowStart_).count();

// //         if (fpsElapsed >= 1.0) {
// //             // Compute rate for the completed 1-second window
// //             currentMeasuredFps = fpsWindowFrames_ / fpsElapsed;

// //             // Reset for the next window
// //             fpsWindowStart_   = std::chrono::steady_clock::now();
// //             fpsWindowFrames_  = 0;
// //             fpsWindowProcMs_  = 0.0;
// //         } else if (fpsElapsed > 0.001) {
// //             // Partial window: report running rate without resetting
// //             currentMeasuredFps = fpsWindowFrames_ / fpsElapsed;
// //         } else {
// //             currentMeasuredFps = 0.0;
// //         }
// //     }
// //     // Keep the class-level window accumulators in sync (used by updateMetrics)
// //     windowFrames_++;
// //     windowProcMs_ += lastProcessingTime_;

// //     uint32_t dropped = 0;
// //     if (haveLastSeq_ && frameId > lastFrameSeq_ + 1) {
// //         dropped = static_cast<uint32_t>(std::min<uint64_t>(frameId - lastFrameSeq_ - 1, UINT32_MAX));
// //     }
// //     lastFrameSeq_ = frameId;
// //     haveLastSeq_ = true;

// // #ifdef __CUDACC__
// //     size_t gpuFree = 0, gpuTotal = 0;
// //     if (useGPU && cudaMemGetInfo(&gpuFree, &gpuTotal) == cudaSuccess && gpuFree < 80ULL * 1024 * 1024) {
// //         spdlog::warn("[AlgorithmConcrete] GPU Low Memory: {} MiB free", gpuFree / (1024*1024));
// //     }
// // #endif

// //     if (metricAggregator_) {
// //         AlgorithmStats as(t_start_sys, t_end_sys);
// //         as.inferenceTimeMs   = lastProcessingTime_;
// //         as.fps               = currentMeasuredFps;
// //         as.avgProcTimeMs     = windowFrames_ > 0 ? windowProcMs_ / windowFrames_ : 0.0;
// //         as.totalProcTimeMs   = totalProcMs_;
// //         as.cudaKernelTimeMs  = cudaKernelTimeMs;
// //         as.droppedFrames     = dropped;
// // #ifdef __CUDACC__
// //         if (useGPU) {
// //             as.gpuFreeMemory  = gpuFree;
// //             as.gpuTotalMemory = gpuTotal;
// //         }
// // #endif
// //         metricAggregator_->mergeAlgorithm(frameId, as);
// //     }

// //     return algoConfig_.algorithmType != AlgorithmType::OpticalFlow_LucasKanade;
// // }

// //================================================================================

// inline void AlgorithmConcrete::updateMetrics(double elapsedSec, uint64_t frameSeq) {
//     std::lock_guard<std::mutex> lock(metricMutex_);
//     if (elapsedSec <= 0.0) elapsedSec = 1.0;

//     // Read current window, then reset it so the next call measures a fresh interval
//     const double fps   = windowFrames_ / elapsedSec;
//     const double avgMs = windowFrames_ > 0 ? (windowProcMs_ / windowFrames_) : 0.0;

//     fps_         = fps;
//     lastFPS_     = fps;
//     avgProcTime_ = avgMs;

//     // -- RESET the class-level window --------------------------------------
//     windowFrames_  = 0;
//     windowProcMs_  = 0.0;
//     windowStart_   = std::chrono::steady_clock::now();
//     // ---------------------------------------------------------------------

//     spdlog::info("[AlgorithmConcrete] Frame {} | FPS: {:.1f} | Avg: {:.2f}ms | {}", 
//                  frameSeq, fps, avgMs, algorithmTypeToString(algoConfig_.algorithmType));
// }

// inline std::string AlgorithmConcrete::algorithmTypeToString(AlgorithmType type) const {
//     switch (type) {
//         case AlgorithmType::Invert: return "Invert";
//         case AlgorithmType::Grayscale: return "Grayscale";
//         case AlgorithmType::EdgeDetection: return "EdgeDetection";
//         case AlgorithmType::GaussianBlur: return "GaussianBlur";
//         case AlgorithmType::SobelEdge: return "SobelEdge";
//         case AlgorithmType::MedianFilter: return "MedianFilter";
//         case AlgorithmType::HistogramEqualization: return "HistogramEqualization";
//         case AlgorithmType::HeterogeneousGaussianBlur: return "HeterogeneousGaussianBlur";
//         case AlgorithmType::OpticalFlow_LucasKanade: return "OpticalFlow_LK";
//         case AlgorithmType::Mandelbrot: return "Mandelbrot";
//         case AlgorithmType::MultiThreadedInvert: return "MultiThreadedInvert";
//         default: return "Unknown";
//     }
// }

// inline void AlgorithmConcrete::reportError(const std::string& msg) {
//     if (errorCallback_) errorCallback_(msg);
//     else spdlog::error("[AlgorithmConcrete] {}", msg);
// }

// //================================================================================
// // parallelFor ? HRL ADAPTIVE VERSION
// //================================================================================
// // [F8 - P2 design note, implement with a compile-test loop after run 2]
// // This spawns and joins threads PER CALL (~26-52 spawns/s at 13 fps), which
// // both costs ~0.2-0.5 ms/frame on the A57 and pollutes the concurrency
// // gene's measured signal (each increment buys parallelism PLUS spawn cost).
// // The production replacement is a persistent worker pool with three hard
// // requirements: (1) it must HONOR the gene - exactly numThreads-1 helpers
// // participate per call (caller works too); a fixed always-all-workers pool
// // re-creates F21 from the other direction; (2) a thread_local in-worker
// // guard so nested parallelFor calls degrade to serial instead of
// // deadlocking; (3) exit-safe lifetime (function-local static pool, joined
// // at process exit) so it never interacts with this class's teardown.
// inline void AlgorithmConcrete::parallelFor(size_t start, size_t end, const std::function<void(size_t)>& func) {
//     // 1. Determine thread count: Priority to RuntimeControls (HRL), fallback to Config
//     size_t numThreads = algoConfig_.concurrencyLevel;
    
//     if (runtimeControls_) {
//         int dynLevel = runtimeControls_->concurrency_level.load(std::memory_order_relaxed);
//         if (dynLevel > 0) {
//             numThreads = static_cast<size_t>(dynLevel);
//         }
//     }

//     // 2. Serial fallback for genuinely tiny workloads or single-thread mode.
//     // [F21-FIX] The old absolute guard `(end - start) < 1000` made EVERY
//     // row-parallel call (parallelFor(0, h, ...) with h = 240 at 320x240 -
//     // EdgeDetection, GaussianBlur, and the CPU share of heterogeneous
//     // paths) run unconditionally SERIAL, so the ERL's concurrency_level
//     // gene actuated nothing on those workloads. Run 2026-07-20 corroborated
//     // it: runtime_concurrency varied 2..4 across generations with no
//     // throughput coupling. The guard now scales with the requested thread
//     // count (>= 16 iterations per worker): 240 rows / 4 threads = 60 rows
//     // ~= 180k ops per worker, far above thread-spawn cost, so row loops
//     // parallelize; genuinely tiny loops (< numThreads*16) stay serial.
//     if (numThreads <= 1 || (end - start) < numThreads * 16) {
//         for (size_t i = start; i < end; ++i) func(i);
//         return;
//     }

//     // 3. Parallel Execution
//     std::vector<std::thread> workers;
//     workers.reserve(numThreads);
    
//     const size_t chunkSize = (end - start + numThreads - 1) / numThreads;

//     for (size_t t = 0; t < numThreads; ++t) {
//         size_t s = start + t * chunkSize;
//         if (s >= end) break;
//         size_t e = std::min(end, s + chunkSize);
        
//         workers.emplace_back([=] { 
//             for (size_t j = s; j < e; j++) func(j); 
//         });
//     }
    
//     for (auto& w : workers) w.join();
// }

// // CPU Operations
// inline void AlgorithmConcrete::processInvertZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
//     const uint8_t* in = static_cast<const uint8_t*>(frame->dataPtr);
//     parallelFor(0, frame->size, [this, in](size_t i) { processedBuffer_[i] = ~in[i]; });
// }

// inline void AlgorithmConcrete::processGrayscaleZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
//     const uint8_t* in = static_cast<const uint8_t*>(frame->dataPtr);
//     parallelFor(0, frame->size, [this, in](size_t i) { processedBuffer_[i] = (i % 2 == 0) ? in[i] : 128; });
// }

// inline void AlgorithmConcrete::processEdgeDetectionZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
//     const uint8_t* in = static_cast<const uint8_t*>(frame->dataPtr);
//     const int w = frame->width, h = frame->height;
//     parallelFor(0, h, [this, in, w](size_t y) {
//         for (int x = 0; x < w; ++x) {
//             int idx = y * w * 2 + x * 2;
//             if (x > 0 && x < w - 1) {
//                 int grad = std::abs(static_cast<int>(in[idx]) - static_cast<int>(in[idx - 2]));
//                 processedBuffer_[idx] = static_cast<uint8_t>(grad);
//                 processedBuffer_[idx + 1] = 128;
//             } else {
//                 processedBuffer_[idx] = in[idx];
//                 processedBuffer_[idx + 1] = in[idx + 1];
//             }
//         }
//     });
// }


// //==============================================================================================================
// // ====================================================================================
// // ACTION REQUIRED:
// // Open 'AlgorithmConcrete_new.h'
// // Find: inline void AlgorithmConcrete::processGaussianBlurZeroCopy(...)
// // Replace the ENTIRE function with this structurally safe version:
// // ====================================================================================

// inline void AlgorithmConcrete::processGaussianBlurZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
//     const int r = algoConfig_.blurRadius;
//     const int w = frame->width, h = frame->height;
//     const uint8_t* in = static_cast<const uint8_t*>(frame->dataPtr);
//     std::vector<uint8_t> temp(frame->size);

//     // ------------------------------------------------------------------
//     // CRITICAL FIX: PRE-CALCULATE WEIGHTS
//     // Prevents 112ms bottleneck and subsequent V4L2 camera driver crash.
//     // ------------------------------------------------------------------
//     if (r <= 0) return; 
//     std::vector<float> weights(2 * r + 1);
//     float wsum_total = 0.0f;
//     for (int d = -r; d <= r; ++d) {
//         weights[d + r] = std::exp(-(d * d) / (2.0f * r * r));
//         wsum_total += weights[d + r];
//     }
//     // Normalize weights so they sum to 1.0
//     for (int d = -r; d <= r; ++d) {
//         weights[d + r] /= wsum_total;
//     }

//     // Horizontal pass
//     parallelFor(0, h, [in, w, r, &temp, &weights](size_t y) {
//         for (int x = 0; x < w; ++x) {
//             float sum = 0.0f;
//             for (int d = -r; d <= r; ++d) {
//                 int xx = utils::local_clamp(x + d, 0, w - 1);
//                 sum += in[y * w * 2 + xx * 2] * weights[d + r];
//             }
//             temp[y * w * 2 + x * 2] = static_cast<uint8_t>(sum);
//             temp[y * w * 2 + x * 2 + 1] = 128; // U/V placeholder
//         }
//     });

//     // Vertical pass
//     parallelFor(0, w, [&temp, w, h, r, this, &weights](size_t x) {
//         for (int y = 0; y < h; ++y) {
//             float sum = 0.0f;
//             for (int d = -r; d <= r; ++d) {
//                 int yy = utils::local_clamp(static_cast<int>(y) + d, 0, h - 1);
//                 sum += temp[yy * w * 2 + x * 2] * weights[d + r];
//             }
//             processedBuffer_[y * w * 2 + x * 2] = static_cast<uint8_t>(sum);
//             processedBuffer_[y * w * 2 + x * 2 + 1] = 128; // U/V placeholder
//         }
//     });
// }
// //==============================================================================================================
// // inline void AlgorithmConcrete::processGaussianBlurZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
// //     const int r = algoConfig_.blurRadius;
// //     const int w = frame->width, h = frame->height;
// //     const uint8_t* in = static_cast<const uint8_t*>(frame->dataPtr);
// //     std::vector<uint8_t> temp(frame->size);

// //     // 1. PRE-CALCULATE WEIGHTS to fix 112ms bottleneck
// //     if (r <= 0) return; // Prevent division by zero
// //     std::vector<float> weights(2 * r + 1);
// //     float wsum_total = 0.0f;
// //     for (int d = -r; d <= r; ++d) {
// //         weights[d + r] = std::exp(-(d * d) / (2.0f * r * r));
// //         wsum_total += weights[d + r];
// //     }
    
// //     // Normalize weights so they sum to 1.0
// //     for (int d = -r; d <= r; ++d) {
// //         weights[d + r] /= wsum_total;
// //     }

// //     // 2. Horizontal pass
// //     parallelFor(0, h, [in, w, r, &temp, &weights](size_t y) {
// //         for (int x = 0; x < w; ++x) {
// //             float sum = 0.0f;
// //             for (int d = -r; d <= r; ++d) {
// //                 int xx = utils::local_clamp(x + d, 0, w - 1);
// //                 sum += in[y * w * 2 + xx * 2] * weights[d + r];
// //             }
// //             temp[y * w * 2 + x * 2] = static_cast<uint8_t>(sum);
// //             temp[y * w * 2 + x * 2 + 1] = 128; // U/V placeholder
// //         }
// //     });

// //     // 3. Vertical pass
// //     parallelFor(0, w, [&temp, w, h, r, this, &weights](size_t x) {
// //         for (int y = 0; y < h; ++y) {
// //             float sum = 0.0f;
// //             for (int d = -r; d <= r; ++d) {
// //                 int yy = utils::local_clamp(static_cast<int>(y) + d, 0, h - 1);
// //                 sum += temp[yy * w * 2 + x * 2] * weights[d + r];
// //             }
// //             processedBuffer_[y * w * 2 + x * 2] = static_cast<uint8_t>(sum);
// //             processedBuffer_[y * w * 2 + x * 2 + 1] = 128;
// //         }
// //     });
// // }
// //==============================================================================================================

// // inline void AlgorithmConcrete::processGaussianBlurZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
// //     const int r = algoConfig_.blurRadius;
// //     const int w = frame->width, h = frame->height;
// //     const uint8_t* in = static_cast<const uint8_t*>(frame->dataPtr);
// //     std::vector<uint8_t> temp(frame->size);

// //     parallelFor(0, h, [in, w, r, &temp](size_t y) {
// //         for (int x = 0; x < w; ++x) {
// //             float sum = 0, wsum = 0;
// //             for (int d = -r; d <= r; ++d) {
// //                 int xx = utils::local_clamp(x + d, 0, w - 1);
// //                 float wt = std::exp(-(d * d) / (2.0f * r * r));
// //                 sum += in[y * w * 2 + xx * 2] * wt;
// //                 wsum += wt;
// //             }
// //             temp[y * w * 2 + x * 2] = static_cast<uint8_t>(sum / wsum);
// //             temp[y * w * 2 + x * 2 + 1] = 128;
// //         }
// //     });

// //     parallelFor(0, w, [&temp, w, h, r, this](size_t x) {
// //         for (int y = 0; y < h; ++y) {
// //             float sum = 0, wsum = 0;
// //             for (int d = -r; d <= r; ++d) {
// //                 int yy = utils::local_clamp(static_cast<int>(y) + d, 0, h - 1);
// //                 float wt = std::exp(-(d * d) / (2.0f * r * r));
// //                 sum += temp[yy * w * 2 + x * 2] * wt;
// //                 wsum += wt;
// //             }
// //             processedBuffer_[y * w * 2 + x * 2] = static_cast<uint8_t>(sum / wsum);
// //             processedBuffer_[y * w * 2 + x * 2 + 1] = 128;
// //         }
// //     });
// // }

// // Note: The above CPU implementations are intentionally simple and not optimized for performance.

// //===============================================================================================================================
// inline void AlgorithmConcrete::processMedianFilterCPUZeroCopy(
//     const std::shared_ptr<ZeroCopyFrameData>& frame)
// {
//     const int w = frame->width;
//     const int h = frame->height;
//     const int windowSize = algoConfig_.medianWindowSize;
//     const int radius = windowSize / 2;

//     const uint8_t* in =
//         static_cast<const uint8_t*>(frame->dataPtr);

//     parallelFor(0, h, [&, this](size_t y) {
//         for (int x = 0; x < w; ++x) {

//             std::array<uint8_t, 25> window{};
//             int count = 0;

//             for (int dy = -radius; dy <= radius; ++dy) {
//                 const int yy =
//                     utils::local_clamp(
//                         static_cast<int>(y) + dy, 0, h - 1);

//                 for (int dx = -radius; dx <= radius; ++dx) {
//                     const int xx =
//                         utils::local_clamp(x + dx, 0, w - 1);

//                     window[count++] =
//                         in[(yy * w + xx) * 2];
//                 }
//             }

//             auto mid = window.begin() + count / 2;

//             std::nth_element(
//                 window.begin(),
//                 mid,
//                 window.begin() + count);

//             const int idx =
//                 (static_cast<int>(y) * w + x) * 2;

//             processedBuffer_[idx] = *mid;
//             processedBuffer_[idx + 1] = 128;
//         }
//     });
// }
// //===============================================================================================================================

// inline void AlgorithmConcrete::processMultiThreadedInvert(const std::shared_ptr<ZeroCopyFrameData>& frame) {
//     const int numThreads = 4; // Jetson Nano has 4 ARM cores
//     const size_t dataSize = frame->width * frame->height;
//     const size_t chunkSize = dataSize / numThreads;
//     std::vector<std::thread> threads;

//     // Spawn threads to process chunks of the image concurrently
//     for (int i = 0; i < numThreads; ++i) {
//         size_t startIdx = i * chunkSize;
//         size_t endIdx = (i == numThreads - 1) ? dataSize : startIdx + chunkSize;
        
//         threads.emplace_back([this, frame, startIdx, endIdx]() {
//             for (size_t j = startIdx; j < endIdx; ++j) {
//                 // Replace this:
//                 //processedBuffer_[j] = 255 - frame->dataPtr[j];

//                 // With this:
//                 uint8_t* data = static_cast<uint8_t*>(frame->dataPtr);
//                 processedBuffer_[j] = 255 - data[j];
//             }
//         });
//     }

//     // Wait for all cores to finish
//     for (auto& t : threads) {
//         if (t.joinable()) t.join();
//     }
// }

// inline void AlgorithmConcrete::processMandelbrot(const std::shared_ptr<ZeroCopyFrameData>& frame) {
//     if (algoConfig_.useGPU) {
//         try {
//             AlgorithmConcreteKernels::launchMandelbrotKernel(cudaRes_, frame, processedBuffer_);
//             return;
//         } catch (const std::exception& e) {
//             reportError("[Mandelbrot GPU ERROR] " + std::string(e.what()) + ". Falling back to CPU.");
//             // Fall through to CPU implementation
//         }
//     }

//     // --- CPU Fallback implementation ---
//     const int width = frame->width;
//     const int height = frame->height;
//     const int max_iter = 100;

//     for (int y = 0; y < height; ++y) {
//         for (int x = 0; x < width; ++x) {
//             float cx = (x * 3.0f / (float)width) - 2.0f;
//             float cy = (y * 3.0f / (float)height) - 1.5f;
//             float zx = 0.0f, zy = 0.0f;
//             int iter = 0;

//             while (zx * zx + zy * zy <= 4.0f && iter < max_iter) {
//                 float tmp = zx * zx - zy * zy + cx;
//                 zy = 2.0f * zx * zy + cy;
//                 zx = tmp;
//                 iter++;
//             }
            
//             processedBuffer_[y * width + x] = (iter == max_iter) ? 0 : (uint8_t)((iter * 255) / max_iter);
//         }
//     }
// }

// // CUDA Wrappers
// inline void AlgorithmConcrete::processSobelEdgeZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
//     AlgorithmConcreteKernels::launchSobelEdgeKernel(cudaRes_, frame, processedBuffer_);
// }

// inline void AlgorithmConcrete::processMedianFilterZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
//     AlgorithmConcreteKernels::launchMedianFilterKernel(cudaRes_, frame, processedBuffer_, algoConfig_.medianWindowSize);
// }

// // inline void AlgorithmConcrete::processHistogramEqualizationZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
// //     AlgorithmConcreteKernels::launchHistogramEqualizationKernel(cudaRes_, frame, processedBuffer_);
// // }

// //=========================================================================================================================


// inline void AlgorithmConcrete::processHistogramEqualizationZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
//     // CPU implementation matching the CUDA HistEq path:
//     //   - Build histogram over the Y/luma channel only.
//     //   - Compute CDF and min non-zero CDF.
//     //   - Remap Y using (cdf[Y] - minCdf) / (N - minCdf) * 255.
//     //   - Set chroma byte to 128, matching the existing CUDA remapKernel.
//     //
//     // The framework stores frames in the same 2-bytes-per-pixel convention used
//     // by the CUDA kernels: byte 0 = Y, byte 1 = chroma placeholder.
//     if (!frame || !frame->dataPtr || frame->width <= 0 || frame->height <= 0 || frame->size == 0) {
//         spdlog::warn("[AlgorithmConcrete] CPU HistEq skipped: invalid frame");
//         return;
//     }

//     const uint8_t* in = static_cast<const uint8_t*>(frame->dataPtr);
//     const int width = frame->width;
//     const int height = frame->height;
//     const int totalPixels = width * height;
//     const size_t expectedSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 2ULL;

//     if (totalPixels <= 0 || frame->size < expectedSize) {
//         spdlog::warn("[AlgorithmConcrete] CPU HistEq size mismatch: frameSize={} expectedAtLeast={}",
//                      frame->size, expectedSize);
//         if (processedBuffer_.size() < frame->size) {
//             processedBuffer_.resize(frame->size);
//         }
//         std::memcpy(processedBuffer_.data(), in, frame->size);
//         return;
//     }

//     if (processedBuffer_.size() < frame->size) {
//         processedBuffer_.resize(frame->size);
//     }

//     std::vector<int> hist(256, 0);
//     std::vector<int> cdf(256, 0);

//     // Histogram over Y channel.
//     for (int y = 0; y < height; ++y) {
//         const int rowBase = y * width * 2;
//         for (int x = 0; x < width; ++x) {
//             const int idx = rowBase + x * 2;
//             ++hist[in[idx]];
//         }
//     }

//     int running = 0;
//     int minCdf = 0;
//     for (int i = 0; i < 256; ++i) {
//         running += hist[i];
//         cdf[i] = running;
//         if (minCdf == 0 && running > 0) {
//             minCdf = running;
//         }
//     }

//     const int denom = totalPixels - minCdf;

//     // Degenerate image: all pixels have the same luma. Keep a valid neutral output.
//     if (denom <= 0) {
//         for (int y = 0; y < height; ++y) {
//             const int rowBase = y * width * 2;
//             for (int x = 0; x < width; ++x) {
//                 const int idx = rowBase + x * 2;
//                 processedBuffer_[idx]     = in[idx];
//                 processedBuffer_[idx + 1] = 128;
//             }
//         }
//         return;
//     }

//     parallelFor(0, height, [this, in, width, &cdf, minCdf, denom](size_t y) {
//         const int rowBase = static_cast<int>(y) * width * 2;
//         for (int x = 0; x < width; ++x) {
//             const int idx = rowBase + x * 2;
//             const int yIn = static_cast<int>(in[idx]);
//             const float norm = static_cast<float>(cdf[yIn] - minCdf) / static_cast<float>(denom);
//             int yOut = static_cast<int>(norm * 255.0f);
//             yOut = utils::local_clamp(yOut, 0, 255);

//             processedBuffer_[idx]     = static_cast<uint8_t>(yOut);
//             processedBuffer_[idx + 1] = 128;
//         }
//     });
// }

// //=====================================================================================================
// inline void AlgorithmConcrete::processHistogramEqualizationCPUZeroCopy(
//     const std::shared_ptr<ZeroCopyFrameData>& frame)
// {
//     if (!frame || !frame->dataPtr || frame->width <= 0 || frame->height <= 0) {
//         return;
//     }

//     const int width = frame->width;
//     const int height = frame->height;
//     const size_t dataSize = frame->size;
//     const uint8_t* input = static_cast<const uint8_t*>(frame->dataPtr);

//     processedBuffer_.resize(dataSize);

//     std::array<int, 256> hist{};
//     std::array<int, 256> cdf{};

//     const int totalPixels = width * height;

//     for (int y = 0; y < height; ++y) {
//         const int rowBase = y * width * 2;
//         for (int x = 0; x < width; ++x) {
//             const int idx = rowBase + x * 2;
//             ++hist[input[idx]];
//         }
//     }

//     cdf[0] = hist[0];
//     for (int i = 1; i < 256; ++i) {
//         cdf[i] = cdf[i - 1] + hist[i];
//     }

//     int minCdf = 0;
//     for (int i = 0; i < 256; ++i) {
//         if (cdf[i] > 0) {
//             minCdf = cdf[i];
//             break;
//         }
//     }

//     const int denom = totalPixels - minCdf;

//     for (int y = 0; y < height; ++y) {
//         const int rowBase = y * width * 2;
//         for (int x = 0; x < width; ++x) {
//             const int idx = rowBase + x * 2;
//             uint8_t newY = input[idx];

//             if (denom > 0) {
//                 const float norm =
//                     static_cast<float>(cdf[input[idx]] - minCdf) /
//                     static_cast<float>(denom);

//                 int mapped = static_cast<int>(norm * 255.0f);
//                 mapped = std::max(0, std::min(255, mapped));
//                 newY = static_cast<uint8_t>(mapped);
//             }

//             processedBuffer_[idx] = newY;
//             processedBuffer_[idx + 1] = 128;
//         }
//     }
// }

// //=========================================================================================================================

// inline void AlgorithmConcrete::processHeterogeneousGaussianBlurZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
//     double gpuSplit = runtimeControls_
//         ? runtimeControls_->gpu_workload_split.load(std::memory_order_relaxed)
//         : 1.0;
//     AlgorithmConcreteKernels::launchHeterogeneousGaussianBlurKernel(cudaRes_, frame, processedBuffer_, algoConfig_.blurRadius, gpuSplit);
// }

// inline void AlgorithmConcrete::processOpticalFlow(const std::shared_ptr<ZeroCopyFrameData>& frame) {
//     if (!frame || !frame->dataPtr) return;
//     if (previousFrame_) {
//         std::shared_ptr<ZeroCopyFrameData> flowOutput;
//         opticalFlowProcessor_->computeOpticalFlow(previousFrame_, frame, flowOutput);
//         if (outputQueueZeroCopy_ && flowOutput) {
//             outputQueueZeroCopy_->push(flowOutput);
//         }
//     }
//     previousFrame_ = frame;
// }

// // CUDA Helpers
// inline void AlgorithmConcrete::checkCudaError(cudaError_t err, const std::string& context) {
//     if (err != cudaSuccess) {
//         reportError("[CUDA ERROR] " + context + ": " + cudaGetErrorString(err));
//     }
// }

// inline double AlgorithmConcrete::timeCudaSectionMs(const std::function<void()>& launch) {
// #ifdef __CUDACC__
//     cudaEvent_t start, stop;
//     cudaEventCreate(&start);
//     cudaEventCreate(&stop);
//     cudaEventRecord(start, 0);
//     launch();
//     cudaEventRecord(stop, 0);
//     cudaEventSynchronize(stop);
//     float ms = 0;
//     cudaEventElapsedTime(&ms, start, stop);
//     cudaEventDestroy(start);
//     cudaEventDestroy(stop);
//     return ms;
// #else
//     auto t0 = std::chrono::high_resolution_clock::now();
//     launch();
//     return std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
// #endif
// }





// // //======================================================================================================================================
// // //================================================================================
// // // AlgorithmConcrete_new.h
// // // FINAL PRODUCTION VERSION ? 100% JETSON NANO 2GB COMPATIBLE + ENHANCED
// // // Fixed: C++11/14 Compliance, Initialization Order, Namespace Scope
// // //================================================================================

// // #pragma once


// // //#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG
// // #define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO

// // #include <cstddef>
// // #include <cstring>
// // #include <limits>
// // #include <vector>
// // #include <mutex>
// // #include <atomic>
// // #include <thread>
// // #include <functional>
// // #include <chrono>
// // #include <cmath>
// // #include <algorithm>
// // #include <memory>

// // #include <array>

// // #include <spdlog/spdlog.h>
// // #include <cuda_runtime.h>
// // #include "CudaUtiles.h"


// // #include "../Interfaces/IAlgorithm.h"
// // #include "../SharedStructures/ZeroCopyFrameData.h"
// // #include "../SharedStructures/SharedQueue.h"
// // #include "../SharedStructures/ThreadManager.h"
// // #include "../SharedStructures/AlgorithmConfig.h"
// // #include "../SharedStructures/LucasKanadeOpticalFlow.h"
// // #include "../SharedStructures/allModulesStatcs.h"
// // #include "../Interfaces/ISystemMetricsAggregator.h"
// // #include "../Others/utils.h"
// // #include "AlgorithmConcreteKernels.cuh"

// // // [FIX] Include Scheduler to access hrl::chooseCpus and hrl::setThreadAffinity
// // #include "../../Module/Scheduler.h" 
// // #include "../../Module/RuntimeControls.h"

// // // within util.h
// // // // [FIX] Local clamp for C++14/11 compatibility on Jetson Nano
// // // namespace {
// // //     template <typename T>
// // //     constexpr const T& local_clamp(const T& v, const T& lo, const T& hi) {
// // //         return (v < lo) ? lo : (hi < v) ? hi : v;
// // //     }
// // // }

// // using namespace hrl;

// // class AlgorithmConcrete : public IAlgorithm {
// // public:
// //     explicit AlgorithmConcrete(ThreadManager& threadManager);
// //     //==========================================================================
// //     // Constructor / Destructor
// //     //==========================================================================
// //     AlgorithmConcrete(std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> inputQueue,
// //                       std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> outputQueue,
// //                       ThreadManager& threadManager,
// //                       std::shared_ptr<ISystemMetricsAggregator> aggregator,
// //                       std::shared_ptr<hrl::RuntimeControls> controls = nullptr);

// //     ~AlgorithmConcrete() override;
// //     //==========================================================================
// //     // Interface Implementation
// //     //==========================================================================
// //     static std::shared_ptr<IAlgorithm> createAlgorithmZeroCopy(
// //         AlgorithmType type,
// //         std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> inputQueue,
// //         std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> outputQueue,
// //         ThreadManager& threadManager,
// //         std::shared_ptr<ISystemMetricsAggregator> aggregator);

// //     void startAlgorithm() override;
// //     void stopAlgorithm() override;
// //     bool isRunning() const { return running_; }
    
// //     bool processFrameZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& inputFrame,
// //                               std::shared_ptr<ZeroCopyFrameData>& outputFrame) override;
    
// //     bool configure(const AlgorithmConfig& config) override;
    
// //     void setErrorCallback(std::function<void(const std::string&)> cb) override;
    
// //     std::tuple<double, double> getAlgorithmMetrics() const override { return {getLastFPS(), lastProcessingTime_}; }
// //     double getLastFPS() const override { return lastFPS_; }
// //     double getFps() const override { return lastFPS_; }
// //     double getAverageProcTime() const override { return avgProcTime_; }
// //     const uint8_t* getProcessedBuffer() const override { return processedBuffer_.data(); }
// //     void setAlgorithmType(AlgorithmType newType) override;

// // private:
// //     void threadLoopZeroCopy();
// //     void updateMetrics(double elapsedSec, uint64_t frameSeq);
// //     std::string algorithmTypeToString(AlgorithmType type) const;
// //     void reportError(const std::string& msg);

// //     // [FIX] Declaration signature matched to definition
// //     void parallelFor(size_t start, size_t end, const std::function<void(size_t)>& func);

// //     // CPU algorithms
// //     void processInvertZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
// //     void processGrayscaleZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
// //     void processEdgeDetectionZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
// //     void processGaussianBlurZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);

// //     // CPU multithreaded algorithms (fallbacks for GPU)
// //     void processMultiThreadedInvert(const std::shared_ptr<ZeroCopyFrameData>& frame) ; // [FIX] Added missing declaration for multi-threaded invert

    

// //     // CUDA wrappers
// //     void processSobelEdgeZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
// //     void processMedianFilterZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);

// //     // 
// //     void processMandelbrot(const std::shared_ptr<ZeroCopyFrameData>& frame); // [FIX] Added missing declaration for Mandelbrot (CPU)
    
// //     void processHistogramEqualizationZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
// //     void processHeterogeneousGaussianBlurZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
// //     void processHistogramEqualizationCPUZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame);
// //     void processOpticalFlow(const std::shared_ptr<ZeroCopyFrameData>& frame);

// //     // CUDA helpers
// //     void checkCudaError(cudaError_t err, const std::string& context);
// //     double timeCudaSectionMs(const std::function<void()>& launch);

// //     // --- State Variables (Order matches initialization for safety) ---
// //     std::atomic<bool> running_{false};
    
// //     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> inputQueueZeroCopy_;
// //     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> outputQueueZeroCopy_;
    
// //     AlgorithmConfig algoConfig_;
// //     std::function<void(const std::string&)> errorCallback_;

// //     // Metrics
// //     mutable std::mutex metricMutex_;
// //     double fps_ = 0.0;
// //     double avgProcTime_ = 0.0;
// //     std::vector<uint8_t> processedBuffer_;
    
// //     mutable double lastFPS_ = 0.0;
// //     mutable double lastProcessingTime_ = 0.0;

// //     uint64_t windowFrames_ = 0;
// //     double windowProcMs_ = 0.0;
// //     std::chrono::steady_clock::time_point windowStart_;

    
// //     // Sliding 1-second window for processFrameZeroCopy FPS (feeds metricAggregator)
// //     std::chrono::steady_clock::time_point fpsWindowStart_;
// //     uint64_t fpsWindowFrames_ = 0;
// //     double   fpsWindowProcMs_ = 0.0;

// //     uint64_t totalFrames_ = 0;
// //     double totalProcMs_ = 0.0;

// //     uint64_t lastFrameSeq_ = 0;
// //     bool haveLastSeq_ = false;

// //     // Persistent GPU resources ? allocated once at startAlgorithm(), freed at stopAlgorithm().
// //     // Eliminates per-frame cudaMalloc/cudaFree and enables async CUDA stream overlap.
// //     AlgorithmConcreteKernels::CudaResources cudaRes_;

// //     // Dependencies
// //     ThreadManager& threadManager_;
// //     std::unique_ptr<LucasKanadeOpticalFlow> opticalFlowProcessor_;
// //     std::shared_ptr<ZeroCopyFrameData> previousFrame_;
// //     std::shared_ptr<ISystemMetricsAggregator> metricAggregator_;
// //     std::shared_ptr<hrl::RuntimeControls> runtimeControls_;
// // };

// // //================================================================================
// // // Inline Implementation
// // //================================================================================

// // inline AlgorithmConcrete::AlgorithmConcrete(ThreadManager& threadManager)
// //     : running_(false),
// //       inputQueueZeroCopy_(nullptr),
// //       outputQueueZeroCopy_(nullptr),
// //       // [FIX] Removed lastUpdateTime_ (not in class), initialized windowStart_
// //       windowStart_(std::chrono::steady_clock::now()),
// //     fpsWindowStart_(std::chrono::steady_clock::now()),
// //       threadManager_(threadManager) {
// //     spdlog::warn("[AlgorithmConcrete] Default constructor used ? no queues!");
// // }

// // // [FIX] Constructor Initialization List Reordered to match declaration order
// // inline AlgorithmConcrete::AlgorithmConcrete(
// //     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> inputQueue,
// //     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> outputQueue,
// //     ThreadManager& threadManager,
// //     std::shared_ptr<ISystemMetricsAggregator> aggregator,
// //     std::shared_ptr<hrl::RuntimeControls> controls)
// //     : running_(false),
// //       inputQueueZeroCopy_(std::move(inputQueue)),
// //       outputQueueZeroCopy_(std::move(outputQueue)),
// //       windowStart_(std::chrono::steady_clock::now()),
// //     fpsWindowStart_(std::chrono::steady_clock::now()),
// //       threadManager_(threadManager),
// //       metricAggregator_(aggregator),
// //       runtimeControls_(controls) {
    
// //     if (!inputQueueZeroCopy_ || !outputQueueZeroCopy_) {
// //         throw std::runtime_error("AlgorithmConcrete: input/output queues cannot be null");
// //     }
// //     spdlog::info("[AlgorithmConcrete] Constructed with ZeroCopy queues");
// // }

// // inline AlgorithmConcrete::~AlgorithmConcrete() {
// //     stopAlgorithm();
// // }

// // inline std::shared_ptr<IAlgorithm> AlgorithmConcrete::createAlgorithmZeroCopy(
// //     AlgorithmType type,
// //     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> inputQueue,
// //     std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> outputQueue,
// //     ThreadManager& threadManager,
// //     std::shared_ptr<ISystemMetricsAggregator> aggregator) {
    
// //     auto algo = std::make_shared<AlgorithmConcrete>(std::move(inputQueue), std::move(outputQueue), threadManager, aggregator, nullptr);
    
// //     // [FIX] C++11 Compatible Struct Initialization (No designated initializers)
// //     AlgorithmConfig cfg;
// //     cfg.algorithmType = type;
// //     algo->configure(cfg);
    
// //     return algo;
// // }

// // inline void AlgorithmConcrete::startAlgorithm() {
// //     if (running_) {
// //         spdlog::warn("[AlgorithmConcrete] Already running");
// //         return;
// //     }
// //     running_ = true;
// //     windowStart_ = std::chrono::steady_clock::now();
// //     windowFrames_ = 0;
// //     windowProcMs_ = 0.0;
    
// //     fpsWindowStart_  = std::chrono::steady_clock::now();
// //     fpsWindowFrames_ = 0;
// //     fpsWindowProcMs_ = 0.0;

// //     // GPU resource pre-allocation is deferred to the first GPU frame dispatch,
// //     // since frame dimensions (width x height) are not known until then.
// //     // See: initCudaResources() call inside each launcher.

// //     threadManager_.addThread("AlgorithmProcessingZeroCopy",
// //         std::thread(&AlgorithmConcrete::threadLoopZeroCopy, this));

// //     spdlog::info("[AlgorithmConcrete] Started ? {}", algorithmTypeToString(algoConfig_.algorithmType));
// // }

// // inline void AlgorithmConcrete::stopAlgorithm() {
// //     if (!running_) return;

// //     running_ = false;

// //     if (inputQueueZeroCopy_)  inputQueueZeroCopy_->stop();
// //     if (outputQueueZeroCopy_) outputQueueZeroCopy_->stop();

// //     threadManager_.joinThreadsFor("AlgorithmProcessingZeroCopy");

// //     // Release persistent GPU resources after the processing thread has exited
// //     AlgorithmConcreteKernels::destroyCudaResources(cudaRes_);

// //     spdlog::info("[AlgorithmConcrete] Stopped cleanly");
// // }

// // inline bool AlgorithmConcrete::configure(const AlgorithmConfig& config) {
// //     algoConfig_ = config;

// //     if (algoConfig_.algorithmType == AlgorithmType::OpticalFlow_LucasKanade) {
// //         opticalFlowProcessor_ = std::make_unique<LucasKanadeOpticalFlow>(algoConfig_.opticalFlowConfig);
// //         previousFrame_.reset();
// //     } else {
// //         opticalFlowProcessor_.reset();
// //     }

// //     spdlog::info("[AlgorithmConcrete] Configured ? {}", algorithmTypeToString(config.algorithmType));
// //     return true;
// // }

// // inline void AlgorithmConcrete::setErrorCallback(std::function<void(const std::string&)> cb) {
// //     errorCallback_ = std::move(cb);
// // }

// // inline void AlgorithmConcrete::setAlgorithmType(AlgorithmType newType) {
// //     std::lock_guard<std::mutex> lock(metricMutex_);
// //     if (algoConfig_.algorithmType == newType) return;

// //     algoConfig_.algorithmType = newType;
// //     if (newType == AlgorithmType::OpticalFlow_LucasKanade) {
// //         opticalFlowProcessor_ = std::make_unique<LucasKanadeOpticalFlow>(algoConfig_.opticalFlowConfig);
// //         previousFrame_.reset();
// //     } else {
// //         opticalFlowProcessor_.reset();
// //     }
// // }

// // //================================================================================
// // // MAIN THREAD LOOP ? ADAPTIVE & ROBUST
// // //================================================================================
// // //==============================================================================================================
// // // AlgorithmConcrete::threadLoopZeroCopy() ? OPTIMIZED WITH TIMER DIAGNOSTICS

// // /*Key change: Removed the line outputQueueZeroCopy_->push(outputFrame); inside the if (proc_ok && outputFrame) block. 
// // The frame is already pushed once by processFrameZeroCopy() (or by processOpticalFlow() for that algorithm type).
// // */
// // //==============================================================================================================

// // inline void AlgorithmConcrete::threadLoopZeroCopy() {
// //     spdlog::info("[AlgorithmConcrete] Thread started (starvation timeout: 5s)");

// //     // ========== TIMER INITIALIZATION ==========
// //     auto starvationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
// //     bool firstFrame = true;
    
// //     // Metric window tracking
// //     auto windowStart = std::chrono::steady_clock::now();
// //     int windowFrames = 0;
// //     double windowProcMs = 0.0;
    
// //     // Adaptation counters
// //     uint64_t adaptationCounter = 0;
// //     int lastConcurrency = -1;
// //     hrl::Affinity lastAffinity = hrl::Affinity::Spread;

// //     while (running_) {
// //         // ========== 1. POP FROM QUEUE WITH TIMEOUT TRACKING ==========
// //         std::shared_ptr<ZeroCopyFrameData> inputFrame;
        
// //         auto pop_start = std::chrono::high_resolution_clock::now();
// //         const int pop_timeout_ms = 100;  // ? Explicit timeout constant
        
// //         //if (!inputQueueZeroCopy_->pop(inputFrame, std::chrono::milliseconds(pop_timeout_ms))) {
// //         if (!inputQueueZeroCopy_->pop(inputFrame)) {
// //             auto pop_elapsed_ms = std::chrono::duration<double, std::milli>(
// //                 std::chrono::high_resolution_clock::now() - pop_start).count();
            
// //             // ? LOG DIAGNOSTIC INFO
// //             spdlog::warn("[AlgorithmConcrete] Queue pop TIMEOUT (waited {}ms, queue_size={})", 
// //                         pop_elapsed_ms, inputQueueZeroCopy_->size());
            
// //             // ? STARVATION CHECK: Has producer died?
// //             if (std::chrono::steady_clock::now() > starvationDeadline) {
// //                 if (firstFrame) {
// //                     reportError("ALGORITHM STARVED ? NO FRAME FOR 5 SECONDS (startup)");
// //                 } else {
// //                     reportError("ALGORITHM STARVED ? NO FRAME FOR 5 SECONDS (mid-run)");
// //                 }
// //                 running_ = false;
// //                 break;
// //             }
// //             continue;
// //         }
        
// //         // ? FRAME RECEIVED: Reset starvation timer for next batch
// //         starvationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        
// //         auto pop_elapsed_ms = std::chrono::duration<double, std::milli>(
// //             std::chrono::high_resolution_clock::now() - pop_start).count();
        
// //         // ========== 2. VALIDATE FRAME ==========
// //         if (!inputFrame || !inputFrame->isValid() || !inputFrame->dataPtr) {
// //             spdlog::warn("[AlgorithmConcrete] Invalid frame received (pop took {}ms)", pop_elapsed_ms);
// //             continue;
// //         }
        
// //         spdlog::debug("[AlgorithmConcrete] Frame {} received (pop took {}ms, queue_size={})", 
// //                      inputFrame->frameNumber, pop_elapsed_ms, inputQueueZeroCopy_->size());

// //         // ========== 3. DYNAMIC ADAPTATION (Every ~10 frames) ==========
// //         if (runtimeControls_ && (++adaptationCounter % 10) == 0) {
// //             const int newConcurrency = runtimeControls_->concurrency_level.load(std::memory_order_relaxed);
// //             const bool gpuEnabled    = runtimeControls_->enable_gpu.load(std::memory_order_relaxed);
// //             const auto newAffinity   = runtimeControls_->affinity.load(std::memory_order_relaxed);

// //             algoConfig_.concurrencyLevel = utils::local_clamp(newConcurrency, 1, 4);
// //             algoConfig_.useGPU = gpuEnabled;

// //             if (newConcurrency != lastConcurrency || newAffinity != lastAffinity) {
// //                 const auto cpus = hrl::chooseCpus(algoConfig_.concurrencyLevel, newAffinity);
// //                 if (!cpus.empty()) {
// //                     hrl::setThreadAffinity(cpus);
// //                     spdlog::debug("[AlgorithmConcrete] Affinity updated: {} cores, {} mode",
// //                                   algoConfig_.concurrencyLevel,
// //                                   (newAffinity == hrl::Affinity::Spread ? "Spread" : "Pack"));
// //                 }
// //                 lastConcurrency = newConcurrency;
// //                 lastAffinity = newAffinity;
// //             }
// //         }

// //         // ========== 4. PROCESS FRAME WITH TIMING ==========
// //         auto proc_start = std::chrono::high_resolution_clock::now();
        
// //         std::shared_ptr<ZeroCopyFrameData> outputFrame;
// //         bool proc_ok = processFrameZeroCopy(inputFrame, outputFrame);
        
// //         auto proc_elapsed_ms = std::chrono::duration<double, std::milli>(
// //             std::chrono::high_resolution_clock::now() - proc_start).count();
        
// //         windowProcMs += proc_elapsed_ms;  // ? Accumulate for averaging

// //         // IMPORTANT: outputFrame is already pushed inside processFrameZeroCopy()
// //         // Do NOT push again here ? that would double the queue entries and halve FPS.

// //         if (proc_ok && outputFrame) {
// //             //outputQueueZeroCopy_->push(outputFrame); // IMPORTANT: outputFrame is already pushed inside processFrameZeroCopy()
// //             windowFrames++;  // ? Count successful frames
            
// //             spdlog::debug("[AlgorithmConcrete] Frame {} processed ({}ms, refcount={})", 
// //                          inputFrame->frameNumber, proc_elapsed_ms, outputFrame.use_count());

// //             if (firstFrame) {
// //                 firstFrame = false;
// //                 spdlog::info("[AlgorithmConcrete] FIRST FRAME PROCESSED | Concurrency={} | GPU={}",
// //                              algoConfig_.concurrencyLevel, algoConfig_.useGPU ? "ON" : "OFF");
// //             }
// //         } else {
// //             spdlog::warn("[AlgorithmConcrete] Frame {} processing FAILED ({}ms)", 
// //                         inputFrame->frameNumber, proc_elapsed_ms);
// //         }

// //         // ========== 5. UPDATE METRICS EVERY 1 SECOND ==========
// //         auto now = std::chrono::steady_clock::now();
// //         double elapsed = std::chrono::duration<double>(now - windowStart).count();
        
// //         if (elapsed >= 1.0) {
// //             // ? Calculate per-second stats
// //             double fps = windowFrames / elapsed;
// //             double avg_proc_ms = windowFrames > 0 ? windowProcMs / windowFrames : 0.0;
            
// //             spdlog::info("[AlgorithmConcrete] METRICS [1s window] | Frames: {} | FPS: {:.1f} | Avg Proc: {:.2f}ms",
// //                         windowFrames, fps, avg_proc_ms);
            
// //             // ? Call updateMetrics with current frame info
// //             if (windowFrames > 0) {
// //                 updateMetrics(elapsed, inputFrame->frameNumber);
// //             }
            
// //             // ? RESET for next window
// //             windowStart = now;
// //             windowFrames = 0;
// //             windowProcMs = 0.0;
// //         }
// //     }

// //     spdlog::info("[AlgorithmConcrete] Thread exited cleanly");
// // }


// // //================================================================================
// // // processFrameZeroCopy ? FINAL HRL ADAPTIVE VERSION (GPU Switching + Fallbacks)
// // //================================================================================
// // inline bool AlgorithmConcrete::processFrameZeroCopy(
// //     const std::shared_ptr<ZeroCopyFrameData>& inputFrame,
// //     std::shared_ptr<ZeroCopyFrameData>& outputFrame)
// // {
// //     if (!inputFrame || !inputFrame->isValid() || !inputFrame->dataPtr || inputFrame->size == 0) {
// //         spdlog::error("[AlgorithmConcrete] Invalid input frame");
// //         return false;
// //     }

// //     const uint64_t frameId = inputFrame->frameNumber;
// //     const auto t_start_steady = std::chrono::steady_clock::now();
// //     const auto t_start_sys = std::chrono::system_clock::now();

// //     processedBuffer_.resize(inputFrame->size);
// //     double cudaKernelTimeMs = 0.0;

// //     // === SINGLE SOURCE OF TRUTH: GPU ENABLED? ===
// //     // Config default ? overridden by Scheduler (most up-to-date)
// //     bool useGPU = algoConfig_.useGPU;

// //     switch (algoConfig_.algorithmType) {
// //         // ??????????????????? CPU-Only Algorithms (Never use GPU) ???????????????????
// //         case AlgorithmType::Invert:
// //         case AlgorithmType::Grayscale:
// //         case AlgorithmType::EdgeDetection:
// //         case AlgorithmType::GaussianBlur:
// //             useGPU = false;
// //             break;

// //         // ??????????????????? Hybrid Algorithms (Respect GPU Toggle) ???????????????????
// //         default:
// //             break; // use current algoConfig_.useGPU
// //     }

// //     // ??????????????????? Dispatch with fallback ???????????????????
// //     switch (algoConfig_.algorithmType) {
// //         case AlgorithmType::Invert:
// //             processInvertZeroCopy(inputFrame);
// //             break;
// //         case AlgorithmType::Grayscale:
// //             processGrayscaleZeroCopy(inputFrame);
// //             break;
// //         case AlgorithmType::EdgeDetection:
// //             processEdgeDetectionZeroCopy(inputFrame);
// //             break;
// //         case AlgorithmType::GaussianBlur:
// //             processGaussianBlurZeroCopy(inputFrame);
// //             break;

// //         case AlgorithmType::SobelEdge:
// //             if (useGPU) {
// //                 try {
// //                     cudaKernelTimeMs = timeCudaSectionMs([&]{
// //                         AlgorithmConcreteKernels::launchSobelEdgeKernel(cudaRes_, inputFrame, processedBuffer_);
// //                     });
// //                 } catch (const std::runtime_error& e) {
// //                     spdlog::warn("[AlgorithmConcrete] GPU Sobel failed ({}); falling back to CPU edge detection", e.what());
// //                     algoConfig_.useGPU = false;
// //                     processEdgeDetectionZeroCopy(inputFrame);
// //                 }
// //             } else {
// //                 processEdgeDetectionZeroCopy(inputFrame);  // Best-effort CPU fallback
// //             }
// //             break;

// //         case AlgorithmType::MedianFilter:
// //             if (useGPU) {
// //                 try {
// //                     cudaKernelTimeMs = timeCudaSectionMs([&]{
// //                         AlgorithmConcreteKernels::launchMedianFilterKernel(cudaRes_, inputFrame, processedBuffer_, algoConfig_.medianWindowSize);
// //                     });
// //                 } catch (const std::runtime_error& e) {
// //                     spdlog::warn("[AlgorithmConcrete] GPU Median failed ({}); falling back to pass-through", e.what());
// //                     algoConfig_.useGPU = false;
// //                     std::memcpy(processedBuffer_.data(), inputFrame->dataPtr, inputFrame->size);
// //                 }
// //             } else {
// //                 // No CPU median - pass-through to save energy
// //                 std::memcpy(processedBuffer_.data(), inputFrame->dataPtr, inputFrame->size);
// //             }
// //             break;
// //         case AlgorithmType::HistogramEqualization:
// //             if (useGPU) {
// //                 try {
// //                     cudaKernelTimeMs = timeCudaSectionMs([&] {
// //                         AlgorithmConcreteKernels::launchHistogramEqualizationKernel(
// //                             cudaRes_, inputFrame, processedBuffer_);
// //                     });
// //                 } catch (const std::runtime_error& e) {
// //                     spdlog::warn(
// //                         "[AlgorithmConcrete] GPU HistEq failed ({}); falling back to CPU histogram equalization",
// //                         e.what());
// //                     processHistogramEqualizationCPUZeroCopy(inputFrame);
// //                     cudaKernelTimeMs = 0.0;
// //                 }
// //             } else {
// //                 processHistogramEqualizationCPUZeroCopy(inputFrame);
// //                 cudaKernelTimeMs = 0.0;
// //             }
// //             break;

// //         case AlgorithmType::HeterogeneousGaussianBlur:
// //             if (useGPU) {
// //                 try {
// //                     cudaKernelTimeMs = timeCudaSectionMs([&]{
// //                         AlgorithmConcreteKernels::launchHeterogeneousGaussianBlurKernel(cudaRes_, inputFrame, processedBuffer_, algoConfig_.blurRadius);
// //                     });
// //                 } catch (const std::runtime_error& e) {
// //                     spdlog::warn("[AlgorithmConcrete] GPU Gaussian failed ({}); falling back to CPU blur", e.what());
// //                     algoConfig_.useGPU = false;
// //                     processGaussianBlurZeroCopy(inputFrame);
// //                 }
// //             } else {
// //                 processGaussianBlurZeroCopy(inputFrame);
// //             }
// //             break;

// //         case AlgorithmType::OpticalFlow_LucasKanade:
// //             processOpticalFlow(inputFrame);
// //             goto metrics_only;

// //         default:
// //             std::memcpy(processedBuffer_.data(), inputFrame->dataPtr, inputFrame->size);
// //             break;
// //     }

// //     // ??????????????????? Output Frame Construction ???????????????????
// // 	// === CRITICAL FIX: ALWAYS CREATE OUTPUT FRAME AND PUSH TO QUEUE ===

// //     {
// //         //auto data = std::make_shared<std::vector<uint8_t>>(std::move(processedBuffer_)); // copy processed result
// //         // Copy instead of move ? retains capacity in processedBuffer_
// //         auto data = std::make_shared<std::vector<uint8_t>>(processedBuffer_);
// //         processedBuffer_.clear();   // optional, keeps capacity
// //         outputFrame = std::make_shared<ZeroCopyFrameData>(
// //             data, data->data(), 
// // 		data->size(),
// //             inputFrame->width, inputFrame->height, -1,
// //             inputFrame->frameNumber,
// //             inputFrame->captureSys, inputFrame->captureSteady
// //         );

// // //===============================================
// // 	// PUSH TO DISPLAY QUEUE (this makes right side update in real-time)
// //         if (outputQueueZeroCopy_) {
// //             outputQueueZeroCopy_->push(outputFrame);
// //             spdlog::debug("[AlgorithmConcrete] Pushed processed frame {} to outputQueueZeroCopy_ (refcount={})", 
// //                          frameId, outputFrame.use_count());
// //         } else {
// //             spdlog::error("[AlgorithmConcrete] outputQueueZeroCopy_ is null - cannot show processed frame");
// //         }
// //     }

// // metrics_only:
// //     const auto t_end_steady = std::chrono::steady_clock::now();
// //     const auto t_end_sys = std::chrono::system_clock::now();
// //     lastProcessingTime_ = std::chrono::duration<double, std::milli>(t_end_steady - t_start_steady).count();

// //     //windowFrames_++;
// //     //windowProcMs_ += lastProcessingTime_;

// //      // -- Sliding 1-second FPS window (feeds metricAggregator / CSV) ----------
// //     fpsWindowFrames_++;
// //     fpsWindowProcMs_ += lastProcessingTime_;
// //     totalFrames_++;
// //     totalProcMs_ += lastProcessingTime_;

// //     double currentMeasuredFps = 30.0;
// //     double windowElapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - windowStart_).count();
// //     if (windowElapsed > 0.001) currentMeasuredFps = windowFrames_ / windowElapsed;

// //     uint32_t dropped = 0;
// //     if (haveLastSeq_ && frameId > lastFrameSeq_ + 1) {
// //         dropped = static_cast<uint32_t>(std::min<uint64_t>(frameId - lastFrameSeq_ - 1, UINT32_MAX));
// //     }
// //     lastFrameSeq_ = frameId;
// //     haveLastSeq_ = true;

// // #ifdef __CUDACC__
// //     size_t gpuFree = 0, gpuTotal = 0;
// //     if (useGPU && cudaMemGetInfo(&gpuFree, &gpuTotal) == cudaSuccess && gpuFree < 80ULL * 1024 * 1024) {
// //         spdlog::warn("[AlgorithmConcrete] GPU Low Memory: {} MiB free", gpuFree / (1024*1024));
// //     }
// // #endif

// //     if (metricAggregator_) {
// //         AlgorithmStats as(t_start_sys, t_end_sys);
// //         as.inferenceTimeMs   = lastProcessingTime_;
// //         as.fps               = currentMeasuredFps;
// //         as.avgProcTimeMs     = windowFrames_ > 0 ? windowProcMs_ / windowFrames_ : 0.0;
// //         as.totalProcTimeMs   = totalProcMs_;
// //         as.cudaKernelTimeMs  = cudaKernelTimeMs;
// //         as.droppedFrames     = dropped;
// // #ifdef __CUDACC__
// //         if (useGPU) {
// //             as.gpuFreeMemory  = gpuFree;
// //             as.gpuTotalMemory = gpuTotal;
// //         }
// // #endif
// //         metricAggregator_->mergeAlgorithm(frameId, as);
// //     }

// //     return algoConfig_.algorithmType != AlgorithmType::OpticalFlow_LucasKanade;
// // }

// // inline void AlgorithmConcrete::updateMetrics(double elapsedSec, uint64_t frameSeq) {
// //     std::lock_guard<std::mutex> lock(metricMutex_);
// //     if (elapsedSec <= 0.0) elapsedSec = 1.0;

// //     const double fps = windowFrames_ / elapsedSec;
// //     const double avgMs = windowFrames_ > 0 ? (windowProcMs_ / windowFrames_) : 0.0;

// //     fps_ = fps;
// //     lastFPS_ = fps;
// //     avgProcTime_ = avgMs;

// //     spdlog::info("[AlgorithmConcrete] Frame {} | FPS: {:.1f} | Avg: {:.2f}ms | {}", 
// //                  frameSeq, fps, avgMs, algorithmTypeToString(algoConfig_.algorithmType));
// // }

// // inline std::string AlgorithmConcrete::algorithmTypeToString(AlgorithmType type) const {
// //     switch (type) {
// //         case AlgorithmType::Invert: return "Invert";
// //         case AlgorithmType::Grayscale: return "Grayscale";
// //         case AlgorithmType::EdgeDetection: return "EdgeDetection";
// //         case AlgorithmType::GaussianBlur: return "GaussianBlur";
// //         case AlgorithmType::SobelEdge: return "SobelEdge";
// //         case AlgorithmType::MedianFilter: return "MedianFilter";
// //         case AlgorithmType::HistogramEqualization: return "HistogramEqualization";
// //         case AlgorithmType::HeterogeneousGaussianBlur: return "HeterogeneousGaussianBlur";
// //         case AlgorithmType::OpticalFlow_LucasKanade: return "OpticalFlow_LK";
// //         case AlgorithmType::Mandelbrot: return "Mandelbrot";
// //         case AlgorithmType::MultiThreadedInvert: return "MultiThreadedInvert";
// //         default: return "Unknown";
// //     }
// // }

// // inline void AlgorithmConcrete::reportError(const std::string& msg) {
// //     if (errorCallback_) errorCallback_(msg);
// //     else spdlog::error("[AlgorithmConcrete] {}", msg);
// // }

// // //================================================================================
// // // parallelFor ? HRL ADAPTIVE VERSION
// // //================================================================================
// // inline void AlgorithmConcrete::parallelFor(size_t start, size_t end, const std::function<void(size_t)>& func) {
// //     // 1. Determine thread count: Priority to RuntimeControls (HRL), fallback to Config
// //     size_t numThreads = algoConfig_.concurrencyLevel;
    
// //     if (runtimeControls_) {
// //         int dynLevel = runtimeControls_->concurrency_level.load(std::memory_order_relaxed);
// //         if (dynLevel > 0) {
// //             numThreads = static_cast<size_t>(dynLevel);
// //         }
// //     }

// //     // 2. Serial fallback for small workloads or single-thread mode
// //     if (numThreads <= 1 || (end - start) < 1000) {
// //         for (size_t i = start; i < end; ++i) func(i);
// //         return;
// //     }

// //     // 3. Parallel Execution
// //     std::vector<std::thread> workers;
// //     workers.reserve(numThreads);
    
// //     const size_t chunkSize = (end - start + numThreads - 1) / numThreads;

// //     for (size_t t = 0; t < numThreads; ++t) {
// //         size_t s = start + t * chunkSize;
// //         if (s >= end) break;
// //         size_t e = std::min(end, s + chunkSize);
        
// //         workers.emplace_back([=] { 
// //             for (size_t j = s; j < e; j++) func(j); 
// //         });
// //     }
    
// //     for (auto& w : workers) w.join();
// // }

// // // CPU Operations
// // inline void AlgorithmConcrete::processInvertZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
// //     const uint8_t* in = static_cast<const uint8_t*>(frame->dataPtr);
// //     parallelFor(0, frame->size, [this, in](size_t i) { processedBuffer_[i] = ~in[i]; });
// // }

// // inline void AlgorithmConcrete::processGrayscaleZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
// //     const uint8_t* in = static_cast<const uint8_t*>(frame->dataPtr);
// //     parallelFor(0, frame->size, [this, in](size_t i) { processedBuffer_[i] = (i % 2 == 0) ? in[i] : 128; });
// // }

// // inline void AlgorithmConcrete::processEdgeDetectionZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
// //     const uint8_t* in = static_cast<const uint8_t*>(frame->dataPtr);
// //     const int w = frame->width, h = frame->height;
// //     parallelFor(0, h, [this, in, w](size_t y) {
// //         for (int x = 0; x < w; ++x) {
// //             int idx = y * w * 2 + x * 2;
// //             if (x > 0 && x < w - 1) {
// //                 int grad = std::abs(static_cast<int>(in[idx]) - static_cast<int>(in[idx - 2]));
// //                 processedBuffer_[idx] = static_cast<uint8_t>(grad);
// //                 processedBuffer_[idx + 1] = 128;
// //             } else {
// //                 processedBuffer_[idx] = in[idx];
// //                 processedBuffer_[idx + 1] = in[idx + 1];
// //             }
// //         }
// //     });
// // }

// // inline void AlgorithmConcrete::processGaussianBlurZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
// //     const int r = algoConfig_.blurRadius;
// //     const int w = frame->width, h = frame->height;
// //     const uint8_t* in = static_cast<const uint8_t*>(frame->dataPtr);
// //     std::vector<uint8_t> temp(frame->size);

// //     parallelFor(0, h, [in, w, r, &temp](size_t y) {
// //         for (int x = 0; x < w; ++x) {
// //             float sum = 0, wsum = 0;
// //             for (int d = -r; d <= r; ++d) {
// //                 int xx = utils::local_clamp(x + d, 0, w - 1);
// //                 float wt = std::exp(-(d * d) / (2.0f * r * r));
// //                 sum += in[y * w * 2 + xx * 2] * wt;
// //                 wsum += wt;
// //             }
// //             temp[y * w * 2 + x * 2] = static_cast<uint8_t>(sum / wsum);
// //             temp[y * w * 2 + x * 2 + 1] = 128;
// //         }
// //     });

// //     parallelFor(0, w, [&temp, w, h, r, this](size_t x) {
// //         for (int y = 0; y < h; ++y) {
// //             float sum = 0, wsum = 0;
// //             for (int d = -r; d <= r; ++d) {
// //                 int yy = utils::local_clamp(static_cast<int>(y) + d, 0, h - 1);
// //                 float wt = std::exp(-(d * d) / (2.0f * r * r));
// //                 sum += temp[yy * w * 2 + x * 2] * wt;
// //                 wsum += wt;
// //             }
// //             processedBuffer_[y * w * 2 + x * 2] = static_cast<uint8_t>(sum / wsum);
// //             processedBuffer_[y * w * 2 + x * 2 + 1] = 128;
// //         }
// //     });
// // }

// // // Note: The above CPU implementations are intentionally simple and not optimized for performance.

// // inline void AlgorithmConcrete::processMultiThreadedInvert(const std::shared_ptr<ZeroCopyFrameData>& frame) {
// //     const int numThreads = 4; // Jetson Nano has 4 ARM cores
// //     const size_t dataSize = frame->width * frame->height;
// //     const size_t chunkSize = dataSize / numThreads;
// //     std::vector<std::thread> threads;

// //     // Spawn threads to process chunks of the image concurrently
// //     for (int i = 0; i < numThreads; ++i) {
// //         size_t startIdx = i * chunkSize;
// //         size_t endIdx = (i == numThreads - 1) ? dataSize : startIdx + chunkSize;
        
// //         threads.emplace_back([this, frame, startIdx, endIdx]() {
// //             for (size_t j = startIdx; j < endIdx; ++j) {
// //                 // Replace this:
// //                 //processedBuffer_[j] = 255 - frame->dataPtr[j];

// //                 // With this:
// //                 uint8_t* data = static_cast<uint8_t*>(frame->dataPtr);
// //                 processedBuffer_[j] = 255 - data[j];
// //             }
// //         });
// //     }

// //     // Wait for all cores to finish
// //     for (auto& t : threads) {
// //         if (t.joinable()) t.join();
// //     }
// // }

// // inline void AlgorithmConcrete::processMandelbrot(const std::shared_ptr<ZeroCopyFrameData>& frame) {
// //     if (algoConfig_.useGPU) {
// //         try {
// //             AlgorithmConcreteKernels::launchMandelbrotKernel(cudaRes_, frame, processedBuffer_);
// //             return;
// //         } catch (const std::exception& e) {
// //             reportError("[Mandelbrot GPU ERROR] " + std::string(e.what()) + ". Falling back to CPU.");
// //             // Fall through to CPU implementation
// //         }
// //     }

// //     // --- CPU Fallback implementation ---
// //     const int width = frame->width;
// //     const int height = frame->height;
// //     const int max_iter = 100;

// //     for (int y = 0; y < height; ++y) {
// //         for (int x = 0; x < width; ++x) {
// //             float cx = (x * 3.0f / (float)width) - 2.0f;
// //             float cy = (y * 3.0f / (float)height) - 1.5f;
// //             float zx = 0.0f, zy = 0.0f;
// //             int iter = 0;

// //             while (zx * zx + zy * zy <= 4.0f && iter < max_iter) {
// //                 float tmp = zx * zx - zy * zy + cx;
// //                 zy = 2.0f * zx * zy + cy;
// //                 zx = tmp;
// //                 iter++;
// //             }
            
// //             processedBuffer_[y * width + x] = (iter == max_iter) ? 0 : (uint8_t)((iter * 255) / max_iter);
// //         }
// //     }
// // }

// // // CUDA Wrappers
// // inline void AlgorithmConcrete::processSobelEdgeZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
// //     AlgorithmConcreteKernels::launchSobelEdgeKernel(cudaRes_, frame, processedBuffer_);
// // }

// // inline void AlgorithmConcrete::processMedianFilterZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
// //     AlgorithmConcreteKernels::launchMedianFilterKernel(cudaRes_, frame, processedBuffer_, algoConfig_.medianWindowSize);
// // }

// // // inline void AlgorithmConcrete::processHistogramEqualizationZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
// // //     AlgorithmConcreteKernels::launchHistogramEqualizationKernel(cudaRes_, frame, processedBuffer_);
// // // }

// // //=========================================================================================================================


// // inline void AlgorithmConcrete::processHistogramEqualizationZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
// //     // CPU implementation matching the CUDA HistEq path:
// //     //   - Build histogram over the Y/luma channel only.
// //     //   - Compute CDF and min non-zero CDF.
// //     //   - Remap Y using (cdf[Y] - minCdf) / (N - minCdf) * 255.
// //     //   - Set chroma byte to 128, matching the existing CUDA remapKernel.
// //     //
// //     // The framework stores frames in the same 2-bytes-per-pixel convention used
// //     // by the CUDA kernels: byte 0 = Y, byte 1 = chroma placeholder.
// //     if (!frame || !frame->dataPtr || frame->width <= 0 || frame->height <= 0 || frame->size == 0) {
// //         spdlog::warn("[AlgorithmConcrete] CPU HistEq skipped: invalid frame");
// //         return;
// //     }

// //     const uint8_t* in = static_cast<const uint8_t*>(frame->dataPtr);
// //     const int width = frame->width;
// //     const int height = frame->height;
// //     const int totalPixels = width * height;
// //     const size_t expectedSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 2ULL;

// //     if (totalPixels <= 0 || frame->size < expectedSize) {
// //         spdlog::warn("[AlgorithmConcrete] CPU HistEq size mismatch: frameSize={} expectedAtLeast={}",
// //                      frame->size, expectedSize);
// //         if (processedBuffer_.size() < frame->size) {
// //             processedBuffer_.resize(frame->size);
// //         }
// //         std::memcpy(processedBuffer_.data(), in, frame->size);
// //         return;
// //     }

// //     if (processedBuffer_.size() < frame->size) {
// //         processedBuffer_.resize(frame->size);
// //     }

// //     std::vector<int> hist(256, 0);
// //     std::vector<int> cdf(256, 0);

// //     // Histogram over Y channel.
// //     for (int y = 0; y < height; ++y) {
// //         const int rowBase = y * width * 2;
// //         for (int x = 0; x < width; ++x) {
// //             const int idx = rowBase + x * 2;
// //             ++hist[in[idx]];
// //         }
// //     }

// //     int running = 0;
// //     int minCdf = 0;
// //     for (int i = 0; i < 256; ++i) {
// //         running += hist[i];
// //         cdf[i] = running;
// //         if (minCdf == 0 && running > 0) {
// //             minCdf = running;
// //         }
// //     }

// //     const int denom = totalPixels - minCdf;

// //     // Degenerate image: all pixels have the same luma. Keep a valid neutral output.
// //     if (denom <= 0) {
// //         for (int y = 0; y < height; ++y) {
// //             const int rowBase = y * width * 2;
// //             for (int x = 0; x < width; ++x) {
// //                 const int idx = rowBase + x * 2;
// //                 processedBuffer_[idx]     = in[idx];
// //                 processedBuffer_[idx + 1] = 128;
// //             }
// //         }
// //         return;
// //     }

// //     parallelFor(0, height, [this, in, width, &cdf, minCdf, denom](size_t y) {
// //         const int rowBase = static_cast<int>(y) * width * 2;
// //         for (int x = 0; x < width; ++x) {
// //             const int idx = rowBase + x * 2;
// //             const int yIn = static_cast<int>(in[idx]);
// //             const float norm = static_cast<float>(cdf[yIn] - minCdf) / static_cast<float>(denom);
// //             int yOut = static_cast<int>(norm * 255.0f);
// //             yOut = utils::local_clamp(yOut, 0, 255);

// //             processedBuffer_[idx]     = static_cast<uint8_t>(yOut);
// //             processedBuffer_[idx + 1] = 128;
// //         }
// //     });
// // }

// // //=====================================================================================================
// // inline void AlgorithmConcrete::processHistogramEqualizationCPUZeroCopy(
// //     const std::shared_ptr<ZeroCopyFrameData>& frame)
// // {
// //     if (!frame || !frame->dataPtr || frame->width <= 0 || frame->height <= 0) {
// //         return;
// //     }

// //     const int width = frame->width;
// //     const int height = frame->height;
// //     const size_t dataSize = frame->size;
// //     const uint8_t* input = static_cast<const uint8_t*>(frame->dataPtr);

// //     processedBuffer_.resize(dataSize);

// //     std::array<int, 256> hist{};
// //     std::array<int, 256> cdf{};

// //     const int totalPixels = width * height;

// //     for (int y = 0; y < height; ++y) {
// //         const int rowBase = y * width * 2;
// //         for (int x = 0; x < width; ++x) {
// //             const int idx = rowBase + x * 2;
// //             ++hist[input[idx]];
// //         }
// //     }

// //     cdf[0] = hist[0];
// //     for (int i = 1; i < 256; ++i) {
// //         cdf[i] = cdf[i - 1] + hist[i];
// //     }

// //     int minCdf = 0;
// //     for (int i = 0; i < 256; ++i) {
// //         if (cdf[i] > 0) {
// //             minCdf = cdf[i];
// //             break;
// //         }
// //     }

// //     const int denom = totalPixels - minCdf;

// //     for (int y = 0; y < height; ++y) {
// //         const int rowBase = y * width * 2;
// //         for (int x = 0; x < width; ++x) {
// //             const int idx = rowBase + x * 2;
// //             uint8_t newY = input[idx];

// //             if (denom > 0) {
// //                 const float norm =
// //                     static_cast<float>(cdf[input[idx]] - minCdf) /
// //                     static_cast<float>(denom);

// //                 int mapped = static_cast<int>(norm * 255.0f);
// //                 mapped = std::max(0, std::min(255, mapped));
// //                 newY = static_cast<uint8_t>(mapped);
// //             }

// //             processedBuffer_[idx] = newY;
// //             processedBuffer_[idx + 1] = 128;
// //         }
// //     }
// // }

// // //=========================================================================================================================

// // inline void AlgorithmConcrete::processHeterogeneousGaussianBlurZeroCopy(const std::shared_ptr<ZeroCopyFrameData>& frame) {
// //     AlgorithmConcreteKernels::launchHeterogeneousGaussianBlurKernel(cudaRes_, frame, processedBuffer_, algoConfig_.blurRadius);
// // }

// // inline void AlgorithmConcrete::processOpticalFlow(const std::shared_ptr<ZeroCopyFrameData>& frame) {
// //     if (!frame || !frame->dataPtr) return;
// //     if (previousFrame_) {
// //         std::shared_ptr<ZeroCopyFrameData> flowOutput;
// //         opticalFlowProcessor_->computeOpticalFlow(previousFrame_, frame, flowOutput);
// //         if (outputQueueZeroCopy_ && flowOutput) {
// //             outputQueueZeroCopy_->push(flowOutput);
// //         }
// //     }
// //     previousFrame_ = frame;
// // }

// // // CUDA Helpers
// // inline void AlgorithmConcrete::checkCudaError(cudaError_t err, const std::string& context) {
// //     if (err != cudaSuccess) {
// //         reportError("[CUDA ERROR] " + context + ": " + cudaGetErrorString(err));
// //     }
// // }

// // inline double AlgorithmConcrete::timeCudaSectionMs(const std::function<void()>& launch) {
// // #ifdef __CUDACC__
// //     cudaEvent_t start, stop;
// //     cudaEventCreate(&start);
// //     cudaEventCreate(&stop);
// //     cudaEventRecord(start, 0);
// //     launch();
// //     cudaEventRecord(stop, 0);
// //     cudaEventSynchronize(stop);
// //     float ms = 0;
// //     cudaEventElapsedTime(&ms, start, stop);
// //     cudaEventDestroy(start);
// //     cudaEventDestroy(stop);
// //     return ms;
// // #else
// //     auto t0 = std::chrono::high_resolution_clock::now();
// //     launch();
// //     return std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
// // #endif
// // }
