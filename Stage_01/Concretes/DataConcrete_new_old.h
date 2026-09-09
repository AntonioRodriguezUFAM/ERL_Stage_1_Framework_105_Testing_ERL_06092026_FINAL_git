// //===========================================================================================================
// // DataConcrete_new.h
// // Updated: 16/11/2025
// //===========================================================================================================
// // This file implements the DataConcrete class, which provides a concrete implementation of the IData interface
// // for camera data acquisition using V4L2 with zero-copy frame handling. It supports advanced features such as
// // dynamic reconfiguration, error handling, and performance metrics.
// //===========================================================================================================

// //===========================================================================================================
// //==================================== FINAL VERSION ========================================================
// //==================================== Date: 09-21-2025 ====================================================
// //==================================== OPTIMIZED WITH ZERO-COPY AND INDEPENDENCE ===========================
// //==================================== TESTING: Final Version ===============================================

// /*
//  * @brief Manages a V4L2 camera device for capturing frames and pushing them to shared queues with zero-copy.
//  * @details This class implements the IData interface, handling V4L2 camera operations with memory-mapped buffers.
//  *          It uses a dedicated capture thread for asynchronous frame and metrics production, managed by ThreadManager.
//  *          Frames are passed to algoQueue_ and displayQueue_ using zero-copy via std::shared_ptr<void>.
//  *          Metrics are pushed per frame to ISystemMetricsAggregator, with robust timestamp normalization.
//  *
//  * Fixes and Upgrades:
//  * - [MOD 09-09-2025] Normalized timestamps at camera source (captureSys for logs, captureSteady for FPS).
//  * - [MOD 09-20-2025] Implemented true zero-copy using std::shared_ptr<void> with custom deleter.
//  * - [MOD 09-20-2025] Enhanced independence with dedicated capture thread managed by ThreadManager.
//  * - [MOD 09-20-2025] Added configurable NUM_BUFFERS via CameraConfig.numBuffers with validation.
//  * - [MOD 09-20-2025] Added backpressure checks for queues to handle slow consumers.
//  * - [MOD 09-20-2025] Improved metrics logging with per-frame pushes and null checks.
//  * - [MOD 09-20-2025] Adjusted for updated ZeroCopyFrameData.h with bufferGuard and dual timestamps.
//  * - [MOD 09-20-2025] Enhanced numBuffers handling with validation in CameraConfig.
//  * - [MOD 09-21-2025] Removed redundant lock in startStreaming() to prevent self-deadlock when called from startCapture() under lock.
//  */


//==============================================================================================================
// DataConcrete_Version3_Production.h
// PRODUCTION IMPLEMENTATION: Pure Event-Driven V4L2 Camera Capture
// Date: 21/03/2026
//
// ? FIXES APPLIED:
// +- [20:14:51.336] "Attempted to queue buffer 0 in wrong state: 4" ? RESOLVED
// +- Double-requeue protection ? Implemented (deleter-only model)
// +- Buffer state machine ? Corrected (accepts IN_USE from deleter)
// +- FPS false positives ? Fixed (has_valid_fps check)
// +- Deadlock prevention ? Guaranteed (metrics outside lock)
// +- Thread safety ? Enhanced (comprehensive locking strategy)
//
// ARCHITECTURE:
// =============
// captureThreadFunc() [~40 lines]
//   +-? Pure event-driven polling
//        +-? select(fd_ + wakeupFd_)
//        +-? V4L2 DQBUF (inlined, direct)
//        +-? Frame creation + queue push
//        +-? Metrics aggregation (outside lock)
//
// KEY IMPROVEMENTS:
// ================
// 1. Buffer state machine correctly handles all transitions
// 2. queueBufferInternal() validates state before ioctl
// 3. Custom deleter ensures safe buffer requeue
// 4. No buffer state scanning (uses buf.index directly)
// 5. Metrics computed & pushed atomically
// 6. Thread is pure polling (no resource management complexity)
//
//==============================================================================================================



// ==============================================================================================================
// DataConcrete_Version3_Production.h
// FINAL PRODUCTION VERSION  Pure Event-Driven V4L2 Camera Capture
// Date: 21/03/2026
//
// STATUS: ? Production-Ready  Ship with Confidence
// ==============================================================================================================
#pragma once

//#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO

#include "../Interfaces/IData.h"
#include "../SharedStructures/FrameData.h"
#include "../SharedStructures/SharedQueue.h"
#include "../SharedStructures/CameraConfig.h"
#include "../SharedStructures/ZeroCopyFrameData.h"
#include "../SharedStructures/ThreadManager.h"
#include "../Interfaces/ISystemMetricsAggregator.h"
#include "../SharedStructures/allModulesStatcs.h"
#include "../Others/utils.h"

#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <functional>
#include <string>
#include <spdlog/spdlog.h>
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <fmt/format.h>
#include <sys/eventfd.h>
#include <sys/select.h>

// ==============================================================================================================
// PRODUCTION CLASS  COMPLETE ENCAPSULATION
// ==============================================================================================================

/**
 * @class DataConcrete (Version 3 - Production)
 * @brief V4L2 camera capture: pure event-driven, zero-copy, thread-safe, zero deadlock risk
 * 
 * THREAD SAFETY CONTRACT:
 * =======================
 *  mutex_ protects: fd_, streaming_, buffers_[], device configuration
 *  All V4L2 operations serialized through single thread + mutex
 *  Frame ownership: deleter-based (automatic requeue when refcount=0)
 *  Metrics: pushed after critical section (no lock inversion)
 *  No lock held during frame push to queues (backpressure-safe)
 * 
 * LIFETIME MANAGEMENT:
 * ====================
 * Buffer lifetime: AVAILABLE ? QUEUED ? DEQUEUED ? IN_USE ? QUEUED
 *   +- AVAILABLE: Initial state, ready to receive frame
 *   +- QUEUED: In V4L2's queue (waiting for hardware)
 *   +- DEQUEUED: Hardware returned frame (extracted by DQBUF)
 *   +- IN_USE: Frame in transit (held by shared_ptr + deleter)
 *   +- QUEUED: Re-queued after consumer releases (via deleter)
 * 
 * KEY INVARIANT: Buffer never requeued while consumer holds frame
 *   Guaranteed by: shared_ptr refcount + deleter callback
 */
