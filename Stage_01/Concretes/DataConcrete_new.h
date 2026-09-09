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
#include <cctype>   // [SEC-FIX] isalnum/tolower for V4L2 control-name normalization
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
#include <limits>
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
    bool setV4L2ControlByName(const std::string& name, int64_t value);
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

    // Last CSI conversion statistics, exported through CameraStats/overlay.
    mutable std::string lastRawBitAlignment_ = "UNKNOWN";
    mutable uint32_t lastRawSampleMin10_ = 0;
    mutable uint32_t lastRawSampleMax10_ = 0;
    mutable double lastRawSampleMean10_ = 0.0;
    mutable uint32_t lastLumaMin8_ = 0;
    mutable uint32_t lastLumaMax8_ = 0;
    mutable double lastLumaMean8_ = 0.0;

    //Helper Mode 4 - 
    //bool setV4L2ControlByName(const std::string& name, int32_t value);
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
//==================================================================================================

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
        if (cfg.numBuffers < 4) cfg.numBuffers = 4;
        if (cfg.numBuffers > 8) cfg.numBuffers = 8;

        // CSI IMX219 direct V4L2 exposes RG10 only. Your v4l2-ctl output shows:
        //   3264x2464@21, 3264x1848@28, 1920x1080@30, 1640x1232@30, 1280x720@60.
        // Therefore, the CSI raw capture MUST be 1280x720 RG10 to obtain the 60 FPS mode.
        // The framework downstream output remains cfg.width x cfg.height YUYV-like grayscale.
        const uint32_t kRG10 = v4l2_fourcc('R', 'G', '1', '0');
        const bool supportsRG10 = deviceSupportsPixelFormat(kRG10);
        const bool supportsYUYV = deviceSupportsPixelFormat(V4L2_PIX_FMT_YUYV);

        if (!supportsRG10 && !supportsYUYV) {
            reportError("Camera supports neither RG10 CSI nor YUYV legacy capture. Use Argus/GStreamer capture path.");
            return false;
        }

        backend_ = supportsRG10 ? CameraBackend::CSI_RG10_TO_YUYV : CameraBackend::USB_YUYV;
        csiRg10Mode_ = (backend_ == CameraBackend::CSI_RG10_TO_YUYV);

        outputWidth_  = static_cast<uint32_t>(cfg.width);
        outputHeight_ = static_cast<uint32_t>(cfg.height);

        uint32_t requestedCaptureWidth  = outputWidth_;
        uint32_t requestedCaptureHeight = outputHeight_;
        uint32_t requestedPixFmt = V4L2_PIX_FMT_YUYV;

        if (csiRg10Mode_) {
            requestedCaptureWidth  = 1280;
            requestedCaptureHeight = 720;
            requestedPixFmt = kRG10;
            cfg.fps = 60;  // Mode-derived FPS for IMX219 1280x720 RG10.

            spdlog::info(
                "[DataConcrete] CSI RG10 mode REQUESTED: capture={}x{}@{} -> output={}x{} YUYV-like grayscale",
                requestedCaptureWidth, requestedCaptureHeight, cfg.fps, outputWidth_, outputHeight_
            );
        } else {
            applyUsbCameraTuning();
            requestedCaptureWidth  = outputWidth_;
            requestedCaptureHeight = outputHeight_;
            requestedPixFmt = V4L2_PIX_FMT_YUYV;

            spdlog::info(
                "[DataConcrete] USB YUYV mode REQUESTED: capture={}x{}@{}",
                requestedCaptureWidth, requestedCaptureHeight, cfg.fps
            );
        }

        v4l2_format vfmt{};
        vfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        vfmt.fmt.pix.width       = requestedCaptureWidth;
        vfmt.fmt.pix.height      = requestedCaptureHeight;
        vfmt.fmt.pix.pixelformat = requestedPixFmt;
        // Jetson IMX219 accepts 1280x720 RG10 only after sensor_mode=4 is selected.
        // Use ANY so the driver can choose the correct field mode for the sensor mode.
        vfmt.fmt.pix.field       = V4L2_FIELD_ANY;

        // Helpful for CSI RG10: 10-bit Bayer is exposed by this driver as 16-bit unpacked samples.
        if (csiRg10Mode_) {
            vfmt.fmt.pix.bytesperline = requestedCaptureWidth * 2u;
            vfmt.fmt.pix.sizeimage    = requestedCaptureWidth * requestedCaptureHeight * 2u;

            // Critical Jetson IMX219 requirement. Your v4l2-ctl test proved that
            // S_FMT alone falls back to 3264x2464.  The driver accepts 1280x720
            // only when sensor_mode=4 is applied before VIDIOC_S_FMT.
            setV4L2ControlByName("bypass_mode", 0);
            setV4L2ControlByName("sensor_mode", 4);

            enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            (void)ioctl(fd_, VIDIOC_STREAMOFF, &type);

            v4l2_requestbuffers reqReset{};
            reqReset.count = 0;
            reqReset.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            reqReset.memory = V4L2_MEMORY_MMAP;
            (void)ioctl(fd_, VIDIOC_REQBUFS, &reqReset);

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (ioctl(fd_, VIDIOC_S_FMT, &vfmt) < 0) {
            reportError(::fmt::format(
                "VIDIOC_S_FMT failed for requested {}x{} {}: {}",
                requestedCaptureWidth,
                requestedCaptureHeight,
                fourccToString(requestedPixFmt),
                std::strerror(errno)
            ));
            return false;
        }

        // Read back the accepted format. Drivers may adjust width/height/stride.
        v4l2_format accepted{};
        accepted.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd_, VIDIOC_G_FMT, &accepted) < 0) {
            reportError(::fmt::format("VIDIOC_G_FMT failed after VIDIOC_S_FMT: {}", std::strerror(errno)));
            return false;
        }

        acceptedPixFmt_ = accepted.fmt.pix.pixelformat;
        captureWidth_   = accepted.fmt.pix.width;
        captureHeight_  = accepted.fmt.pix.height;
        bytesPerLine_   = accepted.fmt.pix.bytesperline;
        sizeImage_      = accepted.fmt.pix.sizeimage;

        spdlog::info(
            "[DataConcrete] Driver accepted format: {}x{} {} bytesperline={} sizeimage={}",
            captureWidth_, captureHeight_, fourccToString(acceptedPixFmt_), bytesPerLine_, sizeImage_
        );

        // STRICT CSI check: never silently continue with 3264x2464, because that is the 21 FPS mode
        // and it also breaks the expected raw-to-output conversion timing for your benchmark.
        if (csiRg10Mode_) {
            if (captureWidth_ != requestedCaptureWidth ||
                captureHeight_ != requestedCaptureHeight ||
                acceptedPixFmt_ != kRG10) {
                reportError(::fmt::format(
                    "CSI mode mismatch. Requested {}x{} RG10 for 60 FPS, but driver accepted {}x{} {}. "
                    "Refusing to continue. First verify direct streaming with: "
                    "v4l2-ctl -d {} --set-fmt-video=width=1280,height=720,pixelformat=RG10 "
                    "--stream-mmap --stream-count=300 --stream-to=/dev/null",
                    requestedCaptureWidth,
                    requestedCaptureHeight,
                    captureWidth_,
                    captureHeight_,
                    fourccToString(acceptedPixFmt_),
                    devicePath_
                ));
                return false;
            }

            if (bytesPerLine_ == 0) {
                bytesPerLine_ = captureWidth_ * 2u;
            }
            if (sizeImage_ == 0) {
                sizeImage_ = bytesPerLine_ * captureHeight_;
            }

            cfg.fps = 60;
            spdlog::info(
                "[DataConcrete] CSI RG10 STRICT configuration OK: capture={}x{} RG10 stride={} image={} -> output={}x{} YUYV-like @ 60 FPS",
                captureWidth_, captureHeight_, bytesPerLine_, sizeImage_, outputWidth_, outputHeight_
            );
        } else {
            csiRg10Mode_ = false;
            backend_ = CameraBackend::USB_YUYV;

            if (acceptedPixFmt_ != V4L2_PIX_FMT_YUYV) {
                reportError(::fmt::format(
                    "USB mode mismatch. Requested YUYV but driver accepted {}. This DataConcrete supports only USB YUYV or CSI RG10.",
                    fourccToString(acceptedPixFmt_)
                ));
                return false;
            }

            // USB/UVC cameras often support VIDIOC_S_PARM / G_PARM. CSI raw usually does not.
            v4l2_streamparm parm{};
            parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            parm.parm.capture.timeperframe.numerator = 1;
            //parm.parm.capture.timeperframe.denominator = static_cast<uint32_t>(std::max(1, cfg.fps));
            parm.parm.capture.timeperframe.denominator = 60;
            if (ioctl(fd_, VIDIOC_S_PARM, &parm) < 0)
            spdlog::warn("[DataConcrete] CSI VIDIOC_S_PARM(60) failed: {}", std::strerror(errno));

            if (ioctl(fd_, VIDIOC_S_PARM, &parm) < 0) {
                spdlog::warn("[DataConcrete] USB VIDIOC_S_PARM failed or ignored: {}", std::strerror(errno));
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
                    "[DataConcrete] USB driver accepted FPS={:.2f} timeperframe={}/{}",
                    acceptedFps, tpf.numerator, tpf.denominator
                );
            }

            v4l2_streamparm got{};
            got.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (ioctl(fd_, VIDIOC_G_PARM, &got) == 0 && got.parm.capture.timeperframe.numerator > 0) {
                double actual = double(got.parm.capture.timeperframe.denominator) /
                                double(got.parm.capture.timeperframe.numerator);
                cfg.fps = int(actual + 0.5);
                spdlog::info("[DataConcrete] CSI sensor accepted {:.1f} FPS", actual);
                if (actual < 55.0)
                    spdlog::warn("[DataConcrete] CSI delivering {:.1f} FPS, not 60 ? check sensor_mode", actual);
            } else {
                cfg.fps = 60;  // fall back to assumed only if read-back fails
            }

            outputWidth_ = captureWidth_;
            outputHeight_ = captureHeight_;
        }

        const uint32_t requestedBuffers = std::max(4u, static_cast<uint32_t>(cfg.numBuffers));
        v4l2_requestbuffers req{};
        req.count = requestedBuffers;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
            reportError(::fmt::format("VIDIOC_REQBUFS failed: {}", std::strerror(errno)));
            return false;
        }

        buffers_.resize(req.count);
        for (uint32_t i = 0; i < req.count; ++i) {
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;

            if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
                reportError(::fmt::format("VIDIOC_QUERYBUF failed for buffer {}: {}", i, std::strerror(errno)));
                return false;
            }

            buffers_[i].length = buf.length;
            buffers_[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);
            if (buffers_[i].start == MAP_FAILED) {
                buffers_[i].start = nullptr;
                reportError(::fmt::format("mmap failed for buffer {}: {}", i, std::strerror(errno)));
                return false;
            }

            buffers_[i].state = AVAILABLE;
            if (!queueBufferInternal(i)) {
                reportError(::fmt::format("Failed to queue initial buffer {}", i));
                return false;
            }
        }

        cfg.width = static_cast<int>(outputWidth_);
        cfg.height = static_cast<int>(outputHeight_);
        cfg.numBuffers = static_cast<int>(req.count);
        cameraConfig_ = cfg;
        configured_ = true;

        spdlog::info(
            "[DataConcrete] CONFIGURED: backend={} device={} capture={}x{} {} stride={} image={} -> output={}x{} YUYV-like @ {} FPS, buffers={}",
            backendName(), devicePath_, captureWidth_, captureHeight_, fourccToString(acceptedPixFmt_),
            bytesPerLine_, sizeImage_, outputWidth_, outputHeight_, cameraConfig_.fps, req.count
        );

        return true;
    } catch (const std::exception& e) {
        reportError(::fmt::format("configure() exception: {}", e.what()));
        return false;
    }
}


