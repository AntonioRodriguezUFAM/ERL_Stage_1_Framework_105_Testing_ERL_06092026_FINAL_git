
// ============================================================================
// CudaUtils.h
// CUDA availability detection for ERL heterogeneous evaluation
// Header-only | C++14 | Jetson Nano compatible
// ============================================================================

#pragma once

#ifndef ERL_CUDA_UTILS_H
#define ERL_CUDA_UTILS_H

#include <atomic>

#include <cuda_runtime.h>
#include <spdlog/spdlog.h>

namespace hrl {

// ----------------------------------------------------------------------------
// Header-only CUDA availability storage.
// C++14-safe: no namespace global variable definition.
// ----------------------------------------------------------------------------
inline std::atomic<bool>& cudaAvailableStorage() {
    static std::atomic<bool> available(false);
    return available;
}

inline void setCudaAvailable(bool value) {
    cudaAvailableStorage().store(value, std::memory_order_relaxed);
}

inline bool isCudaAvailable() {
    return cudaAvailableStorage().load(std::memory_order_relaxed);
}

inline const char* cudaAvailabilityString() {
    return isCudaAvailable() ? "YES" : "NO";
}

/**
 * @brief Detect whether CUDA is actually usable at runtime.
 */
inline bool detectCudaAvailability() {
    int deviceCount = 0;

    cudaError_t err = cudaGetDeviceCount(&deviceCount);

    if (err != cudaSuccess || deviceCount <= 0) {
        spdlog::warn("[CUDA] Runtime unavailable or no CUDA device found. err={} ({})",
                     static_cast<int>(err),
                     cudaGetErrorString(err));

        setCudaAvailable(false);
        return false;
    }

    cudaDeviceProp prop{};
    err = cudaGetDeviceProperties(&prop, 0);

    if (err != cudaSuccess) {
        spdlog::warn("[CUDA] Failed to read CUDA device properties. err={} ({})",
                     static_cast<int>(err),
                     cudaGetErrorString(err));

        setCudaAvailable(false);
        return false;
    }

    err = cudaSetDevice(0);

    if (err != cudaSuccess) {
        spdlog::warn("[CUDA] cudaSetDevice(0) failed. err={} ({})",
                     static_cast<int>(err),
                     cudaGetErrorString(err));

        setCudaAvailable(false);
        return false;
    }

    // Force CUDA runtime initialization.
    err = cudaFree(nullptr);

    if (err != cudaSuccess) {
        spdlog::warn("[CUDA] CUDA runtime initialization failed. err={} ({})",
                     static_cast<int>(err),
                     cudaGetErrorString(err));

        setCudaAvailable(false);
        return false;
    }

    spdlog::info("[CUDA] Detected: YES | Device: {} | Compute Capability: {}.{} | Memory: {:.2f} GB",
                 prop.name,
                 prop.major,
                 prop.minor,
                 static_cast<double>(prop.totalGlobalMem) /
                     (1024.0 * 1024.0 * 1024.0));

    setCudaAvailable(true);
    return true;
}

} // namespace hrl

#endif // ERL_CUDA_UTILS_H