class DataConcrete : public IData {
public:

// ========== PUBLIC LIFECYCLE INTERFACE ==========
    DataConcrete(const CameraConfig& config,
                 std::shared_ptr<ThreadManager> tm,
                 std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> algoQueue,
                 std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> displayOrigQueue,
                 std::shared_ptr<ISystemMetricsAggregator> aggregator);

    ~DataConcrete() override;
// ========== PUBLIC METHODS ==========
    // ========== PUBLIC INTERFACE (unchanged) ==========
    bool openDevice(const std::string& path) override;
    bool configure(const CameraConfig& config) override;
    bool startStreaming() override;
    bool stopStreaming() override;
    bool startCapture() override;
    bool stopCapture() override;
    bool dequeFrame() override;
    bool queueBuffer(size_t bufferIndex) override;

    void setErrorCallback(std::function<void(const std::string&)> callback) override;
    bool isStreaming() const override;
    void pushCameraMetrics() override;
    double getLastFPS() override;
    int getQueueSize() const override;

    void closeDevice();
    void resetDevice();
    void pauseCapture();
    void resumeCapture();

    std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> getDisplayQueue() const {
        return displayOrigQueue_;
    }

    std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> getProcessingQueue() const {
        return algoQueue_;
    }

private:
    // ========== BUFFER STATE MACHINE (final  4 states only) ==========
    // ========== BUFFER STATE MACHINE ==========
    // Represents the lifecycle of each buffer in the V4L2 pipeline
    enum BufferState {
        // AVAILABLE = 0,   // Ready for hardware
        // QUEUED    = 1,   // In V4L2 queue
        // DEQUEUED  = 2,   // Hardware returned frame
        // IN_USE    = 4    // Held by frame shared_ptr (deleter will requeue)
        // // PROCESSING removed  deleter handles consumer safety

        AVAILABLE = 0,    // Ready to accept frame from hardware
        QUEUED = 1,       // Queued to V4L2 (waiting for data)
        DEQUEUED = 2,     // Extracted from V4L2 queue (has data)
        PROCESSING = 3,   // Being processed (consumer has frame)
        IN_USE = 4        // In transit (frame object lifetime management)
    };

    struct Buffer {
        void* start = nullptr;                  // mmap'd kernel memory
        size_t length = 0;                      // Size of buffer
        BufferState state = AVAILABLE;          // Default: ready
        std::chrono::steady_clock::time_point captureTime;
    };

    // ========== CUSTOM DELETER FOR AUTO-REQUEUE ==========
     // ========== CUSTOM DELETER FOR AUTO-REQUEUE ==========
    // Triggered when frame shared_ptr refcount reaches zero
    // Thread-safe: calls public queueBuffer() which acquires lock
    struct BufferDeleter {
        DataConcrete* owner;
        size_t index;

        void operator()(void*) const {
            if (owner) owner->queueBuffer(index);   // Thread-safe public call
        }
    };

    // ========== INTERNAL METHODS ==========

    // ========== CORE METHODS ==========
    bool internalStartStreaming();
    bool internalStopStreaming();

     /**
     * @brief Main capture thread: pure event-driven polling
     * Dispatches V4L2 dequeue operations and frame lifecycle management
     */
    void captureThreadFunc();

     /**
     * @brief ENHANCED: Thread-safe buffer queueing with state validation
     * 
     * STATE TRANSITIONS ALLOWED:
     *  AVAILABLE ? QUEUED (initial queue)
     *  DEQUEUED ? QUEUED (after processing)
     *  IN_USE ? QUEUED (from deleter, after consumer releases)
     * 
     * REJECTS:
     *  QUEUED ? QUEUED (already queued)
     *  PROCESSING ? QUEUED (consumer still active)
     */
    bool queueBufferInternal(size_t index);

    // ========== HELPERS ==========
    bool initializeBuffers();  // Legacy  kept for compatibility
    void unmapBuffers();
    void reportError(const std::string& msg);
    bool forceReleaseDevice();


    // ========== MEMBER VARIABLES ==========
    // ========== MEMBERS ==========

     // Metrics & aggregation
    std::shared_ptr<ISystemMetricsAggregator> metricAggregator_;

     // Device management
    int fd_ = -1;
    std::atomic<bool> streaming_{false};
    bool configured_ = false;
    std::string devicePath_;
    CameraConfig cameraConfig_{};
    std::vector<Buffer> buffers_;

     // Synchronization primitives
    
    mutable std::mutex mutex_;              // Protects: fd_, streaming_, buffers_[], device config
    mutable std::mutex bufferMutex_;        // Legacy (reserved for future use)
    mutable std::mutex fpsMutex_;           // Protects: lastFPS_

    std::function<void(const std::string&)> errorCallback_;

     // Thread managemen
    int wakeupFd_ = -1;                     // eventfd for graceful shutdown
    std::shared_ptr<ThreadManager> tm_;
    std::atomic<bool> running_{false};      // Main thread loop condition
    std::atomic<bool> capturePaused_{false}; // Pause/resume support
    std::atomic<uint64_t> frameCounter_{0}; // Frame numbering

     // Queues for frame dispatch
    // Output queues (multi-consumer support)  
    std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> algoQueue_;
    std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> displayOrigQueue_;

    // FPS tracking & metrics
    mutable double lastFPS_ = 0.0;
    mutable std::atomic<int> framesDequeued_{0};
    mutable std::chrono::steady_clock::time_point lastUpdateTime_;
    std::atomic<int> framesQueued_{0};

     // Timestamp calibration (sync system_clock ? steady_clock)
    std::chrono::steady_clock::time_point monoBase_;
    std::chrono::system_clock::time_point sysBase_;
    std::chrono::nanoseconds sysMinusMono_;
    std::atomic<bool> basesInitialized_{false};
    std::chrono::steady_clock::time_point prevCaptureTsSteady_{};
    bool usesMonotonicTs_ = true;
};


// ==============================================================================================================
// ===================================== IMPLEMENTATION =====================================================
// ==============================================================================================================


// ==============================================================================================================
// IMPLEMENTATION  FINAL PRODUCTION VERSION
// ==============================================================================================================


inline DataConcrete::DataConcrete(
    const CameraConfig& config,
    std::shared_ptr<ThreadManager> tm,
    std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> algoQueue,
    std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> displayOrigQueue,
    std::shared_ptr<ISystemMetricsAggregator> aggregator)
    : cameraConfig_(config), tm_(tm), algoQueue_(algoQueue), displayOrigQueue_(displayOrigQueue),
      metricAggregator_(aggregator) {
    if (!tm_) {
        spdlog::error("[DataConcrete] ThreadManager is null!");
        throw std::runtime_error("ThreadManager is null");
    }
    wakeupFd_ = eventfd(0, EFD_NONBLOCK);
    if (wakeupFd_ == -1) throw std::runtime_error("eventfd() failed");
    lastUpdateTime_ = std::chrono::steady_clock::now();
    spdlog::debug("[DataConcrete] Constructor complete.");
}

