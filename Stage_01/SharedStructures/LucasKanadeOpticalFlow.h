
// LucasKanadeOpticalFlow.h
#pragma once

#include "ZeroCopyFrameData.h"
#include "AlgorithmConfig.h" // [MOD] Renamed from OpticalFlowConfig for consistency
#include "LKof_defines.h"    // [MOD] Included for FILTER_SIZE
#include <vector>
#include <memory>
#include <cmath>
#include <spdlog/spdlog.h>

/**
 * @class LucasKanadeOpticalFlow
 * @brief Computes dense optical flow using the Lucas-Kanade algorithm.
 *
 * This class processes YUYV input frames, extracts the luminance (Y) plane,
 * and computes motion vectors for each pixel. It outputs a frame where the
 * motion vectors are encoded as pixel data.
 */
class LucasKanadeOpticalFlow {
public:
    explicit LucasKanadeOpticalFlow(const OpticalFlowConfig& config);

    bool computeOpticalFlow(
        const std::shared_ptr<ZeroCopyFrameData>& prevFrame,
        const std::shared_ptr<ZeroCopyFrameData>& nextFrame,
        std::shared_ptr<ZeroCopyFrameData>& outputFlowFrame);

private:
    OpticalFlowConfig config_;

    void extractYPlane(const uint8_t* yuyv, uint8_t* y, int width, int height);
    bool matrixInversion(float A[2][2], float B[2], float& Vx, float& Vy);
    void isotropicFilter(const uint8_t* input, uint8_t* output, int width, int height);
    void computeMotionVectors(
        const uint8_t* img1, const uint8_t* img2,
        std::vector<int16_t>& vx, std::vector<int16_t>& vy,
        int width, int height);
};

// --- Implementation ---

inline LucasKanadeOpticalFlow::LucasKanadeOpticalFlow(const OpticalFlowConfig& config)
    : config_(config) {}

/**
 * @brief Extracts the Y (luminance) plane from a YUYV buffer.
 */
inline void LucasKanadeOpticalFlow::extractYPlane(const uint8_t* yuyv, uint8_t* y_plane, int width, int height) {
    for (int i = 0; i < width * height; ++i) {
        y_plane[i] = yuyv[i * 2];
    }
}

inline bool LucasKanadeOpticalFlow::matrixInversion(float A[2][2], float B[2], float& Vx, float& Vy) {
    float det = (A[0][0] * A[1][1]) - (A[0][1] * A[1][0]);
    if (std::abs(det) < config_.threshold) {
        return false;
    }

    float inv_det = 1.0f / det;
    Vx = inv_det * (A[1][1] * (-B[0]) - A[0][1] * (-B[1]));
    Vy = inv_det * (-A[1][0] * (-B[0]) + A[0][0] * (-B[1]));
    return true;
}

inline void LucasKanadeOpticalFlow::isotropicFilter(const uint8_t* input, uint8_t* output, int width, int height) {
    const int offset = FILTER_SIZE / 2;
    for (int y = offset; y < height - offset; ++y) {
        for (int x = offset; x < width - offset; ++x) {
            int accum = 0;
            for (int dy = -offset; dy <= offset; ++dy) {
                for (int dx = -offset; dx <= offset; ++dx) {
                    accum += input[(y + dy) * width + (x + dx)];
                }
            }
            output[y * width + x] = static_cast<uint8_t>(accum / (FILTER_SIZE * FILTER_SIZE));
        }
    }
}