//===================================================================================================
// inline bool DataConcrete::configure(const CameraConfig& config) {
//     try {
//         std::lock_guard<std::mutex> lock(mutex_);

//         if (fd_ < 0) {
//             reportError("Device not opened");
//             return false;
//         }

//         if (streaming_) {
//             internalStopStreaming();
//         }
//         if (!buffers_.empty()) {
//             unmapBuffers();
//         }

//         if (!config.validate()) {
//             reportError("Invalid CameraConfig");
//             return false;
//         }

//         CameraConfig cfg = config;
//         if (cfg.width <= 0)  cfg.width = 320;
//         if (cfg.height <= 0) cfg.height = 240;
//         if (cfg.fps < 30)    cfg.fps = 30;
//         if (cfg.numBuffers < 4) cfg.numBuffers = 4;
//         if (cfg.numBuffers > 8) cfg.numBuffers = 8;

//         // CSI IMX219 direct V4L2 exposes RG10 only. Your v4l2-ctl output shows:
//         //   3264x2464@21, 3264x1848@28, 1920x1080@30, 1640x1232@30, 1280x720@60.
//         // Therefore, the CSI raw capture MUST be 1280x720 RG10 to obtain the 60 FPS mode.
//         // The framework downstream output remains cfg.width x cfg.height YUYV-like grayscale.
//         const uint32_t kRG10 = v4l2_fourcc('R', 'G', '1', '0');
//         const bool supportsRG10 = deviceSupportsPixelFormat(kRG10);
//         const bool supportsYUYV = deviceSupportsPixelFormat(V4L2_PIX_FMT_YUYV);

