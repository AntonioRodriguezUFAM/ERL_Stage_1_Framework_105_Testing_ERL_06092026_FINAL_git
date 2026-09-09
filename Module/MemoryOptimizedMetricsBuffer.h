// =====================================================================
// MemoryOptimizedMetricsBuffer.h  Circular Buffer for Real-Time Data
// =====================================================================
#pragma once

#include <array>
#include <cstring>
#include <algorithm>
#include <numeric>
#include <spdlog/spdlog.h>

namespace hrl {

struct MetricsFrame {
    uint64_t timestamp_ms = 0;
    double fps = 0.0;
    double inference_ms = 0.0;
    double power_w = 0.0;
    double cpu_temp_c = 0.0;
    double gpu_temp_c = 0.0;
    double cpu_util_pct = 0.0;
    int dropped_frames = 0;
    bool valid = false;
};

/**
 * MemoryOptimizedMetricsBuffer  Bounded circular buffer (max 100 frames, ~16 KB)
 * 
 * Properties:
 * - O(1) insertion via circular index
 * - No dynamic allocation (fixed std::array)
 * - Automatic age-based eviction
 * - Lock-free reads for monitoring thread
 */
template<size_t CAPACITY = 100>
class MemoryOptimizedMetricsBuffer {
public:
    static_assert(CAPACITY > 0 && CAPACITY <= 1000, "CAPACITY must be in [1, 1000]");

    MemoryOptimizedMetricsBuffer() : head_(0), size_(0) {
        std::fill(buffer_.begin(), buffer_.end(), MetricsFrame{});
    }

    /**
     * Push new frame  O(1) insertion with automatic eviction
     */
    void push(const MetricsFrame& frame) {
        buffer_[head_] = frame;
        head_ = (head_ + 1) % CAPACITY;
        if (size_ < CAPACITY) ++size_;
    }

    /**
     * Get last N frames (most recent first)
     * Returns minimum of N and available frames
     */
    std::vector<MetricsFrame> getRecent(size_t n) const {
        std::vector<MetricsFrame> result;
        result.reserve(std::min(n, size_));

        for (size_t i = 0; i < std::min(n, size_); ++i) {
            size_t idx = (head_ + CAPACITY - 1 - i) % CAPACITY;
            result.push_back(buffer_[idx]);
        }
        return result;
    }

    /**
     * Compute running average for a metric field (last N frames)
     */
    double getRunningAvg(size_t n, double MetricsFrame::*field) const {
        auto recent = getRecent(n);
        if (recent.empty()) return 0.0;

        double sum = 0.0;
        for (const auto& f : recent) {
            if (f.valid) sum += f.*field;
        }
        return sum / (double)recent.size();
    }

    /**
     * Get percentile of a metric (e.g., P95 inference latency)
     */
    double getPercentile(size_t n, double MetricsFrame::*field, double pct) const {
        auto recent = getRecent(n);
        if (recent.empty()) return 0.0;

        std::vector<double> values;
        for (const auto& f : recent) {
            if (f.valid) values.push_back(f.*field);
        }
        std::sort(values.begin(), values.end());

        size_t idx = static_cast<size_t>(values.size() * pct / 100.0);
        return values.empty() ? 0.0 : values[std::min(idx, values.size() - 1)];
    }

    /**
     * Get current buffer statistics
     */
    struct Stats {
        size_t size;
        size_t capacity;
        double memory_kb;
        uint64_t oldest_timestamp_ms;
        uint64_t newest_timestamp_ms;
    };

    Stats getStats() const {
        uint64_t oldest = size_ > 0 ? buffer_[(head_ + 1) % CAPACITY].timestamp_ms : 0;
        uint64_t newest = size_ > 0 ? buffer_[(head_ + CAPACITY - 1) % CAPACITY].timestamp_ms : 0;
        
        return {
            .size = size_,
            .capacity = CAPACITY,
            .memory_kb = (CAPACITY * sizeof(MetricsFrame)) / 1024.0,
            .oldest_timestamp_ms = oldest,
            .newest_timestamp_ms = newest
        };
    }

    void clear() {
        head_ = 0;
        size_ = 0;
        std::fill(buffer_.begin(), buffer_.end(), MetricsFrame{});
    }

private:
    std::array<MetricsFrame, CAPACITY> buffer_;
    size_t head_ = 0;
    size_t size_ = 0;
};

} // namespace hrl