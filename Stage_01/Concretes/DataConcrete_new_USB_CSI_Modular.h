
//===========================================================================================================
// DataConcrete_new_USB_CSI_Modular.h
// Jetson Nano modular camera capture path for the ERL framework
//
// Purpose:
//   - Keep one DataConcrete class that can use either:
//       1) USB V4L2 YUYV camera, for legacy /dev/video1-style USB capture.
//       2) CSI IMX219 RG10 Bayer camera, for /dev/video0 direct V4L2 capture.
//   - Auto-detect the camera backend from the V4L2 pixel formats exposed by the selected device.
//   - Preserve the existing ERL framework interface:
//       SharedQueue<std::shared_ptr<ZeroCopyFrameData>>
//       ISystemMetricsAggregator::beginFrame(...)
//   - Preserve the algorithm/display contract: downstream frames are always YUYV-like 8-bit frames.
//
// Backend behaviour:
//   - USB_YUYV:
//       Captures configured width/height/fps directly as YUYV and keeps zero-copy mmap lifetime management.
//   - CSI_RG10_TO_YUYV:
//       Captures IMX219 direct V4L2 RG10 at 1280x720@60 FPS and converts/downscales to the configured
//       output size, normally 320x240, using YUYV-like grayscale layout.
//
// Why conversion is needed for CSI:
//   Your IMX219 /dev/video0 reports RG10 only through direct V4L2:
//       3264x2464@21, 3264x1848@28, 1920x1080@30, 1640x1232@30, 1280x720@60.
//   AlgorithmConcrete currently expects YUYV-style frames:
//       width * height * 2 bytes, luminance at even indices, chroma = 128.
//   Therefore CSI RG10 is converted to YUYV-like grayscale before being pushed to AlgorithmConcrete/SdlDisplay.
//===========================================================================================================
#pragma once

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

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <cstdlib>
#include <stdexcept>
#include <linux/videodev2.h>
#include <memory>
#include <mutex>
#include <spdlog/spdlog.h>
#include <string>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <fmt/format.h>

class DataConcrete : public IData {
public:
    DataConcrete(const CameraConfig& config,
                 std::shared_ptr<ThreadManager> tm,
                 std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> algoQueue,
                 std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> displayOrigQueue,
                 std::shared_ptr<ISystemMetricsAggregator> aggregator);

    ~DataConcrete() override;

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
    enum BufferState {
        AVAILABLE = 0,
        QUEUED    = 1,
        DEQUEUED  = 2,
        PROCESSING = 3,
        IN_USE    = 4
    };

    enum class CameraBackend {
        AUTO = 0,
        USB_YUYV,
        CSI_RG10_TO_YUYV
    };

    struct Buffer {
        void* start = nullptr;
        size_t length = 0;
        BufferState state = AVAILABLE;
        std::chrono::steady_clock::time_point captureTime;
    };

    struct BufferDeleter {
        DataConcrete* owner = nullptr;
        size_t index = 0;

        void operator()(void*) const {
            if (owner) {
                owner->queueBuffer(index);
            }
        }
    };

    bool internalStartStreaming();
    bool internalStopStreaming();
    void captureThreadFunc();
    bool queueBufferInternal(size_t index);
    bool initializeBuffers();
    void unmapBuffers();
    void reportError(const std::string& msg);
    bool forceReleaseDevice();

    bool deviceSupportsPixelFormat(uint32_t pixFmt) const;
    bool convertRG10ToYUYV(const void* src, size_t srcBytes, std::vector<uint8_t>& dst) const;
    void applyUsbCameraTuning();
    std::string backendName() const;

    static std::string fourccToString(uint32_t fmt) {
        char f[5] = {
            static_cast<char>(fmt & 0xFF),
            static_cast<char>((fmt >> 8) & 0xFF),
            static_cast<char>((fmt >> 16) & 0xFF),
            static_cast<char>((fmt >> 24) & 0xFF),
            0
        };
        return std::string(f);
    }

private:
    std::shared_ptr<ISystemMetricsAggregator> metricAggregator_;