inline void LucasKanadeOpticalFlow::computeMotionVectors(
    const uint8_t* img1, const uint8_t* img2,
    std::vector<int16_t>& vx, std::vector<int16_t>& vy,
    int width, int height) {
    
    std::vector<float> Ix(width * height, 0);
    std::vector<float> Iy(width * height, 0);
    std::vector<float> It(width * height, 0);

    for (int y = 1; y < height - 1; ++y) {
        for (int x = 1; x < width - 1; ++x) {
            int idx = y * width + x;
            Ix[idx] = (img1[idx + 1] - img1[idx - 1]) / 2.0f;
            Iy[idx] = (img1[idx + width] - img1[idx - width]) / 2.0f;
            It[idx] = static_cast<float>(img2[idx]) - static_cast<float>(img1[idx]);
        }
    }

    int winSize = config_.windowSize;
    int wOffset = winSize / 2;

    for (int y = wOffset; y < height - wOffset; ++y) {
        for (int x = wOffset; x < width - wOffset; ++x) {
            float sumIx2 = 0, sumIy2 = 0, sumIxIy = 0, sumIxIt = 0, sumIyIt = 0;

            for (int dy = -wOffset; dy <= wOffset; ++dy) {
                for (int dx = -wOffset; dx <= wOffset; ++dx) {
                    int idx = (y + dy) * width + (x + dx);
                    float ix = Ix[idx], iy = Iy[idx], it = It[idx];
                    sumIx2 += ix * ix;
                    sumIy2 += iy * iy;
                    sumIxIy += ix * iy;
                    sumIxIt += ix * it;
                    sumIyIt += iy * it;
                }
            }

            float A[2][2] = {{sumIx2, sumIxIy}, {sumIxIy, sumIy2}};
            float B[2] = {sumIxIt, sumIyIt};
            float vx_f, vy_f;

            int out_idx = y * width + x;
            if (matrixInversion(A, B, vx_f, vy_f)) {
                vx[out_idx] = static_cast<int16_t>(vx_f * 256.0f); // Scale for fixed-point representation
                vy[out_idx] = static_cast<int16_t>(vy_f * 256.0f);
            } else {
                vx[out_idx] = 0;
                vy[out_idx] = 0;
            }
        }
    }
}

inline bool LucasKanadeOpticalFlow::computeOpticalFlow(
    const std::shared_ptr<ZeroCopyFrameData>& prevFrame,
    const std::shared_ptr<ZeroCopyFrameData>& nextFrame,
    std::shared_ptr<ZeroCopyFrameData>& outputFlowFrame) {

    if (!prevFrame || !nextFrame || !prevFrame->isValid() || !nextFrame->isValid()) {
        spdlog::warn("[LucasKanade] Invalid input frames.");
        return false;
    }

    const int width = prevFrame->width;
    const int height = prevFrame->height;
    const int frame_size = width * height;

    // Allocate buffers for Y-plane data
    std::vector<uint8_t> prevY(frame_size);
    std::vector<uint8_t> nextY(frame_size);
    
    // Extract luminance from both frames
    extractYPlane(static_cast<uint8_t*>(prevFrame->dataPtr), prevY.data(), width, height);
    extractYPlane(static_cast<uint8_t*>(nextFrame->dataPtr), nextY.data(), width, height);

    // Allocate buffers for motion vectors
    std::vector<int16_t> vx(frame_size, 0);
    std::vector<int16_t> vy(frame_size, 0);

    computeMotionVectors(prevY.data(), nextY.data(), vx, vy, width, height);

    // [FIX] Use modern C++ memory management to avoid leaks.
    // Create a new vector to hold the output flow data.
    auto outputData = std::make_shared<std::vector<uint8_t>>(frame_size * 2 * sizeof(int16_t));
    
    // Copy motion vectors into the single output buffer
    memcpy(outputData->data(), vx.data(), frame_size * sizeof(int16_t));
    memcpy(outputData->data() + frame_size * sizeof(int16_t), vy.data(), frame_size * sizeof(int16_t));

    // [FIX] Use the modern, safe constructor to create the output frame.
    outputFlowFrame = std::make_shared<ZeroCopyFrameData>(
        outputData,
        outputData->size(),
        width,
        height,
        -1, // Processed frames don't have a V4L2 buffer index
        nextFrame->frameNumber,
        nextFrame->captureSys,
        nextFrame->captureSteady
    );

    return true;
}
// End of LucasKanadeOpticalFlow.h