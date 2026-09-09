
//======================================================================================================================================================
// CameraConfig.h
// Date: 09-20-2025
// [MOD 09-20-2025] Added numBuffers field with validation for V4L2 buffer configuration.
//======================================================================================================================================================
//======================================================================================================================================================
// CameraConfig.h
// ERL Stage 1 - Camera configuration for USB YUYV and Jetson IMX219 CSI RG10 capture
// Updated: 2026-06-26
//======================================================================================================================================================

#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

#ifdef __linux__
#include <linux/videodev2.h>
#ifndef V4L2_PIX_FMT_RG10
#define V4L2_PIX_FMT_RG10 v4l2_fourcc('R', 'G', '1', '0')
#endif
#else
// FourCC fallback values for non-Linux compilation/unit checks.
#define V4L2_PIX_FMT_YUYV  0x56595559u // 'YUYV'
#define V4L2_PIX_FMT_MJPEG 0x47504A4Du // 'MJPG'
#define V4L2_PIX_FMT_RG10  0x30314752u // 'RG10'
#endif

#include <spdlog/spdlog.h>

/**
 * @brief Enum that wraps V4L2 pixel format codes used by the ERL camera module.
 *
 * YUYV remains the downstream frame contract used by AlgorithmConcrete and SdlDisplayConcrete.
 * RG10 is the raw IMX219 CSI capture format; DataConcrete converts it to YUYV-like grayscale.
 */
enum class PixelFormat : uint32_t {
    AUTO = 0u,
    YUYV = V4L2_PIX_FMT_YUYV,
    MJPG = V4L2_PIX_FMT_MJPEG,
    RG10 = V4L2_PIX_FMT_RG10
};

/**
 * @brief Preferred camera backend.
 */
enum class CameraBackendPreference : int {
    AUTO = 0,
    USB_YUYV,
    CSI_RG10_TO_YUYV
};

inline const char* pixelFormatToString(PixelFormat fmt) {
    switch (fmt) {
        case PixelFormat::AUTO: return "AUTO";
        case PixelFormat::YUYV: return "YUYV";
        case PixelFormat::MJPG: return "MJPG";
        case PixelFormat::RG10: return "RG10";
        default: return "UNKNOWN";
    }
}

inline uint32_t pixelFormatFourcc(PixelFormat fmt, uint32_t fallback = V4L2_PIX_FMT_YUYV) {
    if (fmt == PixelFormat::AUTO) return fallback;
    return static_cast<uint32_t>(fmt);
}

/**
 * @brief Configuration structure for camera capture.
 *
 * Output fields define what the ERL pipeline receives. Raw capture fields define what V4L2
 * should request from the sensor. For IMX219 CSI, this is normally 1280x720 RG10 with
 * sensor_mode=4 for the 60 FPS mode.
 */
struct CameraConfig {
    // Downstream/output frame contract consumed by AlgorithmConcrete and SdlDisplayConcrete.
    int width = 320;
    int height = 240;
    int fps = 60;
    PixelFormat pixelFormat = PixelFormat::YUYV;
    int numBuffers = 8;

    // Device/backend selection.
    std::string device = "/dev/video0";
    CameraBackendPreference backend = CameraBackendPreference::AUTO;

    // Raw capture configuration for CSI IMX219 direct V4L2.
    int rawCaptureWidth = 1280;
    int rawCaptureHeight = 720;
    int rawCaptureFps = 60;
    PixelFormat rawCapturePixelFormat = PixelFormat::RG10;

    // Jetson sensor controls. Your v4l2-ctl test showed sensor_mode=4 is required for 1280x720 RG10.
    int sensorMode = 4;
    int bypassMode = 0;
    bool lowLatencyMode = false;
    bool forceSensorMode = true;
    bool strictCsiMode = true;

    // CSI RG10 -> YUYV-like grayscale conversion controls.
    // AUTO handles Jetson drivers that expose RG10 as either LSB- or MSB-aligned 16-bit samples.
    std::string rawBitAlignment = "AUTO";  // AUTO, LSB, MSB
    bool autoLumaStretch = true;            // For visible validation display; consistent if kept same for all modes.
    bool debugLumaMetrics = true;           // Publish raw/luma min/max/mean evidence.
    int lumaBlackLevel = -1;                // Optional manual 10-bit black level; -1 = auto/min.
    int lumaWhiteLevel = -1;                // Optional manual 10-bit white level; -1 = auto/max.

    CameraConfig(int w = 320,
                 int h = 240,
                 int f = 60,
                 PixelFormat pf = PixelFormat::YUYV,
                 int nb = 8)
        : width(w), height(h), fps(f), pixelFormat(pf), numBuffers(nb) {
        if (!validate()) {
            spdlog::error(
                "[CameraConfig] Invalid configuration: output={}x{} fps={} pixelFormat={} buffers={} raw={}x{} rawFmt={} sensorMode={} rawBitAlignment={} autoLumaStretch={}",
                width,
                height,
                fps,
                pixelFormatToString(pixelFormat),
                numBuffers,
                rawCaptureWidth,
                rawCaptureHeight,
                pixelFormatToString(rawCapturePixelFormat),
                sensorMode,
                rawBitAlignment,
                autoLumaStretch
            );
            throw std::invalid_argument("Invalid CameraConfig parameters");
        }
    }

    bool wantsCsiRg10() const {
        return backend == CameraBackendPreference::CSI_RG10_TO_YUYV ||
               rawCapturePixelFormat == PixelFormat::RG10;
    }

    bool validate() const {
        if (width <= 0 || height <= 0) {
            spdlog::error("[CameraConfig] Invalid output dimensions: width={}, height={}", width, height);
            return false;
        }
        if (fps <= 0 || fps > 240) {
            spdlog::error("[CameraConfig] Invalid FPS: {}", fps);
            return false;
        }
        if (numBuffers < 2 || numBuffers > 8) {
            spdlog::error("[CameraConfig] numBuffers={} is out of range (2-8)", numBuffers);
            return false;
        }
        if (rawCaptureWidth <= 0 || rawCaptureHeight <= 0) {
            spdlog::error("[CameraConfig] Invalid raw capture dimensions: {}x{}", rawCaptureWidth, rawCaptureHeight);
            return false;
        }
        if (rawCaptureFps <= 0 || rawCaptureFps > 240) {
            spdlog::error("[CameraConfig] Invalid rawCaptureFps: {}", rawCaptureFps);
            return false;
        }
        if (sensorMode < -1 || sensorMode > 30) {
            spdlog::error("[CameraConfig] sensorMode={} out of expected range (-1..30)", sensorMode);
            return false;
        }
        if (bypassMode < -1 || bypassMode > 1) {
            spdlog::error("[CameraConfig] bypassMode={} out of expected range (-1..1)", bypassMode);
            return false;
        }
        if (!(rawBitAlignment == "AUTO" || rawBitAlignment == "LSB" || rawBitAlignment == "MSB")) {
            spdlog::error("[CameraConfig] rawBitAlignment='{}' must be AUTO, LSB, or MSB", rawBitAlignment);
            return false;
        }
        if (lumaBlackLevel < -1 || lumaBlackLevel > 1023 || lumaWhiteLevel < -1 || lumaWhiteLevel > 1023) {
            spdlog::error("[CameraConfig] lumaBlackLevel/lumaWhiteLevel must be -1 or 0..1023");
            return false;
        }
        return true;
    }
};
//======================================================================================================================================================