    int fd_ = -1;
    std::atomic<bool> streaming_{false};
    bool configured_ = false;
    std::string devicePath_;
    CameraConfig cameraConfig_{};              // Downstream/output frame configuration.
    std::vector<Buffer> buffers_;

    mutable std::mutex mutex_;
    mutable std::mutex fpsMutex_;

    std::function<void(const std::string&)> errorCallback_;

    int wakeupFd_ = -1;
    std::shared_ptr<ThreadManager> tm_;
    std::atomic<bool> running_{false};
    std::atomic<bool> capturePaused_{false};
    std::atomic<uint64_t> frameCounter_{0};

    std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> algoQueue_;
    std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> displayOrigQueue_;

    mutable double lastFPS_ = 0.0;
    mutable std::atomic<int> framesDequeued_{0};
    mutable std::chrono::steady_clock::time_point lastUpdateTime_;
    std::atomic<int> framesQueued_{0};

    std::chrono::steady_clock::time_point monoBase_;
    std::chrono::system_clock::time_point sysBase_;
    std::chrono::nanoseconds sysMinusMono_{};
    std::atomic<bool> basesInitialized_{false};
    std::chrono::steady_clock::time_point prevCaptureTsSteady_{};
    bool usesMonotonicTs_ = true;

    // Capture mode negotiated with V4L2.
    uint32_t acceptedPixFmt_ = 0;
    uint32_t captureWidth_ = 0;
    uint32_t captureHeight_ = 0;
    uint32_t outputWidth_ = 0;
    uint32_t outputHeight_ = 0;
    uint32_t bytesPerLine_ = 0;
    uint32_t sizeImage_ = 0;
    bool csiRg10Mode_ = false;
    CameraBackend backend_ = CameraBackend::AUTO;
};

//===========================================================================================================
// Constructor / Destructor
//===========================================================================================================
inline DataConcrete::DataConcrete(
    const CameraConfig& config,
    std::shared_ptr<ThreadManager> tm,
    std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> algoQueue,
    std::shared_ptr<SharedQueue<std::shared_ptr<ZeroCopyFrameData>>> displayOrigQueue,
    std::shared_ptr<ISystemMetricsAggregator> aggregator)
    : metricAggregator_(aggregator),
      cameraConfig_(config),
      tm_(tm),
      algoQueue_(algoQueue),
      displayOrigQueue_(displayOrigQueue)
{
    if (!tm_) {
        throw std::runtime_error("[DataConcrete] ThreadManager is null");
    }

    wakeupFd_ = eventfd(0, EFD_NONBLOCK);
    if (wakeupFd_ == -1) {
        throw std::runtime_error("[DataConcrete] eventfd() failed");
    }

    lastUpdateTime_ = std::chrono::steady_clock::now();
    spdlog::info("[DataConcrete] Constructed modular USB/CSI DataConcrete");
}

inline DataConcrete::~DataConcrete() {
    if (running_) {
        stopCapture();
    }
    if (streaming_) {
        stopStreaming();
    }
    if (fd_ >= 0) {
        closeDevice();
    }
    if (wakeupFd_ != -1) {
        ::close(wakeupFd_);
        wakeupFd_ = -1;
    }
    spdlog::debug("[DataConcrete] Destructor complete");
}

//===========================================================================================================
// Device open / configure
//===========================================================================================================
inline bool DataConcrete::openDevice(const std::string& path) {
    try {
        std::lock_guard<std::mutex> lock(mutex_);

        if (fd_ != -1) {
            unmapBuffers();
            ::close(fd_);
            fd_ = -1;
        }

        spdlog::info("[DataConcrete] Opening camera device: {}", path);
        fd_ = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd_ < 0) {
            reportError(fmt::format("Failed to open {}: {}", path, std::strerror(errno)));
            return false;
        }

        devicePath_ = path;
        spdlog::info("[DataConcrete] Device opened successfully: {} fd={}", path, fd_);
        return true;
    } catch (const std::exception& e) {
        reportError(fmt::format("Exception in openDevice: {}", e.what()));
        return false;
    }
}

