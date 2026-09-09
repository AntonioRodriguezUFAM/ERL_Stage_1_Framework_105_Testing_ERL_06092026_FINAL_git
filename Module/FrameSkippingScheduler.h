// =====================================================================
// FrameSkippingScheduler.h  Dynamic Frame-Skip with Quality Preservation
// =====================================================================
#pragma once

#include <cmath>
#include <algorithm>
#include <spdlog/spdlog.h>

namespace hrl {

/**
 * FrameSkippingScheduler  Intelligent frame dropping to maintain FPS target
 * 
 * Strategy:
 * - Monitor inference latency
 * - If latency > 30ms, enable frame-skip to keep queue bounded
 * - Preserve keyframes (every Nth frame always processed)
 * - Reduce quality (e.g., resolution) instead of full skip if possible
 */
class FrameSkippingScheduler {
public:
    struct FrameDecision {
        bool should_process = true;
        bool is_keyframe = false;
        float quality_scale = 1.0;   // 0.5 = half resolution
        int skip_budget_remaining = 0;
    };

    FrameSkippingScheduler(int keyframe_interval = 5)
        : keyframe_interval_(keyframe_interval),
          frame_counter_(0),
          skip_budget_(0),
          recent_latency_ms_(0.0),
          inference_backlog_(0) {}

    /**
     * Decide whether to process current frame
     * Call with current inference latency estimate
     */
    FrameDecision decide(double inference_latency_ms, int queue_depth) {
        FrameDecision decision;
        
        recent_latency_ms_ = 0.9 * recent_latency_ms_ + 0.1 * inference_latency_ms;
        inference_backlog_ = queue_depth;

        // Keyframe: always process
        if (frame_counter_ % keyframe_interval_ == 0) {
            decision.is_keyframe = true;
            decision.should_process = true;
            decision.quality_scale = 1.0;
            skip_budget_ = 0;
        }
        // Normal frame: decide based on latency
        else {
            // Strategy: If avg latency > 25ms, skip non-keyframes
            if (recent_latency_ms_ > 25.0 && queue_depth > 2) {
                decision.should_process = false;
                skip_budget_++;
            }
            // If backlog is severe, reduce quality instead of skipping
            else if (queue_depth > 5) {
                decision.should_process = true;
                decision.quality_scale = std::max(0.5f, 1.0f - (queue_depth - 3) * 0.1f);
            }
            else {
                decision.should_process = true;
                decision.quality_scale = 1.0f;
            }
        }

        decision.skip_budget_remaining = skip_budget_;
        frame_counter_++;
        
        return decision;
    }

    /**
     * Get current scheduling metrics
     */
    struct Metrics {
        double recent_latency_ms;
        int inference_backlog;
        int frames_skipped;
    };

    Metrics getMetrics() const {
        return {
            .recent_latency_ms = recent_latency_ms_,
            .inference_backlog = inference_backlog_,
            .frames_skipped = skip_budget_
        };
    }

    void reset() {
        frame_counter_ = 0;
        skip_budget_ = 0;
        recent_latency_ms_ = 0.0;
        inference_backlog_ = 0;
    }

private:
    int keyframe_interval_;
    int frame_counter_;
    int skip_budget_;
    double recent_latency_ms_;
    int inference_backlog_;
};

} // namespace hrl