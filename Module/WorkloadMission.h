
//=================================================================================

// ============================================================================
// WorkloadMission.h  runtime workload-switching mission for the continual-ERL
// experiment (one continuous ERL instance, algorithm workload hot-swapped).
//
// EXPERIMENTAL CONTRACT
// ---------------------
//   * Camera, display, Lynsyn, SoC telemetry, metrics aggregation, Scheduler,
//     GeneticAlgorithm, AdaptiveWeightManager and RuntimeControls stay alive.
//   * Only the algorithm workload is replaced at a phase boundary.
//   * The GA generation/population/RNG/adaptive-mutation state and AWM history
//     survive every transition.
//   * The surrogate keeps a separate memory bank per workloadId, preventing
//     cross-workload contamination while allowing true recall when a workload
//     returns (HistogramEqualization at phase 4).
//
// Provides three small pieces, all header-only:
//
//   1. hrl::WorkloadProfile   workload identity/capabilities and surrogate prior.
//   2. hrl::WorkloadMission   parses a phase schedule and owns the mission clock.
//   3. hrl::WorkloadEventLog  durable transition evidence for offline segmentation.
//
// IMPORTANT TIMING RULE
// ---------------------
// Call WorkloadMission::start() when warmup completes. Warmup is intentionally
// outside mission time so the canonical 0300 / 300600 / 600900 / 9001200 s
// phases each contain a full 300 seconds of measured operation.
// ============================================================================
#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

namespace hrl {

// ----------------------------------------------------------------------------
// WorkloadProfile  the contract between the harness and the live ERL.
// ----------------------------------------------------------------------------
struct WorkloadProfile {
    std::string label;                  // Must match stringToAlgorithmType().
    bool        gpuCapable         = true;
    bool        gpuSplitApplicable = false; // true only for genuinely partitioned work.
    double      fpsMax             = 60.0;  // workload-specific surrogate FPS ceiling.
    uint32_t    workloadId         = 0;     // namespaces surrogate memory; 0 = unset.
};

// Stable for the whole process lifetime: the same label always receives the
// same small id, including a later return to an earlier workload. The mutex is
// defensive because the profile may be queried by both harness/control code.
inline uint32_t workloadIdFor(const std::string& label) {
    if (label.empty()) return 0;

    static std::mutex registryMutex;
    static std::vector<std::string> known;

    std::lock_guard<std::mutex> lock(registryMutex);
    for (uint32_t i = 0; i < static_cast<uint32_t>(known.size()); ++i) {
        if (known[i] == label) return i + 1U; // 0 is reserved for "unset".
    }

    known.push_back(label);
    return static_cast<uint32_t>(known.size());
}

inline WorkloadProfile makeWorkloadProfile(const std::string& label,
                                           bool gpuCapable,
                                           double fpsMax = 60.0) {
    if (label.empty()) {
        throw std::invalid_argument("[WorkloadMission] workload label cannot be empty");
    }
    if (!std::isfinite(fpsMax) || fpsMax <= 0.0) {
        throw std::invalid_argument("[WorkloadMission] fpsMax must be finite and > 0");
    }

    WorkloadProfile p;
    p.label              = label;
    p.gpuCapable         = gpuCapable;
    p.gpuSplitApplicable = (label == "HeterogeneousGaussianBlur");
    p.fpsMax             = fpsMax;
    p.workloadId         = workloadIdFor(label);
    return p;
}

// ----------------------------------------------------------------------------
// WorkloadMission  ordered phase schedule with an explicit clock.
// ----------------------------------------------------------------------------
class WorkloadMission {
public:
    struct Phase {
        double      startSec = 0.0; // relative to mission clock start.
        std::string label;
    };

    WorkloadMission() = default;