inline bool DataConcrete::deviceSupportsPixelFormat(uint32_t pixFmt) const {
    if (fd_ < 0) {
        return false;
    }

    v4l2_fmtdesc desc{};
    desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    for (desc.index = 0; ioctl(fd_, VIDIOC_ENUM_FMT, &desc) == 0; ++desc.index) {
        if (desc.pixelformat == pixFmt) {
            return true;
        }
    }

    return false;
}

inline bool DataConcrete::configure(const CameraConfig& config) {
    try {
        std::lock_guard<std::mutex> lock(mutex_);

        if (fd_ < 0) {
            reportError("Device not opened");
            return false;
        }

        if (streaming_) {
            internalStopStreaming();
        }
        if (!buffers_.empty()) {
            unmapBuffers();
        }

        if (!config.validate()) {
            reportError("Invalid CameraConfig");
            return false;
        }

        CameraConfig cfg = config;
        if (cfg.width <= 0)  cfg.width = 320;
        if (cfg.height <= 0) cfg.height = 240;
        if (cfg.fps < 30)    cfg.fps = 30;

        // Your CSI IMX219 reports RG10 only via direct V4L2:
        //   1920x1080@30, 1640x1232@30, 1280x720@60, ...
        // Use RG10 when available; otherwise preserve the legacy YUYV path.
        const uint32_t kRG10 = v4l2_fourcc('R', 'G', '1', '0');
        const bool supportsRG10 = deviceSupportsPixelFormat(kRG10);
        const bool supportsYUYV = deviceSupportsPixelFormat(V4L2_PIX_FMT_YUYV);

        csiRg10Mode_ = supportsRG10;
        backend_ = supportsRG10 ? CameraBackend::CSI_RG10_TO_YUYV
                                : (supportsYUYV ? CameraBackend::USB_YUYV : CameraBackend::AUTO);
        outputWidth_ = static_cast<uint32_t>(cfg.width);
        outputHeight_ = static_cast<uint32_t>(cfg.height);

        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;

        if (csiRg10Mode_) {
            // Highest direct V4L2 mode exposed by your IMX219 for high-rate capture.
            // The framework still receives outputWidth_ x outputHeight_ YUYV-like frames.
            captureWidth_ = 1280;
            captureHeight_ = 720;

            // IMX219 direct V4L2 exposes 1280x720 as a 60 FPS mode.
            // Force 60 here so old USB config files using 30 FPS do not
            // silently keep the capture-limited behaviour.
            cfg.fps = 60;

            fmt.fmt.pix.width = captureWidth_;
            fmt.fmt.pix.height = captureHeight_;
            fmt.fmt.pix.pixelformat = kRG10;

            spdlog::info(
                "[DataConcrete] CSI RG10 mode selected: capture={}x{}@{} -> output={}x{} YUYV-like grayscale",
                captureWidth_, captureHeight_, cfg.fps, outputWidth_, outputHeight_
            );
        } else if (supportsYUYV) {
            applyUsbCameraTuning();
            captureWidth_ = outputWidth_;
            captureHeight_ = outputHeight_;
            fmt.fmt.pix.width = captureWidth_;
            fmt.fmt.pix.height = captureHeight_;
            fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;

            spdlog::info(
                "[DataConcrete] Legacy YUYV mode selected: {}x{}@{}",
                captureWidth_, captureHeight_, cfg.fps
            );
        } else {
            reportError("Camera supports neither RG10 CSI nor YUYV legacy capture. Use Argus/GStreamer capture path.");
            return false;
        }

        if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
            reportError(fmt::format("VIDIOC_S_FMT failed for {}: {}", fourccToString(fmt.fmt.pix.pixelformat), std::strerror(errno)));
            return false;
        }

        // Read back the accepted format. Drivers may adjust width/height/stride.
        v4l2_format accepted{};
        accepted.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd_, VIDIOC_G_FMT, &accepted) < 0) {
            reportError(fmt::format("VIDIOC_G_FMT failed: {}", std::strerror(errno)));
            return false;
        }

        acceptedPixFmt_ = accepted.fmt.pix.pixelformat;
        captureWidth_ = accepted.fmt.pix.width;
        captureHeight_ = accepted.fmt.pix.height;
        bytesPerLine_ = accepted.fmt.pix.bytesperline;
        sizeImage_ = accepted.fmt.pix.sizeimage;
        csiRg10Mode_ = (acceptedPixFmt_ == kRG10);
        backend_ = csiRg10Mode_ ? CameraBackend::CSI_RG10_TO_YUYV : CameraBackend::USB_YUYV;

        if (!csiRg10Mode_ && acceptedPixFmt_ != V4L2_PIX_FMT_YUYV) {
            reportError(fmt::format(
                "Accepted pixel format is {}, but this DataConcrete supports only RG10 and YUYV.",
                fourccToString(acceptedPixFmt_)
            ));
            return false;
        }

        spdlog::info(
            "[DataConcrete] Driver accepted format: {}x{} {} bytesperline={} sizeimage={}",
            captureWidth_, captureHeight_, fourccToString(acceptedPixFmt_), bytesPerLine_, sizeImage_
        );

        v4l2_streamparm parm{};
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = static_cast<uint32_t>(cfg.fps);

        if (ioctl(fd_, VIDIOC_S_PARM, &parm) < 0) {
            spdlog::warn("[DataConcrete] VIDIOC_S_PARM failed or ignored: {}", std::strerror(errno));
        }

        v4l2_streamparm checkParm{};
        checkParm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd_, VIDIOC_G_PARM, &checkParm) == 0) {
            const auto& tpf = checkParm.parm.capture.timeperframe;
            double acceptedFps = 0.0;
            if (tpf.numerator > 0) {
                acceptedFps = static_cast<double>(tpf.denominator) / static_cast<double>(tpf.numerator);
            }
            if (acceptedFps > 0.0) {
                cfg.fps = static_cast<int>(acceptedFps + 0.5);
            }
            spdlog::info(
                "[DataConcrete] Driver accepted FPS={:.2f} timeperframe={}/{}",
                acceptedFps, tpf.numerator, tpf.denominator
            );
        } else {
            spdlog::warn("[DataConcrete] VIDIOC_G_PARM failed: {}", std::strerror(errno));
        }

        const uint32_t requestedBuffers = std::max(4u, static_cast<uint32_t>(std::max(4, config.numBuffers)));
        v4l2_requestbuffers req{};
        req.count = requestedBuffers;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
            reportError(fmt::format("VIDIOC_REQBUFS failed: {}", std::strerror(errno)));
            return false;
        }

        buffers_.resize(req.count);
        for (uint32_t i = 0; i < req.count; ++i) {
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;

            if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
                reportError(fmt::format("VIDIOC_QUERYBUF failed for buffer {}: {}", i, std::strerror(errno)));
                return false;
            }

            buffers_[i].length = buf.length;
            buffers_[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);
            if (buffers_[i].start == MAP_FAILED) {
                buffers_[i].start = nullptr;
                reportError(fmt::format("mmap failed for buffer {}: {}", i, std::strerror(errno)));
                return false;
            }

            buffers_[i].state = AVAILABLE;
            if (!queueBufferInternal(i)) {
                reportError(fmt::format("Failed to queue initial buffer {}", i));
                return false;
            }
        }

        cfg.width = static_cast<int>(outputWidth_);
        cfg.height = static_cast<int>(outputHeight_);
        cfg.numBuffers = static_cast<int>(req.count);
        cameraConfig_ = cfg;
        configured_ = true;

        spdlog::info(
            "[DataConcrete] CONFIGURED: backend={} device={} capture={}x{} {} -> output={}x{} YUYV-like @ {} FPS, buffers={}",
            backendName(), devicePath_, captureWidth_, captureHeight_, fourccToString(acceptedPixFmt_),
            outputWidth_, outputHeight_, cameraConfig_.fps, req.count
        );

        return true;
    } catch (const std::exception& e) {
        reportError(fmt::format("configure() exception: {}", e.what()));
        return false;
    }
}

