// ============================================================================
// PowerSanity.h ? single source of truth for physically-possible SoC power.
// Drop this in and #include it from:
//   - LynsynMonitorConcrete_new.h
//   - SystemMetricsAggregatorConcrete_v3_2.h
//   - AdaptiveWeightManager.h
//
// Rationale: the 2026-07-11 run fed 22.22 W / 40.09 W / 57.49 W into the
// ERL context engine on a board whose MAXN ceiling is 10 W and whose measured
// draw was 3.489 W mean / 6.50 W peak. A closed-loop controller must never
// transition regime on an input that violates physics.
// ============================================================================
#pragma once
#include <cmath>

namespace hrl {

struct PowerSanity {
    static constexpr double kMinW = 0.30;   // below -> rail dead / disconnected
    static constexpr double kMaxW = 12.0;   // above -> physically impossible (Nano MAXN = 10 W)

    static bool valid(double w) {
        return std::isfinite(w) && w >= kMinW && w <= kMaxW;
    }

    static const char* reject_reason(double w) {
        if (!std::isfinite(w)) return "not finite (NaN/Inf)";
        if (w < kMinW)         return "below floor (sensor dead?)";
        if (w > kMaxW)         return "above Jetson Nano 10W ceiling (unit/normalization bug)";
        return "valid";
    }
};

} // namespace hrl