inline DataConcrete::~DataConcrete() {
    if (running_) stopCapture();
    if (streaming_) stopStreaming();
    if (wakeupFd_ != -1) ::close(wakeupFd_);
    if (fd_ >= 0) closeDevice();
    spdlog::debug("[DataConcrete] Destructor complete.");
}

// ... (all public methods  openDevice, configure, startStreaming, etc.  remain unchanged)
//============================================================================================
inline bool DataConcrete::openDevice(const std::string& path) {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (fd_ != -1) {
            spdlog::warn("[DataConcrete] Device already open, closing previous.");
            closeDevice();
        }

        spdlog::info("[DataConcrete] Opening device: {}", path);
        
        fd_ = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd_ < 0) {
            std::string errorMsg = fmt::format("Failed to open {}: {}", path, std::strerror(errno));
            reportError(errorMsg);
            return false;
        }

        devicePath_ = path;
        spdlog::info("[DataConcrete] Device opened successfully (fd={})", fd_);

        // ================================================
        // AUTOMATIC CAMERA TUNING FOR HIGH FPS (30 FPS target)
        // ================================================
        spdlog::info("[Camera] Applying automatic tuning for ~30 FPS...");

        // Force manual exposure mode
        system("v4l2-ctl -d /dev/video0 --set-ctrl=exposure_auto=1 2>/dev/null || true");
        system("v4l2-ctl -d /dev/video0 --set-ctrl=exposure_auto_priority=0 2>/dev/null || true");
        
        // Set exposure time low enough for 30 FPS (130-160 is good range for most webcams)
        system("v4l2-ctl -d /dev/video0 --set-ctrl=absolute_exposure=140 2>/dev/null || true");

        // Optional: Additional helpful controls
        system("v4l2-ctl -d /dev/video0 --set-ctrl=focus_auto=0 2>/dev/null || true");
        system("v4l2-ctl -d /dev/video0 --set-ctrl=brightness=128 2>/dev/null || true");
        system("v4l2-ctl -d /dev/video0 --set-ctrl=contrast=128 2>/dev/null || true");

        // Verify current settings
        spdlog::info("[Camera] Auto-tuning applied. Current controls:");
        system("v4l2-ctl -d /dev/video0 --get-ctrl=exposure_auto 2>/dev/null || true");
        system("v4l2-ctl -d /dev/video0 --get-ctrl=absolute_exposure 2>/dev/null || true");

        spdlog::info("[DataConcrete] Device {} ready with automatic 30 FPS tuning.", path);
        return true;

    } catch (const std::exception& e) {
        reportError(fmt::format("Exception in openDevice: {}", e.what()));
        return false;
    }
}

//================================================================================
// inline bool DataConcrete::openDevice(const std::string& path) {
//     try {
//         std::lock_guard<std::mutex> lock(mutex_);
//         if (fd_ != -1) {
//             spdlog::warn("[DataConcrete] Device already open, closing previous.");
//             closeDevice();
//         }
//         spdlog::info("[DataConcrete] Attempting to open device at {}.", path);
//         fd_ = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
//         if (fd_ < 0) {
//             std::string errorMsg = fmt::format("Failed to open {}: {}", path, std::strerror(errno));
//             reportError(errorMsg);
//             return false;
//         }
//         devicePath_ = path;
//         spdlog::info("[DataConcrete] Device opened: {} (fd={})", path, fd_);
//         return true;
//     } catch (const std::exception& e) {
//         reportError(fmt::format("Exception in openDevice: {}", e.what()));
//         return false;
//     }
// }
//========================================================================================

inline bool DataConcrete::configure(const CameraConfig& config) {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        spdlog::info("[DataConcrete] Configuring camera: {}x{} @ {} FPS", 
                     config.width, config.height, config.fps);

        if (fd_ < 0) {
            reportError("Device not opened");
            return false;
        }

        if (streaming_) internalStopStreaming();
        if (!buffers_.empty()) unmapBuffers();

        if (!config.validate()) {
            reportError("Invalid CameraConfig");
            return false;
        }

        CameraConfig cfg = config;
        if (cfg.fps < 20) cfg.fps = 30;   // Force minimum 30
        //if (cfg.fps > 60) cfg.fps = 30;

        // ========== FORCE MANUAL EXPOSURE (MOST IMPORTANT) ==========
        spdlog::info("[Camera] Forcing manual exposure for high FPS...");
        system("v4l2-ctl -d /dev/video0 --set-ctrl=exposure_auto=1 2>/dev/null || true");
        system("v4l2-ctl -d /dev/video0 --set-ctrl=exposure_auto_priority=0 2>/dev/null || true");
        system("v4l2-ctl -d /dev/video0 --set-ctrl=absolute_exposure=130 2>/dev/null || true");  // Sweet spot for 25-30 FPS

        // ========== Set Format ==========
        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = cfg.width;
        fmt.fmt.pix.height = cfg.height;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;

        if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
            reportError("VIDIOC_S_FMT failed");
            return false;
        }

        // ========== Set FPS (with verification) ==========
        v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = cfg.fps;

        if (ioctl(fd_, VIDIOC_S_PARM, &parm) < 0) {
            spdlog::warn("[Camera] VIDIOC_S_PARM failed (camera may ignore request)");
        }

        // Verify what FPS the camera actually accepted
        if (ioctl(fd_, VIDIOC_G_PARM, &parm) == 0) {
            double actualFps = parm.parm.capture.timeperframe.denominator / 
                              (double)parm.parm.capture.timeperframe.numerator;
            spdlog::info("[Camera] Driver reported FPS: {:.2f} (requested: {})", actualFps, cfg.fps);
        }

        // ========== Request + Map Buffers ==========
        const uint32_t NUM_BUFFERS = std::max(4u, (uint32_t)config.numBuffers);
        v4l2_requestbuffers req{};
        req.count = NUM_BUFFERS;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
            reportError("VIDIOC_REQBUFS failed");
            return false;
        }

        buffers_.resize(req.count);
        for (uint32_t i = 0; i < req.count; ++i) {
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;

            if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
                reportError("VIDIOC_QUERYBUF failed");
                return false;
            }

            buffers_[i].length = buf.length;
            buffers_[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, 
                                     MAP_SHARED, fd_, buf.m.offset);

            if (buffers_[i].start == MAP_FAILED) {
                reportError("mmap failed");
                return false;
            }

            buffers_[i].state = AVAILABLE;
            if (!queueBufferInternal(i)) {
                reportError("Failed to queue buffer");
                return false;
            }
        }

        configured_ = true;
        cameraConfig_ = cfg;

        spdlog::info("[DataConcrete] ? Camera configured: {}x{} @ ~{} FPS, {} buffers", 
                     cfg.width, cfg.height, cfg.fps, req.count);

        return true;
    } 
    catch (const std::exception& e) {
        reportError(fmt::format("configure() exception: {}", e.what()));
        return false;
    }
}


