#pragma once

#include <atomic>
#include <cstdint>

#include "MetricsSnapshot.h"

namespace hrl {

enum class Affinity { Spread, Pack };
enum class PolicyMode { UNKNOWN, MAX_PERFORMANCE, LOW_POWER, BALANCED };

struct MaybeDouble {
    bool has;
    double value;

    MaybeDouble() : has(false), value(0.0) {}
    explicit MaybeDouble(double v) : has(true), value(v) {}
};

struct MaybeBool {
    bool has;
    bool value;

    MaybeBool() : has(false), value(false) {}
    explicit MaybeBool(bool v) : has(true), value(v) {}
};

struct RLAction {
    PolicyMode mode = PolicyMode::BALANCED;
    MaybeDouble target_fps;
    MaybeDouble power_budget_watts;
    MaybeBool prefer_gpu;
    MaybeDouble gpu_workload_split;
    MaybeDouble cpu_max_freq_khz;

    RLAction() = default;

    RLAction(PolicyMode m, double fps, double watts, bool gpu)
        : mode(m),
          target_fps(fps),
          power_budget_watts(watts),
          prefer_gpu(gpu) {}

    RLAction(PolicyMode m, double fps, double watts, bool gpu,
             double split, double cpu_khz)
        : mode(m),
          target_fps(fps),
          power_budget_watts(watts),
          prefer_gpu(gpu),
          gpu_workload_split(split),
          cpu_max_freq_khz(cpu_khz) {}
};

struct RuntimeControls {
    std::atomic<int> concurrency_level;
    std::atomic<bool> enable_gpu;
    std::atomic<int> governor_hint;
    std::atomic<Affinity> affinity;
    std::atomic<double> gpu_workload_split;

    // Scheduler-transformed CPU maximum-frequency command in kHz.
    // This is a command/cap, not measured hardware frequency.
    std::atomic<long> commanded_cpu_max_freq_khz;

    std::atomic<std::uint32_t> cpu_affinity_mask;

    // Action/effect timing telemetry.
    std::atomic<std::uint64_t> action_generation;
    std::atomic<std::uint64_t> action_apply_end_ns;
    std::atomic<std::uint64_t> algorithm_observed_generation;
    std::atomic<std::uint64_t> algorithm_observed_time_ns;
    std::atomic<std::uint64_t> action_response_generation;
    std::atomic<std::uint64_t> action_response_latency_ns;

    // -----------------------------------------------------------------
    // Dynamic-workload provenance (continual-ERL experiment).
    // -----------------------------------------------------------------
    // requested_workload_epoch is written by EvolutionarySelector while its
    // GA barrier is held. AlgorithmModule copies it to active_workload_epoch
    // only AFTER the old worker has been fully quiesced and BEFORE the
    // replacement worker starts. AlgorithmConcrete then stamps every
    // successfully processed frame with that active epoch.
    //
    // This is intentionally separate from algorithm_observed_generation:
    // that existing field tracks action/effect timing, not workload identity.
    std::atomic<std::uint64_t> requested_workload_epoch;
    std::atomic<std::uint64_t> active_workload_epoch;
    std::atomic<std::uint64_t> algorithm_processed_workload_epoch;
    std::atomic<std::uint64_t> algorithm_processed_frame_id;

    RuntimeControls()
        : concurrency_level(2),
          enable_gpu(true),
          governor_hint(0),
          affinity(Affinity::Spread),
          gpu_workload_split(1.0),
          commanded_cpu_max_freq_khz(0),
          cpu_affinity_mask(0xF),
          action_generation(0),
          action_apply_end_ns(0),
          algorithm_observed_generation(0),
          algorithm_observed_time_ns(0),
          action_response_generation(0),
          action_response_latency_ns(0),
          requested_workload_epoch(0),
          active_workload_epoch(0),
          algorithm_processed_workload_epoch(0),
          algorithm_processed_frame_id(0) {}

    RuntimeControls(const RuntimeControls&) = delete;
    RuntimeControls& operator=(const RuntimeControls&) = delete;
};

} // namespace hrl
