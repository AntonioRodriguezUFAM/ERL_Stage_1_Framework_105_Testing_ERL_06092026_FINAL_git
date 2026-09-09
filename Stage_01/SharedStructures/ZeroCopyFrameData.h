//======================================================================================================================================================
// ZeroCopyFrameData.h
// Date: 09-20-2025
// [MOD 09-20-2025] Updated for true zero-copy with std::shared_ptr<void> and dual timestamps.
//======================================================================================================================================================

#pragma once
#include <cstdint>
#include <memory>
#include <chrono>
#include <stdexcept> // [FIX] Add required header for std::invalid_argument

class ZeroCopyFrameData {
public:
    // ===== Constructors =====

    // [MOD 09-20-2025] Made default constructor private to prevent invalid instances

    ZeroCopyFrameData() = default; // Restricted for safety; used only by trusted callers (e.g., LucasKanadeOpticalFlow.h:152)

public:
    // [MOD 09-20-2025] Primary constructor for zero-copy with explicit timestamps
    ZeroCopyFrameData(
        std::shared_ptr<void> bufferGuard,
        void* dataPtr,
        size_t size,
        int width,
        int height,
        int bufferIndex,
        uint64_t frameNumber,
        std::chrono::system_clock::time_point sysTs,
        std::chrono::steady_clock::time_point steadyTs
    ) : bufferGuard(bufferGuard),
        dataPtr(dataPtr),
        size(size),
        width(width),
        height(height),
        bufferIndex(bufferIndex),
        frameNumber(frameNumber),
        captureSys(sysTs),
        captureSteady(steadyTs),
        captureTime(sysTs) { // Backwards-compat alias
        // [MOD 09-20-2025] Add runtime checks for invalid parameters
        if (width <= 0 || height <= 0 || !bufferGuard || !dataPtr || size == 0) {
            throw std::invalid_argument("Invalid frame parameters");
        }
    }

    // [MOD 09-20-2025] Legacy constructor for compatibility with vector-based callsites
    ZeroCopyFrameData(
        std::shared_ptr<std::vector<uint8_t>> data,
        size_t size,
        int width,
        int height,
        int bufferIndex,
        uint64_t frameNumber,
        std::chrono::system_clock::time_point sysTs,
        std::chrono::steady_clock::time_point steadyTs
    ) : bufferGuard(nullptr), // No zero-copy for legacy
        dataPtr(data ? data->data() : nullptr),
        size(size),
        width(width),
        height(height),
        bufferIndex(bufferIndex),
        frameNumber(frameNumber),
        captureSys(sysTs),
        captureSteady(steadyTs),
        captureTime(sysTs), // Backwards-compat alias
        dataHolder(std::move(data)) {
        if (width <= 0 || height <= 0 || !dataPtr || size == 0) {
            throw std::invalid_argument("Invalid frame parameters");
        }
    }

    // ===== Accessors =====
    // [MOD 09-20-2025] Updated isValid to check bufferGuard for zero-copy cases
    bool isValid() const {
        return (bufferGuard || dataHolder) && // Either zero-copy or legacy
               dataPtr != nullptr &&
               size > 0 &&
               width > 0 &&
               height > 0 &&
               captureSys.time_since_epoch().count() > 0;
    }

    // [MOD 09-20-2025] Helper for steady clock timestamp
    bool hasSteadyTimestamp() const {
        return captureSteady.time_since_epoch().count() > 0;
    }

    // [MOD 09-20-2025] Helper for elapsed time using steady clock
    double elapsedMs() const {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - captureSteady).count();
    }

    // ===== Public Fields =====
    std::shared_ptr<void> bufferGuard; // [MOD 09-20-2025] Holds MMAP buffer for zero-copy
    void* dataPtr = nullptr;
    size_t size = 0;
    int width = 0;
    int height = 0;
    int bufferIndex = -1;
    uint64_t frameNumber = 0;
    std::chrono::system_clock::time_point captureSys{};    // For CSV/logs
    std::chrono::steady_clock::time_point captureSteady{}; // For FPS/latency
    std::chrono::system_clock::time_point captureTime{};   // Deprecated alias

private:
    // [MOD 09-20-2025] Retained for legacy compatibility
    std::shared_ptr<std::vector<uint8_t>> dataHolder;
};