//===========================================================================================================
// Streaming / capture lifecycle
//===========================================================================================================
inline bool DataConcrete::startStreaming() {
    std::lock_guard<std::mutex> lock(mutex_);
    return internalStartStreaming();
}

inline bool DataConcrete::stopStreaming() {
    std::lock_guard<std::mutex> lock(mutex_);
    return internalStopStreaming();
}

inline bool DataConcrete::startCapture() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (running_) {
        return true;
    }

    if (!configured_) {
        reportError("Cannot start capture: camera not configured");
        return false;
    }

    if (!internalStartStreaming()) {
        return false;
    }

    running_ = true;
    tm_->addThread(Component::Camera, std::thread(&DataConcrete::captureThreadFunc, this));
    spdlog::info("[DataConcrete] Capture thread launched");
    return true;
}

inline bool DataConcrete::stopCapture() {
    if (!running_.exchange(false)) {
        return true;
    }

    spdlog::info("[DataConcrete] Stopping capture thread");

    if (wakeupFd_ != -1) {
        uint64_t one = 1;
        (void)::write(wakeupFd_, &one, sizeof(one));
    }

    if (tm_) {
        tm_->joinThreadsFor(Component::Camera);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        internalStopStreaming();
    }

    if (metricAggregator_) {
        metricAggregator_->forceFlushBatch();
    }

    spdlog::info("[DataConcrete] Camera capture stopped");
    return true;
}