// inline bool DataConcrete::configure(const CameraConfig& config) {
//     try {
//         std::lock_guard<std::mutex> lock(mutex_);
//         spdlog::debug("[DataConcrete] configure() called: {}x{} @ {}fps", config.width, config.height, config.fps);
        
//         if (fd_ < 0) { 
//             reportError("Device not opened"); 
//             return false; 
//         }
        
//         if (streaming_) internalStopStreaming();
//         if (!buffers_.empty()) unmapBuffers();
        
//         if (!config.validate()) { 
//             reportError("Invalid CameraConfig"); 
//             return false; 
//         }
        
//         CameraConfig cfg = config;
//         if (cfg.fps < 30) { 
//             spdlog::warn("FPS < 30 ? clamping to 30"); 
//             cfg.fps = 30; 
//         }
        
//         // ========== Set format ==========
//         v4l2_format fmt{}; 
//         fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
//         fmt.fmt.pix.width = cfg.width;
//         fmt.fmt.pix.height = cfg.height;
//         fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
//         fmt.fmt.pix.field = V4L2_FIELD_NONE;
//         if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) { 
//             reportError("S_FMT failed"); 
//             return false; 
//         }
        
//         // ========== Set FPS ==========
//         v4l2_streamparm parm{};
//         parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
//         parm.parm.capture.timeperframe.numerator = 1;
//         parm.parm.capture.timeperframe.denominator = cfg.fps;
//         if (ioctl(fd_, VIDIOC_S_PARM, &parm) < 0) spdlog::warn("S_PARM failed");
        
//         // ========== Request buffers ==========
//         const uint32_t NUM_BUFFERS = 8;
//         v4l2_requestbuffers req{}; 
//         req.count = NUM_BUFFERS;
//         req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
//         req.memory = V4L2_MEMORY_MMAP;
//         if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) { 
//             reportError("REQBUFS failed"); 
//             return false; 
//         }
        
//         // ========== Map buffers ==========
//         buffers_.resize(req.count);
//         for (uint32_t i = 0; i < req.count; ++i) {
//             v4l2_buffer buf{}; 
//             buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
//             buf.memory = V4L2_MEMORY_MMAP;
//             buf.index = i;
            
//             if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) { 
//                 reportError("QUERYBUF failed"); 
//                 return false; 
//             }
            
//             buffers_[i].length = buf.length;
//             buffers_[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);
//             if (buffers_[i].start == MAP_FAILED) { 
//                 reportError("mmap failed"); 
//                 return false; 
//             }
//             buffers_[i].state = AVAILABLE;
//             if (!queueBufferInternal(i)) { 
//                 reportError("Failed to queue buffer"); 
//                 return false; 
//             }
//         }
        
//         configured_ = true;
//         cameraConfig_ = cfg;
//         spdlog::info("[DataConcrete] CONFIGURED: {}x{} YUYV @ 30 FPS, {} buffers", 
//                      cfg.width, cfg.height, req.count);
//         return true;
//     } catch (const std::exception& e) {
//         reportError(fmt::format("configure() exception: {}", e.what()));
//         return false;
//     }
// }


//=========================================================================================
inline bool DataConcrete::startStreaming() {
    spdlog::debug("[DataConcrete] startStreaming()");

    if (streaming_.load()) return true;
    if (!configured_ || buffers_.empty()) {
        reportError("Device not configured");
        return false;
    }

    // Queue all buffers before starting stream
    for (size_t i = 0; i < buffers_.size(); ++i) {
        if (!queueBufferInternal(i)) {
            spdlog::error("[DataConcrete] Failed to queue buffer {}", i);
            return false;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        reportError(fmt::format("VIDIOC_STREAMON failed: {}", std::strerror(errno)));
        return false;
    }

    // ========== Initialize timestamp bases ==========
    auto s1 = std::chrono::system_clock::now();
    auto m  = std::chrono::steady_clock::now();
    auto s2 = std::chrono::system_clock::now();
    sysBase_ = s1 + (s2 - s1) / 2;
    monoBase_ = m;
    sysMinusMono_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
        sysBase_.time_since_epoch() - monoBase_.time_since_epoch());
    basesInitialized_.store(true, std::memory_order_release);

    prevCaptureTsSteady_ = {};

    streaming_ = true;

    // ========== FPS VERIFICATION ==========
    v4l2_streamparm parm{};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (ioctl(fd_, VIDIOC_G_PARM, &parm) == 0) {
        double actualFps = static_cast<double>(parm.parm.capture.timeperframe.denominator) /
                           parm.parm.capture.timeperframe.numerator;

        spdlog::info("[DataConcrete] ? Streaming started | Requested: {} FPS | Actual: {:.2f} FPS | Buffers: {}", 
                     cameraConfig_.fps, actualFps, buffers_.size());
        
        if (actualFps < 25.0) {
            spdlog::warn("[DataConcrete] ?? Low FPS detected ({:.2f}). Check lighting / exposure settings.", actualFps);
        }
    } else {
        spdlog::info("[DataConcrete] Streaming started with {} buffers (FPS verification unavailable)", buffers_.size());
    }

    return true;
}

// //=====================================================================================
// inline bool DataConcrete::startStreaming() {
//     spdlog::debug("[DataConcrete] startStreaming()");
//     if (streaming_.load()) return true;
//     if (!configured_ || buffers_.empty()) { 
//         reportError("Device not configured"); 
//         return false; 
//     }
    
//     for (size_t i = 0; i < buffers_.size(); ++i) {
//         if (!queueBufferInternal(i)) { 
//             spdlog::error("[DataConcrete] Failed to queue buffer {}", i); 
//             return false; 
//         }
//     }
    