    // Spec: "t0:LabelA,t1:LabelB,...". Times must be finite, non-negative,
    // strictly increasing and the first phase must start at exactly zero.
    static WorkloadMission parse(const std::string& spec) {
        WorkloadMission m;
        std::stringstream ss(spec);
        std::string tok;

        while (std::getline(ss, tok, ',')) {
            trim_(tok);
            if (tok.empty()) continue;

            const std::string::size_type colon = tok.find(':');
            if (colon == std::string::npos) {
                throw std::runtime_error("[WorkloadMission] bad token '" + tok + "'");
            }

            Phase ph;
            try {
                ph.startSec = std::stod(tok.substr(0, colon));
            } catch (const std::exception&) {
                throw std::runtime_error("[WorkloadMission] bad phase time in token '" + tok + "'");
            }

            ph.label = tok.substr(colon + 1);
            trim_(ph.label);

            if (!std::isfinite(ph.startSec) || ph.startSec < 0.0) {
                throw std::runtime_error("[WorkloadMission] phase time must be finite and >= 0");
            }
            if (ph.label.empty()) {
                throw std::runtime_error("[WorkloadMission] phase workload label cannot be empty");
            }
            if (!m.phases_.empty() && ph.startSec <= m.phases_.back().startSec) {
                throw std::runtime_error("[WorkloadMission] phase times must strictly increase");
            }

            m.phases_.push_back(ph);
        }

        if (m.phases_.empty()) {
            throw std::runtime_error("[WorkloadMission] empty mission spec");
        }
        if (std::abs(m.phases_.front().startSec) > 1e-9) {
            throw std::runtime_error("[WorkloadMission] first phase must start at t=0");
        }

        return m;
    }

    bool enabled() const { return !phases_.empty(); }
    bool hasTransitions() const { return phases_.size() >= 2; }

    const std::string& initialLabel() const { return phases_.front().label; }
    const std::string& currentLabel() const { return phases_.at(currentIdx_).label; }
    const Phase& currentPhase() const { return phases_.at(currentIdx_); }
    const std::vector<Phase>& phases() const { return phases_; }
    size_t currentPhaseIndex() const { return currentIdx_; }
    size_t phaseCount() const { return phases_.size(); }

    // Start (or restart) the mission clock  call at WARMUP COMPLETE.
    void start(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        clockStart_ = now;
        started_    = true;
        currentIdx_ = 0;
    }

    bool started() const { return started_; }

    double elapsedSec(std::chrono::steady_clock::time_point now =
                          std::chrono::steady_clock::now()) const {
        if (!started_) return 0.0;
        return std::chrono::duration<double>(now - clockStart_).count();
    }

    // Returns true until the caller commits the boundary with confirmAdvanced().
    // Keeping advancement explicit makes failed hot-swaps recoverable: a failed
    // transition does not silently move the mission to a workload that never ran.
    bool dueTransition(std::chrono::steady_clock::time_point now,
                       Phase& next) const {
        if (!started_ || currentIdx_ + 1 >= phases_.size()) return false;
        if (elapsedSec(now) >= phases_[currentIdx_ + 1].startSec) {
            next = phases_[currentIdx_ + 1];
            return true;
        }
        return false;
    }

    void confirmAdvanced() {
        if (currentIdx_ + 1 < phases_.size()) ++currentIdx_;
    }

    double lastBoundarySec() const { return phases_.back().startSec; }

private:
    static bool whitespace_(char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }

    static void trim_(std::string& s) {
        while (!s.empty() && whitespace_(s.front())) s.erase(s.begin());
        while (!s.empty() && whitespace_(s.back()))  s.pop_back();
    }