inline void DataConcrete::pauseCapture() {
    capturePaused_ = true;
    spdlog::info("[DataConcrete] Capture paused");
}

inline void DataConcrete::resumeCapture() {
    capturePaused_ = false;
    spdlog::info("[DataConcrete] Capture resumed");
}

inline bool DataConcrete::internalStartStreaming() {
    if (streaming_) {
        return true;
    }

    if (!configured_ || buffers_.empty()) {
        reportError("Cannot STREAMON: camera not configured or no buffers mapped");
        return false;
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        reportError(fmt::format("VIDIOC_STREAMON failed: {}", std::strerror(errno)));
        return false;
    }

    auto s1 = std::chrono::system_clock::now();
    auto m = std::chrono::steady_clock::now();
    auto s2 = std::chrono::system_clock::now();
    sysBase_ = s1 + (s2 - s1) / 2;
    monoBase_ = m;
    sysMinusMono_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
        sysBase_.time_since_epoch() - monoBase_.time_since_epoch()
    );
    basesInitialized_.store(true, std::memory_order_release);

    prevCaptureTsSteady_ = {};
    frameCounter_.store(0, std::memory_order_relaxed);
    framesDequeued_.store(0, std::memory_order_relaxed);
    framesQueued_.store(0, std::memory_order_relaxed);
    streaming_ = true;

    spdlog::info("[DataConcrete] Streaming STARTED");
    return true;
}