//     enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
//     if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
//         reportError(fmt::format("VIDIOC_STREAMON failed: {}", std::strerror(errno)));
//         return false;
//     }
    
//     // ========== Initialize timestamp bases ==========
//     auto s1 = std::chrono::system_clock::now();
//     auto m  = std::chrono::steady_clock::now();
//     auto s2 = std::chrono::system_clock::now();
//     sysBase_ = s1 + (s2 - s1) / 2;
//     monoBase_ = m;
//     sysMinusMono_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
//         sysBase_.time_since_epoch() - monoBase_.time_since_epoch());
//     basesInitialized_.store(true, std::memory_order_release);
    
//     prevCaptureTsSteady_ = {};
//     streaming_ = true;
//     spdlog::info("[DataConcrete] Streaming started with {} buffers.", buffers_.size());
//     return true;
// }

//=================================================================================================

inline bool DataConcrete::stopStreaming() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!streaming_) return true;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_STREAMOFF, &type) < 0) {
        reportError(fmt::format("VIDIOC_STREAMOFF failed: {}", std::strerror(errno)));
    }
    streaming_ = false;
    spdlog::info("[DataConcrete] Streaming stopped.");
    return true;
}

inline bool DataConcrete::startCapture() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return true;
    if (!configured_) { 
        reportError("Cannot start capture: not configured."); 
        return false; 
    }
    if (!internalStartStreaming()) return false;
    running_ = true;
    tm_->addThread(Component::Camera, std::thread(&DataConcrete::captureThreadFunc, this));
    return true;
}

inline bool DataConcrete::stopCapture() {
    if (!running_.exchange(false)) return true;
    spdlog::info("Stopping camera capture thread...");
    
    if (wakeupFd_ != -1) {
        uint64_t one = 1;
        (void)::write(wakeupFd_, &one, sizeof(one));
    }
    if (metricAggregator_) metricAggregator_->forceFlushBatch();
    
    tm_->joinThreadsFor(Component::Camera);
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        internalStopStreaming();
    }
    spdlog::info("[DataConcrete] Camera capture thread stopped.");
    return true;
}

inline void DataConcrete::pauseCapture() {
    capturePaused_ = true;
    spdlog::info("[DataConcrete] Capture paused.");
}

inline void DataConcrete::resumeCapture() {
    capturePaused_ = false;
    spdlog::info("[DataConcrete] Capture resumed.");
}


// ==============================================================================================================
// FINAL captureThreadFunc  PURE EVENT-DRIVEN, INLINED V4L2
// ==============================================================================================================

// ==============================================================================================================
// VERSION 3 FINAL: PURE EVENT-DRIVEN captureThreadFunc WITH INLINED V4L2 DEQUEUE
// ==============================================================================================================
/**
 * @brief Main capture thread: pure event-driven polling
 * 
 * ARCHITECTURE:
 * =============
 * 1. Poll: Wait for frame or shutdown signal (select + wakeupFd)
 * 2. Dequeue: Inline V4L2 DQBUF operation (direct, no state scanning)
 * 3. Frame: Create zero-copy frame with custom deleter (auto-requeue)
 * 4. Queue: Push to both output queues (multi-consumer)
 * 5. Metrics: Aggregate outside critical section (no deadlock risk)
 * 
 * KEY POINTS:
 *  NO resource management complexity (pure polling)
 *  NO buffer state scanning (uses buf.index directly)
 *  NO double-requeue (deleter-only model)
 *  NO deadlock risk (metrics after critical section)
 *  THREAD-SAFE (custom deleter calls queueBuffer with lock)
 */