//         if (!supportsRG10 && !supportsYUYV) {
//             reportError("Camera supports neither RG10 CSI nor YUYV legacy capture. Use Argus/GStreamer capture path.");
//             return false;
//         }

//         backend_ = supportsRG10 ? CameraBackend::CSI_RG10_TO_YUYV : CameraBackend::USB_YUYV;
//         csiRg10Mode_ = (backend_ == CameraBackend::CSI_RG10_TO_YUYV);

//         outputWidth_  = static_cast<uint32_t>(cfg.width);
//         outputHeight_ = static_cast<uint32_t>(cfg.height);

//         uint32_t requestedCaptureWidth  = outputWidth_;
//         uint32_t requestedCaptureHeight = outputHeight_;
//         uint32_t requestedPixFmt = V4L2_PIX_FMT_YUYV;

//         if (csiRg10Mode_) {
//             requestedCaptureWidth  = 1280;
//             requestedCaptureHeight = 720;
//             requestedPixFmt = kRG10;
//             cfg.fps = 60;  // Mode-derived FPS for IMX219 1280x720 RG10.

//             spdlog::info(
//                 "[DataConcrete] CSI RG10 mode REQUESTED: capture={}x{}@{} -> output={}x{} YUYV-like grayscale",
//                 requestedCaptureWidth, requestedCaptureHeight, cfg.fps, outputWidth_, outputHeight_
//             );
//         } else {
//             applyUsbCameraTuning();
//             requestedCaptureWidth  = outputWidth_;
//             requestedCaptureHeight = outputHeight_;
//             requestedPixFmt = V4L2_PIX_FMT_YUYV;

//             spdlog::info(
//                 "[DataConcrete] USB YUYV mode REQUESTED: capture={}x{}@{}",
//                 requestedCaptureWidth, requestedCaptureHeight, cfg.fps
//             );
//         }

//         v4l2_format vfmt{};
//         vfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
//         vfmt.fmt.pix.width       = requestedCaptureWidth;
//         vfmt.fmt.pix.height      = requestedCaptureHeight;
//         vfmt.fmt.pix.pixelformat = requestedPixFmt;
//         // Jetson IMX219 accepts 1280x720 RG10 only after sensor_mode=4 is selected.
//         // Use ANY so the driver can choose the correct field mode for the sensor mode.
//         vfmt.fmt.pix.field       = V4L2_FIELD_ANY;

//         // Helpful for CSI RG10: 10-bit Bayer is exposed by this driver as 16-bit unpacked samples.
//         if (csiRg10Mode_) {
//             vfmt.fmt.pix.bytesperline = requestedCaptureWidth * 2u;
//             vfmt.fmt.pix.sizeimage    = requestedCaptureWidth * requestedCaptureHeight * 2u;

//             // Critical Jetson IMX219 requirement. Your v4l2-ctl test proved that
//             // S_FMT alone falls back to 3264x2464.  The driver accepts 1280x720
//             // only when sensor_mode=4 is applied before VIDIOC_S_FMT.
//             setV4L2ControlByName("bypass_mode", 0);
//             setV4L2ControlByName("sensor_mode", 4);

//             enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
//             (void)ioctl(fd_, VIDIOC_STREAMOFF, &type);

//             v4l2_requestbuffers reqReset{};
//             reqReset.count = 0;
//             reqReset.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
//             reqReset.memory = V4L2_MEMORY_MMAP;
//             (void)ioctl(fd_, VIDIOC_REQBUFS, &reqReset);