inline bool DataConcrete::internalStopStreaming() {
    if (!streaming_) {
        return true;
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (fd_ >= 0 && ioctl(fd_, VIDIOC_STREAMOFF, &type) < 0) {
        spdlog::warn("[DataConcrete] VIDIOC_STREAMOFF failed: {}", std::strerror(errno));
    }

    streaming_ = false;
    return true;
}

//===========================================================================================================
// Backend utilities
//===========================================================================================================
inline std::string DataConcrete::backendName() const {
    switch (backend_) {
        case CameraBackend::USB_YUYV: return "USB_YUYV";
        case CameraBackend::CSI_RG10_TO_YUYV: return "CSI_RG10_TO_YUYV";
        case CameraBackend::AUTO:
        default: return "AUTO";
    }
}

inline void DataConcrete::applyUsbCameraTuning() {
    // USB webcam controls are best-effort. CSI IMX219 direct V4L2 RG10 does not use these UVC controls.
    if (devicePath_.empty()) {
        return;
    }

    auto runCtrl = [this](const std::string& ctrl) {
        const std::string cmd = fmt::format("v4l2-ctl -d {} --set-ctrl={} 2>/dev/null || true", devicePath_, ctrl);
        std::system(cmd.c_str());
    };

    spdlog::info("[DataConcrete] Applying best-effort USB camera tuning on {}", devicePath_);
    runCtrl("exposure_auto=1");
    runCtrl("exposure_auto_priority=0");
    runCtrl("absolute_exposure=130");
    runCtrl("focus_auto=0");
}

//===========================================================================================================
// RG10 Bayer -> YUYV-like grayscale conversion
//===========================================================================================================
inline bool DataConcrete::convertRG10ToYUYV(const void* src, size_t srcBytes, std::vector<uint8_t>& dst) const {
    if (!src || outputWidth_ == 0 || outputHeight_ == 0 || captureWidth_ == 0 || captureHeight_ == 0) {
        return false;
    }

    const uint32_t outW = outputWidth_;
    const uint32_t outH = outputHeight_;
    const size_t outBytes = static_cast<size_t>(outW) * static_cast<size_t>(outH) * 2u;
    dst.resize(outBytes);

    const uint8_t* rawBytes = static_cast<const uint8_t*>(src);
    const uint32_t strideBytes = (bytesPerLine_ > 0) ? bytesPerLine_ : (captureWidth_ * 2u);
    const uint32_t pixelsPerLine = std::max(1u, strideBytes / 2u);

    // Nearest-neighbour downsample from Bayer RG10 to luma-only YUYV-like output.
    // RG10 is treated as unpacked 16-bit little-endian values with 10 useful bits.
    for (uint32_t y = 0; y < outH; ++y) {
        const uint32_t srcY = std::min(captureHeight_ - 1u,
                                       static_cast<uint32_t>((static_cast<uint64_t>(y) * captureHeight_) / outH));

        for (uint32_t x = 0; x < outW; ++x) {
            const uint32_t srcX = std::min(captureWidth_ - 1u,
                                           static_cast<uint32_t>((static_cast<uint64_t>(x) * captureWidth_) / outW));
            const size_t rawIndex = static_cast<size_t>(srcY) * pixelsPerLine + srcX;
            const size_t byteIndex = rawIndex * 2u;

            uint8_t y8 = 0;
            if (byteIndex + 1u < srcBytes) {
                uint16_t v = 0;
                std::memcpy(&v, rawBytes + byteIndex, sizeof(uint16_t));
                // Standard unpacked 10-bit Bayer: useful bits are commonly in bits [9:0].
                // Convert 10-bit intensity to 8-bit luma.
                y8 = static_cast<uint8_t>((v & 0x03FFu) >> 2);
            }

            const size_t outIndex = (static_cast<size_t>(y) * outW + x) * 2u;
            dst[outIndex] = y8;
            dst[outIndex + 1u] = 128u;
        }
    }

    return true;
}

//===========================================================================================================
// Capture thread
//===========================================================================================================
inline void DataConcrete::captureThreadFunc() {
    spdlog::info(
        "[DataConcrete] Capture thread STARTED | backend={} device={} capture={}x{} {} -> output={}x{}",
        backendName(), devicePath_, captureWidth_, captureHeight_, fourccToString(acceptedPixFmt_), outputWidth_, outputHeight_
    );

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

        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 5000;

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
            break;
        }

        if (!FD_ISSET(fd_, &fds)) {
            continue;
        }

        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            reportError(fmt::format("VIDIOC_DQBUF failed: {}", std::strerror(errno)));
            break;
        }

        const size_t bufferIndex = static_cast<size_t>(buf.index);
        if (bufferIndex >= buffers_.size()) {
            spdlog::error("[DataConcrete] Invalid V4L2 buffer index {}", bufferIndex);
            continue;
        }

        std::chrono::steady_clock::time_point capMono;
        std::chrono::system_clock::time_point capSys;
        const bool haveDriverTs = (buf.timestamp.tv_sec != 0 || buf.timestamp.tv_usec != 0);

        if (usesMonotonicTs_ && haveDriverTs && basesInitialized_.load(std::memory_order_acquire)) {
            const auto monoNs = std::chrono::seconds(buf.timestamp.tv_sec) +
                                std::chrono::microseconds(buf.timestamp.tv_usec);
            capMono = std::chrono::steady_clock::time_point(
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(monoNs)
            );
            capSys = std::chrono::system_clock::time_point(capMono.time_since_epoch() + sysMinusMono_);
        } else {
            capMono = std::chrono::steady_clock::now();
            capSys = std::chrono::system_clock::now();
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            buffers_[bufferIndex].state = IN_USE;
            buffers_[bufferIndex].captureTime = capMono;
        }

        if (buf.bytesused == 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            queueBufferInternal(bufferIndex);
            continue;
        }

        const uint64_t frameId = frameCounter_.fetch_add(1, std::memory_order_relaxed);
        framesDequeued_.fetch_add(1, std::memory_order_relaxed);

        double fpsMeasured = static_cast<double>(std::max(1, cameraConfig_.fps));
        if (prevCaptureTsSteady_.time_since_epoch().count() != 0) {
            const double dtMs = std::chrono::duration<double, std::milli>(capMono - prevCaptureTsSteady_).count();
            if (dtMs > 0.0) {
                fpsMeasured = 1000.0 / dtMs;
            }
        }
        prevCaptureTsSteady_ = capMono;

        {
            std::lock_guard<std::mutex> fpsLock(fpsMutex_);
            lastFPS_ = fpsMeasured;
        }

        std::shared_ptr<ZeroCopyFrameData> frame;
        size_t downstreamBytes = 0;
        bool frameValid = false;

        if (csiRg10Mode_) {
            auto converted = std::make_shared<std::vector<uint8_t>>();
            if (!convertRG10ToYUYV(buffers_[bufferIndex].start, buf.bytesused, *converted)) {
                spdlog::warn("[DataConcrete] RG10 conversion failed for frame {}", frameId);
                std::lock_guard<std::mutex> lock(mutex_);
                queueBufferInternal(bufferIndex);
                continue;
            }

            // Raw V4L2 RG10 buffer can be returned immediately after conversion.
            {
                std::lock_guard<std::mutex> lock(mutex_);
                queueBufferInternal(bufferIndex);
            }

            downstreamBytes = converted->size();
            frame = std::make_shared<ZeroCopyFrameData>(
                converted,
                converted->data(),
                converted->size(),
                static_cast<int>(outputWidth_),
                static_cast<int>(outputHeight_),
                -1,
                frameId,
                capSys,
                capMono
            );
        } else {
            // Legacy YUYV path keeps true V4L2 zero-copy behaviour.
            auto guard = std::shared_ptr<void>(
                buffers_[bufferIndex].start,
                BufferDeleter{this, bufferIndex}
            );

            downstreamBytes = buf.bytesused;
            frame = std::make_shared<ZeroCopyFrameData>(
                guard,
                buffers_[bufferIndex].start,
                buf.bytesused,
                static_cast<int>(outputWidth_),
                static_cast<int>(outputHeight_),
                bufferIndex,
                frameId,
                capSys,
                capMono
            );
        }

        frameValid = (frame && frame->isValid());
        if (!frameValid) {
            spdlog::warn("[DataConcrete] Invalid frame {} dropped", frameId);
            continue;
        }

        if (metricAggregator_) {
            CameraStats camStats{};
            camStats.frameNumber = frameId;
            camStats.fps = fpsMeasured;
            camStats.frameWidth = outputWidth_;
            camStats.frameHeight = outputHeight_;
            camStats.frameSize = static_cast<uint64_t>(downstreamBytes);
            camStats.timestamp = capSys;

            if (camStats.isValid()) {
                metricAggregator_->beginFrame(frameId, camStats);
                metricAggregator_->overlayStats("camera", {
                    {"FPS", camStats.fps},
                    {"FrameWidth", static_cast<double>(camStats.frameWidth)},
                    {"FrameHeight", static_cast<double>(camStats.frameHeight)},
                    {"FrameSize", static_cast<double>(camStats.frameSize)},
                    {"RawCaptureWidth", static_cast<double>(captureWidth_)},
                    {"RawCaptureHeight", static_cast<double>(captureHeight_)},
                    {"RawBytesUsed", static_cast<double>(buf.bytesused)},
                    {"CSI_RG10_Mode", csiRg10Mode_ ? 1.0 : 0.0}
                }, capSys);
            } else {
                spdlog::warn(
                    "[DataConcrete] Invalid CameraStats frame={} fps={:.2f} size={}",
                    frameId, fpsMeasured, downstreamBytes
                );
            }
        }

        bool pushed = false;
        const size_t maxQueueSize = static_cast<size_t>(std::max(4, cameraConfig_.numBuffers)) * 4u;

        if (algoQueue_) {
            if (algoQueue_->size() < maxQueueSize) {
                algoQueue_->push(frame);
                pushed = true;
            } else {
                spdlog::warn("[DataConcrete] Algo queue full; dropping frame {}", frameId);
            }
        }

        if (displayOrigQueue_) {
            if (displayOrigQueue_->size() < maxQueueSize) {
                displayOrigQueue_->push(frame);
                pushed = true;
            } else {
                spdlog::warn("[DataConcrete] Display queue full; dropping frame {}", frameId);
            }
        }

        if (pushed) {
            framesQueued_.fetch_add(1, std::memory_order_relaxed);
        }

        if ((frameId % 120u) == 0u) {
            spdlog::info(
                "[DataConcrete][{}] frame={} fps={:.2f} raw={}x{} {} bytesUsed={} output={}x{} bytes={} queued={}",
                backendName(), frameId, fpsMeasured, captureWidth_, captureHeight_, fourccToString(acceptedPixFmt_),
                buf.bytesused, outputWidth_, outputHeight_, downstreamBytes, pushed ? "yes" : "no"
            );
        }
    }

    spdlog::info("[DataConcrete] Capture thread EXITED CLEANLY");
}