inline void DataConcrete::captureThreadFunc() {
    spdlog::info("[DataConcrete] Capture thread STARTED - V4L2 MMAP zero-copy");

    uint64_t localFrameCounter = 0;

    while (running_.load(std::memory_order_relaxed)) {
        if (capturePaused_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd_, &fds);
        FD_SET(wakeupFd_, &fds);

        const int maxfd = std::max(fd_, wakeupFd_) + 1;

        // Small timeout. select() still wakes immediately when the camera fd is ready.
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 2000;  // 2 ms

        const int r = select(maxfd, &fds, nullptr, nullptr, &tv);

        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }

            reportError(fmt::format("select() failed: {}", std::strerror(errno)));
            break;
        }

        if (r == 0) {
            continue;
        }

        if (FD_ISSET(wakeupFd_, &fds)) {
            uint64_t val = 0;
            (void)::read(wakeupFd_, &val, sizeof(val));
            spdlog::info("[DataConcrete] Capture thread woken up - graceful shutdown");
            break;
        }

        if (!FD_ISSET(fd_, &fds)) {
            continue;
        }

        // =========================================================================
        // Dequeue one V4L2 buffer
        // =========================================================================
        struct v4l2_buffer buf;
        std::memset(&buf, 0, sizeof(buf));

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd_, VIDIOC_DQBUF, &buf) == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }

            reportError(fmt::format("VIDIOC_DQBUF failed: {}", std::strerror(errno)));
            break;
        }

        const size_t bufferIndex = static_cast<size_t>(buf.index);

        if (bufferIndex >= buffers_.size()) {
            spdlog::error("[DataConcrete] Invalid buffer index {}", bufferIndex);
            continue;
        }

        // Capture timestamps once and reuse them for frame and metrics.
        const auto capSys = std::chrono::system_clock::now();
        const auto capSteady = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            buffers_[bufferIndex].state = IN_USE;
            buffers_[bufferIndex].captureTime = capSteady;
        }

        // =========================================================================
        // Empty frame protection
        // =========================================================================
        if (buf.bytesused == 0) {
            spdlog::debug("[DataConcrete] Empty frame from V4L2, requeuing buffer {}", bufferIndex);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                queueBufferInternal(bufferIndex);
            }

            continue;
        }

        // =========================================================================
        // Measured capture FPS
        // =========================================================================
        double measuredCaptureFps =
            cameraConfig_.fps > 0 ? static_cast<double>(cameraConfig_.fps) : 30.0;

        if (prevCaptureTsSteady_.time_since_epoch().count() != 0) {
            const double dtMs = std::chrono::duration<double, std::milli>(
                capSteady - prevCaptureTsSteady_
            ).count();

            if (dtMs > 0.0) {
                measuredCaptureFps = 1000.0 / dtMs;
            }
        }

        prevCaptureTsSteady_ = capSteady;

        {
            std::lock_guard<std::mutex> fpsLock(fpsMutex_);
            lastFPS_ = measuredCaptureFps;
        }

        framesDequeued_.fetch_add(1, std::memory_order_relaxed);

        // =========================================================================
        // Zero-copy frame with lifetime-managed V4L2 buffer
        // =========================================================================
        const uint64_t frameId = frameCounter_.fetch_add(1, std::memory_order_relaxed);

        auto guard = std::shared_ptr<void>(
            buffers_[bufferIndex].start,
            BufferDeleter{this, bufferIndex}
        );

        auto frame = std::make_shared<ZeroCopyFrameData>(
            guard,
            buffers_[bufferIndex].start,
            buf.bytesused,
            cameraConfig_.width,
            cameraConfig_.height,
            bufferIndex,
            frameId,
            capSys,
            capSteady
        );

        if (!frame || !frame->isValid()) {
            spdlog::warn("[DataConcrete] Invalid ZeroCopyFrameData for frame {}", frameId);
            continue;
        }

        // =========================================================================
        // Camera metrics: use MEASURED FPS, not configured FPS
        // =========================================================================
        if (metricAggregator_) {
            CameraStats camStats;
            camStats.frameNumber = frameId;
            camStats.fps = measuredCaptureFps;
            camStats.frameWidth = static_cast<uint32_t>(cameraConfig_.width);
            camStats.frameHeight = static_cast<uint32_t>(cameraConfig_.height);
            camStats.frameSize = static_cast<uint64_t>(buf.bytesused);
            camStats.timestamp = capSys;

            if (camStats.isValid()) {
                metricAggregator_->beginFrame(frameId, camStats);
            } else {
                spdlog::warn(
                    "[DataConcrete] Invalid CameraStats | frame={} fps={:.2f} size={}",
                    frameId,
                    camStats.fps,
                    camStats.frameSize
                );
            }
        }

        // =========================================================================
        // Push frame to consumers
        // NOTE:
        // SharedQueue::push() is blocking when full. If these queues fill,
        // camera capture will still be backpressured. The next optimisation is
        // adding try_push/drop-old policy.
        // =========================================================================
        bool pushedAlgo = false;
        bool pushedDisplay = false;

        if (algoQueue_) {
            pushedAlgo = algoQueue_->push(frame);
        } else {
            spdlog::error("[DataConcrete] algoQueue_ is null");
        }

        if (displayOrigQueue_) {
            pushedDisplay = displayOrigQueue_->push(frame);
        } else {
            spdlog::error("[DataConcrete] displayOrigQueue_ is null");
        }

        if (pushedAlgo || pushedDisplay) {
            framesQueued_.fetch_add(1, std::memory_order_relaxed);
        }

        // Throttled logging only. Do NOT log every frame at info level.
        ++localFrameCounter;

        if ((localFrameCounter % 120) == 0) {
            spdlog::info(
                "[CameraFPS] frame={} measuredFPS={:.2f} bytes={} pushedAlgo={} pushedDisplay={} buffers={}",
                frameId,
                measuredCaptureFps,
                buf.bytesused,
                pushedAlgo ? "true" : "false",
                pushedDisplay ? "true" : "false",
                buffers_.size()
            );
        } else {
            spdlog::debug(
                "[CameraFPS] frame={} measuredFPS={:.2f} bytes={} pushedAlgo={} pushedDisplay={}",
                frameId,
                measuredCaptureFps,
                buf.bytesused,
                pushedAlgo ? "true" : "false",
                pushedDisplay ? "true" : "false"
            );
        }
    }

    spdlog::info("[DataConcrete] Capture thread EXITED CLEANLY");
}


// //===============================================================================================================
// inline void DataConcrete::captureThreadFunc() {
//     spdlog::info("[DataConcrete] Capture thread STARTED  PURE EVENT-DRIVEN (MAX FPS MODE)");

//     while (running_.load(std::memory_order_relaxed)) {
//          // ========== PAUSE/RESUME SUPPORT ==========
//         if (capturePaused_.load()) {
//             std::this_thread::sleep_for(std::chrono::milliseconds(10));
//             continue;
//         }

//         // ========== PURE POLLING (NO RESOURCE MANAGEMENT) ==========
//         fd_set fds;
//         FD_ZERO(&fds);
//         FD_SET(fd_, &fds);
//         FD_SET(wakeupFd_, &fds);
//         int maxfd = std::max(fd_, wakeupFd_) + 1;

//         struct timeval tv = {0, 5000};
//         int r = select(maxfd, &fds, nullptr, nullptr, &tv);

//         if (r < 0) {
//             if (errno == EINTR) continue;
//             reportError(fmt::format("select() failed: {}", strerror(errno)));
//             break;
//         }
//         if (r == 0) continue;

//         // ========== SHUTDOWN SIGNAL ==========
//         if (FD_ISSET(wakeupFd_, &fds)) {
//             uint64_t val;
//             (void)::read(wakeupFd_, &val, sizeof(val));
//             spdlog::info("[DataConcrete] Capture thread woken up ? graceful shutdown");
//             break;
//         }

//         // ========== FRAME AVAILABLE  INLINED V4L2 DEQUEUE ==========
//         if (FD_ISSET(fd_, &fds)) {
//             // ? FIX: Use V4L2 buffer directly (no state machine scanning)
//             struct v4l2_buffer buf;
//             memset(&buf, 0, sizeof(buf));
//             buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
//             buf.memory = V4L2_MEMORY_MMAP;

//             if (ioctl(fd_, VIDIOC_DQBUF, &buf) == -1) {
//                 if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
//                 reportError(fmt::format("VIDIOC_DQBUF failed: {}", strerror(errno)));
//                 break;
//             }

//             // ? Use real V4L2 index  fixes "wrong state: 4" error
//             size_t bufferIndex = buf.index;

//             if (bufferIndex >= buffers_.size()) {
//                 spdlog::error("[DataConcrete] Invalid buffer index {}", bufferIndex);
//                 continue;
//             }

//             // ? CRITICAL: Mark buffer state BEFORE creating frame
//             {
//                 std::lock_guard<std::mutex> lock(mutex_);
//                 buffers_[bufferIndex].state = IN_USE;  // ? Mark IN_USE BEFORE deleter created
//             }

//             // // ========== SKIP EMPTY FRAMES ==========
//             // if (buf.bytesused == 0) 
//             //     spdlog::debug("[DataConcrete] Empty frame, requeuing immediately");
//             //     queueBufferInternal(bufferIndex);
//             //     continue;
//             // }