//             std::this_thread::sleep_for(std::chrono::milliseconds(100));
//         }

//         if (ioctl(fd_, VIDIOC_S_FMT, &vfmt) < 0) {
//             reportError(::fmt::format(
//                 "VIDIOC_S_FMT failed for requested {}x{} {}: {}",
//                 requestedCaptureWidth,
//                 requestedCaptureHeight,
//                 fourccToString(requestedPixFmt),
//                 std::strerror(errno)
//             ));
//             return false;
//         }

//         // Read back the accepted format. Drivers may adjust width/height/stride.
//         v4l2_format accepted{};
//         accepted.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
//         if (ioctl(fd_, VIDIOC_G_FMT, &accepted) < 0) {
//             reportError(::fmt::format("VIDIOC_G_FMT failed after VIDIOC_S_FMT: {}", std::strerror(errno)));
//             return false;
//         }

//         acceptedPixFmt_ = accepted.fmt.pix.pixelformat;
//         captureWidth_   = accepted.fmt.pix.width;
//         captureHeight_  = accepted.fmt.pix.height;
//         bytesPerLine_   = accepted.fmt.pix.bytesperline;
//         sizeImage_      = accepted.fmt.pix.sizeimage;

//         spdlog::info(
//             "[DataConcrete] Driver accepted format: {}x{} {} bytesperline={} sizeimage={}",
//             captureWidth_, captureHeight_, fourccToString(acceptedPixFmt_), bytesPerLine_, sizeImage_
//         );

//         // STRICT CSI check: never silently continue with 3264x2464, because that is the 21 FPS mode
//         // and it also breaks the expected raw-to-output conversion timing for your benchmark.
//         if (csiRg10Mode_) {
//             if (captureWidth_ != requestedCaptureWidth ||
//                 captureHeight_ != requestedCaptureHeight ||
//                 acceptedPixFmt_ != kRG10) {
//                 reportError(::fmt::format(
//                     "CSI mode mismatch. Requested {}x{} RG10 for 60 FPS, but driver accepted {}x{} {}. "
//                     "Refusing to continue. First verify direct streaming with: "
//                     "v4l2-ctl -d {} --set-fmt-video=width=1280,height=720,pixelformat=RG10 "
//                     "--stream-mmap --stream-count=300 --stream-to=/dev/null",
//                     requestedCaptureWidth,
//                     requestedCaptureHeight,
//                     captureWidth_,
//                     captureHeight_,
//                     fourccToString(acceptedPixFmt_),
//                     devicePath_
//                 ));
//                 return false;
//             }

//             if (bytesPerLine_ == 0) {
//                 bytesPerLine_ = captureWidth_ * 2u;
//             }
//             if (sizeImage_ == 0) {
//                 sizeImage_ = bytesPerLine_ * captureHeight_;
//             }

//             cfg.fps = 60;
//             spdlog::info(
//                 "[DataConcrete] CSI RG10 STRICT configuration OK: capture={}x{} RG10 stride={} image={} -> output={}x{} YUYV-like @ 60 FPS",
//                 captureWidth_, captureHeight_, bytesPerLine_, sizeImage_, outputWidth_, outputHeight_
//             );
//         } else {
//             csiRg10Mode_ = false;
//             backend_ = CameraBackend::USB_YUYV;

//             if (acceptedPixFmt_ != V4L2_PIX_FMT_YUYV) {
//                 reportError(::fmt::format(
//                     "USB mode mismatch. Requested YUYV but driver accepted {}. This DataConcrete supports only USB YUYV or CSI RG10.",
//                     fourccToString(acceptedPixFmt_)
//                 ));
//                 return false;
//             }

//             // USB/UVC cameras often support VIDIOC_S_PARM / G_PARM. CSI raw usually does not.
//             v4l2_streamparm parm{};
//             parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
//             parm.parm.capture.timeperframe.numerator = 1;
//             parm.parm.capture.timeperframe.denominator = static_cast<uint32_t>(std::max(1, cfg.fps));

//             if (ioctl(fd_, VIDIOC_S_PARM, &parm) < 0) {
//                 spdlog::warn("[DataConcrete] USB VIDIOC_S_PARM failed or ignored: {}", std::strerror(errno));
//             }

//             v4l2_streamparm checkParm{};
//             checkParm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
//             if (ioctl(fd_, VIDIOC_G_PARM, &checkParm) == 0) {
//                 const auto& tpf = checkParm.parm.capture.timeperframe;
//                 double acceptedFps = 0.0;
//                 if (tpf.numerator > 0) {
//                     acceptedFps = static_cast<double>(tpf.denominator) / static_cast<double>(tpf.numerator);
//                 }
//                 if (acceptedFps > 0.0) {
//                     cfg.fps = static_cast<int>(acceptedFps + 0.5);
//                 }
//                 spdlog::info(
//                     "[DataConcrete] USB driver accepted FPS={:.2f} timeperframe={}/{}",
//                     acceptedFps, tpf.numerator, tpf.denominator
//                 );
//             }

//             outputWidth_ = captureWidth_;
//             outputHeight_ = captureHeight_;
//         }

//         const uint32_t requestedBuffers = std::max(4u, static_cast<uint32_t>(cfg.numBuffers));
//         v4l2_requestbuffers req{};
//         req.count = requestedBuffers;
//         req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
//         req.memory = V4L2_MEMORY_MMAP;

//         if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
//             reportError(::fmt::format("VIDIOC_REQBUFS failed: {}", std::strerror(errno)));
//             return false;
//         }

//         buffers_.resize(req.count);
//         for (uint32_t i = 0; i < req.count; ++i) {
//             v4l2_buffer buf{};
//             buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
//             buf.memory = V4L2_MEMORY_MMAP;
//             buf.index = i;