//===========================================================================================================
// Buffer lifecycle
//===========================================================================================================
inline bool DataConcrete::queueBufferInternal(size_t index) {
    if (fd_ < 0 || index >= buffers_.size()) {
        return false;
    }

    BufferState state = buffers_[index].state;
    if (state == QUEUED) {
        return true;
    }

    if (state != AVAILABLE && state != DEQUEUED && state != IN_USE) {
        spdlog::error("[DataConcrete] queueBufferInternal({}) invalid state={}", index, static_cast<int>(state));
        return false;
    }

    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = static_cast<uint32_t>(index);

    if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
        reportError(fmt::format("VIDIOC_QBUF failed for buffer {}: {}", index, std::strerror(errno)));
        return false;
    }

    buffers_[index].state = QUEUED;
    return true;
}

inline bool DataConcrete::queueBuffer(size_t bufferIndex) {
    std::lock_guard<std::mutex> lock(mutex_);
    return queueBufferInternal(bufferIndex);
}

inline void DataConcrete::unmapBuffers() {
    for (auto& buf : buffers_) {
        if (buf.start && buf.start != MAP_FAILED) {
            munmap(buf.start, buf.length);
        }
        buf.start = nullptr;
        buf.length = 0;
        buf.state = AVAILABLE;
    }
    buffers_.clear();
}