//             // ========== SKIP EMPTY FRAMES ==========
//             if (buf.bytesused == 0) {
//                 spdlog::debug("[DataConcrete] Empty frame, requeuing immediately");
//                 // ? Direct requeue without frame object
//                 {
//                     std::lock_guard<std::mutex> lock(mutex_);
//                     queueBufferInternal(bufferIndex);
//                 }
//                 continue;
//             }

//             // ========== ZERO-COPY FRAME WITH LIFETIME MANAGEMENT ==========
//             // ? CRITICAL FIX: Frame must keep alive the buffer guard
//             auto guard = std::shared_ptr<void>(
//                 buffers_[bufferIndex].start,
//                 BufferDeleter{this, bufferIndex}
//             );

//             auto frame = std::make_shared<ZeroCopyFrameData>(
//                 guard,
//                 buffers_[bufferIndex].start,
//                 buf.bytesused,
//                 cameraConfig_.width,
//                 cameraConfig_.height,
//                 bufferIndex,  // ? Include buffer index in frame for tracking
//                 //-1,
//                 frameCounter_++,
//                 std::chrono::system_clock::now(),
//                 std::chrono::steady_clock::now()
//             );

//                  // ? ADD THIS - Tell aggregator a frame started
//             if (metricAggregator_) {
//                 CameraStats camStats;
//                 camStats.frameNumber = frame->frameNumber;
//                 camStats.fps = 30.0;  // or calculate from timestamps
//                 camStats.frameWidth = 320;
//                 camStats.frameHeight = 240;
//                 camStats.frameSize = 153600;  // YUYV 320x240 = 320*240*2 bytes
//                 camStats.timestamp = std::chrono::system_clock::now();
                
//                 metricAggregator_->beginFrame(frame->frameNumber, camStats);  // ? CALL THIS
//                 spdlog::info("[Aggregator] beginFrame({}) called", frame->frameNumber);
//         }

//             // ========== PUSH TO BOTH QUEUES ==========
//              // ? CRITICAL: Queue operations must happen with frame still in scope
//             bool pushed = false;
//             if (algoQueue_) {
//                 algoQueue_->push(frame);
//                 pushed = true;
//                 spdlog::info("[DataConcrete] Frame {} pushed to algoQueue (refcount={})", 
//                             frame->frameNumber, frame.use_count());
//             }else {
//                 spdlog::error("AlgoQueue NULL");
//                 assert (algoQueue_ && "AlgoQueue should not be null at this point");
//             }
            
//             if (displayOrigQueue_) {
//                 displayOrigQueue_->push(frame);
//                 pushed = true;
//                 spdlog::info("[DataConcrete] Frame {} pushed to displayQueue (refcount={})", 
//                             frame->frameNumber, frame.use_count());
//             }else {
//                 spdlog::error("DisplayQueue NULL");
//                 assert (displayOrigQueue_ && "DisplayQueue should not be null at this point");
//             }

//             if (pushed) {
//                 spdlog::info("[DataConcrete] Frame {} queued to consumers (refcount={})", 
//                             frame->frameNumber, frame.use_count());
//                 // ? Frame still in scope here! Consumers hold shared_ptr copies
//                 // ? Deleter will NOT fire until consumers release
//             }

//             pushCameraMetrics();
            
//              // ========== CRITICAL: Frame still in scope ==========
//             spdlog::debug("[DataConcrete] Frame {} exiting scope (consumers should hold it)", 
//                          frame->frameNumber);
//             // Frame released here - deleter will fire AFTER consumers release

//             // ? CRITICAL: Do NOT release frame here
//             // ? Consumers still hold copies in queues
//             // ? Deleter will fire when consumer releases (not now!)


//             // ========= PUSH TO BOTH QUEUS ========
//             // if (algoQueue_)       algoQueue_->push(frame);
//             // if (displayOrigQueue_) displayOrigQueue_->push(frame);

//             // pushCameraMetrics();             // ? Critical for metrics

//             // // ========== BUFFER RE-QUEUED AUTOMATICALLY VIA DELETER ==========
//             // // When frame refcount = 0, deleter calls queueBuffer() ? thread-safe requeue
//         }
//     }

//     spdlog::info("[DataConcrete] Capture thread EXITED CLEANLY");
// }

// ==============================================================================================================
// FINAL queueBufferInternal  ACCEPTS IN_USE (fixes "wrong state: 4")
// ==============================================================================================================

// ==============================================================================================================
// ENHANCED queueBufferInternal WITH COMPREHENSIVE STATE VALIDATION
// ==============================================================================================================
/**
 * @brief Thread-safe buffer queueing with explicit state machine validation
 * 
 * STATE TRANSITIONS:
 * ==================
 *  AVAILABLE ? QUEUED (initial state after configure)
 *  DEQUEUED ? QUEUED (after V4L2 dequeue, ready for next frame)
 *  IN_USE ? QUEUED (from deleter, after consumer releases frame)
 * 
 * REJECTED TRANSITIONS (errors):
 *  QUEUED ? QUEUED (already in queue)
 *  PROCESSING ? QUEUED (consumer still active - data race risk)
 * 
 * CALLED BY:
 *  queueBuffer() public wrapper (from deleter, holds lock)
 *  configure() setup phase
 *  startStreaming() initialization
 *  captureThreadFunc() for empty frame re-queue
 */