    std::vector<Phase> phases_;
    size_t currentIdx_ = 0;
    bool started_ = false;
    std::chrono::steady_clock::time_point clockStart_{};
};

// ----------------------------------------------------------------------------
// Workload transition evidence.
// ----------------------------------------------------------------------------
struct WorkloadTransitionEvidence {
    double      missionElapsedSec     = 0.0;
    size_t      phaseIndex            = 0;
    std::string fromWorkload;
    std::string toWorkload;
    double      transitionTotalMs     = 0.0; // begin barrier -> ERL context commit
    double      swapGapMs             = 0.0; // AlgorithmConcrete stop -> replacement start
    double      firstNewFrameLatencyMs = -1.0; // transition begin -> first proven replacement frame
    uint64_t    inputFramesDrained    = 0;
    uint64_t    outputFramesDrained   = 0;
    uint64_t    frameIdBefore         = 0;
    uint64_t    frameIdAfter          = 0;
    uint64_t    generationBefore      = 0;
    uint64_t    generationAfter       = 0;
    uint64_t    workloadEpochBefore   = 0;
    uint64_t    workloadEpochAfter    = 0;
    uint64_t    algorithmProcessedEpochAfter = 0;
    uint64_t    algorithmProcessedFrameAfter = 0;
    uint64_t    surrogateObsBefore    = 0;
    uint64_t    surrogateObsAfter     = 0;
    std::string awmRegimeBefore;
    std::string awmRegimeAfter;
};

// ----------------------------------------------------------------------------
// WorkloadEventLog  one durable row per transition.
// ----------------------------------------------------------------------------
// This file is the segmentation key for metrics/telemetry/Lynsyn/action/Pareto
// streams. It additionally proves controller continuity: generation and AWM
// regime are sampled on both sides of the algorithm-only hot swap.
class WorkloadEventLog {
public:
    bool open(const std::string& path) {
        out_.open(path, std::ios::out | std::ios::trunc);
        if (!out_.is_open()) {
            spdlog::error("[WorkloadEventLog] cannot open '{}'", path);
            return false;
        }

        out_ << "wall_time_ms,mission_elapsed_s,phase_index,"
                "from_workload,to_workload,transition_total_ms,algorithm_downtime_ms,"
                "first_new_frame_latency_ms,input_frames_drained,output_frames_drained,"
                "frame_id_before,frame_id_after,"
                "generation_before,generation_after,"
                "workload_epoch_before,workload_epoch_after,"
                "algorithm_processed_epoch_after,algorithm_processed_frame_after,"
                "surrogate_obs_before,surrogate_obs_after,"
                "awm_regime_before,awm_regime_after\n";
        out_.flush();
        return true;
    }

    void write(const WorkloadTransitionEvidence& e) {
        if (!out_.is_open()) return;

        // system_clock is used because this column is an actual wall-clock epoch
        // timestamp. Mission timing itself remains steady_clock-based and immune
        // to NTP/system-time adjustments.
        const int64_t wall = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        out_ << wall << ',' << e.missionElapsedSec << ',' << e.phaseIndex << ','
             << csv_(e.fromWorkload) << ',' << csv_(e.toWorkload) << ','
             << e.transitionTotalMs << ',' << e.swapGapMs << ','
             << e.firstNewFrameLatencyMs << ','
             << e.inputFramesDrained << ',' << e.outputFramesDrained << ','
             << e.frameIdBefore << ',' << e.frameIdAfter << ','
             << e.generationBefore << ',' << e.generationAfter << ','
             << e.workloadEpochBefore << ',' << e.workloadEpochAfter << ','
             << e.algorithmProcessedEpochAfter << ',' << e.algorithmProcessedFrameAfter << ','
             << e.surrogateObsBefore << ',' << e.surrogateObsAfter << ','
             << csv_(e.awmRegimeBefore) << ',' << csv_(e.awmRegimeAfter) << '\n';
        out_.flush(); // transitions are rare; durability is more valuable here.
    }

    // Compatibility overload matching the original lightweight interface.
    void write(double missionElapsedSec, size_t phaseIndex,
               const std::string& from, const std::string& to,
               double swapGapMs,
               uint64_t frameIdBefore, uint64_t frameIdAfter,
               uint64_t erlGenerationHint) {
        WorkloadTransitionEvidence e;
        e.missionElapsedSec = missionElapsedSec;
        e.phaseIndex = phaseIndex;
        e.fromWorkload = from;
        e.toWorkload = to;
        e.transitionTotalMs = swapGapMs;
        e.swapGapMs = swapGapMs;
        e.frameIdBefore = frameIdBefore;
        e.frameIdAfter = frameIdAfter;
        e.generationBefore = erlGenerationHint;
        e.generationAfter = erlGenerationHint;
        write(e);
    }

private:
    static std::string csv_(const std::string& s) {
        if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
        std::string out = "\"";
        for (char c : s) {
            if (c == '\"') out += "\"\"";
            else out += c;
        }
        out += '\"';
        return out;
    }

    std::ofstream out_;
};

} // namespace hrl