//===========================================================================================================
// Utility / interface methods
//===========================================================================================================
inline void DataConcrete::reportError(const std::string& msg) {
    if (errorCallback_) {
        errorCallback_(msg);
    } else {
        spdlog::error("[DataConcrete] {}", msg);
    }
}

inline void DataConcrete::setErrorCallback(std::function<void(const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    errorCallback_ = std::move(callback);
}

inline bool DataConcrete::isStreaming() const {
    return streaming_.load(std::memory_order_relaxed);
}

inline void DataConcrete::pushCameraMetrics() {
    // Metrics are emitted per frame in captureThreadFunc().
}

inline double DataConcrete::getLastFPS() {
    std::lock_guard<std::mutex> lock(fpsMutex_);
    return lastFPS_;
}

inline int DataConcrete::getQueueSize() const {
    return framesQueued_.load(std::memory_order_relaxed);
}

inline void DataConcrete::closeDevice() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (streaming_) {
        internalStopStreaming();
    }

    unmapBuffers();

    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }

    configured_ = false;
    spdlog::info("[DataConcrete] Device closed");
}

inline void DataConcrete::resetDevice() {
    if (running_) {
        stopCapture();
    }
    closeDevice();
    spdlog::info("[DataConcrete] Device reset");
}

inline bool DataConcrete::dequeFrame() {
    // Legacy interface. The active implementation uses captureThreadFunc().
    return false;
}

inline bool DataConcrete::forceReleaseDevice() {
    if (running_) {
        stopCapture();
    }
    if (streaming_) {
        stopStreaming();
    }
    closeDevice();
    return true;
}

inline bool DataConcrete::initializeBuffers() {
    // Buffers are initialized in configure(). Kept for IData compatibility.
    return true;
}