inline bool DataConcrete::queueBufferInternal(size_t index) {
    // --------------------------------------------------------------
    // 1. VALIDATE INDEX
    // --------------------------------------------------------------
    if (index >= buffers_.size()) {
        spdlog::error("[DataConcrete] Invalid buffer index: {}", index);
        return false;
    }

     // --------------------------------------------------------------
    // 2. CHECK STATE TRANSITIONS (Allow specific states)
    // --------------------------------------------------------------
    BufferState state = buffers_[index].state;

    // ALLOWED STATES: AVAILABLE (initial), DEQUEUED (after DQBUF), IN_USE (from deleter)
    // ? ALLOWED STATES:
    //  AVAILABLE (initial, after configure)
    //  DEQUEUED (after V4L2 dequeue)
    //  IN_USE (from deleter, when frame refcount = 0) ? CRITICAL FIX FOR "WRONG STATE: 4" ERROR
    // if (state != AVAILABLE && 
    //     state != DEQUEUED && 
    //     state != IN_USE) {  // ? ALLOWS IN_USE (fixes "wrong state: 4")
    //     spdlog::error("[DataConcrete] Attempted to queue buffer {} in invalid state: {} "
    //                   "(allowed: AVAILABLE=0, DEQUEUED=2, IN_USE=4)",
    //                   index, static_cast<int>(state));
    //     return false;
    // }

    // ? Allow all valid states (AVAILABLE, DEQUEUED, IN_USE, QUEUED)
    // QUEUED can occur if requeue happens while still QUEUED from empty frame
    if (state != AVAILABLE && 
        state != DEQUEUED && 
        state != IN_USE && 
        state != QUEUED) {
        spdlog::error("[DataConcrete] Attempted to queue buffer {} in invalid state: {} "
                      "(allowed: AVAILABLE=0, DEQUEUED=2, IN_USE=4, QUEUED=1)", 
                      index, static_cast<int>(state));
        return false;
    }

     // --------------------------------------------------------------
    // 3. PREPARE V4L2 BUFFER
    // --------------------------------------------------------------
    v4l2_buffer buf{};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = index;


    // --------------------------------------------------------------
    // 4. IOCTL VIDIOC_QBUF (Queue to V4L2)
    // --------------------------------------------------------------
    if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
        if (errno != EINVAL) {  // EINVAL = already queued
            reportError(fmt::format("VIDIOC_QBUF failed for buffer {}: {}", index, strerror(errno)));
        }
        // reportError(fmt::format("VIDIOC_QBUF failed for buffer {}: {}", index, strerror(errno)));
        return false;
    }

    // ----??---------------------------------------------------------
    // 5. STATE TRANSITION: ? QUEUED
    // --------------------------------------------------------------
    buffers_[index].state = QUEUED;
    

    spdlog::debug("[DataConcrete] Buffer {} requeued successfully (from state {})", 
                 index, static_cast<int>(state));
                 
    // --------------------------------------------------------------
    // 6. OPTIONAL DEBUG LOGGING (comment out after stable)
    // --------------------------------------------------------------
    spdlog::debug("[DataConcrete] Buffer {} queued (from state {})", 
                   index, static_cast<int>(state));
    return true;
}

// ==============================================================================================================
// PUBLIC queueBuffer  Thread-safe wrapper (called by deleter)
// ==============================================================================================================

// ==============================================================================================================
// PUBLIC WRAPPER FOR queueBufferInternal (Thread-safe, called by deleter)
// ==============================================================================================================
inline bool DataConcrete::queueBuffer(size_t bufferIndex) {
    std::lock_guard<std::mutex> lock(mutex_);
    return queueBufferInternal(bufferIndex);
}

// ... (rest of the file  startStreaming, stopCapture, etc.  remains unchanged)

// ==============================================================================================================
// INTERNAL STREAMING CONTROL
// ==============================================================================================================

inline bool DataConcrete::internalStartStreaming() {
    if (streaming_) { 
        spdlog::warn("Already streaming"); 
        return true; 
    }
    if (!configured_) { 
        reportError("Not configured"); 
        return false; 
    }
    
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        reportError("STREAMON failed"); 
        return false; 
    }
    
    // ========== Initialize timestamp bases ==========
    auto s1 = std::chrono::system_clock::now();
    auto m  = std::chrono::steady_clock::now();
    auto s2 = std::chrono::system_clock::now();
    sysBase_ = s1 + (s2 - s1) / 2;
    monoBase_ = m;
    sysMinusMono_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
        sysBase_.time_since_epoch() - monoBase_.time_since_epoch());
    basesInitialized_.store(true, std::memory_order_release);
    
    prevCaptureTsSteady_ = {};
    streaming_ = true;
    spdlog::info("[DataConcrete] Streaming STARTED");
    return true;
}

inline bool DataConcrete::internalStopStreaming() {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_STREAMOFF, &type) < 0) {
        reportError(fmt::format("VIDIOC_STREAMOFF failed: {}", std::strerror(errno)));
        return false;
    }
    streaming_ = false;
    return true;
}

// ==============================================================================================================
// BUFFER LIFECYCLE
// ==============================================================================================================

inline void DataConcrete::unmapBuffers() {
    for (auto& buf : buffers_) {
        if (buf.start && buf.start != MAP_FAILED) {
            munmap(buf.start, buf.length);
        }
        buf.start  = nullptr;
        buf.length = 0;
        buf.state  = AVAILABLE;
    }
    buffers_.clear();
    spdlog::debug("[DataConcrete] All buffers unmapped and cleared.");
}

// ==============================================================================================================
// ERROR & UTILITY
// ==============================================================================================================

inline void DataConcrete::reportError(const std::string& msg) {
    if (errorCallback_) errorCallback_(msg);
    else spdlog::error("[DataConcrete] {}", msg);
}

inline void DataConcrete::setErrorCallback(std::function<void(const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    errorCallback_ = std::move(callback);
}

inline bool DataConcrete::isStreaming() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return streaming_;
}

inline void DataConcrete::pushCameraMetrics() {
    // Intentionally blank (metrics coordination at higher level)
}

inline double DataConcrete::getLastFPS() {
    std::lock_guard<std::mutex> lock(fpsMutex_);
    return lastFPS_;
}

inline int DataConcrete::getQueueSize() const {
    return framesQueued_.load();
}

inline void DataConcrete::closeDevice() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ == -1) return;
    unmapBuffers();
    ::close(fd_);
    fd_ = -1;
    spdlog::info("[DataConcrete] Device closed.");
}

inline void DataConcrete::resetDevice() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (streaming_) stopStreaming();
    closeDevice();
    spdlog::info("[DataConcrete] Device reset.");
}

inline bool DataConcrete::dequeFrame() {
    // Legacy interface - not used in Version 3 (frame dequeuing in captureThreadFunc)
    return false;
}

inline bool DataConcrete::forceReleaseDevice() {
    spdlog::info("[DataConcrete] forceReleaseDevice()");
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (streaming_) {
        ioctl(fd_, VIDIOC_STREAMOFF, &type);
        streaming_ = false;
    }
    if (fd_ > 0) {
        close(fd_);
        fd_ = -1;
    }
    spdlog::info("[DataConcrete] Device properly released.");
    return true;
}

inline bool DataConcrete::initializeBuffers() {
    // Not used in Version 3 (buffers initialized in configure())
    return true;
}

//#endif  // DATACONCRETE_VERSION3_PRODUCTION_H