//             if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
//                 reportError(::fmt::format("VIDIOC_QUERYBUF failed for buffer {}: {}", i, std::strerror(errno)));
//                 return false;
//             }

//             buffers_[i].length = buf.length;
//             buffers_[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);
//             if (buffers_[i].start == MAP_FAILED) {
//                 buffers_[i].start = nullptr;
//                 reportError(::fmt::format("mmap failed for buffer {}: {}", i, std::strerror(errno)));
//                 return false;
//             }

//             buffers_[i].state = AVAILABLE;
//             if (!queueBufferInternal(i)) {
//                 reportError(::fmt::format("Failed to queue initial buffer {}", i));
//                 return false;
//             }
//         }

//         cfg.width = static_cast<int>(outputWidth_);
//         cfg.height = static_cast<int>(outputHeight_);
//         cfg.numBuffers = static_cast<int>(req.count);
//         cameraConfig_ = cfg;
//         configured_ = true;

//         spdlog::info(
//             "[DataConcrete] CONFIGURED: backend={} device={} capture={}x{} {} stride={} image={} -> output={}x{} YUYV-like @ {} FPS, buffers={}",
//             backendName(), devicePath_, captureWidth_, captureHeight_, fourccToString(acceptedPixFmt_),
//             bytesPerLine_, sizeImage_, outputWidth_, outputHeight_, cameraConfig_.fps, req.count
//         );

