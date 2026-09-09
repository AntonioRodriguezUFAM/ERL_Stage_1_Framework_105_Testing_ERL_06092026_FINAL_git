// FrameGrayView.hpp

/**
A. Add a portable grayscale accessor (one header-only helper)

Create a tiny helper (header-only) that converts the current frames pixel format into a GRAY8 pointer with a valid stride. Put this somewhere both AlgorithmConcrete_new.h 
and AlgorithmConcreteKernels.cu can include (e.g. Stage_01/SharedStructures/FrameGrayView.hpp).
*/

// Stage_01/SharedStructures/FrameGrayView.hpp
#pragma once
#include <cstdint>
#include <vector>
#include "ZeroCopyFrameData.h"
#include "CameraConfig.h"  // for PixelFormat

// Returns a pointer to a contiguous GRAY8 buffer and fills (w,h,stride).
// - If the source frame is YUYV, we copy out the Y plane (every 2nd byte).
// - If strideBytes==0, derive stride from width.
// - If the source is already GRAY8 (in case you add it later), we return its CPU ptr directly.
// - If MJPG or unsupported, we return nullptr.
//
// NOTE: The returned pointer is valid as long as `tmp` outlives the use.
//       For direct view (future GRAY8), we still set stride/w/h properly.
inline const uint8_t* makeGrayView(
    const ZeroCopyFrameData& f,
    std::vector<uint8_t>&    tmp,
    int&                     w,
    int&                     h,
    int&                     strideBytesOut)
{
    w = static_cast<int>(f.width);
    h = static_cast<int>(f.height);

    // If you later add PixelFormat::GRAY8, uncomment this block:
    // if (f.format == PixelFormat::GRAY8) {
    //     strideBytesOut = (f.strideBytes > 0) ? static_cast<int>(f.strideBytes) : w;
    //     return static_cast<const uint8_t*>(f.dataPtr);
    // }

    if (f.format == PixelFormat::YUYV) {
        const int srcStride = (f.strideBytes > 0)
                                ? static_cast<int>(f.strideBytes)
                                : w * 2; // YUYV => 2 bytes/pixel

        tmp.resize(static_cast<size_t>(w) * static_cast<size_t>(h));

        const auto* src = static_cast<const uint8_t*>(f.dataPtr);
        for (int y = 0; y < h; ++y) {
            const uint8_t* row = src + y * srcStride;
            uint8_t*       dst = tmp.data() + static_cast<size_t>(y) * static_cast<size_t>(w);
            // YUYV: [Y0 U0 Y1 V0] => copy Y0,Y1,...
            for (int x = 0; x < w; ++x) {
                dst[x] = row[x * 2];
            }
        }
        strideBytesOut = w;
        return tmp.data();
    }

    // MJPG or anything else not handled here
    strideBytesOut = 0;
    return nullptr;
}

// Convenience: returns a pointer and ignores the stride (sets to width).
inline const uint8_t* makeGrayView(
    const ZeroCopyFrameData& f,
    std::vector<uint8_t>&    tmp,
    int&                     w,
    int&                     h)
{
    int stride = 0;
    return makeGrayView(f, tmp, w, h, stride);
}
