// =====================================================================
// LatencyHistogram.hpp  Lock-Free Histogram for Real-Time Percentiles
// =====================================================================
#pragma once

#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>

namespace hrl {

/**
 * LatencyHistogram  O(1) insertion, O(n) percentile query
 * Lock-free design for producer threads
 * 
 * Binning: 0-1ms, 1-2ms, ..., 49-50ms, 50ms+
 */
class LatencyHistogram {
public:
    static constexpr size_t NUM_BINS = 51;  // 0-50ms + overflow
    static constexpr double BIN_WIDTH_MS = 1.0;

    LatencyHistogram() {
        for (size_t i = 0; i < NUM_BINS; ++i) {
            bins_[i].store(0, std::memory_order_relaxed);
        }
        total_count_.store(0, std::memory_order_relaxed);
    }

    /**
     * Record a latency sample  O(1) lock-free operation
     */
    void recordSample(double latency_ms) {
        size_t bin = std::min(
            static_cast<size_t>(latency_ms / BIN_WIDTH_MS),
            NUM_BINS - 1
        );
        bins_[bin].fetch_add(1, std::memory_order_relaxed);
        total_count_.fetch_add(1, std::memory_order_acquire);
    }

    /**
     * Get percentile  e.g., 95th percentile latency
     * Not lock-free; call from monitoring thread
     */
    double getPercentile(double pct) const {
        uint64_t total = total_count_.load(std::memory_order_acquire);
        if (total == 0) return 0.0;

        uint64_t target_count = static_cast<uint64_t>((pct / 100.0) * total);
        uint64_t cumulative = 0;

        for (size_t i = 0; i < NUM_BINS; ++i) {
            cumulative += bins_[i].load(std::memory_order_relaxed);
            if (cumulative >= target_count) {
                return (i + 0.5) * BIN_WIDTH_MS;  // Bin midpoint
            }
        }
        return 50.0 + BIN_WIDTH_MS;
    }

    /**
     * Get average latency
     */
    double getAverage() const {
        uint64_t total = total_count_.load(std::memory_order_acquire);
        if (total == 0) return 0.0;

        double sum = 0.0;
        for (size_t i = 0; i < NUM_BINS; ++i) {
            uint64_t count = bins_[i].load(std::memory_order_relaxed);
            sum += count * (i + 0.5) * BIN_WIDTH_MS;
        }
        return sum / total;
    }

    /**
     * Get histogram snapshot for logging
     */
    std::array<uint64_t, NUM_BINS> snapshot() const {
        std::array<uint64_t, NUM_BINS> result;
        for (size_t i = 0; i < NUM_BINS; ++i) {
            result[i] = bins_[i].load(std::memory_order_acquire);
        }
        return result;
    }

    void reset() {
        for (size_t i = 0; i < NUM_BINS; ++i) {
            bins_[i].store(0, std::memory_order_relaxed);
        }
        total_count_.store(0, std::memory_order_release);
    }

private:
    std::array<std::atomic<uint64_t>, NUM_BINS> bins_;
    std::atomic<uint64_t> total_count_;
};

} // namespace hrl