//         return true;
//     } catch (const std::exception& e) {
//         reportError(::fmt::format("configure() exception: {}", e.what()));
//         return false;
//     }
// }

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
    if (devicePath_.empty() || fd_ < 0) {
        return;
    }

    // [SEC-FIX] This used to build a shell command with the config-supplied
    // device path and run it via std::system():
    //     v4l2-ctl -d <devicePath_> --set-ctrl=... 2>/dev/null || true
    // Problems:
    //   1. Command injection: CameraConfig.device comes from JSON and was
    //      interpolated into a root shell (this process needs root for DVFS).
    //      A device string like "/dev/video0; <anything>" executes <anything>.
    //   2. PATH dependency on an external v4l2-ctl binary.
    //   3. "2>/dev/null || true" swallowed every failure - which hid the fact
    //      that "absolute_exposure" is not a valid v4l2-ctl control name (the
    //      UVC control is "exposure_absolute"), so absolute exposure was
    //      likely never actually applied.
    // Replaced with the native ioctl path already present in this file
    // (setV4L2ControlByName now normalizes names, so v4l2-ctl-style
    // snake_case matches the kernel's "Exposure, Auto" spelling).
    spdlog::info("[DataConcrete] Applying best-effort USB camera tuning on {}", devicePath_);
    setV4L2ControlByName("exposure_auto", 1);           // kernel: "Exposure, Auto" (1 = manual on UVC)
    setV4L2ControlByName("exposure_auto_priority", 0);  // kernel: "Exposure, Auto Priority"
    setV4L2ControlByName("exposure_absolute", 130);     // kernel: "Exposure (Absolute)"; old shell name was wrong
    setV4L2ControlByName("focus_auto", 0);              // kernel: "Focus, Auto"
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
    //dst.resize(outBytes);
    dst.assign(outBytes, 128u);

    const uint8_t* rawBytes = static_cast<const uint8_t*>(src);
    const uint32_t strideBytes = (bytesPerLine_ > 0) ? bytesPerLine_ : (captureWidth_ * 2u);
    const uint32_t pixelsPerLine = std::max(1u, strideBytes / 2u);

    // Nearest-neighbour downsample from Bayer RG10 to luma-only YUYV-like output.
    // RG10 is treated as unpacked 16-bit little-endian values with 10 useful bits.
  auto sampleAt = [&](uint32_t srcX, uint32_t srcY, bool msbAligned) -> uint16_t {
        srcX = std::min(captureWidth_ - 1u, srcX);
        srcY = std::min(captureHeight_ - 1u, srcY);
        const size_t rawIndex = static_cast<size_t>(srcY) * pixelsPerLine + srcX;
        const size_t byteIndex = rawIndex * 2u;
        if (byteIndex + 1u >= srcBytes) return 0;

        uint16_t v = 0;
        std::memcpy(&v, rawBytes + byteIndex, sizeof(uint16_t));

        // Jetson/IMX219 RG10 may arrive as 16-bit little-endian with the 10 valid bits
        // either LSB-aligned (bits 9:0) or MSB-aligned (bits 15:6), depending on driver path.
        return msbAligned ? static_cast<uint16_t>((v >> 6) & 0x03FFu)
                          : static_cast<uint16_t>(v & 0x03FFu);
    };

    // Auto-detect bit alignment from a small grid. A wrong LSB decode often produces near-zero
    // luma and causes both Original and Processed displays to look black.
    bool msbAligned = false;
    {
        uint64_t lsbSum = 0, msbSum = 0;
        uint16_t lsbMax = 0, msbMax = 0;
        uint32_t n = 0;

        const uint32_t stepX = std::max(1u, captureWidth_ / 32u);
        const uint32_t stepY = std::max(1u, captureHeight_ / 24u);
        for (uint32_t y = 0; y < captureHeight_; y += stepY) {
            for (uint32_t x = 0; x < captureWidth_; x += stepX) {
                const size_t rawIndex = static_cast<size_t>(y) * pixelsPerLine + x;
                const size_t byteIndex = rawIndex * 2u;
                if (byteIndex + 1u >= srcBytes) continue;
                uint16_t v = 0;
                std::memcpy(&v, rawBytes + byteIndex, sizeof(uint16_t));
                const uint16_t lsb = static_cast<uint16_t>(v & 0x03FFu);
                const uint16_t msb = static_cast<uint16_t>((v >> 6) & 0x03FFu);
                lsbSum += lsb; msbSum += msb;
                lsbMax = std::max(lsbMax, lsb);
                msbMax = std::max(msbMax, msb);
                ++n;
            }
        }

        const double lsbMean = n ? static_cast<double>(lsbSum) / n : 0.0;
        const double msbMean = n ? static_cast<double>(msbSum) / n : 0.0;

        if (cameraConfig_.rawBitAlignment == "MSB") {
            msbAligned = true;
        } else if (cameraConfig_.rawBitAlignment == "LSB") {
            msbAligned = false;
        } else {
            // Prefer the interpretation with substantially more signal.
            msbAligned = (msbMax > lsbMax * 2u && msbMean > lsbMean * 1.5);
        }

        lastRawBitAlignment_ = msbAligned ? "MSB" : "LSB";
    }

    // First pass: get raw 10-bit range over the exact downsampled pixels.
    uint16_t rawMin = 1023;
    uint16_t rawMax = 0;
    uint64_t rawSum = 0;
    uint32_t rawCount = 0;

    for (uint32_t y = 0; y < outH; ++y) {
        const uint32_t srcY = std::min(captureHeight_ - 1u,
                                       static_cast<uint32_t>((static_cast<uint64_t>(y) * captureHeight_) / outH));
        for (uint32_t x = 0; x < outW; ++x) {
            const uint32_t srcX = std::min(captureWidth_ - 1u,
                                           static_cast<uint32_t>((static_cast<uint64_t>(x) * captureWidth_) / outW));
            const uint16_t s10 = sampleAt(srcX, srcY, msbAligned);
            rawMin = std::min(rawMin, s10);
            rawMax = std::max(rawMax, s10);
            rawSum += s10;
            ++rawCount;
        }
    } // <--- This is the end of your First Pass loop

    // ========================================================================
    // PATCH 4: CAMERA LUMA GUARD (Noise Floor Protection)
    // Place it HERE to overwrite rawMin/rawMax before 'black' and 'white' use them.
    // ========================================================================
    if (cameraConfig_.autoLumaStretch) {
        int dynamic_range = rawMax - rawMin;
        
        // If the sensor range is narrower than 20 codes, it's just thermal noise 
        // in pitch blackness. Fallback to full 10-bit scale to avoid stretching static.
        if (dynamic_range < 20) { 
            rawMin = 0;
            rawMax = 1023; 
            spdlog::debug("[DataConcrete] Low dynamic range ({}) detected in dark frame. Hard-resetting to full 10-bit scale.", dynamic_range);
        }
    }
    // ========================================================================

    // int black = cameraConfig_.lumaBlackLevel >= 0 ? cameraConfig_.lumaBlackLevel : static_cast<int>(rawMin);
    // int white = cameraConfig_.lumaWhiteLevel >= 0 ? cameraConfig_.lumaWhiteLevel : static_cast<int>(rawMax);
    
    // if (white <= black + 4) {
    //     black = 0;
    //     white = 1023;
    // }

    int black = cameraConfig_.lumaBlackLevel >= 0 ? cameraConfig_.lumaBlackLevel : static_cast<int>(rawMin);
    int white = cameraConfig_.lumaWhiteLevel >= 0 ? cameraConfig_.lumaWhiteLevel : static_cast<int>(rawMax);

    // // Patch 4 ? Camera Luma Guard 
    // if (cameraConfig_.lumaBlackLevel < 0) {   // "Auto" mode requested
    //     const int rawRange = rawMax - rawMin;   // e.g., 66 - 63 = 3
    //     constexpr int MIN_MEANINGFUL_RANGE = 10; // adjust to your sensor's bit depth

    //     if (rawRange < MIN_MEANINGFUL_RANGE) {
    //         spdlog::warn("[Camera] Auto luma would stretch {} codes of noise to full range. "
    //                      "Forcing safe fallback black=0, white=1023.", rawRange);
    //         cameraConfig_.lumaBlackLevel = 0;
    //         cameraConfig_.lumaWhiteLevel = 1023;
    //     } else {
    //         cameraConfig_.lumaBlackLevel = rawMin;
    //         cameraConfig_.lumaWhiteLevel = rawMax;
    //     }
    // }


    // ================================================================
    // PATCH 4: CAMERA LUMA GUARD (Noise Floor Protection)
    // ================================================================
    // If the dynamic range is less than 20, it is sensor thermal noise 
    // in a dark room. Do not stretch it.
    if (white - black < 20) {
        black = 0;
        white = 1023;
        
        // Optional: Log it so you know when the camera is effectively "blind"
        // spdlog::debug("[DataConcrete] Noise floor detected (DR: {}). Falling back to absolute 10-bit scale.", (white - black));
    }
    // ================================================================

    uint32_t yMin = 255;
    uint32_t yMax = 0;
    uint64_t ySum = 0;

    // Second pass: write YUYV-like grayscale. Y is written in video range [16,235]
    // because SdlDisplayConcrete converts YUYV to RGB by subtracting 16 from luma.

    // Second pass: write YUYV-like grayscale...

    for (uint32_t y = 0; y < outH; ++y) {
        const uint32_t srcY = std::min(captureHeight_ - 1u,
                                       static_cast<uint32_t>((static_cast<uint64_t>(y) * captureHeight_) / outH));

        for (uint32_t x = 0; x < outW; ++x) {
            const uint32_t srcX = std::min(captureWidth_ - 1u,
                                           static_cast<uint32_t>((static_cast<uint64_t>(x) * captureWidth_) / outW));
            const size_t rawIndex = static_cast<size_t>(srcY) * pixelsPerLine + srcX;
            const size_t byteIndex = rawIndex * 2u;
            const uint16_t s10 = sampleAt(srcX, srcY, msbAligned);

            uint8_t y8 = 0;
            // if (byteIndex + 1u < srcBytes) {
            //     uint16_t v = 0;
            //     std::memcpy(&v, rawBytes + byteIndex, sizeof(uint16_t));
            //     // Standard unpacked 10-bit Bayer: useful bits are commonly in bits [9:0].
            //     // Convert 10-bit intensity to 8-bit luma.
            //     y8 = static_cast<uint8_t>((v & 0x03FFu) >> 2);
            if (cameraConfig_.autoLumaStretch) {
                const int clamped = std::max(black, std::min(white, static_cast<int>(s10)));
                y8 = static_cast<uint8_t>(((clamped - black) * 255) / std::max(1, white - black));
            } else {
                y8 = static_cast<uint8_t>(std::min(255u, static_cast<unsigned>(s10 >> 2)));
            }

            // Convert full-range debug luma to YUYV video-range luma so SDL RGB conversion shows it.
            const uint8_t yVideo = static_cast<uint8_t>(16u + ((static_cast<unsigned>(y8) * 219u + 127u) / 255u));

            const size_t outIndex = (static_cast<size_t>(y) * outW + x) * 2u;
            //dst[outIndex] = y8;
            dst[outIndex] = yVideo;
            dst[outIndex + 1u] = 128u;

            yMin = std::min(yMin, static_cast<uint32_t>(yVideo));
            yMax = std::max(yMax, static_cast<uint32_t>(yVideo));
            ySum += yVideo;
        }
    }
    lastRawSampleMin10_ = rawMin;
    lastRawSampleMax10_ = rawMax;
    lastRawSampleMean10_ = rawCount ? static_cast<double>(rawSum) / rawCount : 0.0;
    lastLumaMin8_ = yMin;
    lastLumaMax8_ = yMax;
    lastLumaMean8_ = rawCount ? static_cast<double>(ySum) / rawCount : 0.0;

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

        // Temporary high-value CSI debug: confirms the driver is actually delivering raw frames.
        // Expected for IMX219 strict mode: bytesused around 1280*720*2 = 1843200, stride around 2560.
        const int dqCountForDebug = framesDequeued_.load(std::memory_order_relaxed);
        if (csiRg10Mode_ && (dqCountForDebug % 30) == 0) {
            spdlog::info(
                "[DataConcrete][DQBUF] index={} bytesused={} capture={}x{} {} stride={} image={} backend={}",
                buf.index,
                buf.bytesused,
                captureWidth_,
                captureHeight_,
                fourccToString(acceptedPixFmt_),
                bytesPerLine_,
                sizeImage_,
                backendName()
            );
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
            camStats.rawCaptureWidth = captureWidth_;
            camStats.rawCaptureHeight = captureHeight_;
            camStats.rawPixelFormat = fourccToString(acceptedPixFmt_);
            camStats.rawBytesUsed = static_cast<uint64_t>(buf.bytesused);
            camStats.rawBytesPerLine = bytesPerLine_;
            camStats.rawSizeImage = sizeImage_;
            camStats.backend = backendName();
            camStats.rawBitAlignment = lastRawBitAlignment_;
            camStats.rawSampleMin10 = lastRawSampleMin10_;
            camStats.rawSampleMax10 = lastRawSampleMax10_;
            camStats.rawSampleMean10 = lastRawSampleMean10_;
            camStats.lumaMin8 = lastLumaMin8_;
            camStats.lumaMax8 = lastLumaMax8_;
            camStats.lumaMean8 = lastLumaMean8_;
            camStats.sensorMode = cameraConfig_.sensorMode;
            camStats.bypassMode = cameraConfig_.bypassMode;
            camStats.csiRg10Mode = csiRg10Mode_;
            camStats.convertedToYuyvLike = csiRg10Mode_;
            camStats.framesDequeued = static_cast<uint64_t>(framesDequeued_.load(std::memory_order_relaxed));
            camStats.framesQueued = static_cast<uint64_t>(framesQueued_.load(std::memory_order_relaxed));
            camStats.droppedFrames = 0;

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
                    {"RawBytesPerLine", static_cast<double>(bytesPerLine_)},
                    {"RawSizeImage", static_cast<double>(sizeImage_)},
                    {"RawSampleMin10", static_cast<double>(lastRawSampleMin10_)},
                    {"RawSampleMax10", static_cast<double>(lastRawSampleMax10_)},
                    {"RawSampleMean10", lastRawSampleMean10_},
                    {"LumaMin8", static_cast<double>(lastLumaMin8_)},
                    {"LumaMax8", static_cast<double>(lastLumaMax8_)},
                    {"LumaMean8", lastLumaMean8_},
                    {"SensorMode", static_cast<double>(cameraConfig_.sensorMode)},
                    {"BypassMode", static_cast<double>(cameraConfig_.bypassMode)},
                    {"CSI_RG10_Mode", csiRg10Mode_ ? 1.0 : 0.0},
                    {"ConvertedToYUYVLike", csiRg10Mode_ ? 1.0 : 0.0},
                    {"FramesDequeued", static_cast<double>(framesDequeued_.load(std::memory_order_relaxed))},
                    {"FramesQueued", static_cast<double>(framesQueued_.load(std::memory_order_relaxed))}
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

        // if ((frameId % 120u) == 0u) {
        //     spdlog::info(
        //         "[DataConcrete][{}] frame={} fps={:.2f} raw={}x{} {} bytesUsed={} output={}x{} bytes={} queued={}",
        //         backendName(), frameId, fpsMeasured, captureWidth_, captureHeight_, fourccToString(acceptedPixFmt_),
        //         buf.bytesused, outputWidth_, outputHeight_, downstreamBytes, pushed ? "yes" : "no"
        //     );
        // }
        
        if ((frameId % 120u) == 0u) {
            spdlog::info(
                "[DataConcrete][{}] frame={} fps={:.2f} raw={}x{} {} bytesUsed={} output={}x{} bytes={} queued={} align={} raw10[min/max/mean]={}/{}/{:.1f} luma[min/max/mean]={}/{}/{:.1f}",
                backendName(), frameId, fpsMeasured, captureWidth_, captureHeight_, fourccToString(acceptedPixFmt_),
                buf.bytesused, outputWidth_, outputHeight_, downstreamBytes, pushed ? "yes" : "no",
                lastRawBitAlignment_, lastRawSampleMin10_, lastRawSampleMax10_, lastRawSampleMean10_,
                lastLumaMin8_, lastLumaMax8_, lastLumaMean8_
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


//===========================================================================================================
// Utility / Helper Sensor_mode =4, For IMX219, sensor_mode=4 is often the 1280×720 60 FPS mode.
//===========================================================================================================

inline bool DataConcrete::setV4L2ControlByName(const std::string& name, int64_t value) {
    if (fd_ < 0) {
        return false;
    }

    // [SEC-FIX support] Normalize control names the same way v4l2-ctl does
    // (lowercase; runs of non-alphanumerics collapse to '_'; trimmed), so
    // "exposure_auto" matches the kernel's "Exposure, Auto". Idempotent for
    // names that are already snake_case ("sensor_mode", "bypass_mode"), so
    // existing CSI callers are unaffected.
    auto normalize = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        bool lastSep = true;   // also trims leading separators
        for (char c : s) {
            const unsigned char uc = static_cast<unsigned char>(c);
            if (std::isalnum(uc)) {
                out.push_back(static_cast<char>(std::tolower(uc)));
                lastSep = false;
            } else if (!lastSep) {
                out.push_back('_');
                lastSep = true;
            }
        }
        while (!out.empty() && out.back() == '_') {
            out.pop_back();
        }
        return out;
    };
    const std::string wanted = normalize(name);

    v4l2_queryctrl query{};
    query.id = V4L2_CTRL_FLAG_NEXT_CTRL;

    while (ioctl(fd_, VIDIOC_QUERYCTRL, &query) == 0) {
        const std::string ctrlName(reinterpret_cast<const char*>(query.name));

        if (!(query.flags & V4L2_CTRL_FLAG_DISABLED) && normalize(ctrlName) == wanted) {
            // sensor_mode on Jetson IMX219 is reported as int64.  Use
            // VIDIOC_S_EXT_CTRLS for 64-bit controls, and the simpler
            // VIDIOC_S_CTRL path for bool/menu/int controls such as bypass_mode.
            if (query.type == V4L2_CTRL_TYPE_INTEGER64) {
                v4l2_ext_control extCtrl{};
                extCtrl.id = query.id;
                extCtrl.value64 = value;

                v4l2_ext_controls extCtrls{};
                extCtrls.ctrl_class = V4L2_CTRL_ID2CLASS(query.id);
                extCtrls.count = 1;
                extCtrls.controls = &extCtrl;

                if (ioctl(fd_, VIDIOC_S_EXT_CTRLS, &extCtrls) == 0) {
                    spdlog::info(
                        "[DataConcrete] V4L2 control '{}' set to {} using VIDIOC_S_EXT_CTRLS",
                        name, static_cast<long long>(value)
                    );
                    return true;
                }
            } else {
                v4l2_control ctrl{};
                ctrl.id = query.id;
                ctrl.value = static_cast<int32_t>(value);

                if (ioctl(fd_, VIDIOC_S_CTRL, &ctrl) == 0) {
                    spdlog::info(
                        "[DataConcrete] V4L2 control '{}' set to {} using VIDIOC_S_CTRL",
                        name, static_cast<long long>(value)
                    );
                    return true;
                }
            }

            spdlog::warn(
                "[DataConcrete] Failed to set V4L2 control '{}' to {}: {}",
                name, static_cast<long long>(value), std::strerror(errno)
            );
            return false;
        }

        query.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }

    spdlog::warn(
        "[DataConcrete] V4L2 control '{}' not available on {}",
        name, devicePath_
    );
    return false;
}

// inline bool DataConcrete::setV4L2ControlByName(const std::string& name, int32_t value) {
//     if (fd_ < 0) return false;

//     v4l2_queryctrl query{};
//     query.id = V4L2_CTRL_FLAG_NEXT_CTRL;

//     while (ioctl(fd_, VIDIOC_QUERYCTRL, &query) == 0) {
//         const std::string ctrlName =
//             query.name ? reinterpret_cast<const char*>(query.name) : "";

//         if (!(query.flags & V4L2_CTRL_FLAG_DISABLED) && ctrlName == name) {
//             v4l2_control ctrl{};
//             ctrl.id = query.id;
//             ctrl.value = value;

//             if (ioctl(fd_, VIDIOC_S_CTRL, &ctrl) == 0) {
//                 spdlog::info("[DataConcrete] V4L2 control '{}' set to {}", name, value);
//                 return true;
//             }

//             spdlog::warn(
//                 "[DataConcrete] Failed to set V4L2 control '{}' to {}: {}",
//                 name,
//                 value,
//                 std::strerror(errno)
//             );
//             return false;
//         }

//         query.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
//     }

//     spdlog::debug("[DataConcrete] V4L2 control '{}' not available on {}", name, devicePath_);
//     return false;
// }
