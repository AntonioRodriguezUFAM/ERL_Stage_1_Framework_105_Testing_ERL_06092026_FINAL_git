


//

//================================================== GeneticAlgorithm_05.h ===========================================================
// FINAL GOLD STANDARD ? PhD-Ready (Merged Best of GA_03 + GA_04)
// Pareto + Scalar Hybrid | Clean | Complete | Jetson Nano Optimized
// GeneticAlgorithm_05.h - FINAL GOLD STANDARD ? PhD-Ready
//=====================================================================================================================================

#pragma once

#include <vector>
#include <random>
#include <algorithm>
#include <limits>
#include <memory>
#include <cmath>
#include <chrono>
#include <string>
#include <unordered_map>
#include <utility>
#include <spdlog/spdlog.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>

#include "RuntimeControls.h"
#include "../Stage_01/SharedStructures/allModulesStatcs.h"
#include "IScheduler.h"
#include "AdaptiveWeightManager.h"
#include "PowerSanity.h"          // physical-plausibility check used by headroom recovery
#include "SurrogateModel.h"   // online counterfactual fitness surrogate (thesis gap 2)

#include "../Stage_01/Concretes/SystemMetricsAggregatorConcrete_v3_2.h"
#include "../Stage_01/Others/utils.h"

namespace hrl {

// ===================================================================
// GAConfig
// ===================================================================
struct GAConfig {
    size_t   popSize             = 12;
    size_t   minPopSize          = 6;
    size_t   eliteCount          = 3;
    double   crossoverRate       = 0.75;
    double   mutationRate        = 0.18;   // Base / floor mutation rate
    double   explorationDecay    = 0.96;
    int      controlIntervalMs   = 800;
    size_t   reductionStartGen   = 10;
    double   reductionRatio      = 0.6;

    // [Adaptive mutation] Plateau-driven mutation control (extracted from
    // AdaptivePopulationManager, made Pareto-safe ? it scales the rate only,
    // never touches selection, so it composes cleanly with NSGA-II).
    // When best fitness improves, mutation eases toward exploit; when it
    // plateaus, mutation ramps up toward maxMutationRate to escape stagnation.
    bool   adaptiveMutationEnabled = true;
    double maxMutationRate         = 0.35;   // Hard cap when fully plateaued
    // Relative improvement threshold (fraction of |prev best fitness|) below
    // which a generation counts as "stagnating". Scaled to the fitness signal
    // (which sits in the ~±0.05?0.20 band) rather than the old absolute 0.01.
    double mutationPlateauRelThreshold = 0.02;   // 2% relative improvement
    double mutationPlateauAbsFloor     = 0.0005; // abs floor for near-zero fitness
    double mutationImproveFactor       = 0.8;    // multiply base when improving
    double mutationPlateauStep         = 0.10;   // +10% of base per plateau gen

    // [Surrogate] Online counterfactual fitness (thesis gap 2). When enabled, all
    // candidates are scored by a learned per-config surrogate instead of the shared
    // measured snapshot, and under-explored configs receive an optimism bonus.
    // Disable for the ablation baseline (surrogate-off vs surrogate-on).
    bool   surrogateEnabled       = true;
    double surrogateExploreWeight = 0.15;   // UCB bonus scale in fitness units

    // [FIX P2] Reality gating for counterfactual fitness.
    int    surrogateMinSupport      = 3;    // below this support, predictions are pessimised
    double surrogateColdFpsCeiling  = 8.0;  // fps ceiling before any real measurement exists
    double surrogatePredFpsCapRatio = 1.2;  // pred_fps <= ratio * best measured fps this run
    // [FIX P7] Physics-prior FPS ceiling, per workload (was hard-coded 60 for all).
    double surrogateFpsMax          = 60.0;

    // [FINAL FIX L1] Processing latency is a separate physical quantity from
    // frame period (1000/FPS).  The surrogate therefore needs an independent
    // cold-start latency prior instead of deriving latency from throughput.
    double surrogateLatencyPriorMs  = 10.0;

    // [FINAL FIX F1] Exact-bucket feasibility gate.  Repeated bounded evidence
    // timeouts are treated as a constraint violation, not merely a soft scalar
    // penalty.  This prevents a low-power configuration that repeatedly stalls
    // from remaining Rank-0 because of attractive power/temperature objectives.
    bool     hardFeasibilityEnabled         = true;
    uint32_t hardFeasibilityMinAttempts     = 3;
    double   hardFeasibilityMaxTimeoutRatio = 0.50;

    // [FINAL FIX D1] Preserve one representative of each macro policy mode in
    // every offspring population.  Representation does NOT imply admissibility:
    // a thermally unsafe MAX candidate may remain genetically available while
    // constraint-domination prevents it from being selected.
    bool enforceModeDiversity = true;

    // [FINAL FIX H1] When measured FPS is below the performance threshold while
    // physically-valid power and temperatures still have headroom, LOW_POWER is
    // temporarily inadmissible.  This encodes the intended control semantics:
    // "spend available resource headroom to recover required performance".
    bool   performanceHeadroomConstraint = true;
    double performanceRecoveryRatio       = 0.70;
    double performancePowerHeadroomW      = 0.50;
    double performanceThermalHeadroomC    = 2.0;

    // [FINAL FIX T1] In a measured THERMAL_CRITICAL state, MAX_PERFORMANCE is
    // a hard feasibility violation rather than only another weighted penalty.
    bool thermalCriticalBlocksMax = true;

    // Whether the active algorithm physically consumes the continuous GPU split.
    // ConfigManager sets this true only for HeterogeneousGaussianBlur. For
    // MedianFilter and other binary CPU/GPU algorithms, the surrogate uses the
    // canonical split {0,1} implied by GPU state so identical hardware states
    // cannot acquire different model buckets.
    // Fail closed: continuous split is opt-in.  ConfigManager/EvolutionarySelector
    // must set true explicitly for HeterogeneousGaussianBlur.  Binary workloads
    // (HistogramEqualization, MedianFilter, SobelEdge, etc.) remain canonical {0,1}.
    bool gpuSplitApplicable = false;
    bool workloadGpuCapable = true;

    // Runtime workload identity for the continuous dynamic-adaptation mission.
    // This is controller context, not a reset trigger. workloadId namespaces
    // the surrogate memory so evidence from different algorithms never mixes.
    std::string activeWorkload = "UNSPECIFIED";
    uint32_t    activeWorkloadId = 0;

    // Pareto evidence export for PhD validation.
    // Disabled by default to preserve normal runtime behaviour.
    bool        export_pareto_evidence = false;
    std::string pareto_csv_path = "output/erl_pareto_front.csv";
    std::string action_csv_path = "output/erl_action_trace.csv";

    // Do not let missing Lynsyn/power block Pareto evidence generation.
    // On Jetson runs the power stream often starts later than FPS/camera metrics.
    int    stableFramesRequired = 1;
    double minFpsForEvolution = 1.0;
    double minPowerForEvolution = 0.0;
    bool   requirePowerForEvolution = false;

    // AdaptiveWeightManager (opt-in ? default preserves current behaviour)
    bool                           adaptive_weights_enabled = false;
    AdaptiveWeightManager::Config  awm_config               = {};

    // Reproducibility: if rngSeed >= 0, the GA seeds std::mt19937 deterministically.
    // If rngSeed < 0 (default), it falls back to std::random_device for entropy.
    // Set this (e.g. from config "seed") so ERL runs are seed-stable for the thesis.
    long rngSeed = -1;

    // Search-space floor for target_fps gene. Raised from the old hardcoded 8.0
    // so the population cannot collapse onto the ~8 fps low-clock corner.
    // Applied in initializePopulation(), mutate(), and crossover().
    double minTargetFps = 20.0;
    double maxTargetFps = 60.0;

    // Workload policy. SobelEdge is GPU-dominant on the validated Jetson Nano
    // data; ConfigManager enables this flag only for SobelEdge by default.
    // ThermalGovernor remains authoritative and may still revoke GPU access.
    bool   forceGpuForWorkload = false;

    // Fixed performance reference used by HIGH_PERFORMANCE AWM/objective logic.
    // It is intentionally independent of the evolvable target_fps gene.
    double performanceTargetFps = 60.0;

    // [PhD FIX] Explicit scalarization weights for the fitness function.
    // These bridge the gap between EvolutionarySelector config and the GA's scalar fitness.
    double weight_fps     = 1.0;
    double weight_power   = 0.5;
    double weight_temp    = 0.5;
    double weight_latency = 0.3;

    // [P0-F17] Temperature-objective onset thresholds. The old hard-coded
    // 75C/80C onsets in calculateObjectives() were unreachable on Jetson
    // Nano (39-65C observed across ERL and baselines), so objs[2] was
    // identically zero and temperature never influenced dominance, crowding,
    // or fitness. Defaults track the calibrated AdaptiveWeightManager warn
    // thresholds from AdaptiveWeightManager::Config as the default source of truth;
    // override per run via
    // obj_temp_onset_cpu_c / obj_temp_onset_gpu_c in the Scheduler block.
    double temp_onset_cpu_c = AdaptiveWeightManager::Config().thermal_warn_cpu_c;
    double temp_onset_gpu_c = AdaptiveWeightManager::Config().thermal_warn_gpu_c;

};

// ===================================================================
// PhD timing instrumentation: per-generation GA phase breakdown.
// All values are wall-clock milliseconds measured with steady_clock.
// total_compute_ms excludes the final selected-action CSV append; the outer
// EvolutionarySelector separately measures complete controller active time.
// ===================================================================
struct GATimingMetrics {
    double weight_update_ms{0.0};
    double evaluation_ms{0.0};
    double nsga_sort_ms{0.0};
    double selection_ms{0.0};
    double scheduler_apply_ms{0.0};
    double action_dispatch_overhead_ms{0.0};
    double pareto_export_ms{0.0};
    double diagnostics_ms{0.0};
    double mutation_ms{0.0};
    double offspring_ms{0.0};
    double postprocess_ms{0.0};
    double total_compute_ms{0.0};
    bool valid{false};
};

// ===================================================================
// ParetoIndividual (Single struct ? Dual Evaluation)
// ===================================================================
struct ParetoIndividual {
    RLAction genome;

    // Multi-objective (all minimized)
    std::vector<double> objectives;   // [normFPS, normEnergy, normTemp, normLatency]

    // Scalarized single fitness (for ranking & elitism)
    double fitness = -std::numeric_limits<double>::max();

    // NSGA-II
    double crowdingDistance = 0.0;
    int    rank = 0;

    // [FINAL FIX F1/T1/H1] Constraint-domination state.
    // Feasible candidates always dominate infeasible candidates.  Among
    // infeasible candidates, smaller violation is preferred before normal
    // objective dominance is considered.
    bool   feasible = true;
    double constraintViolation = 0.0;

    // History
    int    age = 0;
    int    generation = 0;

    // [PhD deterministic telemetry] Prediction/provenance used for THIS exact
    // candidate evaluation. These fields are copied with the selected individual,
    // preserving the decision rationale even after the population is replaced.
    SurrogatePrediction surrogatePrediction{};
    bool   surrogateUsed = false;
    double surrogateExplorationBonus = 0.0;
    double surrogateStallPenalty = 0.0;
    double surrogateConfigFreqKhz = 0.0;
    bool   surrogateConfigGpu = false;
    double surrogateConfigGpuSplit = 0.0;
    int    surrogateConfigConcurrency = 0;

    // Exact scalar-fitness decomposition used to rank this candidate.
    std::array<double, 4> fitnessWeights{{0.0, 0.0, 0.0, 0.0}};
    double fitFpsComponent = 0.0;
    double fitPowerComponent = 0.0;
    double fitTempComponent = 0.0;
    double fitLatencyComponent = 0.0;
    double fitGpuBonus = 0.0;
    double fitBeforeAge = 0.0;
    double fitAgeFactor = 1.0;
    double fitAfterAge = 0.0;
    double fitFinal = -std::numeric_limits<double>::max();

    ParetoIndividual() = default;

    bool operator<(const ParetoIndividual& other) const {
        if (rank != other.rank) return rank < other.rank;
        return crowdingDistance > other.crowdingDistance;
    }

    bool dominates(const ParetoIndividual& other) const;

    // bool dominates(const ParetoIndividual& other) const {
    //     if (objectives.size() != other.objectives.size()) return false;
    //     bool strictlyBetter = false;
    //     for (size_t i = 0; i < objectives.size(); ++i) {
    //         if (objectives[i] > other.objectives[i]) return false;
    //         if (objectives[i] < other.objectives[i]) strictlyBetter = true;
    //     }
    //     return strictlyBetter;
    // }
};

// ===================================================================
// GeneticAlgorithm
// Description: 
// ===================================================================
class GeneticAlgorithm {
public:
    // Overloaded constructor accepting awmCfg  
    // GeneticAlgorithm(const GAConfig& cfg,
    //                  std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator,
    //                  std::shared_ptr<hrl::RuntimeControls> runtimeControls,
    //                  IScheduler* scheduler = nullptr,
    //                  const hrl::AdaptiveWeightManager::Config& awmCfg)

    // Four-argument production constructor: preserves cfg.awm_config exactly.
    // This avoids the old API trap where omitting the fifth argument silently
    // replaced cfg.awm_config with a fresh AdaptiveWeightManager::Config{}.
    GeneticAlgorithm(const GAConfig& cfg,
                     std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator,
                     std::shared_ptr<hrl::RuntimeControls> runtimeControls,
                     IScheduler* scheduler = nullptr)
        : GeneticAlgorithm(cfg, std::move(aggregator), std::move(runtimeControls),
                           scheduler, cfg.awm_config)
    {}

    // Explicit five-argument constructor used when the caller intentionally
    // supplies an AWM configuration separate from the GAConfig copy.
    GeneticAlgorithm(const GAConfig& cfg,
                     std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator,
                     std::shared_ptr<hrl::RuntimeControls> runtimeControls,
                     IScheduler* scheduler,
                     const hrl::AdaptiveWeightManager::Config& awmCfg)
        : cfg_(cfg),
          aggregator_(std::move(aggregator)),
          runtimeControls_(std::move(runtimeControls)),
          rng_(cfg.rngSeed >= 0
                   ? static_cast<std::mt19937::result_type>(cfg.rngSeed)
                   : std::random_device{}()),
          scheduler_(scheduler),
          generation_(0),
          stableFrames_(0),
          surrogate_(makeSurrogateConfig(cfg))
    {
        // One authoritative AWM configuration for feasibility, regime detection,
        // headroom recovery and evidence telemetry.
        cfg_.awm_config = awmCfg;

        // Establish the initial workload bank before the first population is
        // evaluated.  The same SurrogateModel object remains alive for the full
        // mission; later workload changes only switch its active bank.
        currentWorkload_ = cfg_.activeWorkload.empty()
            ? std::string("UNSPECIFIED") : cfg_.activeWorkload;
        currentWorkloadId_ = cfg_.activeWorkloadId;
        surrogate_.setWorkloadContext(currentWorkloadId_,
                                      currentWorkload_,
                                      cfg_.gpuSplitApplicable,
                                      cfg_.surrogateFpsMax,
                                      cfg_.surrogateLatencyPriorMs);
        workloadMaxMeasuredFps_[currentWorkloadId_] = 0.0;

        if (cfg_.popSize == 0) {
            spdlog::warn("[GeneticAlgorithm] popSize=0 is invalid; forcing popSize=1");
            cfg_.popSize = 1;
        }
        cfg_.eliteCount = std::min(cfg_.eliteCount, cfg_.popSize);
        cfg_.minPopSize = std::max<size_t>(1, std::min(cfg_.minPopSize, cfg_.popSize));
        cfg_.crossoverRate = utils::local_clamp(cfg_.crossoverRate, 0.0, 1.0);
        cfg_.mutationRate = utils::local_clamp(cfg_.mutationRate, 0.0, 1.0);
        cfg_.maxMutationRate = utils::local_clamp(cfg_.maxMutationRate,
                                                   cfg_.mutationRate, 1.0);
        cfg_.hardFeasibilityMaxTimeoutRatio =
            utils::local_clamp(cfg_.hardFeasibilityMaxTimeoutRatio, 0.0, 1.0);
        cfg_.performanceRecoveryRatio =
            utils::local_clamp(cfg_.performanceRecoveryRatio, 0.0, 1.0);
        if (cfg_.minTargetFps > cfg_.maxTargetFps) {
            std::swap(cfg_.minTargetFps, cfg_.maxTargetFps);
        }

        if (cfg_.enforceModeDiversity && cfg_.popSize < 3) {
            spdlog::warn("[GeneticAlgorithm] Mode diversity requested with popSize={}; "
                         "at least 3 individuals are required for MAX/BALANCED/LOW",
                         cfg_.popSize);
        }

        if (cfg_.rngSeed >= 0) {
            spdlog::info("[GeneticAlgorithm] Deterministic RNG seed = {}", cfg_.rngSeed);
        } else {
            spdlog::info("[GeneticAlgorithm] Non-deterministic RNG (random_device)");
        }

        // Configuration audit: these values materially change ERL behavior and
        // must be visible before a data-producing run.
        spdlog::info(
            "[GA-Config] gpu_split_applicable={} | AWM warn CPU/GPU={:.1f}/{:.1f}C "
            "crit CPU/GPU={:.1f}/{:.1f}C | obj-temp onset CPU/GPU={:.1f}/{:.1f}C",
            cfg_.gpuSplitApplicable ? 1 : 0,
            cfg_.awm_config.thermal_warn_cpu_c,
            cfg_.awm_config.thermal_warn_gpu_c,
            cfg_.awm_config.thermal_crit_cpu_c,
            cfg_.awm_config.thermal_crit_gpu_c,
            cfg_.temp_onset_cpu_c,
            cfg_.temp_onset_gpu_c);

        if (std::abs(cfg_.temp_onset_cpu_c - cfg_.awm_config.thermal_warn_cpu_c) > 1e-9 ||
            std::abs(cfg_.temp_onset_gpu_c - cfg_.awm_config.thermal_warn_gpu_c) > 1e-9) {
            spdlog::warn(
                "[GA-Config] Temperature objective onsets differ from AWM warning thresholds. "
                "This is allowed, but confirm it is intentional before collecting data.");
        }

        if (cfg_.gpuSplitApplicable) {
            spdlog::warn(
                "[GA-Config] Continuous GPU split is ENABLED. Confirm the active workload "
                "physically partitions one frame across CPU/GPU (currently expected only "
                "for HeterogeneousGaussianBlur).");
        }

        currentMutationRate_ = cfg_.mutationRate;
        initializePopulation();
        if (cfg_.export_pareto_evidence) ensureEvidenceFiles();

        spdlog::info(
            "[GeneticAlgorithm] Pareto-ERL ready (pop={}, reduction_gen={}, evidence={}, "
            "force_gpu={}, perf_target={:.1f})",
            cfg_.popSize, cfg_.reductionStartGen,
            cfg_.export_pareto_evidence ? "ON" : "OFF",
            cfg_.forceGpuForWorkload ? "YES" : "NO",
            cfg_.performanceTargetFps);

        if (cfg_.adaptive_weights_enabled) {
            weightManager_ = std::make_unique<hrl::AdaptiveWeightManager>(cfg_.awm_config);
            spdlog::info(
                "[GeneticAlgorithm] AdaptiveWeightManager ENABLED "
                "(update_interval={} gens, smoothing={:.2f})",
                cfg_.awm_config.update_interval_gens,
                cfg_.awm_config.transition_smoothing);
        }
    }

//================================================================================================
// [FIX P5] Called when the selector force-applies a recovery configuration
    // outside the GA's control: the next evidence window must not be credited
    // to the previously selected genome (it did not produce those frames).
    void notifyExternalOverride() { hasLastApplied_ = false; }

    // =====================================================================
    // Runtime workload-context update WITHOUT controller reset.
    // =====================================================================
    // Preserved across this call:
    //   generation_, population_, RNG stream, adaptive-mutation state,
    //   AdaptiveWeightManager object/history, Scheduler and RuntimeControls.
    //
    // Changed in place:
    //   workload identity, split capability and workload-specific surrogate prior.
    // The surrogate itself is never reconstructed; it switches to a retained
    // workload-specific bucket bank so recurrence can reuse prior evidence.
    void setWorkloadContext(uint32_t workloadId,
                            const std::string& name,
                            bool gpuCapable,
                            bool gpuSplitApplicable,
                            double surrogateFpsMax)
    {
        const std::string next = name.empty() ? std::string("UNSPECIFIED") : name;
        const std::string previous = currentWorkload_;
        const uint32_t previousId = currentWorkloadId_;

        currentWorkload_ = next;
        currentWorkloadId_ = workloadId;
        cfg_.activeWorkload = next;
        cfg_.activeWorkloadId = workloadId;
        cfg_.workloadGpuCapable = gpuCapable;
        cfg_.gpuSplitApplicable = gpuCapable && gpuSplitApplicable;
        if (std::isfinite(surrogateFpsMax) && surrogateFpsMax > 0.0) {
            cfg_.surrogateFpsMax = surrogateFpsMax;
        }

        // IMPORTANT: the SurrogateModel object itself is preserved. Only its
        // active workload bank/prior changes. A return to the same workloadId
        // reactivates the previously learned bank instead of cold-starting.
        surrogate_.setWorkloadContext(currentWorkloadId_,
                                      currentWorkload_,
                                      cfg_.gpuSplitApplicable,
                                      cfg_.surrogateFpsMax,
                                      cfg_.surrogateLatencyPriorMs);

        // Best measured FPS is also workload-conditioned. This avoids allowing
        // a fast Histogram observation to cap/pessimise Sobel predictions while
        // still restoring Histogram's prior evidence on phase-4 recurrence.
        maxMeasuredFps_ = workloadMaxMeasuredFps_[currentWorkloadId_];

        // Existing population survives. Only genes that are physically invalid
        // under the new workload are canonicalised (fractional split -> {0,1}
        // for binary algorithms). No population reinitialisation occurs.
        for (auto& individual : population_) {
            canonicalizeGenomeForWorkload(individual.genome);
        }

        // The pre-transition selected action did not produce post-transition
        // measurements. Drop only that temporal credit edge; all learning state
        // (generation, population, RNG, mutation state, AWM history) survives.
        hasLastApplied_ = false;
        ++workloadEpoch_;

        spdlog::info(
            "[GA-WORKLOAD] id={} '{}' -> id={} '{}' | epoch={} | "
            "generation={} PRESERVED | population={} PRESERVED | AWM={} PRESERVED | "
            "split={} | recalled_surrogate_obs={}",
            previousId, previous, currentWorkloadId_, currentWorkload_,
            workloadEpoch_, generation_, population_.size(),
            weightManager_ ? weightManager_->getCurrentRegimeName() : "FIXED_WEIGHTS",
            cfg_.gpuSplitApplicable ? "continuous" : "binary",
            surrogate_.currentWorkloadObservations());
    }

    const std::string& getCurrentWorkload() const { return currentWorkload_; }
    uint32_t getCurrentWorkloadId() const { return currentWorkloadId_; }
    uint64_t getWorkloadEpoch() const { return workloadEpoch_; }
    uint64_t getCurrentWorkloadSurrogateObservations() const {
        return surrogate_.currentWorkloadObservations();
    }

    std::string exportSurrogateCSV() const { return surrogate_.exportCSV(); }

void evolve(const hrl::MetricsSnapshot& snapshot,
            bool creditPreviousAction = true,
            bool previousActionStalled = false) {
    // if (!isSnapshotStable(snapshot)) {
    //     lastTiming_ = GATimingMetrics{};
    //     return;
    // }
    // [FINAL FIX F2] A timeout must still reach evaluateParetoPopulation()
    // so the exact applied bucket receives observeStarvation(). The timeout
    // is feasibility evidence only; it is no longer injected into the physical
    // FPS/latency EMA.
    if (!previousActionStalled && !isSnapshotStable(snapshot)) {
        lastTiming_ = GATimingMetrics{};
        return;
    }

    using TimingClock = std::chrono::steady_clock;
    const auto totalStart = TimingClock::now();
    lastTiming_ = GATimingMetrics{};

    auto elapsedMs = [](const TimingClock::time_point& a,
                        const TimingClock::time_point& b) -> double {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };

    // ============================================================
    // 0) Adaptive-weight update
    // ============================================================
    auto phaseStart = TimingClock::now();
    // [FINAL FIX A1] Regime classification is a controller-state decision,
    // not a surrogate-credit decision.  Even when the previous action timed out,
    // the latest valid snapshot must still be allowed to trigger
    // HIGH_PERFORMANCE / thermal recovery.  Only surrogate learning is gated by
    // creditPreviousAction below.
    if (weightManager_ && snapshot.valid) {
        // HIGH_PERFORMANCE detection uses a fixed experimental target, never an
        // evolvable low target_fps gene.
        const double targetFps = cfg_.performanceTargetFps;

        bool changed = weightManager_->update(
            snapshot,
            static_cast<int>(generation_),
            targetFps
        );

        if (changed) {
            spdlog::info(
                "[GeneticAlgorithm] Weight regime -> {} | weights=[{:.3f},{:.3f},{:.3f},{:.3f}]",
                weightManager_->getCurrentRegimeName(),
                weightManager_->getWeights()[0],
                weightManager_->getWeights()[1],
                weightManager_->getWeights()[2],
                weightManager_->getWeights()[3]
            );
        }
    }
    auto phaseEnd = TimingClock::now();
    lastTiming_.weight_update_ms = elapsedMs(phaseStart, phaseEnd);

    // ============================================================
    // 1) Evaluate current population
    // ============================================================
    phaseStart = TimingClock::now();
    evaluateParetoPopulation(snapshot, creditPreviousAction, previousActionStalled);
    phaseEnd = TimingClock::now();
    lastTiming_.evaluation_ms = elapsedMs(phaseStart, phaseEnd);

    // ============================================================
    // 2) NSGA-II ranking + crowding
    // ============================================================
    phaseStart = TimingClock::now();
    nonDominatedSortAndCrowding();
    phaseEnd = TimingClock::now();
    lastTiming_.nsga_sort_ms = elapsedMs(phaseStart, phaseEnd);
    lastEvaluatedRank0Count_ = countRank0();

    // ============================================================
    // 3) Select evaluated Rank-0 action
    // ============================================================
    phaseStart = TimingClock::now();
    const ParetoIndividual* selected = selectBestEvaluatedPareto();
    ParetoIndividual selectedCopy;
    const bool haveSelected = (selected != nullptr);

    // Capture how decisive the Rank-0 choice was BEFORE population replacement.
    // Mandatory reset: without this, a generation with no runner-up inherits
    // the previous generation's rank0_runnerup_fitness in the CSV.
    lastRank0RunnerUpFitness_ = std::numeric_limits<double>::quiet_NaN();
    lastSelectionMargin_ = std::numeric_limits<double>::quiet_NaN();
    if (haveSelected) {
        double runnerUp = -std::numeric_limits<double>::max();
        bool haveRunnerUp = false;
        for (const auto& ind : population_) {
            if (ind.rank != 0 || &ind == selected) continue;
            if (ind.feasible != selected->feasible) continue;
            if (!haveRunnerUp || ind.fitness > runnerUp) {
                runnerUp = ind.fitness;
                haveRunnerUp = true;
            }
        }
        if (haveRunnerUp) {
            lastRank0RunnerUpFitness_ = runnerUp;
            lastSelectionMargin_ = selected->fitness - runnerUp;
        }
        selectedCopy = *selected;  // Preserve evidence after population replacement.
    }
    phaseEnd = TimingClock::now();
    lastTiming_.selection_ms = elapsedMs(phaseStart, phaseEnd);

    // ============================================================
    // 4) Scheduler / runtime actuation
    // ============================================================
    phaseStart = TimingClock::now();
    if (haveSelected) {
        lastTiming_.scheduler_apply_ms = applySelectedParetoToRuntime(selectedCopy, snapshot);
    }
    phaseEnd = TimingClock::now();
    const double actionDispatchTotalMs = elapsedMs(phaseStart, phaseEnd);
    lastTiming_.action_dispatch_overhead_ms = std::max(
        0.0, actionDispatchTotalMs - lastTiming_.scheduler_apply_ms);

    // ============================================================
    // 5) Pareto-front evidence (kept before mutation/replacement)
    // ============================================================
    phaseStart = TimingClock::now();
    exportParetoEvidence(snapshot, selected);
    phaseEnd = TimingClock::now();
    lastTiming_.pareto_export_ms = elapsedMs(phaseStart, phaseEnd);

    phaseStart = TimingClock::now();
    logDominanceAnalysis();
    logParetoStats();
    phaseEnd = TimingClock::now();
    lastTiming_.diagnostics_ms = elapsedMs(phaseStart, phaseEnd);

    // ============================================================
    // 5b) Adaptive mutation
    // ============================================================
    phaseStart = TimingClock::now();
    {
        // [P0-F18] Use the exact same deployment semantics for the mutation
        // progress signal. Feasible => maximize scalar fitness. All-infeasible
        // => minimize constraint violation (represented as -violation because
        // updateAdaptiveMutationRate() interprets larger values as improvement).
        const ParetoIndividual* mutationReference = selectBestEvaluatedPareto();
        if (mutationReference) {
            const bool usingConstraintSignal = !mutationReference->feasible;
            const double mutationSignal = usingConstraintSignal
                ? -mutationReference->constraintViolation
                : mutationReference->fitness;

            const double r = updateAdaptiveMutationRate(mutationSignal);
            if (generation_ % 10 == 0) {
                spdlog::info("[GA-Pareto] Adaptive mutation rate={:.3f} "
                             "(plateau={}, signal={:.4f}, source={})",
                             r, mutationPlateauCounter_, mutationSignal,
                             usingConstraintSignal ? "constraint" : "fitness");
            }
        } else {
            // No evaluated candidate exists; retain the current rate rather than
            // inventing a signal from population_[0].
            spdlog::warn("[GA-Pareto] Adaptive mutation skipped: no evaluated candidate");
        }
    }
    phaseEnd = TimingClock::now();
    lastTiming_.mutation_ms = elapsedMs(phaseStart, phaseEnd);

    // ============================================================
    // 5c) RL replay seeding must consume the CURRENT evaluated front.
    // Calling it after offspring replacement would see unevaluated children
    // whose default rank is zero and would falsely report them as Pareto seeds.
    // ============================================================
    phaseStart = TimingClock::now();
    seedRLReplayBuffer(snapshot);
    phaseEnd = TimingClock::now();
    lastTiming_.postprocess_ms = elapsedMs(phaseStart, phaseEnd);

    // ============================================================
    // 6) Generate next population
    // ============================================================
    phaseStart = TimingClock::now();
    reducePopulation();
    auto newPop = selectEliteAndOffspring();

    // [FINAL FIX D1] Crossover/elitism can otherwise extinguish an entire
    // policy mode (observed as 8/8 BALANCED and later 8/8 MAX populations).
    // Keep one MAX, one BALANCED and one LOW representative available.
    ensureModeDiversity(newPop);

    population_ = std::move(newPop);
    phaseEnd = TimingClock::now();
    lastTiming_.offspring_ms = elapsedMs(phaseStart, phaseEnd);

    // ============================================================
    // 7) Post-offspring diversity maintenance. Mode diversity is already
    // guaranteed by ensureModeDiversity(); this generic injector remains a
    // compatibility hook and only acts if the population is undersized.
    // ============================================================
    phaseStart = TimingClock::now();
    injectDiversityIfNeeded();
    phaseEnd = TimingClock::now();
    lastTiming_.postprocess_ms += elapsedMs(phaseStart, phaseEnd);

    const auto computeEnd = TimingClock::now();
    lastTiming_.total_compute_ms = elapsedMs(totalStart, computeEnd);
    lastTiming_.valid = true;

    // Export the selected-action row after phase timings are finalised. This
    // deliberately leaves the row's own file-append time out of ga_compute_total_ms;
    // EvolutionarySelector's outer erl_active_wall_ms includes it.
    if (haveSelected) {
        exportSelectedActionEvidence(snapshot, selectedCopy);
    }

    ++generation_;
}
//=========================================================================================
    // void evolve(const hrl::MetricsSnapshot& snapshot) {
    //     if (!isSnapshotStable(snapshot)) return;

    //     // Update adaptive weights BEFORE fitness evaluation
    //     if (weightManager_) {
    //         double targetFps = population_.empty() ? 30.0 :
    //             (population_[0].genome.target_fps.has ? population_[0].genome.target_fps.value : 30.0);
    //         bool changed = weightManager_->update(snapshot, static_cast<int>(generation_), targetFps);
    //         if (changed) {
    //             spdlog::info("[GeneticAlgorithm] Weight regime ? {} | weights=[{:.3f},{:.3f},{:.3f},{:.3f}]",
    //                          weightManager_->getCurrentRegimeName(),
    //                          weightManager_->getWeights()[0],
    //                          weightManager_->getWeights()[1],
    //                          weightManager_->getWeights()[2],
    //                          weightManager_->getWeights()[3]);
    //         }
    //     }
        
    //     // 1) Evaluate the CURRENT population against the latest measured snapshot.
    //     evaluateParetoPopulation(snapshot);      // Dual: objectives + fitness
    //     // 2) Rank by non-dominated sorting and crowding distance.
    //     nonDominatedSortAndCrowding();
    //     // 3) Select the best evaluated Rank-0 individual BEFORE offspring generation.
    //     //    This is the key thesis-proof fix: the applied action is from the evaluated Pareto front.
    //     const ParetoIndividual* selected = selectBestEvaluatedPareto();

    //     // 4) Apply the selected evaluated Pareto action to runtime controls.
    //     if (selected) {
    //         applySelectedParetoToRuntime(*selected, snapshot);
    //     }

    //     // 5) Export evidence before population mutation/replacement.
    //     exportParetoEvidence(snapshot, selected);
    //     if (selected) {
    //         exportSelectedActionEvidence(snapshot, *selected);
    //     }

    //     logDominanceAnalysis();     // PhD thesis material
    //     logParetoStats();
       
    //     // 6) Generate the NEXT population only after applying/logging the current front.
    //     reducePopulation();
    //     auto newPop = selectEliteAndOffspring();
    //     population_ = std::move(newPop);

    //     ++generation_;
    //     applyBestParetoToRuntime(snapshot);
    //     seedRLReplayBuffer(snapshot);
    //     injectDiversityIfNeeded();
       
    // }

    RLAction getBestAction() const {
        // [P0-F17] Centralize action eligibility in selectBestEvaluatedPareto().
        // This prevents public callers from accidentally selecting an infeasible
        // Rank-0 candidate if a ranking/telemetry regression occurs elsewhere.
        const ParetoIndividual* best = selectBestEvaluatedPareto();
        return best ? best->genome : RLAction{};
    }

    // Thesis data export ? CSV of weight history
    std::string exportWeightHistory() const {
        if (weightManager_) return weightManager_->exportHistoryCSV();
        return "adaptive_weights_disabled\n";
    }

    // Current regime label for logging / thesis
    std::string getCurrentRegimeName() const {
        if (weightManager_) return weightManager_->getCurrentRegimeName();
        return "FIXED_WEIGHTS";
    }

    GATimingMetrics getLastTimingMetrics() const { return lastTiming_; }
    size_t getCurrentGeneration() const { return generation_; }

 


private:
    GAConfig cfg_;
    std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator_;
    std::shared_ptr<hrl::RuntimeControls> runtimeControls_;
    std::vector<ParetoIndividual> population_;
    std::mt19937 rng_;
    IScheduler* scheduler_ = nullptr;
    size_t generation_ = 0;
    int stableFrames_ = 0;
    std::unique_ptr<AdaptiveWeightManager> weightManager_;
    mutable std::mutex evidenceMutex_;
    GATimingMetrics lastTiming_{};
    int lastEvaluatedRank0Count_{0};

     // [FIX] Restored: declaration was lost while merging the trap patches.
    // Written in evolve() (runner-up capture) and read by
    // exportSelectedActionEvidence() -> rank0_runnerup_fitness CSV column.
    double lastRank0RunnerUpFitness_{std::numeric_limits<double>::quiet_NaN()};

    double lastSelectionMargin_{std::numeric_limits<double>::quiet_NaN()};

    // [Adaptive mutation] runtime state
    double currentMutationRate_ = 0.18;   // Initialised from cfg_.mutationRate in ctor
    double prevBestFitness_     = -std::numeric_limits<double>::max();
    int    mutationPlateauCounter_ = 0;

    // [Surrogate] online counterfactual fitness model + last-applied tracking.
    // snap on cycle N reflects the action applied on cycle N-1, so we observe the
    // last-applied config against the current snapshot (temporal credit assignment).
    SurrogateModel surrogate_;
    RLAction       lastAppliedGenome_;
    bool           hasLastApplied_ = false;

    // Current workload context. These fields change in place and deliberately
    // do not own/recreate any controller component.
    std::string currentWorkload_{"UNSPECIFIED"};
    uint32_t currentWorkloadId_{0};
    uint64_t workloadEpoch_{0};
    std::unordered_map<uint32_t, double> workloadMaxMeasuredFps_;

    double         maxMeasuredFps_ = 0.0;   // best hardware-verified FPS for active workload

//     // ===================================================================
//     // Simulate simulateRuntimeControlsFromAction
//     // ===================================================================
//     void simulateRuntimeControlsFromAction(const RLAction& action,
//                                        RuntimeControls& simControls) const {
//     int concurrency = 2;
//     bool enableGpu = true;
//     Affinity affinity = Affinity::Spread;

//     switch (action.mode) {
//         case PolicyMode::MAX_PERFORMANCE:
//             concurrency = 4;
//             enableGpu = true;
//             affinity = Affinity::Spread;
//             break;

//         case PolicyMode::LOW_POWER:
//             concurrency = 1;
//             enableGpu = false;
//             affinity = Affinity::Pack;
//             break;

//         case PolicyMode::BALANCED:
//             concurrency = 2;
//             enableGpu = action.prefer_gpu.has ? action.prefer_gpu.value : true;
//             affinity = Affinity::Spread;
//             break;

//         case PolicyMode::UNKNOWN:
//         default:
//             concurrency = 2;
//             enableGpu = true;
//             affinity = Affinity::Spread;
//             break;
//     }

//     if (action.prefer_gpu.has) {
//         enableGpu = action.prefer_gpu.value;
//     }

//     simControls.concurrency_level.store(concurrency, std::memory_order_relaxed);
//     simControls.enable_gpu.store(enableGpu, std::memory_order_relaxed);
//     simControls.affinity.store(affinity, std::memory_order_relaxed);
// }

    // ===================================================================
    // Evaluation (Dual: Pareto + Scalar)
    // ===================================================================
    // ===================================================================
    // [Surrogate] Build the surrogate model configuration from GAConfig.
    // ===================================================================
    // static SurrogateModel::Config makeSurrogateConfig(const GAConfig& cfg) {
    //     SurrogateModel::Config sc;
    //     sc.exploration_weight = cfg.surrogateExploreWeight;
    //     return sc;
    // }

    static SurrogateModel::Config makeSurrogateConfig(const GAConfig& cfg) {
        SurrogateModel::Config sc;
        sc.exploration_weight   = cfg.surrogateExploreWeight;
        sc.fps_max              = cfg.surrogateFpsMax;          // per-workload FPS envelope
        sc.latency_prior_ms     = cfg.surrogateLatencyPriorMs;  // independent processing-latency prior
        sc.gpu_split_applicable = cfg.gpuSplitApplicable;       // binary workloads collapse split to {0,1}
        return sc;
    }

    void canonicalizeGenomeForWorkload(RLAction& g) const {
        // UNKNOWN is never part of the evolutionary search space.  This guard
        // also cleans legacy/replayed genomes before they reach the scheduler.
        if (g.mode == PolicyMode::UNKNOWN) {
            g.mode = PolicyMode::BALANCED;
        }

        // Workload capability is a hard physical constraint. A CPU-only
        // algorithm may not carry a fictitious GPU action across a transition.
        if (!cfg_.workloadGpuCapable) {
            g.prefer_gpu = MaybeBool(false);
            g.gpu_workload_split = MaybeDouble(0.0);
        } else if (cfg_.forceGpuForWorkload) {
            // Optional experiment policy; ThermalGovernor may still revoke GPU.
            g.prefer_gpu = MaybeBool(true);
        }

        // [FINAL FIX S1] Binary CPU/GPU algorithms must never preserve a
        // fictitious continuous split gene.  Canonicalising the genome itself
        // (not just the surrogate key) makes action telemetry honest as well:
        // CPU => split 0, GPU => split 1.
        if (!cfg_.gpuSplitApplicable) {
            const bool gpu = cfg_.workloadGpuCapable && (g.prefer_gpu.has
                ? g.prefer_gpu.value
                : (g.mode != PolicyMode::LOW_POWER));
            g.gpu_workload_split = MaybeDouble(gpu ? 1.0 : 0.0);
        } else if (g.gpu_workload_split.has) {
            g.gpu_workload_split.value =
                utils::local_clamp(g.gpu_workload_split.value, 0.0, 1.0);
        }

        // MAX_PERFORMANCE has fixed semantic intent: do not let a low evolved
        // target relabel a configuration as "high performance".
        if (g.mode == PolicyMode::MAX_PERFORMANCE) {
            g.target_fps = MaybeDouble(cfg_.performanceTargetFps);
        }
    }

    // ===================================================================
    // [Surrogate] Extract the discretised hardware config a genome represents.
    // freq: explicit gene if present, else derived from PolicyMode.
    // split: work-split gene if present (thesis gap 1), else GPU on=>0.5 / off=>0.
    // conc: derived from mode/budget to mirror the Scheduler's mapping.
    // ===================================================================
    void configFromGenome(const RLAction& g, double& freq_khz, bool& gpu,
                          double& split, int& conc) const {
        gpu = cfg_.workloadGpuCapable && (g.prefer_gpu.has
            ? g.prefer_gpu.value
            : (g.mode != PolicyMode::LOW_POWER));
        if (g.cpu_max_freq_khz.has) {
            if (g.mode == PolicyMode::LOW_POWER) {
                freq_khz = utils::local_clamp(g.cpu_max_freq_khz.value, 460800.0, 1479000.0);
            } else {
                freq_khz = utils::local_clamp(g.cpu_max_freq_khz.value, 102000.0, 1479000.0);
            }
        } else {
            switch (g.mode) {
                case PolicyMode::MAX_PERFORMANCE: freq_khz = 1479000.0; break;
                case PolicyMode::LOW_POWER:       freq_khz = 1020000.0; break;
                default:                          freq_khz =  921600.0; break;
            }
        }
        // [Micro-scheduler] Only algorithms that physically implement a
        // continuous CPU/GPU partition may use the split gene as a surrogate
        // dimension. Binary algorithms (e.g. MedianFilter) are canonicalised
        // to 0=CPU or 1=GPU, preventing fictitious duplicate model states.
        if (!cfg_.gpuSplitApplicable) {
            split = gpu ? 1.0 : 0.0;
        } else if (g.gpu_workload_split.has) {
            split = g.gpu_workload_split.value;
            if (split < 0.0) split = 0.0;
            if (split > 1.0) split = 1.0;
        } else {
            split = gpu ? 1.0 : 0.0;
        }
        double budget = g.power_budget_watts.has ? g.power_budget_watts.value : 4.0;
        switch (g.mode) {
            case PolicyMode::MAX_PERFORMANCE: conc = 4; break;
            case PolicyMode::LOW_POWER:       conc = (budget < 3.5) ? 1 : 2; break;
            default:                          conc = 2; break;
        }
    }

    // Build a snapshot with the surrogate's predicted outcome for `g`, copying
    // any unrelated fields from the real snapshot so downstream code is unchanged.
    hrl::MetricsSnapshot predictedSnapshot(const hrl::MetricsSnapshot& base,
                                           const RLAction& g) const {
        double freq; bool gpu; double split; int conc;
        configFromGenome(g, freq, gpu, split, conc);
        SurrogatePrediction p = surrogate_.predict(freq, gpu, split, conc);
        hrl::MetricsSnapshot s = base;   // preserve unrelated fields
        s.fps             = p.fps;
        s.avg_power_w_alg = p.power_w;
        s.avg_latency_ms  = p.latency_ms;
        s.cpu_temp_c      = p.temp_c;
        return s;
    }

    void evaluateParetoPopulation(const hrl::MetricsSnapshot& snap,
                                  bool creditPreviousAction,
                                  bool previousActionStalled) {
        // // One action -> one evidence window -> one surrogate credit update.
        // if (cfg_.surrogateEnabled && hasLastApplied_) {
        //     double freq; bool gpu; double split; int conc;
        //     configFromGenome(lastAppliedGenome_, freq, gpu, split, conc);
        //     if (previousActionStalled) {
        //         surrogate_.observeStarvation(freq, gpu, split, conc);
        //     } else if (creditPreviousAction) {
        //         surrogate_.observe(freq, gpu, split, conc,
        //                            snap.fps, snap.avg_power_w_alg,
        //                            snap.avg_latency_ms, snap.cpu_temp_c);
        //     }
        // }
        // One action -> one evidence window -> one surrogate credit update.
        if (cfg_.surrogateEnabled && hasLastApplied_) {
            double freq; bool gpu; double split; int conc;
            configFromGenome(lastAppliedGenome_, freq, gpu, split, conc);
            if (previousActionStalled) {
                // [FINAL FIX F2] A timeout is real feasibility evidence but it
                // is NOT an exact physical FPS/latency sample.  Fresh-frame poll
                // count is not guaranteed to equal processed-frame count, so do
                // not poison the physical EMA with a fabricated throughput.
                surrogate_.observeStarvation(freq, gpu, split, conc);
            } else if (creditPreviousAction) {
                surrogate_.observe(freq, gpu, split, conc,
                                   snap.fps, snap.avg_power_w_alg,
                                   snap.avg_latency_ms, snap.cpu_temp_c);
                if (snap.fps > maxMeasuredFps_) {
                    maxMeasuredFps_ = snap.fps;
                    workloadMaxMeasuredFps_[currentWorkloadId_] = maxMeasuredFps_;
                } // workload-conditioned reality cap
            }
        }

        for (auto& ind : population_) {
            canonicalizeGenomeForWorkload(ind.genome);
            hrl::RuntimeControls simControls;
            simControls.concurrency_level.store(2, std::memory_order_relaxed);
            simControls.enable_gpu.store(true, std::memory_order_relaxed);
            simulateRuntimeControlsFromAction(ind.genome, simControls);

            // Preserve the exact counterfactual prediction used for THIS candidate.
            hrl::MetricsSnapshot evalSnap = snap;
            ind.surrogateUsed = false;
            ind.surrogatePrediction = SurrogatePrediction{};
            ind.surrogateExplorationBonus = 0.0;
            ind.surrogateStallPenalty = 0.0;
            ind.surrogateConfigFreqKhz = 0.0;
            ind.surrogateConfigGpu = false;
            ind.surrogateConfigGpuSplit = 0.0;
            ind.surrogateConfigConcurrency = 0;
            ind.feasible = true;
            ind.constraintViolation = 0.0;

            if (cfg_.surrogateEnabled) {
                double freq; bool gpu; double split; int conc;
                configFromGenome(ind.genome, freq, gpu, split, conc);

                ind.surrogateConfigFreqKhz = freq;
                ind.surrogateConfigGpu = gpu;
                ind.surrogateConfigGpuSplit = split;
                ind.surrogateConfigConcurrency = conc;

                // ind.surrogatePrediction = surrogate_.predict(freq, gpu, split, conc);
                // ind.surrogateUsed = true;
                // evalSnap.fps             = ind.surrogatePrediction.fps;

                ind.surrogatePrediction = surrogate_.predict(freq, gpu, split, conc);

                // [FIX P2] Reality gating: unverified optimism must not enter fitness.
                {
                    SurrogatePrediction& p = ind.surrogatePrediction;
                    const double fpsCap = (maxMeasuredFps_ > 0.0)
                        ? cfg_.surrogatePredFpsCapRatio * maxMeasuredFps_
                        : cfg_.surrogateColdFpsCeiling;
                    const bool lowSupport = !p.from_data ||
                        p.support_count < static_cast<uint32_t>(cfg_.surrogateMinSupport);
                    if (lowSupport) {
                        const double pessimistic = (maxMeasuredFps_ > 0.0)
                            ? 0.5 * maxMeasuredFps_
                            : 0.5 * cfg_.surrogateColdFpsCeiling;
                        p.fps = std::min(p.fps, std::max(1.0, pessimistic));
                    }
                    if (p.fps > fpsCap) p.fps = fpsCap;   // unverified over-envelope claims are inadmissible

                    // [FINAL FIX L1] DO NOT impose latency >= 1000/FPS.
                    // 1000/FPS is the inter-frame period, not per-frame
                    // processing/E2E latency.  Latency remains an independently
                    // learned surrogate dimension.
                }

                ind.surrogateUsed = true;
                evalSnap.fps             = ind.surrogatePrediction.fps;

                evalSnap.avg_power_w_alg = ind.surrogatePrediction.power_w;
                evalSnap.avg_latency_ms  = ind.surrogatePrediction.latency_ms;
                evalSnap.cpu_temp_c      = ind.surrogatePrediction.temp_c;

                ind.surrogateExplorationBonus =
                    surrogate_.explorationBonus(freq, gpu, split, conc);
                ind.surrogateStallPenalty =
                    surrogate_.stallPenalty(freq, gpu, split, conc);

                // [FINAL FIX F1] Exact-bucket timeout feasibility.  Keep the
                // existing soft stall penalty for sparse evidence, but once an
                // exact configuration has enough attempts and a majority timeout
                // rate, constraint-domination removes it from the feasible front.
                if (cfg_.hardFeasibilityEnabled) {
                    const SurrogateFeasibilityStats fs =
                        surrogate_.feasibilityStats(freq, gpu, split, conc);
                    if (fs.attempts >= cfg_.hardFeasibilityMinAttempts &&
                        fs.timeout_ratio > cfg_.hardFeasibilityMaxTimeoutRatio) {
                        ind.feasible = false;
                        ind.constraintViolation += 1.0 + fs.timeout_ratio;
                    }
                }
            }

            // [FINAL FIX H1] Performance recovery with resource headroom.
            // If measured throughput is below the required band while power and
            // temperatures are safely below their limits, LOW_POWER is not an
            // admissible action.  This directly prevents the "2 W / 2 FPS while
            // cool" trap observed in HistogramEqualization.
            if (cfg_.performanceHeadroomConstraint &&
                ind.genome.mode == PolicyMode::LOW_POWER) {
                const double requiredFps =
                    cfg_.performanceRecoveryRatio * cfg_.performanceTargetFps;
                const bool fpsDeficit = snap.fps < requiredFps;
                const bool powerHeadroom =
                    PowerSanity::valid(snap.avg_power_w_alg) &&
                    snap.avg_power_w_alg <
                        (cfg_.awm_config.battery_critical_watts -
                         cfg_.performancePowerHeadroomW);
                const bool thermalHeadroom =
                    snap.cpu_temp_c <
                        (cfg_.awm_config.thermal_warn_cpu_c -
                         cfg_.performanceThermalHeadroomC) &&
                    snap.gpu_temp_c <
                        (cfg_.awm_config.thermal_warn_gpu_c -
                         cfg_.performanceThermalHeadroomC);

                if (fpsDeficit && powerHeadroom && thermalHeadroom) {
                    ind.feasible = false;
                    const double deficitRatio =
                        requiredFps > 0.0
                            ? utils::local_clamp(
                                  (requiredFps - snap.fps) / requiredFps,
                                  0.0, 1.0)
                            : 0.0;
                    ind.constraintViolation += 1.0 + deficitRatio;
                }
            }

            // [FINAL FIX T1] Thermal safety is an admissibility rule, not just
            // another scalar weight.  Preserve MAX genetically for later
            // recovery, but prevent selection while the measured board is in
            // the configured critical thermal band.
            if (cfg_.thermalCriticalBlocksMax &&
                ind.genome.mode == PolicyMode::MAX_PERFORMANCE) {
                const double cpuOver = std::max(
                    0.0, snap.cpu_temp_c - cfg_.awm_config.thermal_crit_cpu_c);
                const double gpuOver = std::max(
                    0.0, snap.gpu_temp_c - cfg_.awm_config.thermal_crit_gpu_c);
                if (cpuOver > 0.0 || gpuOver > 0.0) {
                    ind.feasible = false;
                    ind.constraintViolation +=
                        1.0 + (cpuOver + gpuOver) / 10.0;
                }
            }

            ind.objectives = calculateObjectives(evalSnap, ind.genome, simControls);
            ind.fitness = calculateScalarizedFitness(ind.objectives, evalSnap,
                                                      ind.genome, ind.age, &ind);

            // Optimism under uncertainty is added AFTER scalar fitness/age decay,
            // preserving the existing controller arithmetic exactly.
            ind.fitness += ind.surrogateExplorationBonus;
            ind.fitness -= ind.surrogateStallPenalty;
            ind.fitFinal = ind.fitness;
            ind.age++;
        }
    }


//================================================================
    //  Return vector of normalized objectives (all to be minimized)
    std::vector<double> calculateObjectives(const hrl::MetricsSnapshot& snap,
                                            const RLAction& action,
                                            const RuntimeControls& controls) {
        std::vector<double> objs(4);
        // Objective 1: FPS (minimize negative FPS = maximize FPS)

        const bool highPerformanceRegime =
            weightManager_ && weightManager_->getCurrentRegime() == SystemRegime::HIGH_PERFORMANCE;
        const double fpsScore = action.target_fps.has
            ? (1.0 / (1.0 + std::abs(snap.fps - action.target_fps.value))) : 0.0;
        const double directThroughput = cfg_.performanceTargetFps > 0.0
            ? utils::local_clamp(snap.fps / cfg_.performanceTargetFps, 0.0, 2.0)
            : 0.0;

        // [P0] Frequency-aware energy model. The measured snapshot power reflects
        // the CURRENT clock, but each candidate would run at its own gene frequency.
        // Scale the energy estimate by (candidate_freq / max_freq) so a low-clock
        // genome is correctly scored as cheaper and a high-clock genome as costlier.
        // Without this, LOW_POWER and MAX_PERFORMANCE candidates score near-identical
        // energy and the Pareto front collapses onto the idle low-clock corner.
        // const double kMaxFreqKhz = 1479000.0;
        // double freqRatio = action.cpu_max_freq_khz.has
        //     ? (action.cpu_max_freq_khz.value / kMaxFreqKhz)
        //     : 1.0;
        // freqRatio = freqRatio < 0.05 ? 0.05 : (freqRatio > 1.0 ? 1.0 : freqRatio);

        // double energy  = snap.avg_power_w_alg * freqRatio
        //                  * (1.0 + 0.3 * (controls.concurrency_level.load() - 2));
        // [FIX P4] With the surrogate on, snap.avg_power_w_alg is already the
        // per-candidate PREDICTED power at this genome's own frequency/config;
        // multiplying by freqRatio double-counted the clock and let 102 MHz
        // genomes buy the energy objective ~15-20x below reality (Pareto rank-0
        // then filled with LOW_POWER fictions). Keep the legacy discount only
        // for the surrogate-off ablation baseline.
        // NOTE: this objective is normalized POWER (watts), not physical energy.
        // CSV schema v3 retains the legacy `obj_energy` column name for analysis
        // compatibility; interpret that column as obj_power for v3 runs.
        double powerObjective = snap.avg_power_w_alg;
        if (!cfg_.surrogateEnabled) {
            const double kMaxFreqKhz = 1479000.0;
            double freqRatio = action.cpu_max_freq_khz.has
                ? (action.cpu_max_freq_khz.value / kMaxFreqKhz)
                : 1.0;
            freqRatio = freqRatio < 0.05 ? 0.05 : (freqRatio > 1.0 ? 1.0 : freqRatio);
            powerObjective *= freqRatio *
                (1.0 + 0.3 * (controls.concurrency_level.load() - 2));
        }
        // Temperature objective. In surrogate mode the model currently predicts
        // one temperature dimension (stored as cpu_temp_c in evalSnap). The base
        // snapshot's gpu_temp_c belongs to the PREVIOUS real action and must not
        // be mixed into candidate-specific counterfactual scoring. Until the
        // surrogate predicts GPU temperature independently, use only its modeled
        // temperature. Surrogate-off evaluation may use both measured sensors.
        double tempPen = std::max(0.0, snap.cpu_temp_c - cfg_.temp_onset_cpu_c);
        if (!cfg_.surrogateEnabled) {
            tempPen += std::max(0.0, snap.gpu_temp_c - cfg_.temp_onset_gpu_c);
        }
        const double latency = snap.avg_latency_ms;

        // In HIGH_PERFORMANCE, maximize actual predicted throughput against a
        // fixed reference. Other regimes preserve the legacy target-matching term.
        objs[0] = highPerformanceRegime ? -directThroughput : -fpsScore;           // Maximize FPS // Objective 1: FPS (minimize negative FPS = maximize FPS)
        objs[1] = powerObjective / 10.0; // Minimize normalized power (legacy CSV name: obj_energy)
        objs[2] = tempPen / 20.0;      // Minimize temperature  // Objective 3: Temperature penalty (minimize)
        objs[3] = latency / 50.0;      // Minimize latency  // Objective 4: Latency (minimize)  // normalize
        return objs;
    }

// ===================================================================
// NSGA-II Core
// ===================================================================
    void nonDominatedSortAndCrowding();
    void calculateCrowdingDistance(std::vector<int>& front);
    const ParetoIndividual& tournamentSelect(int tournamentSize);
    
    // Scalarized fitness: FPS=50%, Power=20%, Temp=20%, Latency=10% (PhD dual evaluation)
    double calculateScalarizedFitness(const std::vector<double>& objs,
                                                     const hrl::MetricsSnapshot& snap,
                                                     const RLAction& action,
                                                     int age,
                                                     ParetoIndividual* auditOut) const ;

    // ===================================================================
    // Evolution Operators
    // ===================================================================
    ParetoIndividual crossover(const ParetoIndividual& p1, const ParetoIndividual& p2);
    //void mutate(ParetoIndividual& ind);
    void mutate(ParetoIndividual& ind, const hrl::MetricsSnapshot& snap);

    std::vector<ParetoIndividual> selectEliteAndOffspring();

    // [FINAL FIX D1] Hard population invariant: retain at least one
    // MAX_PERFORMANCE, one BALANCED and one LOW_POWER genome.
    void ensureModeDiversity(std::vector<ParetoIndividual>& pop);

    void reducePopulation();
    void injectDiversityIfNeeded();

    // ===================================================================
    // Runtime & Logging
    // ===================================================================
    // void applyBestParetoToRuntime(const hrl::MetricsSnapshot& snapshot);
    // void seedRLReplayBuffer(const hrl::MetricsSnapshot& snapshot);
    // void logParetoStats();
    // void logDominanceAnalysis();

    // bool isSnapshotStable(const hrl::MetricsSnapshot& snap);
    // void initializePopulation();

    // ===================================================================
    // Runtime & Logging
    // ===================================================================
    void applyBestParetoToRuntime(const hrl::MetricsSnapshot& snapshot);

    const ParetoIndividual* selectBestEvaluatedPareto() const;

    double applySelectedParetoToRuntime(const ParetoIndividual& selected,
                                        const hrl::MetricsSnapshot& snapshot);

    void exportParetoEvidence(const hrl::MetricsSnapshot& snap,
                              const ParetoIndividual* selected);

    void exportSelectedActionEvidence(const hrl::MetricsSnapshot& snap,
                                      const ParetoIndividual& selected);

    void ensureEvidenceFiles();
    static bool ensureParentDirectoryForFile(const std::string& filePath);

    int countRank0() const;

    void simulateRuntimeControlsFromAction(const RLAction& action,
                                           RuntimeControls& simControls) const;

    void seedRLReplayBuffer(const hrl::MetricsSnapshot& snapshot);
    void logParetoStats();
    void logDominanceAnalysis();

    bool isSnapshotStable(const hrl::MetricsSnapshot& snap);
    void initializePopulation();

    // [Adaptive mutation] Update currentMutationRate_ from best-fitness trend.
    // Pure rate control ? does not alter selection, so it is Pareto-safe.
    double updateAdaptiveMutationRate(double currentBestFitness);
};

// ===================================================================
// Implementations (add to .cpp or keep inline)
// ===================================================================
// ... (nonDominatedSortAndCrowding, calculateCrowdingDistance, tournamentSelect remain as in your GA_03/04)

// Crossover & Mutate
//ParetoIndividual GeneticAlgorithm::crossover(const ParetoIndividual& p1, const ParetoIndividual& p2) { /* same as before */ }


// ====================== Inline Implementations ======================
inline bool ParetoIndividual::dominates(const ParetoIndividual& other) const {
    // [FINAL FIX F1/T1/H1] Deb-style constraint domination:
    //   1) feasible always beats infeasible;
    //   2) among infeasible candidates, smaller violation wins;
    //   3) ties fall through to normal Pareto objective dominance.
    if (feasible && !other.feasible) return true;
    if (!feasible && other.feasible) return false;

    if (!feasible && !other.feasible) {
        constexpr double kConstraintEps = 1e-12;
        if (constraintViolation + kConstraintEps < other.constraintViolation)
            return true;
        if (other.constraintViolation + kConstraintEps < constraintViolation)
            return false;
    }

    if (objectives.size() != other.objectives.size()) return false;
    bool strictlyBetter = false;
    for (size_t i = 0; i < objectives.size(); ++i) {
        if (objectives[i] > other.objectives[i]) return false;
        if (objectives[i] < other.objectives[i]) strictlyBetter = true;
    }
    return strictlyBetter;
}




  // ===================================================================
    // Evolution Operators (UPDATED for ParetoIndividual)
    // ===================================================================
    ParetoIndividual GeneticAlgorithm::crossover(const ParetoIndividual& p1,
                                                   const ParetoIndividual& p2) {
        ParetoIndividual child;
        std::uniform_real_distribution<double> mix(0.0, 1.0);

        child.genome.mode = mix(rng_) < 0.5 ? p1.genome.mode : p2.genome.mode;

        if (p1.genome.target_fps.has && p2.genome.target_fps.has) {
            const double avg = 0.5 * (p1.genome.target_fps.value + p2.genome.target_fps.value);
            child.genome.target_fps =
                MaybeDouble(utils::local_clamp(avg, cfg_.minTargetFps, cfg_.maxTargetFps));
        } else if (p1.genome.target_fps.has || p2.genome.target_fps.has) {
            child.genome.target_fps = p1.genome.target_fps.has
                ? p1.genome.target_fps : p2.genome.target_fps;
        }

        if (p1.genome.power_budget_watts.has && p2.genome.power_budget_watts.has) {
            child.genome.power_budget_watts = MaybeDouble(
                0.5 * (p1.genome.power_budget_watts.value +
                       p2.genome.power_budget_watts.value));
        } else if (p1.genome.power_budget_watts.has || p2.genome.power_budget_watts.has) {
            child.genome.power_budget_watts = p1.genome.power_budget_watts.has
                ? p1.genome.power_budget_watts : p2.genome.power_budget_watts;
        }

        if (p1.genome.prefer_gpu.has && p2.genome.prefer_gpu.has) {
            child.genome.prefer_gpu = mix(rng_) < 0.5
                ? p1.genome.prefer_gpu : p2.genome.prefer_gpu;
        } else if (p1.genome.prefer_gpu.has || p2.genome.prefer_gpu.has) {
            child.genome.prefer_gpu = p1.genome.prefer_gpu.has
                ? p1.genome.prefer_gpu : p2.genome.prefer_gpu;
        }

        // CPU frequency is a discrete DVFS gene: inherit from one parent.
        if (p1.genome.cpu_max_freq_khz.has || p2.genome.cpu_max_freq_khz.has) {
            const RLAction& src = mix(rng_) < 0.5 ? p1.genome : p2.genome;
            child.genome.cpu_max_freq_khz = src.cpu_max_freq_khz.has
                ? src.cpu_max_freq_khz
                : (p1.genome.cpu_max_freq_khz.has
                    ? p1.genome.cpu_max_freq_khz
                    : p2.genome.cpu_max_freq_khz);
        }

        // Continuous split is averaged only for workloads that actually consume it;
        // canonicalizeGenomeForWorkload() collapses binary algorithms to {0,1}.
        if (p1.genome.gpu_workload_split.has && p2.genome.gpu_workload_split.has) {
            const double avg = 0.5 * (p1.genome.gpu_workload_split.value +
                                      p2.genome.gpu_workload_split.value);
            child.genome.gpu_workload_split =
                MaybeDouble(utils::local_clamp(avg, 0.0, 1.0));
        } else if (p1.genome.gpu_workload_split.has || p2.genome.gpu_workload_split.has) {
            child.genome.gpu_workload_split = p1.genome.gpu_workload_split.has
                ? p1.genome.gpu_workload_split : p2.genome.gpu_workload_split;
        }

        canonicalizeGenomeForWorkload(child.genome);
        return child;
    }



//void GeneticAlgorithm::mutate(ParetoIndividual& ind) { /* same as before */ }

    void GeneticAlgorithm::mutate(ParetoIndividual& ind, const hrl::MetricsSnapshot& snap) {
        (void)snap;
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        if (dist(rng_) >= currentMutationRate_) return;

        if (ind.genome.target_fps.has) {
            std::normal_distribution<double> fpsMut(0.0, 4.0);
            double newFps = ind.genome.target_fps.value + fpsMut(rng_);
            ind.genome.target_fps = MaybeDouble(utils::local_clamp(newFps, cfg_.minTargetFps, cfg_.maxTargetFps));
        }

        if (ind.genome.prefer_gpu.has && dist(rng_) < 0.25) {
            ind.genome.prefer_gpu = MaybeBool(!ind.genome.prefer_gpu.value);
        }

        // Power budget is an actual scheduler/GA gene; keep it evolvable.
        if (ind.genome.power_budget_watts.has && dist(rng_) < 0.30) {
            std::normal_distribution<double> powerMut(0.0, 0.5);
            const double p = ind.genome.power_budget_watts.value + powerMut(rng_);
            ind.genome.power_budget_watts = MaybeDouble(utils::local_clamp(p, 1.0, 10.0));
        }

        // Mutate continuous split only when the workload physically supports it.
        if (cfg_.gpuSplitApplicable && ind.genome.gpu_workload_split.has) {
            std::normal_distribution<double> splitMut(0.0, 0.15);
            double s = ind.genome.gpu_workload_split.value + splitMut(rng_);
            ind.genome.gpu_workload_split = MaybeDouble(utils::local_clamp(s, 0.0, 1.0));
        }

        // [P0] Mutate the CPU frequency gene by stepping to a neighbouring DVFS level.
        if (ind.genome.cpu_max_freq_khz.has) {
            static const long kFreqSteps[] = {102000, 460800, 921600, 1190400, 1479000};
            const int n = 5;
            // Find nearest current index
            long cur = static_cast<long>(ind.genome.cpu_max_freq_khz.value);
            int idx = 0; long best = std::abs(kFreqSteps[0] - cur);
            for (int k = 1; k < n; ++k) {
                long d = std::abs(kFreqSteps[k] - cur);
                if (d < best) { best = d; idx = k; }
            }
            int step = (dist(rng_) < 0.5) ? -1 : 1;
            idx = utils::local_clamp(idx + step, 0, n - 1);
            ind.genome.cpu_max_freq_khz = MaybeDouble(static_cast<double>(kFreqSteps[idx]));
        }

        if (dist(rng_) < 0.07) {
            std::uniform_int_distribution<int> modeDist(1, 3);
            ind.genome.mode = static_cast<PolicyMode>(modeDist(rng_));
        }
        canonicalizeGenomeForWorkload(ind.genome);
    }




// selectEliteAndOffspring, reducePopulation, injectDiversityIfNeeded, applyBestParetoToRuntime, etc. ? use the clean versions from GA_04



    // ===================================================================
    // Selection + Population Management
    // ===================================================================
    std::vector<ParetoIndividual> GeneticAlgorithm::selectEliteAndOffspring() {
        std::vector<ParetoIndividual> newPop;

        // [P0-F16] Defensive: tournamentSelect() draws indices in
        // [0, population_.size()-1]; on an empty population that distribution
        // is degenerate and indexing is UB. Self-heal instead of crashing.
        if (population_.empty()) {
            spdlog::error("[GA-Pareto] Population empty at selection - reinitializing");
            initializePopulation();
        }

        // Elitism (population_ is sorted by rank/crowding - see [P0-F16])
        for (size_t i = 0; i < std::min(cfg_.eliteCount, population_.size()); ++i) {
            newPop.push_back(population_[i]);
        }

        // Offspring. Honor cfg_.crossoverRate; the previous implementation
        // always crossed parents even when the configured rate was lower.
        std::bernoulli_distribution doCrossover(cfg_.crossoverRate);
        std::bernoulli_distribution chooseParent(0.5);

        while (newPop.size() < cfg_.popSize) {
            const auto& p1 = tournamentSelect(3);
            const auto& p2 = tournamentSelect(3);

            ParetoIndividual child;
            if (doCrossover(rng_)) {
                child = crossover(p1, p2);
            } else {
                child = chooseParent(rng_) ? p1 : p2;

                // A copied parent becomes a NEW unevaluated offspring. Never carry
                // stale rank/objectives/feasibility/provenance into the next generation.
                child.objectives.clear();
                child.fitness = -std::numeric_limits<double>::max();
                child.rank = 0;
                child.crowdingDistance = 0.0;
                child.feasible = true;
                child.constraintViolation = 0.0;
                child.age = 0;
                child.surrogatePrediction = SurrogatePrediction{};
                child.surrogateUsed = false;
                child.surrogateExplorationBonus = 0.0;
                child.surrogateStallPenalty = 0.0;
                child.fitFinal = -std::numeric_limits<double>::max();
            }

            mutate(child, hrl::MetricsSnapshot{});
            canonicalizeGenomeForWorkload(child.genome);
            newPop.push_back(std::move(child));
        }
        return newPop;
    }


    // ===================================================================
    // [FINAL FIX D1] Mode-diversity floor
    // ===================================================================
    // The Aug-19 HistogramEqualization trace showed long 8/8 BALANCED and
    // 8/8 MAX monocultures.  Mutation is too weak to guarantee recovery after
    // a mode goes extinct, so diversity is enforced as a population invariant.
    // This does NOT bypass feasibility: an unsafe representative can remain in
    // the population while constraint-domination prevents its selection.
    void GeneticAlgorithm::ensureModeDiversity(
        std::vector<ParetoIndividual>& pop)
    {
        if (!cfg_.enforceModeDiversity || pop.size() < 3) return;

        auto countMode = [&pop](PolicyMode mode) -> size_t {
            return static_cast<size_t>(std::count_if(
                pop.begin(), pop.end(),
                [mode](const ParetoIndividual& x) {
                    return x.genome.mode == mode;
                }));
        };

        const std::array<PolicyMode, 3> required{{
            PolicyMode::MAX_PERFORMANCE,
            PolicyMode::BALANCED,
            PolicyMode::LOW_POWER
        }};

        for (PolicyMode missing : required) {
            if (countMode(missing) > 0) continue;

            // Replace a member belonging to a donor mode that still has >1
            // representative, so fixing one missing mode cannot immediately
            // delete another required mode.
            size_t replaceIndex = pop.size() - 1;
            bool donorFound = false;
            for (size_t i = pop.size(); i-- > 0;) {
                if (countMode(pop[i].genome.mode) > 1) {
                    replaceIndex = i;
                    donorFound = true;
                    break;
                }
            }
            if (!donorFound) replaceIndex = pop.size() - 1;

            ParetoIndividual candidate = pop[replaceIndex];
            candidate.genome.mode = missing;

            // Give each injected representative a semantically valid clock
            // anchor.  Other genes remain inherited to avoid resetting the
            // entire search trajectory.
            if (missing == PolicyMode::MAX_PERFORMANCE) {
                candidate.genome.target_fps =
                    MaybeDouble(cfg_.performanceTargetFps);
                candidate.genome.cpu_max_freq_khz =
                    MaybeDouble(1479000.0);
            } else if (missing == PolicyMode::BALANCED) {
                candidate.genome.cpu_max_freq_khz =
                    MaybeDouble(921600.0);
            } else { // LOW_POWER
                candidate.genome.cpu_max_freq_khz =
                    MaybeDouble(460800.0);
            }

            canonicalizeGenomeForWorkload(candidate.genome);

            // This is a new, unevaluated representative for the NEXT
            // generation. Reset stale donor decision-state fields.
            candidate.objectives.clear();
            candidate.fitness = -std::numeric_limits<double>::max();
            candidate.rank = 0;
            candidate.crowdingDistance = 0.0;
            candidate.feasible = true;
            candidate.constraintViolation = 0.0;
            candidate.age = 0;
            candidate.surrogateUsed = false;
            candidate.surrogatePrediction = SurrogatePrediction{};
            candidate.surrogateExplorationBonus = 0.0;
            candidate.surrogateStallPenalty = 0.0;
            candidate.fitFinal = -std::numeric_limits<double>::max();

            pop[replaceIndex] = std::move(candidate);
        }
    }

    void GeneticAlgorithm::reducePopulation() {
        if (generation_ < cfg_.reductionStartGen) return;

        // [P0-F16] population_ is sorted by (rank, crowding) at the end of
        // nonDominatedSortAndCrowding(), so truncation keeps the best
        // individuals with no duplicates. The old index-based fill
        // (population_[survivors.size()]) could re-copy rank-0 members that
        // were already in `survivors`, because rank-0 individuals were
        // scattered through an unsorted array. Semantics preserved: keep at
        // least minPopSize, and keep the whole rank-0 front even when it
        // exceeds the reduction target.
        size_t rank0 = 0;
        for (const auto& ind : population_) {
            if (ind.rank == 0) ++rank0;
        }

        size_t target = std::max(cfg_.minPopSize,
                                 static_cast<size_t>(population_.size() * cfg_.reductionRatio));
        target = std::max(target, rank0);

        if (population_.size() > target) {
            population_.resize(target);
        }

        spdlog::info("[GA-Pareto] Dynamic reduction -> size {} (gen {})", population_.size(), generation_);
    }

    void GeneticAlgorithm::injectDiversityIfNeeded() {
        if (generation_ % 5 == 0 && population_.size() < cfg_.popSize) {
            spdlog::debug("[GA-Pareto] Injecting diversity");
            // Simple random injection (same logic as initializePopulation)
            ParetoIndividual randomInd;
            std::uniform_real_distribution<double> fpsDist(cfg_.minTargetFps, cfg_.maxTargetFps);
            std::uniform_int_distribution<int> modeDist(1, 3);
            std::bernoulli_distribution gpuBias(0.65);

            randomInd.genome.mode = static_cast<PolicyMode>(modeDist(rng_));
            randomInd.genome.target_fps = MaybeDouble(fpsDist(rng_));
            randomInd.genome.prefer_gpu = MaybeBool(gpuBias(rng_));
            canonicalizeGenomeForWorkload(randomInd.genome);
            population_.push_back(randomInd);
        }
    }

    // ===================================================================
    // Runtime Application & Logging (PhD helpers)
    // ===================================================================
    // ===================================================================
    // Runtime & Logging
    // ===================================================================
   // void applyBestParetoToRuntime(const hrl::MetricsSnapshot& snapshot);
    //void seedRLReplayBuffer(const hrl::MetricsSnapshot& snapshot);
    //void logParetoStats();
    //void logDominanceAnalysis();

    //bool isSnapshotStable(const hrl::MetricsSnapshot& snap);
    // void initializePopulation();
    // ===================================================================

    // void hrl::GeneticAlgorithm::applyBestParetoToRuntime(const hrl::MetricsSnapshot& snapshot) {
    //     if (population_.empty() || !runtimeControls_) return;
    //     const auto& best = population_[0];
    //     if (scheduler_) scheduler_->apply(best.genome, snapshot, *runtimeControls_);

    //     spdlog::debug("[GeneticAlgorithm] Applied best: mode={}, target_fps={:.1f}, gpu={}",
    //                   static_cast<int>(best.genome.mode),
    //                   best.genome.target_fps.has ? best.genome.target_fps.value : 0.0,
    //                   best.genome.prefer_gpu.has ? (best.genome.prefer_gpu.value ? "YES" : "NO") : "AUTO");
    // }

// ===================================================================
// applyBestParetoToRuntime: apply Rank-0 individual with best fitness
// ===================================================================
void hrl::GeneticAlgorithm::applyBestParetoToRuntime(const hrl::MetricsSnapshot& snapshot) {
    if (population_.empty() || !runtimeControls_) return;

    // [P0-F17] Legacy actuation path uses the same feasibility-aware selector
    // as the primary evolve() path.  Never maintain two subtly different
    // definitions of "best" action.
    const ParetoIndividual* best = selectBestEvaluatedPareto();
    if (!best) {
        spdlog::warn("[GeneticAlgorithm] No selectable Pareto solution available");
        return;
    }

    if (scheduler_) scheduler_->apply(best->genome, snapshot, *runtimeControls_);
    

    spdlog::debug("[GeneticAlgorithm] Applied Rank-0 best: mode={}, fps={:.1f}, gpu={}, fitness={:.4f}",
                  static_cast<int>(best->genome.mode),
                  best->genome.target_fps.has ? best->genome.target_fps.value : 0.0,
                  best->genome.prefer_gpu.has ? (best->genome.prefer_gpu.value ? "YES" : "NO") : "AUTO",
                  best->fitness);
}

// ===================================================================
// seedRLReplayBuffer: push Rank-0 solutions as RL experience (ERL)
// ===================================================================
void GeneticAlgorithm::seedRLReplayBuffer(const hrl::MetricsSnapshot& snapshot) {
    // ERL hybridization: Rank-0 genomes serve as high-quality seeds for RL
    // In a full ERL implementation these would be pushed to a shared replay buffer.
    int rank0Count = 0;
    for (const auto& ind : population_) {
        if (ind.rank == 0) ++rank0Count;
    }
    spdlog::debug("[GeneticAlgorithm] ERL: {} Rank-0 solutions available for RL seeding (gen {})",
                  rank0Count, generation_);
    (void)snapshot;  // suppress unused warning until RL buffer is integrated
}



// ===================================================================
// logParetoStats: generation summary for PhD documentation
// ===================================================================
void GeneticAlgorithm::logParetoStats() {
    if (population_.empty()) return;

    int rank0Count = 0;
    double bestFitness = -std::numeric_limits<double>::max();
    double avgFitness  = 0.0;

    for (const auto& ind : population_) {
        if (ind.rank == 0) ++rank0Count;
        if (ind.fitness > bestFitness) bestFitness = ind.fitness;
        avgFitness += ind.fitness;
    }
    avgFitness /= static_cast<double>(population_.size());

    spdlog::info("[GA-Pareto] Gen {} | Pop={} | Rank-0={} | BestFit={:.4f} | AvgFit={:.4f}",
                 generation_, population_.size(), rank0Count, bestFitness, avgFitness);
}



    bool GeneticAlgorithm::isSnapshotStable(const hrl::MetricsSnapshot& snap) {
        if (!snap.valid || snap.fps < cfg_.minFpsForEvolution) {
            stableFrames_ = 0;
            return false;
        }

        if (cfg_.requirePowerForEvolution && snap.avg_power_w_alg < cfg_.minPowerForEvolution) {
            stableFrames_ = 0;
            return false;
        }

        stableFrames_++;
        return stableFrames_ >= std::max(1, cfg_.stableFramesRequired);
    }

    void GeneticAlgorithm::initializePopulation() {
        population_.resize(cfg_.popSize);
        std::uniform_real_distribution<double> fpsDist(cfg_.minTargetFps, cfg_.maxTargetFps);
        std::uniform_int_distribution<int> modeDist(1, 3);
        std::bernoulli_distribution gpuBias(0.65);

        // [P0] Discrete Jetson Nano CPU DVFS steps (kHz). Seeding the frequency
        // gene across these lets the Pareto front span the full clock range
        // instead of collapsing onto the 102 MHz floor via PolicyMode alone.
        static const long kFreqSteps[] = {102000, 460800, 921600, 1190400, 1479000};
        std::uniform_int_distribution<int> freqIdx(0, 4);
        std::uniform_real_distribution<double> splitDist(0.0, 1.0);

        for (size_t i = 0; i < cfg_.popSize; ++i) {
            auto& ind = population_[i];

            // [FINAL FIX D1] Seed generation zero with all three macro modes
            // represented. Remaining individuals are random as before.
            if (cfg_.enforceModeDiversity && cfg_.popSize >= 3 && i < 3) {
                static const PolicyMode kSeedModes[3] = {
                    PolicyMode::MAX_PERFORMANCE,
                    PolicyMode::BALANCED,
                    PolicyMode::LOW_POWER
                };
                ind.genome.mode = kSeedModes[i];
            } else {
                ind.genome.mode = static_cast<PolicyMode>(modeDist(rng_));
            }
            ind.genome.target_fps = MaybeDouble(fpsDist(rng_));
            ind.genome.prefer_gpu = MaybeBool(gpuBias(rng_));
            ind.genome.power_budget_watts = MaybeDouble(3.0 + rng_() % 5);
            ind.genome.cpu_max_freq_khz =
                MaybeDouble(static_cast<double>(kFreqSteps[freqIdx(rng_)]));
            // Seed split in [0,1]; binary workloads are immediately canonicalized to {0,1}.
            ind.genome.gpu_workload_split = MaybeDouble(splitDist(rng_));
            canonicalizeGenomeForWorkload(ind.genome);
        }
        spdlog::info("[GeneticAlgorithm] Initialized population of size {} "
                     "(target_fps range {:.0f}-{:.0f}, freq gene enabled)",
                     cfg_.popSize, cfg_.minTargetFps, cfg_.maxTargetFps);
    }

    // ===================================================================
    // [Adaptive mutation] Plateau-driven mutation-rate control.
    // Ramps mutation up when best fitness stagnates, eases it down when
    // fitness improves. Operates only on the rate ? selection is untouched,
    // so this is safe to run alongside NSGA-II non-dominated sorting.
    // Directly targets the observed failure mode (best fitness drifting
    // negative after ~gen 500 with no escape mechanism).
    // ===================================================================
    double GeneticAlgorithm::updateAdaptiveMutationRate(double currentBestFitness) {
        if (!cfg_.adaptiveMutationEnabled) {
            currentMutationRate_ = cfg_.mutationRate;
            return currentMutationRate_;
        }

        // First call: just seed the baseline, no adjustment yet.
        if (prevBestFitness_ == -std::numeric_limits<double>::max()) {
            prevBestFitness_ = currentBestFitness;
            currentMutationRate_ = cfg_.mutationRate;
            return currentMutationRate_;
        }

        const double improvement = currentBestFitness - prevBestFitness_;
        prevBestFitness_ = currentBestFitness;

        // Relative threshold scaled to the fitness magnitude, with an absolute
        // floor so near-zero fitness doesn't make the threshold collapse to 0.
        const double scale = std::max(std::abs(currentBestFitness),
                                      cfg_.mutationPlateauAbsFloor);
        const double improveThreshold = cfg_.mutationPlateauRelThreshold * scale;

        if (improvement > improveThreshold) {
            // Improving -> exploit: ease mutation back toward (below) base.
            mutationPlateauCounter_ = 0;
            currentMutationRate_ = cfg_.mutationRate * cfg_.mutationImproveFactor;
        } else {
            // Stagnating -> explore: ramp mutation up, capped.
            ++mutationPlateauCounter_;
            const double factor = 1.0 + cfg_.mutationPlateauStep * mutationPlateauCounter_;
            currentMutationRate_ = std::min(cfg_.mutationRate * factor,
                                            cfg_.maxMutationRate);
        }

        // Guard rails
        currentMutationRate_ = std::min(std::max(currentMutationRate_, 0.0),
                                        cfg_.maxMutationRate);
        return currentMutationRate_;
    }

// ===================================================================
// IMPLEMENTATIONS (NSGA-II + Dominance)
// ===================================================================
//void GeneticAlgorithm::nonDominatedSortAndCrowding() { /* your existing correct implementation */ }
//void GeneticAlgorithm::calculateCrowdingDistance(std::vector<int>& front) { /* your existing correct implementation */ }
//const ParetoIndividual& GeneticAlgorithm::tournamentSelect(int tournamentSize) { /* your existing correct implementation */ }


// IMPLEMENTATION DETAILS BELOW (can be moved to .cpp if desired)

/*
1. Fast Non-Dominated Sort
This function separates your population into "Fronts" (Rank 0 is the true Pareto front, Rank 1 is the next best, etc.).
 It uses a helper lambda to determine if Individual A completely dominates Individual B.
*/
void GeneticAlgorithm::nonDominatedSortAndCrowding() {
    const size_t n = population_.size();
    if (n == 0) return;

    // S[i] contains a list of indices of individuals that individual 'i' dominates
    std::vector<std::vector<int>> S(n);
    // n_dom[i] is the domination count: how many individuals dominate individual 'i'
    std::vector<int> n_dom(n, 0);
    
    std::vector<std::vector<int>> fronts;
    std::vector<int> currentFront;

    // [P0-F17] CRITICAL: use the canonical ParetoIndividual::dominates().
    // The old local lambda compared objectives only and therefore bypassed
    // feasible/constraintViolation, allowing infeasible LOW_POWER candidates
    // to receive Rank 0 and reach the scheduler.  One dominance definition is
    // now shared by sorting, feasibility, and the deterministic audit trail.
    auto dominates = [](const ParetoIndividual& a, const ParetoIndividual& b) {
        return a.dominates(b);
    };

    // 1. Calculate domination counts and build the first front
    for (size_t p = 0; p < n; ++p) {
        S[p].clear();
        n_dom[p] = 0;
        for (size_t q = 0; q < n; ++q) {
            if (p == q) continue;
            
            if (dominates(population_[p], population_[q])) {
                S[p].push_back(q);
            } else if (dominates(population_[q], population_[p])) {
                n_dom[p]++;
            }
        }
        
        // If nothing dominates p, it belongs to the first Pareto front (Rank 0)
        if (n_dom[p] == 0) {
            population_[p].rank = 0;
            currentFront.push_back(p);
        }
    }

    fronts.push_back(currentFront);

    // 2. Build subsequent fronts
    int i = 0;
    while (!fronts[i].empty()) {
        std::vector<int> nextFront;
        for (int p : fronts[i]) {
            for (int q : S[p]) {
                n_dom[q]--; // Since p was in the previous front, remove its domination effect
                if (n_dom[q] == 0) {
                    population_[q].rank = i + 1;
                    nextFront.push_back(q);
                }
            }
        }
        i++;
        if (!nextFront.empty()) {
            fronts.push_back(nextFront);
        } else {
            break; // All individuals sorted
        }
    }

    // 3. Assign Crowding Distance for each front independently
    for (auto& front : fronts) {
        if (front.empty()) continue;
        calculateCrowdingDistance(front); // Call our helper
    }

    // [P0-F16] CRITICAL: order the population by the NSGA-II crowded
    // comparison (rank asc, crowding desc - ParetoIndividual::operator<).
    // selectEliteAndOffspring() takes the first eliteCount entries as
    // "elites" and reducePopulation() truncates from the back; both silently
    // assumed this ordering. Without it, elites were arbitrary individuals
    // and reduction could duplicate rank-0 members via index-based fill.
    // (The per-front index vectors above are not used after this point, so
    // invalidating them by reordering population_ is safe.)
    std::sort(population_.begin(), population_.end());
}

/*
2. Crowding Distance Calculation
I separated this into a helper function calculateCrowdingDistance(std::vector<int>& front). You should declare this as a private function in your header. 
It ensures that boundary individuals (the absolute best at a specific objective) are always preserved.
*/

// Add this declaration to your .h file:
// void calculateCrowdingDistance(std::vector<int>& front);

void GeneticAlgorithm::calculateCrowdingDistance(std::vector<int>& front) {
    size_t l = front.size();
    
    // Initialize crowding distance to 0 for everyone in the current front
    for (int idx : front) {
        population_[idx].crowdingDistance = 0.0;
    }
    
    // If 1 or 2 items, they are boundaries by default. Preserve them with infinity.
    if (l <= 2) {
        for (int idx : front) {
            population_[idx].crowdingDistance = std::numeric_limits<double>::infinity();
        }
        return;
    }

    size_t numObjectives = population_[front[0]].objectives.size();
    
    // Calculate density across each objective axis independently
    for (size_t m = 0; m < numObjectives; ++m) {
        // Sort the current front based solely on objective 'm'
        std::sort(front.begin(), front.end(), [&](int a, int b) {
            return population_[a].objectives[m] < population_[b].objectives[m];
        });

        // The extremes of the front get infinite distance (guaranteed survival)
        population_[front[0]].crowdingDistance = std::numeric_limits<double>::infinity();
        population_[front[l - 1]].crowdingDistance = std::numeric_limits<double>::infinity();

        double objMin = population_[front[0]].objectives[m];
        double objMax = population_[front[l - 1]].objectives[m];
        double range = objMax - objMin;

        if (range == 0.0) continue; // Prevent division by zero if all values are identical

        // For all intermediate individuals, calculate normalized distance to neighbors
        for (size_t j = 1; j < l - 1; ++j) {
            if (population_[front[j]].crowdingDistance != std::numeric_limits<double>::infinity()) {
                double diff = population_[front[j + 1]].objectives[m] - population_[front[j - 1]].objectives[m];
                population_[front[j]].crowdingDistance += diff / range;
            }
        }
    }
}

/*
3. NSGA-II Tournament Selection (Crowded-Comparison Operator)
This selection logic acts on the operator< you already defined in ParetoIndividual, 
ensuring the algorithm prefers lower ranks, but breaks ties by preferring higher crowding distances (exploring less-crowded areas of the Pareto front).
*/
const ParetoIndividual& GeneticAlgorithm::tournamentSelect(int tournamentSize) {
    std::uniform_int_distribution<size_t> dist(0, population_.size() - 1);
    
    size_t bestIdx = dist(rng_);

    for (int i = 1; i < tournamentSize; ++i) {
        size_t contenderIdx = dist(rng_);
        
        const auto& best = population_[bestIdx];
        const auto& contender = population_[contenderIdx];

        // NSGA-II Crowded-Comparison Operator:
        // 1. Better Rank (Lower is better) wins.
        if (contender.rank < best.rank) {
            bestIdx = contenderIdx;
        } 
        // 2. If ranks are tied, pick the one in the less crowded region (Higher distance)
        else if (contender.rank == best.rank) {
            if (contender.crowdingDistance > best.crowdingDistance) {
                bestIdx = contenderIdx;
            }
        }
    }
    
    return population_[bestIdx];
}

// ===================================================================
// Scalarized fitness: weighted combination of normalized objectives
// Weights: FPS=50%, Power=20%, Temperature=20%, Latency=10%
// ===================================================================
double GeneticAlgorithm::calculateScalarizedFitness(const std::vector<double>& objs,
                                                     const hrl::MetricsSnapshot& snap,
                                                     const RLAction& action,
                                                     int age,
                                                     ParetoIndividual* auditOut) const {
    if (objs.size() < 4) return -std::numeric_limits<double>::max();

    // objs = [normFPS, normEnergy, normTemp, normLat] (all minimized).
    // This exact authoritative weight vector is also persisted in the candidate.
    std::array<double, 4> w = {cfg_.weight_fps, cfg_.weight_power,
                               cfg_.weight_temp, cfg_.weight_latency};
    if (weightManager_) {
        w = weightManager_->getWeights();
    }

    const double fpsComponent   = -objs[0] * w[0];
    const double powerComponent = -objs[1] * w[1];
    const double tempComponent  = -objs[2] * w[2];
    const double latComponent   = -objs[3] * w[3];

    double gpuBonus = 0.0;
    // The measured gpu_util_avg belongs to the currently executing action, not
    // to each counterfactual candidate. Do not leak that stale state into
    // surrogate-mode fitness. Keep the legacy bonus only for surrogate-off
    // ablation where the snapshot and evaluated action share the measured state.
    if (!cfg_.surrogateEnabled &&
        action.prefer_gpu.has && action.prefer_gpu.value &&
        snap.gpu_util_avg > 15.0 && snap.gpu_util_avg < 85.0) {
        gpuBonus = 0.05;
    }

    const double beforeAge = fpsComponent + powerComponent +
                             tempComponent + latComponent + gpuBonus;
    const double ageFactor = std::pow(0.985, static_cast<double>(age) / 50.0);
    const double afterAge = beforeAge * ageFactor;

    if (auditOut) {
        auditOut->fitnessWeights = w;
        auditOut->fitFpsComponent = fpsComponent;
        auditOut->fitPowerComponent = powerComponent;
        auditOut->fitTempComponent = tempComponent;
        auditOut->fitLatencyComponent = latComponent;
        auditOut->fitGpuBonus = gpuBonus;
        auditOut->fitBeforeAge = beforeAge;
        auditOut->fitAgeFactor = ageFactor;
        auditOut->fitAfterAge = afterAge;
        auditOut->fitFinal = afterAge; // exploration bonus is appended by caller
    }

    return afterAge;
}


    // double calculateScalarizedFitness(const hrl::MetricsSnapshot& snap,
    //                                   const RLAction& action,
    //                                   const RuntimeControls& /*controls*/) {
    //     double f = 0.0;
    //     if (action.target_fps.has) {
    //         double err = std::abs(snap.fps - action.target_fps.value);
    //         f += 0.50 * (1.0 / (1.0 + err));
    //     }
    //     f += -0.20 * snap.avg_power_w_alg;

    //     if (snap.cpu_temp_c > 75.0) f += -2.0 * (snap.cpu_temp_c - 75.0);
    //     if (snap.gpu_temp_c > 80.0) f += -2.0 * (snap.gpu_temp_c - 80.0);

    //     f += -0.10 * snap.avg_latency_ms / 50.0;

    //     if (action.prefer_gpu.has && action.prefer_gpu.value &&
    //         snap.gpu_util_avg > 15.0 && snap.gpu_util_avg < 85.0) {
    //         f += 0.20;
    //     }

    //     f *= std::pow(0.985, generation_ / 50.0);
    //     return f;
    // }


//=======================================================================================================================================================================================
void GeneticAlgorithm::logDominanceAnalysis() {
    if (population_.size() < 2) return;
    
    spdlog::info("\n===== PARETO FRONT ANALYSIS (Generation {}) =====", generation_);
    
    std::vector<size_t> frontIndices;
    for (size_t i = 0; i < population_.size(); ++i) {
        if (population_[i].rank == 0) frontIndices.push_back(i);
    }
    
    for (size_t idx : frontIndices) {
        const auto& ind = population_[idx];
        spdlog::info("  [Rank-0] ID={} | Fitness={:.4f} | FPS Goal={:.1f} | Power={:.2f}W | "
                     "Temp Penalty={:.2f} | Latency={:.2f}ms | Crowding Dist={:.4f}",
                     idx, ind.fitness,
                     ind.genome.target_fps.has ? ind.genome.target_fps.value : 0.0,
                     ind.objectives[1] * 10.0,
                     ind.objectives[2] * 20.0,
                     ind.objectives[3] * 50.0,
                     ind.crowdingDistance);
    }
    
    spdlog::info("\n  ===== DOMINANCE COMPARISON =====");
    if (!frontIndices.empty() && population_.size() > frontIndices.size()) {
        const auto& best = population_[frontIndices[0]];
        for (size_t i = frontIndices.size(); i < std::min(size_t(3), population_.size()); ++i) {
            const auto& challenger = population_[i];
            spdlog::info("  [Rank-{}] vs [Rank-0]: {} dominates because:",
                         challenger.rank, best.dominates(challenger) ? "Best" : "Trade-off");
            
            const char* objName[] = {"FPS", "Energy", "Temperature", "Latency"};
            for (size_t obj = 0; obj < best.objectives.size(); ++obj) {
                spdlog::info("    - {}: Best={:.3f} vs Challenger={:.3f} {}",
                             objName[obj], best.objectives[obj], challenger.objectives[obj],
                             best.objectives[obj] <= challenger.objectives[obj] ? "? Better" : "? Worse");
            }
        }
    }
    spdlog::info("\n");
}

// ===================================================================
// Side-effect-free simulation of runtime controls for GA evaluation.
// This avoids touching real sysfs/GPU/DVFS while evaluating candidates.
// ===================================================================
void GeneticAlgorithm::simulateRuntimeControlsFromAction(
    const RLAction& action,
    RuntimeControls& simControls) const
{
    int concurrency = 2;
    bool enableGpu = true;
    Affinity affinity = Affinity::Spread;

    switch (action.mode) {
        case PolicyMode::MAX_PERFORMANCE:
            concurrency = 4;
            enableGpu = true;
            affinity = Affinity::Spread;
            break;

        case PolicyMode::LOW_POWER:
            concurrency = 1;
            enableGpu = false;
            affinity = Affinity::Pack;
            break;

        case PolicyMode::BALANCED:
            concurrency = 2;
            enableGpu = action.prefer_gpu.has ? action.prefer_gpu.value : true;
            affinity = Affinity::Spread;
            break;

        case PolicyMode::UNKNOWN:
        default:
            concurrency = 2;
            enableGpu = true;
            affinity = Affinity::Spread;
            break;
    }

    if (action.prefer_gpu.has) {
        enableGpu = action.prefer_gpu.value;
    }

    simControls.concurrency_level.store(concurrency, std::memory_order_relaxed);
    simControls.enable_gpu.store(enableGpu, std::memory_order_relaxed);
    simControls.affinity.store(affinity, std::memory_order_relaxed);
}

// ===================================================================
// Select the best evaluated Rank-0 Pareto individual.
// ===================================================================
const ParetoIndividual* GeneticAlgorithm::selectBestEvaluatedPareto() const
{
    if (population_.empty()) {
        return nullptr;
    }

    constexpr double kConstraintEps = 1e-12;

    auto evaluated = [](const ParetoIndividual& ind) -> bool {
        return !ind.objectives.empty() && std::isfinite(ind.fitness);
    };

    // 1) Normal path: the highest-fitness FEASIBLE member of Rank 0.
    const ParetoIndividual* selected = nullptr;
    for (const auto& ind : population_) {
        if (!evaluated(ind) || ind.rank != 0 || !ind.feasible) continue;
        if (!selected || ind.fitness > selected->fitness) {
            selected = &ind;
        }
    }
    if (selected) return selected;

    // 2) Defense-in-depth against a future ranking regression. If any feasible
    // evaluated candidate exists at all, choose the best rank first and scalar
    // fitness second. An infeasible candidate must never beat available feasibility.
    for (const auto& ind : population_) {
        if (!evaluated(ind) || !ind.feasible) continue;
        if (!selected ||
            ind.rank < selected->rank ||
            (ind.rank == selected->rank && ind.fitness > selected->fitness)) {
            selected = &ind;
        }
    }
    if (selected) {
        spdlog::error(
            "[GA-P0] No feasible Rank-0 candidate, but a feasible Rank-{} candidate exists; "
            "using it as deployment defense-in-depth",
            selected->rank);
        return selected;
    }

    // 3) Pathological/degraded case: every evaluated candidate is infeasible.
    // Choose the globally smallest normalized constraint violation. Pareto rank
    // and scalar fitness are only deterministic tie-breakers. Never use an
    // arbitrary population_[0] fallback.
    for (const auto& ind : population_) {
        if (!evaluated(ind) || !std::isfinite(ind.constraintViolation)) continue;

        if (!selected) {
            selected = &ind;
            continue;
        }

        if (ind.constraintViolation + kConstraintEps < selected->constraintViolation) {
            selected = &ind;
            continue;
        }

        if (std::abs(ind.constraintViolation - selected->constraintViolation) <=
            kConstraintEps) {
            if (ind.rank < selected->rank ||
                (ind.rank == selected->rank && ind.fitness > selected->fitness)) {
                selected = &ind;
            }
        }
    }

    if (selected) {
        spdlog::warn(
            "[GA-P0] ALL EVALUATED CANDIDATES INFEASIBLE: selecting least violation "
            "{:.6f}, rank={}, fitness={:.4f}",
            selected->constraintViolation, selected->rank, selected->fitness);
    }

    // No evaluated candidate exists (e.g. getBestAction() before first evolve()).
    // Return nullptr so callers leave hardware unchanged / return a default action.
    return selected;
}

// ===================================================================
// Apply selected evaluated Pareto individual to real runtime controls.
// ===================================================================
double GeneticAlgorithm::applySelectedParetoToRuntime(
    const ParetoIndividual& selected,
    const hrl::MetricsSnapshot& snapshot)
{
    if (!runtimeControls_) {
        return 0.0;
    }

    // // [Surrogate] Remember what we are about to apply; next cycle's snapshot is
    // // this action's measured outcome and will be fed to surrogate_.observe().
    // lastAppliedGenome_ = selected.genome;
    // hasLastApplied_    = true;

    // if (scheduler_) {
    //     scheduler_->apply(selected.genome, snapshot, *runtimeControls_);
    // }

    // [Surrogate] Remember what we are about to apply; next cycle's snapshot is
    // this action's measured outcome and will be fed to surrogate_.observe().
    lastAppliedGenome_ = selected.genome;
    hasLastApplied_    = true;

    // [PhD FIX] Diagnostic trace for HetGB continuous split gene crash
    if (selected.genome.gpu_workload_split.has) {
        double safe_split = utils::local_clamp(selected.genome.gpu_workload_split.value, 0.0, 1.0);
        spdlog::info("[GA-Split-Trace] Dispatching continuous split gene: {:.3f}", safe_split);
    }

    double schedulerApplyMs = 0.0;
    if (scheduler_) {
        const auto schedulerStart = std::chrono::steady_clock::now();
        scheduler_->apply(selected.genome, snapshot, *runtimeControls_);
        const auto schedulerEnd = std::chrono::steady_clock::now();
        schedulerApplyMs = std::chrono::duration<double, std::milli>(
            schedulerEnd - schedulerStart).count();
    }

    // [PhD timing] Publish a 1-based action epoch only after all scheduler
    // actuation has completed. AlgorithmConcrete timestamps the first frame
    // that observes this epoch, yielding true action-to-effect response time.
    const uint64_t applyEndNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const uint64_t actionEpoch = static_cast<uint64_t>(generation_) + 1ULL;
    runtimeControls_->action_apply_end_ns.store(applyEndNs, std::memory_order_relaxed);
    runtimeControls_->action_generation.store(actionEpoch, std::memory_order_release);

    spdlog::info(
        "[ERL SELECTED] Gen={} | Rank={} | Fit={:.4f} | mode={} | targetFPS={:.1f} | gpu={} | "
        "fps={:.2f} | power={:.3f}W | J/frame={:.6f} | latency={:.3f}ms",
        generation_,
        selected.rank,
        selected.fitness,
        static_cast<int>(selected.genome.mode),
        selected.genome.target_fps.has ? selected.genome.target_fps.value : 0.0,
        selected.genome.prefer_gpu.has
            ? (selected.genome.prefer_gpu.value ? "YES" : "NO")
            : "AUTO",
        snapshot.fps,
        snapshot.avg_power_w_alg,
        snapshot.joulesPerFrame,
        snapshot.avg_latency_ms
    );

    return schedulerApplyMs;
}


// ===================================================================
// Ensure output directory and evidence CSV files exist immediately.
// This creates headers even before the first successful ERL generation.
// ===================================================================
bool GeneticAlgorithm::ensureParentDirectoryForFile(const std::string& filePath)
{
    const std::size_t slash = filePath.find_last_of("/");
    if (slash == std::string::npos) {
        return true;
    }

    const std::string dir = filePath.substr(0, slash);
    if (dir.empty()) {
        return true;
    }

    std::string current;
    for (char c : dir) {
        current.push_back(c);
        if (c == '/') {
            if (current.size() > 1) {
                ::mkdir(current.c_str(), 0755);
            }
        }
    }

    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
        spdlog::warn("[GA-Pareto] Could not create directory '{}': {}", dir, std::strerror(errno));
        return false;
    }
    return true;
}

void GeneticAlgorithm::ensureEvidenceFiles()
{
    // Telemetry schema v3 adds `feasible` and `constraint_violation` to both
    // Pareto and selected-action evidence.  Bumping the row schema prevents
    // downstream analysis scripts from silently interpreting the new columns
    // as the older 91/67-column v2 layout.
    std::lock_guard<std::mutex> guard(evidenceMutex_);

    ensureParentDirectoryForFile(cfg_.pareto_csv_path);
    if (!std::ifstream(cfg_.pareto_csv_path).good()) {
        std::ofstream out(cfg_.pareto_csv_path, std::ios::out);
        if (out.is_open()) {
            out << "schema_version,generation,workload_id,workload,workload_epoch,individual_id,rank,fitness,crowding_distance,obj_fps,obj_energy,obj_temp,obj_latency,target_fps_has,target_fps,power_budget_has,power_budget_watts,prefer_gpu_has,prefer_gpu,gpu_workload_split_has,gpu_workload_split,gpu_split_applicable,cpu_max_freq_has,cpu_max_freq_khz,policy_mode,awm_regime,awm_detected_regime,awm_w_fps,awm_w_power,awm_w_temp,awm_w_latency,awm_trigger_field,awm_trigger_value,awm_trigger_threshold,awm_regime_streak,awm_last_update_generation,surrogate_used,surrogate_source,surrogate_confidence,surrogate_from_data,surrogate_support_count,surrogate_neighbor_count,surrogate_nearest_distance,surrogate_exploration_bonus,surrogate_stall_penalty,surrogate_config_freq_khz,surrogate_config_gpu,surrogate_config_gpu_split,surrogate_config_concurrency,pred_fps,pred_power_w,pred_latency_ms,pred_temp_c,fit_fps_component,fit_power_component,fit_temp_component,fit_latency_component,fit_gpu_bonus,fit_before_age,fit_age_factor,fit_after_age,fit_final,feasible,constraint_violation,selected,measured_fps,measured_power_w,measured_joules_per_frame,algorithm_processing_ms,pipeline_e2e_ms,cpu_temp_c,gpu_temp_c\n";
        } else {
            spdlog::warn("[GA-Pareto] Could not create Pareto CSV: {}", cfg_.pareto_csv_path);
        }
    }

    ensureParentDirectoryForFile(cfg_.action_csv_path);
    if (!std::ifstream(cfg_.action_csv_path).good()) {
        std::ofstream out(cfg_.action_csv_path, std::ios::out);
        if (out.is_open()) {
            out << "schema_version,generation,workload_id,workload,workload_epoch,action_epoch,frame_id,active_algorithm_workload_epoch,algorithm_processed_workload_epoch,algorithm_processed_frame_id,rank0_count,selected_rank,selected_fitness,rank0_runnerup_fitness,fitness_margin,policy_mode,target_fps_has,target_fps,power_budget_has,power_budget_watts,prefer_gpu_has,prefer_gpu,gpu_workload_split_has,gpu_workload_split,gpu_split_applicable,cpu_max_freq_has,cpu_max_freq_gene_khz,cpu_max_freq_commanded_khz,runtime_enable_gpu,runtime_gpu_workload_split,runtime_concurrency,runtime_affinity,awm_regime,awm_detected_regime,awm_w_fps,awm_w_power,awm_w_temp,awm_w_latency,awm_trigger_field,awm_trigger_value,awm_trigger_threshold,awm_regime_streak,awm_last_update_generation,surrogate_used,surrogate_source,surrogate_confidence,surrogate_from_data,surrogate_support_count,surrogate_neighbor_count,surrogate_nearest_distance,surrogate_exploration_bonus,surrogate_stall_penalty,surrogate_config_freq_khz,surrogate_config_gpu,surrogate_config_gpu_split,surrogate_config_concurrency,pred_fps,pred_power_w,pred_latency_ms,pred_temp_c,obj_fps,obj_energy,obj_temp,obj_latency,fit_fps_component,fit_power_component,fit_temp_component,fit_latency_component,fit_gpu_bonus,fit_before_age,fit_age_factor,fit_after_age,fit_final,feasible,constraint_violation,measured_fps,measured_power_w,measured_joules_per_frame,algorithm_processing_ms,pipeline_e2e_ms,ga_compute_total_ms,ga_weight_update_ms,ga_evaluation_ms,ga_nsga_sort_ms,ga_selection_ms,ga_scheduler_apply_ms,ga_action_dispatch_overhead_ms,ga_pareto_export_ms,ga_diagnostics_ms,ga_mutation_ms,ga_offspring_ms,ga_postprocess_ms,action_apply_end_ns,algorithm_observed_epoch,algorithm_observed_time_ns,action_response_epoch,action_response_ms,cpu_temp_c,gpu_temp_c\n";
        } else {
            spdlog::warn("[GA-Pareto] Could not create action CSV: {}", cfg_.action_csv_path);
        }
    }
}

// ===================================================================
// Count Rank-0 solutions.
// ===================================================================
int GeneticAlgorithm::countRank0() const
{
    int count = 0;

    for (const auto& ind : population_) {
        if (ind.rank == 0) {
            ++count;
        }
    }

    return count;
}

// ===================================================================
// Export full Pareto front evidence.
// ===================================================================
void GeneticAlgorithm::exportParetoEvidence(
    const hrl::MetricsSnapshot& snap,
    const ParetoIndividual* selected)
{
    if (!cfg_.export_pareto_evidence) return;

    std::lock_guard<std::mutex> guard(evidenceMutex_);
    ensureParentDirectoryForFile(cfg_.pareto_csv_path);
    const bool writeHeader = !std::ifstream(cfg_.pareto_csv_path).good();
    std::ofstream out(cfg_.pareto_csv_path, std::ios::app);
    if (!out.is_open()) {
        spdlog::warn("[GA-Pareto] Could not open Pareto CSV: {}", cfg_.pareto_csv_path);
        return;
    }
    if (writeHeader) out << "schema_version,generation,workload_id,workload,workload_epoch,individual_id,rank,fitness,crowding_distance,obj_fps,obj_energy,obj_temp,obj_latency,target_fps_has,target_fps,power_budget_has,power_budget_watts,prefer_gpu_has,prefer_gpu,gpu_workload_split_has,gpu_workload_split,gpu_split_applicable,cpu_max_freq_has,cpu_max_freq_khz,policy_mode,awm_regime,awm_detected_regime,awm_w_fps,awm_w_power,awm_w_temp,awm_w_latency,awm_trigger_field,awm_trigger_value,awm_trigger_threshold,awm_regime_streak,awm_last_update_generation,surrogate_used,surrogate_source,surrogate_confidence,surrogate_from_data,surrogate_support_count,surrogate_neighbor_count,surrogate_nearest_distance,surrogate_exploration_bonus,surrogate_stall_penalty,surrogate_config_freq_khz,surrogate_config_gpu,surrogate_config_gpu_split,surrogate_config_concurrency,pred_fps,pred_power_w,pred_latency_ms,pred_temp_c,fit_fps_component,fit_power_component,fit_temp_component,fit_latency_component,fit_gpu_bonus,fit_before_age,fit_age_factor,fit_after_age,fit_final,feasible,constraint_violation,selected,measured_fps,measured_power_w,measured_joules_per_frame,algorithm_processing_ms,pipeline_e2e_ms,cpu_temp_c,gpu_temp_c\n";

    const std::string awmRegime = weightManager_ ? weightManager_->getCurrentRegimeName() : "FIXED_WEIGHTS";
    const std::string awmDetected = weightManager_ ? weightManager_->getLastDetectedRegimeName() : "FIXED_WEIGHTS";
    const std::string awmTriggerField = weightManager_ ? weightManager_->getLastTriggerField() : "none";
    const double awmTriggerValue = weightManager_ ? weightManager_->getLastTriggerValue() : 0.0;
    const double awmTriggerThreshold = weightManager_ ? weightManager_->getLastTriggerThreshold() : 0.0;
    const int awmStreak = weightManager_ ? weightManager_->getRegimeStreak() : 0;
    const int awmLastUpdate = weightManager_ ? weightManager_->getLastUpdateGeneration() : -1;

    for (size_t i = 0; i < population_.size(); ++i) {
        const auto& ind = population_[i];
        const bool isSelected = selected && (&ind == selected);

        const double targetFps = ind.genome.target_fps.has ? ind.genome.target_fps.value : 0.0;
        const double powerBudget = ind.genome.power_budget_watts.has ? ind.genome.power_budget_watts.value : 0.0;
        const int preferGpu = ind.genome.prefer_gpu.has ? (ind.genome.prefer_gpu.value ? 1 : 0) : -1;
        const double splitGene = ind.genome.gpu_workload_split.has ? ind.genome.gpu_workload_split.value : 0.0;
        const double cpuFreqGene = ind.genome.cpu_max_freq_khz.has ? ind.genome.cpu_max_freq_khz.value : 0.0;

        out << 4 << ","
            << generation_ << "," << currentWorkloadId_ << "," << currentWorkload_ << "," << workloadEpoch_ << "," << i << "," << ind.rank << ","
            << ind.fitness << "," << ind.crowdingDistance << ",";

        if (ind.objectives.size() >= 4) {
            out << ind.objectives[0] << "," << ind.objectives[1] << ","
                << ind.objectives[2] << "," << ind.objectives[3] << ",";
        } else {
            out << "0,0,0,0,";
        }

        out << (ind.genome.target_fps.has ? 1 : 0) << "," << targetFps << ","
            << (ind.genome.power_budget_watts.has ? 1 : 0) << "," << powerBudget << ","
            << (ind.genome.prefer_gpu.has ? 1 : 0) << "," << preferGpu << ","
            << (ind.genome.gpu_workload_split.has ? 1 : 0) << "," << splitGene << ","
            << (cfg_.gpuSplitApplicable ? 1 : 0) << ","
            << (ind.genome.cpu_max_freq_khz.has ? 1 : 0) << "," << cpuFreqGene << ","
            << static_cast<int>(ind.genome.mode) << ","
            << awmRegime << "," << awmDetected << ","
            << ind.fitnessWeights[0] << "," << ind.fitnessWeights[1] << ","
            << ind.fitnessWeights[2] << "," << ind.fitnessWeights[3] << ","
            << awmTriggerField << "," << awmTriggerValue << "," << awmTriggerThreshold << ","
            << awmStreak << "," << awmLastUpdate << ","
            << (ind.surrogateUsed ? 1 : 0) << ","
            << SurrogateModel::sourceName(ind.surrogatePrediction.source) << ","
            << ind.surrogatePrediction.confidence << ","
            << (ind.surrogatePrediction.from_data ? 1 : 0) << ","
            << ind.surrogatePrediction.support_count << ","
            << ind.surrogatePrediction.neighbor_count << ","
            << ind.surrogatePrediction.nearest_distance << ","
            << ind.surrogateExplorationBonus << ","
            << ind.surrogateStallPenalty << ","
            << ind.surrogateConfigFreqKhz << ","
            << (ind.surrogateConfigGpu ? 1 : 0) << ","
            << ind.surrogateConfigGpuSplit << ","
            << ind.surrogateConfigConcurrency << ","
            << ind.surrogatePrediction.fps << ","
            << ind.surrogatePrediction.power_w << ","
            << ind.surrogatePrediction.latency_ms << ","
            << ind.surrogatePrediction.temp_c << ","
            << ind.fitFpsComponent << "," << ind.fitPowerComponent << ","
            << ind.fitTempComponent << "," << ind.fitLatencyComponent << ","
            << ind.fitGpuBonus << "," << ind.fitBeforeAge << ","
            << ind.fitAgeFactor << "," << ind.fitAfterAge << "," << ind.fitFinal << ","
            << (ind.feasible ? 1 : 0) << "," << ind.constraintViolation << ","
            << (isSelected ? 1 : 0) << ","
            << snap.fps << "," << snap.avg_power_w_alg << "," << snap.joulesPerFrame << ","
            << snap.avg_inference_ms << "," << snap.end_to_end_latency_ms << ","
            << snap.cpu_temp_c << "," << snap.gpu_temp_c << "\n";
    }
}

// ===================================================================
// Export selected action trace.
// ===================================================================
void GeneticAlgorithm::exportSelectedActionEvidence(
    const hrl::MetricsSnapshot& snap,
    const ParetoIndividual& selected)
{
    if (!cfg_.export_pareto_evidence) return;

    std::lock_guard<std::mutex> guard(evidenceMutex_);
    ensureParentDirectoryForFile(cfg_.action_csv_path);
    const bool writeHeader = !std::ifstream(cfg_.action_csv_path).good();
    std::ofstream out(cfg_.action_csv_path, std::ios::app);
    if (!out.is_open()) {
        spdlog::warn("[GA-Pareto] Could not open action CSV: {}", cfg_.action_csv_path);
        return;
    }
    if (writeHeader) out << "schema_version,generation,workload_id,workload,workload_epoch,action_epoch,frame_id,active_algorithm_workload_epoch,algorithm_processed_workload_epoch,algorithm_processed_frame_id,rank0_count,selected_rank,selected_fitness,rank0_runnerup_fitness,fitness_margin,policy_mode,target_fps_has,target_fps,power_budget_has,power_budget_watts,prefer_gpu_has,prefer_gpu,gpu_workload_split_has,gpu_workload_split,gpu_split_applicable,cpu_max_freq_has,cpu_max_freq_gene_khz,cpu_max_freq_commanded_khz,runtime_enable_gpu,runtime_gpu_workload_split,runtime_concurrency,runtime_affinity,awm_regime,awm_detected_regime,awm_w_fps,awm_w_power,awm_w_temp,awm_w_latency,awm_trigger_field,awm_trigger_value,awm_trigger_threshold,awm_regime_streak,awm_last_update_generation,surrogate_used,surrogate_source,surrogate_confidence,surrogate_from_data,surrogate_support_count,surrogate_neighbor_count,surrogate_nearest_distance,surrogate_exploration_bonus,surrogate_stall_penalty,surrogate_config_freq_khz,surrogate_config_gpu,surrogate_config_gpu_split,surrogate_config_concurrency,pred_fps,pred_power_w,pred_latency_ms,pred_temp_c,obj_fps,obj_energy,obj_temp,obj_latency,fit_fps_component,fit_power_component,fit_temp_component,fit_latency_component,fit_gpu_bonus,fit_before_age,fit_age_factor,fit_after_age,fit_final,feasible,constraint_violation,measured_fps,measured_power_w,measured_joules_per_frame,algorithm_processing_ms,pipeline_e2e_ms,ga_compute_total_ms,ga_weight_update_ms,ga_evaluation_ms,ga_nsga_sort_ms,ga_selection_ms,ga_scheduler_apply_ms,ga_action_dispatch_overhead_ms,ga_pareto_export_ms,ga_diagnostics_ms,ga_mutation_ms,ga_offspring_ms,ga_postprocess_ms,action_apply_end_ns,algorithm_observed_epoch,algorithm_observed_time_ns,action_response_epoch,action_response_ms,cpu_temp_c,gpu_temp_c\n";

    const int rtGpu = runtimeControls_
        ? (runtimeControls_->enable_gpu.load(std::memory_order_relaxed) ? 1 : 0) : -1;
    const double rtSplit = runtimeControls_
        ? runtimeControls_->gpu_workload_split.load(std::memory_order_relaxed) : -1.0;
    const int rtConcurrency = runtimeControls_
        ? runtimeControls_->concurrency_level.load(std::memory_order_relaxed) : -1;
    const int rtAffinity = runtimeControls_
        ? static_cast<int>(runtimeControls_->affinity.load(std::memory_order_relaxed)) : -1;
    const long appliedCpuKhz = runtimeControls_
        ? runtimeControls_->commanded_cpu_max_freq_khz.load(std::memory_order_relaxed) : 0L;

    // Explicit workload provenance. Do not confuse algorithm_observed_epoch
    // below with workload epoch: the former is action/effect timing telemetry.
    const uint64_t activeAlgorithmWorkloadEpoch = runtimeControls_
        ? runtimeControls_->active_workload_epoch.load(std::memory_order_acquire) : workloadEpoch_;
    const uint64_t algorithmProcessedWorkloadEpoch = runtimeControls_
        ? runtimeControls_->algorithm_processed_workload_epoch.load(std::memory_order_acquire) : workloadEpoch_;
    const uint64_t algorithmProcessedFrameId = runtimeControls_
        ? runtimeControls_->algorithm_processed_frame_id.load(std::memory_order_acquire) : snap.frameId;

    const uint64_t actionEpoch = runtimeControls_
        ? runtimeControls_->action_generation.load(std::memory_order_acquire)
        : static_cast<uint64_t>(generation_) + 1ULL;
    const uint64_t applyEndNs = runtimeControls_
        ? runtimeControls_->action_apply_end_ns.load(std::memory_order_acquire) : 0ULL;
    const uint64_t observedEpoch = runtimeControls_
        ? runtimeControls_->algorithm_observed_generation.load(std::memory_order_acquire) : 0ULL;
    const uint64_t observedNs = runtimeControls_
        ? runtimeControls_->algorithm_observed_time_ns.load(std::memory_order_acquire) : 0ULL;
    const uint64_t responseEpoch = runtimeControls_
        ? runtimeControls_->action_response_generation.load(std::memory_order_acquire) : 0ULL;
    const uint64_t responseNs = runtimeControls_
        ? runtimeControls_->action_response_latency_ns.load(std::memory_order_acquire) : 0ULL;
    const double actionResponseMs = responseEpoch > 0
        ? static_cast<double>(responseNs) / 1.0e6 : -1.0;

    const std::string awmRegime = weightManager_ ? weightManager_->getCurrentRegimeName() : "FIXED_WEIGHTS";
    const std::string awmDetected = weightManager_ ? weightManager_->getLastDetectedRegimeName() : "FIXED_WEIGHTS";
    const std::string awmTriggerField = weightManager_ ? weightManager_->getLastTriggerField() : "none";
    const double awmTriggerValue = weightManager_ ? weightManager_->getLastTriggerValue() : 0.0;
    const double awmTriggerThreshold = weightManager_ ? weightManager_->getLastTriggerThreshold() : 0.0;
    const int awmStreak = weightManager_ ? weightManager_->getRegimeStreak() : 0;
    const int awmLastUpdate = weightManager_ ? weightManager_->getLastUpdateGeneration() : -1;

    const double targetFps = selected.genome.target_fps.has ? selected.genome.target_fps.value : 0.0;
    const double powerBudget = selected.genome.power_budget_watts.has ? selected.genome.power_budget_watts.value : 0.0;
    const int preferGpu = selected.genome.prefer_gpu.has ? (selected.genome.prefer_gpu.value ? 1 : 0) : -1;
    const double splitGene = selected.genome.gpu_workload_split.has ? selected.genome.gpu_workload_split.value : 0.0;
    const double cpuFreqGene = selected.genome.cpu_max_freq_khz.has ? selected.genome.cpu_max_freq_khz.value : 0.0;

    const double obj0 = selected.objectives.size() > 0 ? selected.objectives[0] : 0.0;
    const double obj1 = selected.objectives.size() > 1 ? selected.objectives[1] : 0.0;
    const double obj2 = selected.objectives.size() > 2 ? selected.objectives[2] : 0.0;
    const double obj3 = selected.objectives.size() > 3 ? selected.objectives[3] : 0.0;

    // Selected-action evidence schema v5 adds explicit dynamic-workload
    // provenance. Pareto-population evidence remains schema v4.
    out << 5 << ","
        << generation_ << "," << currentWorkloadId_ << "," << currentWorkload_ << "," << workloadEpoch_ << "," << actionEpoch << "," << snap.frameId << ","
        << activeAlgorithmWorkloadEpoch << "," << algorithmProcessedWorkloadEpoch << "," << algorithmProcessedFrameId << ","
        << lastEvaluatedRank0Count_ << "," << selected.rank << "," << selected.fitness << ","
        << lastRank0RunnerUpFitness_ << "," << lastSelectionMargin_ << ","
        << static_cast<int>(selected.genome.mode) << ","
        << (selected.genome.target_fps.has ? 1 : 0) << "," << targetFps << ","
        << (selected.genome.power_budget_watts.has ? 1 : 0) << "," << powerBudget << ","
        << (selected.genome.prefer_gpu.has ? 1 : 0) << "," << preferGpu << ","
        << (selected.genome.gpu_workload_split.has ? 1 : 0) << "," << splitGene << ","
        << (cfg_.gpuSplitApplicable ? 1 : 0) << ","
        << (selected.genome.cpu_max_freq_khz.has ? 1 : 0) << "," << cpuFreqGene << ","
        << appliedCpuKhz << "," << rtGpu << "," << rtSplit << ","
        << rtConcurrency << "," << rtAffinity << ","
        << awmRegime << "," << awmDetected << ","
        << selected.fitnessWeights[0] << "," << selected.fitnessWeights[1] << ","
        << selected.fitnessWeights[2] << "," << selected.fitnessWeights[3] << ","
        << awmTriggerField << "," << awmTriggerValue << "," << awmTriggerThreshold << ","
        << awmStreak << "," << awmLastUpdate << ","
        << (selected.surrogateUsed ? 1 : 0) << ","
        << SurrogateModel::sourceName(selected.surrogatePrediction.source) << ","
        << selected.surrogatePrediction.confidence << ","
        << (selected.surrogatePrediction.from_data ? 1 : 0) << ","
        << selected.surrogatePrediction.support_count << ","
        << selected.surrogatePrediction.neighbor_count << ","
        << selected.surrogatePrediction.nearest_distance << ","
        << selected.surrogateExplorationBonus << ","
        << selected.surrogateStallPenalty << ","
        << selected.surrogateConfigFreqKhz << ","
        << (selected.surrogateConfigGpu ? 1 : 0) << ","
        << selected.surrogateConfigGpuSplit << ","
        << selected.surrogateConfigConcurrency << ","
        << selected.surrogatePrediction.fps << ","
        << selected.surrogatePrediction.power_w << ","
        << selected.surrogatePrediction.latency_ms << ","
        << selected.surrogatePrediction.temp_c << ","
        << obj0 << "," << obj1 << "," << obj2 << "," << obj3 << ","
        << selected.fitFpsComponent << "," << selected.fitPowerComponent << ","
        << selected.fitTempComponent << "," << selected.fitLatencyComponent << ","
        << selected.fitGpuBonus << "," << selected.fitBeforeAge << ","
        << selected.fitAgeFactor << "," << selected.fitAfterAge << "," << selected.fitFinal << ","
        << (selected.feasible ? 1 : 0) << "," << selected.constraintViolation << ","
        << snap.fps << "," << snap.avg_power_w_alg << "," << snap.joulesPerFrame << ","
        << snap.avg_inference_ms << "," << snap.end_to_end_latency_ms << ","
        << lastTiming_.total_compute_ms << "," << lastTiming_.weight_update_ms << ","
        << lastTiming_.evaluation_ms << "," << lastTiming_.nsga_sort_ms << ","
        << lastTiming_.selection_ms << "," << lastTiming_.scheduler_apply_ms << ","
        << lastTiming_.action_dispatch_overhead_ms << "," << lastTiming_.pareto_export_ms << ","
        << lastTiming_.diagnostics_ms << "," << lastTiming_.mutation_ms << ","
        << lastTiming_.offspring_ms << "," << lastTiming_.postprocess_ms << ","
        << applyEndNs << "," << observedEpoch << "," << observedNs << ","
        << responseEpoch << "," << actionResponseMs << ","
        << snap.cpu_temp_c << "," << snap.gpu_temp_c << "\n";
}



} // namespace hrl



// //================================================== GeneticAlgorithm_05.h ===========================================================
// // FINAL GOLD STANDARD ? PhD-Ready (Merged Best of GA_03 + GA_04)
// // Pareto + Scalar Hybrid | Clean | Complete | Jetson Nano Optimized
// // GeneticAlgorithm_05.h - FINAL GOLD STANDARD ? PhD-Ready
// //=====================================================================================================================================

// #pragma once

// #include <vector>
// #include <random>
// #include <algorithm>
// #include <limits>
// #include <memory>
// #include <cmath>
// #include <spdlog/spdlog.h>

// #include <array>
// #include <cstdint>
// #include <fstream>
// #include <iomanip>
// #include <mutex>
// #include <sstream>
// #include <cerrno>
// #include <cstring>
// #include <sys/stat.h>
// #include <sys/types.h>

// #include "RuntimeControls.h"
// #include "../Stage_01/SharedStructures/allModulesStatcs.h"
// #include "RuntimeControls.h"   // ? ADD THIS
// #include "IScheduler.h"
// #include "AdaptiveWeightManager.h"
// #include "PowerSanity.h"          // physical-plausibility check used by headroom recovery
// #include "SurrogateModel.h"   // online counterfactual fitness surrogate (thesis gap 2)

// #include "../Stage_01/Concretes/SystemMetricsAggregatorConcrete_v3_2.h"
// #include "../Stage_01/Others/utils.h"

// namespace hrl {

// // ===================================================================
// // GAConfig
// // ===================================================================
// struct GAConfig {
//     size_t   popSize             = 12;
//     size_t   minPopSize          = 6;
//     size_t   eliteCount          = 3;
//     double   crossoverRate       = 0.75;
//     double   mutationRate        = 0.18;   // Base / floor mutation rate
//     double   explorationDecay    = 0.96;
//     int      controlIntervalMs   = 800;
//     size_t   reductionStartGen   = 10;
//     double   reductionRatio      = 0.6;

//     // [Adaptive mutation] Plateau-driven mutation control (extracted from
//     // AdaptivePopulationManager, made Pareto-safe ? it scales the rate only,
//     // never touches selection, so it composes cleanly with NSGA-II).
//     // When best fitness improves, mutation eases toward exploit; when it
//     // plateaus, mutation ramps up toward maxMutationRate to escape stagnation.
//     bool   adaptiveMutationEnabled = true;
//     double maxMutationRate         = 0.35;   // Hard cap when fully plateaued
//     // Relative improvement threshold (fraction of |prev best fitness|) below
//     // which a generation counts as "stagnating". Scaled to the fitness signal
//     // (which sits in the ~±0.05?0.20 band) rather than the old absolute 0.01.
//     double mutationPlateauRelThreshold = 0.02;   // 2% relative improvement
//     double mutationPlateauAbsFloor     = 0.0005; // abs floor for near-zero fitness
//     double mutationImproveFactor       = 0.8;    // multiply base when improving
//     double mutationPlateauStep         = 0.10;   // +10% of base per plateau gen

//     // [Surrogate] Online counterfactual fitness (thesis gap 2). When enabled, all
//     // candidates are scored by a learned per-config surrogate instead of the shared
//     // measured snapshot, and under-explored configs receive an optimism bonus.
//     // Disable for the ablation baseline (surrogate-off vs surrogate-on).
//     bool   surrogateEnabled       = true;
//     double surrogateExploreWeight = 0.15;   // UCB bonus scale in fitness units

//     // [FIX P2] Reality gating for counterfactual fitness.
//     int    surrogateMinSupport      = 3;    // below this support, predictions are pessimised
//     double surrogateColdFpsCeiling  = 8.0;  // fps ceiling before any real measurement exists
//     double surrogatePredFpsCapRatio = 1.2;  // pred_fps <= ratio * best measured fps this run
//     // [FIX P7] Physics-prior FPS ceiling, per workload (was hard-coded 60 for all).
//     double surrogateFpsMax          = 60.0;

//     // [FINAL FIX L1] Processing latency is a separate physical quantity from
//     // frame period (1000/FPS).  The surrogate therefore needs an independent
//     // cold-start latency prior instead of deriving latency from throughput.
//     double surrogateLatencyPriorMs  = 10.0;

//     // [FINAL FIX F1] Exact-bucket feasibility gate.  Repeated bounded evidence
//     // timeouts are treated as a constraint violation, not merely a soft scalar
//     // penalty.  This prevents a low-power configuration that repeatedly stalls
//     // from remaining Rank-0 because of attractive power/temperature objectives.
//     bool     hardFeasibilityEnabled         = true;
//     uint32_t hardFeasibilityMinAttempts     = 3;
//     double   hardFeasibilityMaxTimeoutRatio = 0.50;

//     // [FINAL FIX D1] Preserve one representative of each macro policy mode in
//     // every offspring population.  Representation does NOT imply admissibility:
//     // a thermally unsafe MAX candidate may remain genetically available while
//     // constraint-domination prevents it from being selected.
//     bool enforceModeDiversity = true;

//     // [FINAL FIX H1] When measured FPS is below the performance threshold while
//     // physically-valid power and temperatures still have headroom, LOW_POWER is
//     // temporarily inadmissible.  This encodes the intended control semantics:
//     // "spend available resource headroom to recover required performance".
//     bool   performanceHeadroomConstraint = true;
//     double performanceRecoveryRatio       = 0.70;
//     double performancePowerHeadroomW      = 0.50;
//     double performanceThermalHeadroomC    = 2.0;

//     // [FINAL FIX T1] In a measured THERMAL_CRITICAL state, MAX_PERFORMANCE is
//     // a hard feasibility violation rather than only another weighted penalty.
//     bool thermalCriticalBlocksMax = true;

//     // Whether the active algorithm physically consumes the continuous GPU split.
//     // ConfigManager sets this true only for HeterogeneousGaussianBlur. For
//     // MedianFilter and other binary CPU/GPU algorithms, the surrogate uses the
//     // canonical split {0,1} implied by GPU state so identical hardware states
//     // cannot acquire different model buckets.
//     // Fail closed: continuous split is opt-in.  ConfigManager/EvolutionarySelector
//     // must set true explicitly for HeterogeneousGaussianBlur.  Binary workloads
//     // (HistogramEqualization, MedianFilter, SobelEdge, etc.) remain canonical {0,1}.
//     bool gpuSplitApplicable = false;

//     // Pareto evidence export for PhD validation.
//     // Disabled by default to preserve normal runtime behaviour.
//     bool        export_pareto_evidence = false;
//     std::string pareto_csv_path = "output/erl_pareto_front.csv";
//     std::string action_csv_path = "output/erl_action_trace.csv";

//     // Do not let missing Lynsyn/power block Pareto evidence generation.
//     // On Jetson runs the power stream often starts later than FPS/camera metrics.
//     int    stableFramesRequired = 1;
//     double minFpsForEvolution = 1.0;
//     double minPowerForEvolution = 0.0;
//     bool   requirePowerForEvolution = false;

//     // AdaptiveWeightManager (opt-in ? default preserves current behaviour)
//     bool                           adaptive_weights_enabled = false;
//     AdaptiveWeightManager::Config  awm_config               = {};

//     // Reproducibility: if rngSeed >= 0, the GA seeds std::mt19937 deterministically.
//     // If rngSeed < 0 (default), it falls back to std::random_device for entropy.
//     // Set this (e.g. from config "seed") so ERL runs are seed-stable for the thesis.
//     long rngSeed = -1;

//     // Search-space floor for target_fps gene. Raised from the old hardcoded 8.0
//     // so the population cannot collapse onto the ~8 fps low-clock corner.
//     // Applied in initializePopulation(), mutate(), and crossover().
//     double minTargetFps = 20.0;
//     double maxTargetFps = 60.0;

//     // Workload policy. SobelEdge is GPU-dominant on the validated Jetson Nano
//     // data; ConfigManager enables this flag only for SobelEdge by default.
//     // ThermalGovernor remains authoritative and may still revoke GPU access.
//     bool   forceGpuForWorkload = false;

//     // Fixed performance reference used by HIGH_PERFORMANCE AWM/objective logic.
//     // It is intentionally independent of the evolvable target_fps gene.
//     double performanceTargetFps = 60.0;

//     // [PhD FIX] Explicit scalarization weights for the fitness function.
//     // These bridge the gap between EvolutionarySelector config and the GA's scalar fitness.
//     double weight_fps     = 1.0;
//     double weight_power   = 0.5;
//     double weight_temp    = 0.5;
//     double weight_latency = 0.3;

//     // [P0-F17] Temperature-objective onset thresholds. The old hard-coded
//     // 75C/80C onsets in calculateObjectives() were unreachable on Jetson
//     // Nano (39-65C observed across ERL and baselines), so objs[2] was
//     // identically zero and temperature never influenced dominance, crowding,
//     // or fitness. Defaults track the calibrated AdaptiveWeightManager warn
//     // thresholds (57C) as the single source of truth; override per run via
//     // obj_temp_onset_cpu_c / obj_temp_onset_gpu_c in the Scheduler block.
//     double temp_onset_cpu_c = AdaptiveWeightManager::Config().thermal_warn_cpu_c;
//     double temp_onset_gpu_c = AdaptiveWeightManager::Config().thermal_warn_gpu_c;

// };

// // ===================================================================
// // PhD timing instrumentation: per-generation GA phase breakdown.
// // All values are wall-clock milliseconds measured with steady_clock.
// // total_compute_ms excludes the final selected-action CSV append; the outer
// // EvolutionarySelector separately measures complete controller active time.
// // ===================================================================
// struct GATimingMetrics {
//     double weight_update_ms{0.0};
//     double evaluation_ms{0.0};
//     double nsga_sort_ms{0.0};
//     double selection_ms{0.0};
//     double scheduler_apply_ms{0.0};
//     double action_dispatch_overhead_ms{0.0};
//     double pareto_export_ms{0.0};
//     double diagnostics_ms{0.0};
//     double mutation_ms{0.0};
//     double offspring_ms{0.0};
//     double postprocess_ms{0.0};
//     double total_compute_ms{0.0};
//     bool valid{false};
// };

// // ===================================================================
// // ParetoIndividual (Single struct ? Dual Evaluation)
// // ===================================================================
// struct ParetoIndividual {
//     RLAction genome;

//     // Multi-objective (all minimized)
//     std::vector<double> objectives;   // [normFPS, normEnergy, normTemp, normLatency]

//     // Scalarized single fitness (for ranking & elitism)
//     double fitness = -std::numeric_limits<double>::max();

//     // NSGA-II
//     double crowdingDistance = 0.0;
//     int    rank = 0;

//     // [FINAL FIX F1/T1/H1] Constraint-domination state.
//     // Feasible candidates always dominate infeasible candidates.  Among
//     // infeasible candidates, smaller violation is preferred before normal
//     // objective dominance is considered.
//     bool   feasible = true;
//     double constraintViolation = 0.0;

//     // History
//     int    age = 0;
//     int    generation = 0;

//     // [PhD deterministic telemetry] Prediction/provenance used for THIS exact
//     // candidate evaluation. These fields are copied with the selected individual,
//     // preserving the decision rationale even after the population is replaced.
//     SurrogatePrediction surrogatePrediction{};
//     bool   surrogateUsed = false;
//     double surrogateExplorationBonus = 0.0;
//     double surrogateStallPenalty = 0.0;
//     double surrogateConfigFreqKhz = 0.0;
//     bool   surrogateConfigGpu = false;
//     double surrogateConfigGpuSplit = 0.0;
//     int    surrogateConfigConcurrency = 0;

//     // Exact scalar-fitness decomposition used to rank this candidate.
//     std::array<double, 4> fitnessWeights{{0.0, 0.0, 0.0, 0.0}};
//     double fitFpsComponent = 0.0;
//     double fitPowerComponent = 0.0;
//     double fitTempComponent = 0.0;
//     double fitLatencyComponent = 0.0;
//     double fitGpuBonus = 0.0;
//     double fitBeforeAge = 0.0;
//     double fitAgeFactor = 1.0;
//     double fitAfterAge = 0.0;
//     double fitFinal = -std::numeric_limits<double>::max();

//     ParetoIndividual() = default;

//     bool operator<(const ParetoIndividual& other) const {
//         if (rank != other.rank) return rank < other.rank;
//         return crowdingDistance > other.crowdingDistance;
//     }

//     bool dominates(const ParetoIndividual& other) const;

//     // bool dominates(const ParetoIndividual& other) const {
//     //     if (objectives.size() != other.objectives.size()) return false;
//     //     bool strictlyBetter = false;
//     //     for (size_t i = 0; i < objectives.size(); ++i) {
//     //         if (objectives[i] > other.objectives[i]) return false;
//     //         if (objectives[i] < other.objectives[i]) strictlyBetter = true;
//     //     }
//     //     return strictlyBetter;
//     // }
// };

// // ===================================================================
// // GeneticAlgorithm
// // Description: 
// // ===================================================================
// class GeneticAlgorithm {
// public:
//     // Overloaded constructor accepting awmCfg  
//     // GeneticAlgorithm(const GAConfig& cfg,
//     //                  std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator,
//     //                  std::shared_ptr<hrl::RuntimeControls> runtimeControls,
//     //                  IScheduler* scheduler = nullptr,
//     //                  const hrl::AdaptiveWeightManager::Config& awmCfg)

//     GeneticAlgorithm(const GAConfig& cfg,
//                      std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator,
//                      std::shared_ptr<hrl::RuntimeControls> runtimeControls,
//                      IScheduler* scheduler = nullptr,
//                      const hrl::AdaptiveWeightManager::Config& awmCfg =
//                          hrl::AdaptiveWeightManager::Config{})
//         : cfg_(cfg),
//           aggregator_(std::move(aggregator)),
//           runtimeControls_(std::move(runtimeControls)),
//           rng_(cfg.rngSeed >= 0
//                    ? static_cast<std::mt19937::result_type>(cfg.rngSeed)
//                    : std::random_device{}()),
//           scheduler_(scheduler),
//           generation_(0),
//           stableFrames_(0),
//           surrogate_(makeSurrogateConfig(cfg))
//     {
//         // Single authoritative AWM configuration/instance. The production
//         // EvolutionarySelector passes gaCfg.awm_config explicitly as arg 5.
//         cfg_.awm_config = awmCfg;

//         if (cfg_.rngSeed >= 0) {
//             spdlog::info("[GeneticAlgorithm] Deterministic RNG seed = {}", cfg_.rngSeed);
//         } else {
//             spdlog::info("[GeneticAlgorithm] Non-deterministic RNG (random_device)");
//         }

//         currentMutationRate_ = cfg_.mutationRate;
//         initializePopulation();
//         if (cfg_.export_pareto_evidence) ensureEvidenceFiles();

//         spdlog::info("[GeneticAlgorithm] Pareto-ERL ready (pop={}, reduction_gen={}, evidence={}, force_gpu={}, perf_target={:.1f})",
//                      cfg_.popSize, cfg_.reductionStartGen,
//                      cfg_.export_pareto_evidence ? "ON" : "OFF",
//                      cfg_.forceGpuForWorkload ? "YES" : "NO",
//                      cfg_.performanceTargetFps);

//         if (cfg_.adaptive_weights_enabled) {
//             weightManager_ = std::make_unique<hrl::AdaptiveWeightManager>(awmCfg);
//             spdlog::info("[GeneticAlgorithm] AdaptiveWeightManager ENABLED (update_interval={} gens, smoothing={:.2f})",
//                          awmCfg.update_interval_gens, awmCfg.transition_smoothing);
//         }
//     }

// //================================================================================================
// // [FIX P5] Called when the selector force-applies a recovery configuration
//     // outside the GA's control: the next evidence window must not be credited
//     // to the previously selected genome (it did not produce those frames).
//     void notifyExternalOverride() { hasLastApplied_ = false; }


// void evolve(const hrl::MetricsSnapshot& snapshot,
//             bool creditPreviousAction = true,
//             bool previousActionStalled = false) {
//     // if (!isSnapshotStable(snapshot)) {
//     //     lastTiming_ = GATimingMetrics{};
//     //     return;
//     // }
//     // [FINAL FIX F2] A timeout must still reach evaluateParetoPopulation()
//     // so the exact applied bucket receives observeStarvation(). The timeout
//     // is feasibility evidence only; it is no longer injected into the physical
//     // FPS/latency EMA.
//     if (!previousActionStalled && !isSnapshotStable(snapshot)) {
//         lastTiming_ = GATimingMetrics{};
//         return;
//     }

//     using TimingClock = std::chrono::steady_clock;
//     const auto totalStart = TimingClock::now();
//     lastTiming_ = GATimingMetrics{};

//     auto elapsedMs = [](const TimingClock::time_point& a,
//                         const TimingClock::time_point& b) -> double {
//         return std::chrono::duration<double, std::milli>(b - a).count();
//     };

//     // ============================================================
//     // 0) Adaptive-weight update
//     // ============================================================
//     auto phaseStart = TimingClock::now();
//     // [FINAL FIX A1] Regime classification is a controller-state decision,
//     // not a surrogate-credit decision.  Even when the previous action timed out,
//     // the latest valid snapshot must still be allowed to trigger
//     // HIGH_PERFORMANCE / thermal recovery.  Only surrogate learning is gated by
//     // creditPreviousAction below.
//     if (weightManager_ && snapshot.valid) {
//         // HIGH_PERFORMANCE detection uses a fixed experimental target, never an
//         // evolvable low target_fps gene.
//         const double targetFps = cfg_.performanceTargetFps;

//         bool changed = weightManager_->update(
//             snapshot,
//             static_cast<int>(generation_),
//             targetFps
//         );

//         if (changed) {
//             spdlog::info(
//                 "[GeneticAlgorithm] Weight regime -> {} | weights=[{:.3f},{:.3f},{:.3f},{:.3f}]",
//                 weightManager_->getCurrentRegimeName(),
//                 weightManager_->getWeights()[0],
//                 weightManager_->getWeights()[1],
//                 weightManager_->getWeights()[2],
//                 weightManager_->getWeights()[3]
//             );
//         }
//     }
//     auto phaseEnd = TimingClock::now();
//     lastTiming_.weight_update_ms = elapsedMs(phaseStart, phaseEnd);

//     // ============================================================
//     // 1) Evaluate current population
//     // ============================================================
//     phaseStart = TimingClock::now();
//     evaluateParetoPopulation(snapshot, creditPreviousAction, previousActionStalled);
//     phaseEnd = TimingClock::now();
//     lastTiming_.evaluation_ms = elapsedMs(phaseStart, phaseEnd);

//     // ============================================================
//     // 2) NSGA-II ranking + crowding
//     // ============================================================
//     phaseStart = TimingClock::now();
//     nonDominatedSortAndCrowding();
//     phaseEnd = TimingClock::now();
//     lastTiming_.nsga_sort_ms = elapsedMs(phaseStart, phaseEnd);
//     lastEvaluatedRank0Count_ = countRank0();

//     // ============================================================
//     // 3) Select evaluated Rank-0 action
//     // ============================================================
//     phaseStart = TimingClock::now();
//     const ParetoIndividual* selected = selectBestEvaluatedPareto();
//     ParetoIndividual selectedCopy;
//     const bool haveSelected = (selected != nullptr);

//     // Capture how decisive the Rank-0 choice was BEFORE population replacement.
//     //lastRank0RunnerUpFitness_ = std::numeric_limits<double>::quiet_NaN();
//     lastSelectionMargin_ = std::numeric_limits<double>::quiet_NaN();
//     if (haveSelected) {
//         double runnerUp = -std::numeric_limits<double>::max();
//         bool haveRunnerUp = false;
//         for (const auto& ind : population_) {
//             if (ind.rank != 0 || &ind == selected) continue;
//             if (!haveRunnerUp || ind.fitness > runnerUp) {
//                 runnerUp = ind.fitness;
//                 haveRunnerUp = true;
//             }
//         }
//         if (haveRunnerUp) {
//             lastRank0RunnerUpFitness_ = runnerUp;
//             lastSelectionMargin_ = selected->fitness - runnerUp;
//         }
//         selectedCopy = *selected;  // Preserve evidence after population replacement.
//     }
//     phaseEnd = TimingClock::now();
//     lastTiming_.selection_ms = elapsedMs(phaseStart, phaseEnd);

//     // ============================================================
//     // 4) Scheduler / runtime actuation
//     // ============================================================
//     phaseStart = TimingClock::now();
//     if (haveSelected) {
//         lastTiming_.scheduler_apply_ms = applySelectedParetoToRuntime(selectedCopy, snapshot);
//     }
//     phaseEnd = TimingClock::now();
//     const double actionDispatchTotalMs = elapsedMs(phaseStart, phaseEnd);
//     lastTiming_.action_dispatch_overhead_ms = std::max(
//         0.0, actionDispatchTotalMs - lastTiming_.scheduler_apply_ms);

//     // ============================================================
//     // 5) Pareto-front evidence (kept before mutation/replacement)
//     // ============================================================
//     phaseStart = TimingClock::now();
//     exportParetoEvidence(snapshot, selected);
//     phaseEnd = TimingClock::now();
//     lastTiming_.pareto_export_ms = elapsedMs(phaseStart, phaseEnd);

//     phaseStart = TimingClock::now();
//     logDominanceAnalysis();
//     logParetoStats();
//     phaseEnd = TimingClock::now();
//     lastTiming_.diagnostics_ms = elapsedMs(phaseStart, phaseEnd);

//     // ============================================================
//     // 5b) Adaptive mutation
//     // ============================================================
//     phaseStart = TimingClock::now();
//     {
//         // [P0-F18] Mutation progress must follow the same feasibility semantics
//         // as selection.  If a feasible Rank-0 candidate exists, track its best
//         // scalar fitness.  If the whole front is infeasible, use -violation as
//         // the progress signal (higher is better), so moving toward feasibility
//         // counts as improvement instead of rewarding an unsafe/high-fitness point.
//         double mutationSignal = -std::numeric_limits<double>::max();
//         bool usingConstraintSignal = false;

//         for (const auto& ind : population_) {
//             if (ind.rank == 0 && ind.feasible && ind.fitness > mutationSignal) {
//                 mutationSignal = ind.fitness;
//             }
//         }

//         if (mutationSignal == -std::numeric_limits<double>::max()) {
//             double minViolation = std::numeric_limits<double>::max();
//             for (const auto& ind : population_) {
//                 if (ind.rank == 0 && !ind.feasible) {
//                     minViolation = std::min(minViolation, ind.constraintViolation);
//                 }
//             }
//             if (minViolation < std::numeric_limits<double>::max()) {
//                 mutationSignal = -minViolation;
//                 usingConstraintSignal = true;
//             }
//         }

//         // Pre-evaluation defensive fallback only.  In a normal evaluated
//         // generation, one of the two branches above must provide the signal.
//         if (mutationSignal == -std::numeric_limits<double>::max() && !population_.empty()) {
//             mutationSignal = population_[0].fitness;
//         }

//         double r = updateAdaptiveMutationRate(mutationSignal);
//         if (generation_ % 10 == 0) {
//             spdlog::info("[GA-Pareto] Adaptive mutation rate={:.3f} "
//                          "(plateau={}, signal={:.4f}, source={})",
//                          r, mutationPlateauCounter_, mutationSignal,
//                          usingConstraintSignal ? "constraint" : "fitness");
//         }
//     }
//     phaseEnd = TimingClock::now();
//     lastTiming_.mutation_ms = elapsedMs(phaseStart, phaseEnd);

//     // ============================================================
//     // 6) Generate next population
//     // ============================================================
//     phaseStart = TimingClock::now();
//     reducePopulation();
//     auto newPop = selectEliteAndOffspring();

//     // [FINAL FIX D1] Crossover/elitism can otherwise extinguish an entire
//     // policy mode (observed as 8/8 BALANCED and later 8/8 MAX populations).
//     // Keep one MAX, one BALANCED and one LOW representative available.
//     ensureModeDiversity(newPop);

//     population_ = std::move(newPop);
//     phaseEnd = TimingClock::now();
//     lastTiming_.offspring_ms = elapsedMs(phaseStart, phaseEnd);

//     // ============================================================
//     // 7) Replay/diversity post-processing
//     // ============================================================
//     phaseStart = TimingClock::now();
//     seedRLReplayBuffer(snapshot);
//     injectDiversityIfNeeded();
//     phaseEnd = TimingClock::now();
//     lastTiming_.postprocess_ms = elapsedMs(phaseStart, phaseEnd);

//     const auto computeEnd = TimingClock::now();
//     lastTiming_.total_compute_ms = elapsedMs(totalStart, computeEnd);
//     lastTiming_.valid = true;

//     // Export the selected-action row after phase timings are finalised. This
//     // deliberately leaves the row's own file-append time out of ga_compute_total_ms;
//     // EvolutionarySelector's outer erl_active_wall_ms includes it.
//     if (haveSelected) {
//         exportSelectedActionEvidence(snapshot, selectedCopy);
//     }

//     ++generation_;
// }
// //=========================================================================================
//     // void evolve(const hrl::MetricsSnapshot& snapshot) {
//     //     if (!isSnapshotStable(snapshot)) return;

//     //     // Update adaptive weights BEFORE fitness evaluation
//     //     if (weightManager_) {
//     //         double targetFps = population_.empty() ? 30.0 :
//     //             (population_[0].genome.target_fps.has ? population_[0].genome.target_fps.value : 30.0);
//     //         bool changed = weightManager_->update(snapshot, static_cast<int>(generation_), targetFps);
//     //         if (changed) {
//     //             spdlog::info("[GeneticAlgorithm] Weight regime ? {} | weights=[{:.3f},{:.3f},{:.3f},{:.3f}]",
//     //                          weightManager_->getCurrentRegimeName(),
//     //                          weightManager_->getWeights()[0],
//     //                          weightManager_->getWeights()[1],
//     //                          weightManager_->getWeights()[2],
//     //                          weightManager_->getWeights()[3]);
//     //         }
//     //     }
        
//     //     // 1) Evaluate the CURRENT population against the latest measured snapshot.
//     //     evaluateParetoPopulation(snapshot);      // Dual: objectives + fitness
//     //     // 2) Rank by non-dominated sorting and crowding distance.
//     //     nonDominatedSortAndCrowding();
//     //     // 3) Select the best evaluated Rank-0 individual BEFORE offspring generation.
//     //     //    This is the key thesis-proof fix: the applied action is from the evaluated Pareto front.
//     //     const ParetoIndividual* selected = selectBestEvaluatedPareto();

//     //     // 4) Apply the selected evaluated Pareto action to runtime controls.
//     //     if (selected) {
//     //         applySelectedParetoToRuntime(*selected, snapshot);
//     //     }

//     //     // 5) Export evidence before population mutation/replacement.
//     //     exportParetoEvidence(snapshot, selected);
//     //     if (selected) {
//     //         exportSelectedActionEvidence(snapshot, *selected);
//     //     }

//     //     logDominanceAnalysis();     // PhD thesis material
//     //     logParetoStats();
       
//     //     // 6) Generate the NEXT population only after applying/logging the current front.
//     //     reducePopulation();
//     //     auto newPop = selectEliteAndOffspring();
//     //     population_ = std::move(newPop);

//     //     ++generation_;
//     //     applyBestParetoToRuntime(snapshot);
//     //     seedRLReplayBuffer(snapshot);
//     //     injectDiversityIfNeeded();
       
//     // }

//     RLAction getBestAction() const {
//         // [P0-F17] Centralize action eligibility in selectBestEvaluatedPareto().
//         // This prevents public callers from accidentally selecting an infeasible
//         // Rank-0 candidate if a ranking/telemetry regression occurs elsewhere.
//         const ParetoIndividual* best = selectBestEvaluatedPareto();
//         return best ? best->genome : RLAction{};
//     }

//     // Thesis data export ? CSV of weight history
//     std::string exportWeightHistory() const {
//         if (weightManager_) return weightManager_->exportHistoryCSV();
//         return "adaptive_weights_disabled\n";
//     }

//     // Current regime label for logging / thesis
//     std::string getCurrentRegimeName() const {
//         if (weightManager_) return weightManager_->getCurrentRegimeName();
//         return "FIXED_WEIGHTS";
//     }

//     GATimingMetrics getLastTimingMetrics() const { return lastTiming_; }
//     size_t getCurrentGeneration() const { return generation_; }

// private:
//     GAConfig cfg_;
//     std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator_;
//     std::shared_ptr<hrl::RuntimeControls> runtimeControls_;
//     std::vector<ParetoIndividual> population_;
//     std::mt19937 rng_;
//     IScheduler* scheduler_ = nullptr;
//     size_t generation_ = 0;
//     int stableFrames_ = 0;
//     std::unique_ptr<AdaptiveWeightManager> weightManager_;
//     mutable std::mutex evidenceMutex_;
//     GATimingMetrics lastTiming_{};
//     int lastEvaluatedRank0Count_{0};

//      // [FIX] Restored: declaration was lost while merging the trap patches.
//     // Written in evolve() (runner-up capture) and read by
//     // exportSelectedActionEvidence() -> rank0_runnerup_fitness CSV column.
//     double lastRank0RunnerUpFitness_{std::numeric_limits<double>::quiet_NaN()};

//     double lastSelectionMargin_{std::numeric_limits<double>::quiet_NaN()};

//     // [Adaptive mutation] runtime state
//     double currentMutationRate_ = 0.18;   // Initialised from cfg_.mutationRate in ctor
//     double prevBestFitness_     = -std::numeric_limits<double>::max();
//     int    mutationPlateauCounter_ = 0;

//     // [Surrogate] online counterfactual fitness model + last-applied tracking.
//     // snap on cycle N reflects the action applied on cycle N-1, so we observe the
//     // last-applied config against the current snapshot (temporal credit assignment).
//     SurrogateModel surrogate_;
//     RLAction       lastAppliedGenome_;
//     bool           hasLastApplied_ = false;

//     double         maxMeasuredFps_ = 0.0;   // [FIX P2] best hardware-verified fps this run

// //     // ===================================================================
// //     // Simulate simulateRuntimeControlsFromAction
// //     // ===================================================================
// //     void simulateRuntimeControlsFromAction(const RLAction& action,
// //                                        RuntimeControls& simControls) const {
// //     int concurrency = 2;
// //     bool enableGpu = true;
// //     Affinity affinity = Affinity::Spread;

// //     switch (action.mode) {
// //         case PolicyMode::MAX_PERFORMANCE:
// //             concurrency = 4;
// //             enableGpu = true;
// //             affinity = Affinity::Spread;
// //             break;

// //         case PolicyMode::LOW_POWER:
// //             concurrency = 1;
// //             enableGpu = false;
// //             affinity = Affinity::Pack;
// //             break;

// //         case PolicyMode::BALANCED:
// //             concurrency = 2;
// //             enableGpu = action.prefer_gpu.has ? action.prefer_gpu.value : true;
// //             affinity = Affinity::Spread;
// //             break;

// //         case PolicyMode::UNKNOWN:
// //         default:
// //             concurrency = 2;
// //             enableGpu = true;
// //             affinity = Affinity::Spread;
// //             break;
// //     }

// //     if (action.prefer_gpu.has) {
// //         enableGpu = action.prefer_gpu.value;
// //     }

// //     simControls.concurrency_level.store(concurrency, std::memory_order_relaxed);
// //     simControls.enable_gpu.store(enableGpu, std::memory_order_relaxed);
// //     simControls.affinity.store(affinity, std::memory_order_relaxed);
// // }

//     // ===================================================================
//     // Evaluation (Dual: Pareto + Scalar)
//     // ===================================================================
//     // ===================================================================
//     // [Surrogate] Build the surrogate model configuration from GAConfig.
//     // ===================================================================
//     // static SurrogateModel::Config makeSurrogateConfig(const GAConfig& cfg) {
//     //     SurrogateModel::Config sc;
//     //     sc.exploration_weight = cfg.surrogateExploreWeight;
//     //     return sc;
//     // }

//     static SurrogateModel::Config makeSurrogateConfig(const GAConfig& cfg) {
//         SurrogateModel::Config sc;
//         sc.exploration_weight   = cfg.surrogateExploreWeight;
//         sc.fps_max              = cfg.surrogateFpsMax;          // per-workload FPS envelope
//         sc.latency_prior_ms     = cfg.surrogateLatencyPriorMs;  // independent processing-latency prior
//         sc.gpu_split_applicable = cfg.gpuSplitApplicable;       // binary workloads collapse split to {0,1}
//         return sc;
//     }

//     void canonicalizeGenomeForWorkload(RLAction& g) const {
//         // UNKNOWN is never part of the evolutionary search space.  This guard
//         // also cleans legacy/replayed genomes before they reach the scheduler.
//         if (g.mode == PolicyMode::UNKNOWN) {
//             g.mode = PolicyMode::BALANCED;
//         }

//         // Workload-level GPU preference remains authoritative unless thermal
//         // safety later revokes GPU access in the Scheduler.
//         if (cfg_.forceGpuForWorkload) {
//             g.prefer_gpu = MaybeBool(true);
//         }

//         // [FINAL FIX S1] Binary CPU/GPU algorithms must never preserve a
//         // fictitious continuous split gene.  Canonicalising the genome itself
//         // (not just the surrogate key) makes action telemetry honest as well:
//         // CPU => split 0, GPU => split 1.
//         if (!cfg_.gpuSplitApplicable) {
//             const bool gpu = g.prefer_gpu.has
//                 ? g.prefer_gpu.value
//                 : (g.mode != PolicyMode::LOW_POWER);
//             g.gpu_workload_split = MaybeDouble(gpu ? 1.0 : 0.0);
//         } else if (g.gpu_workload_split.has) {
//             g.gpu_workload_split.value =
//                 utils::local_clamp(g.gpu_workload_split.value, 0.0, 1.0);
//         }

//         // MAX_PERFORMANCE has fixed semantic intent: do not let a low evolved
//         // target relabel a configuration as "high performance".
//         if (g.mode == PolicyMode::MAX_PERFORMANCE) {
//             g.target_fps = MaybeDouble(cfg_.performanceTargetFps);
//         }
//     }

//     // ===================================================================
//     // [Surrogate] Extract the discretised hardware config a genome represents.
//     // freq: explicit gene if present, else derived from PolicyMode.
//     // split: work-split gene if present (thesis gap 1), else GPU on=>0.5 / off=>0.
//     // conc: derived from mode/budget to mirror the Scheduler's mapping.
//     // ===================================================================
//     void configFromGenome(const RLAction& g, double& freq_khz, bool& gpu,
//                           double& split, int& conc) const {
//         gpu = g.prefer_gpu.has ? g.prefer_gpu.value
//                                : (g.mode != PolicyMode::LOW_POWER);
//         if (g.cpu_max_freq_khz.has) {
//             if (g.mode == PolicyMode::LOW_POWER) {
//                 freq_khz = utils::local_clamp(g.cpu_max_freq_khz.value, 460800.0, 1479000.0);
//             } else {
//                 freq_khz = utils::local_clamp(g.cpu_max_freq_khz.value, 102000.0, 1479000.0);
//             }
//         } else {
//             switch (g.mode) {
//                 case PolicyMode::MAX_PERFORMANCE: freq_khz = 1479000.0; break;
//                 case PolicyMode::LOW_POWER:       freq_khz = 1020000.0; break;
//                 default:                          freq_khz =  921600.0; break;
//             }
//         }
//         // [Micro-scheduler] Only algorithms that physically implement a
//         // continuous CPU/GPU partition may use the split gene as a surrogate
//         // dimension. Binary algorithms (e.g. MedianFilter) are canonicalised
//         // to 0=CPU or 1=GPU, preventing fictitious duplicate model states.
//         if (!cfg_.gpuSplitApplicable) {
//             split = gpu ? 1.0 : 0.0;
//         } else if (g.gpu_workload_split.has) {
//             split = g.gpu_workload_split.value;
//             if (split < 0.0) split = 0.0;
//             if (split > 1.0) split = 1.0;
//         } else {
//             split = gpu ? 1.0 : 0.0;
//         }
//         double budget = g.power_budget_watts.has ? g.power_budget_watts.value : 4.0;
//         switch (g.mode) {
//             case PolicyMode::MAX_PERFORMANCE: conc = 4; break;
//             case PolicyMode::LOW_POWER:       conc = (budget < 3.5) ? 1 : 2; break;
//             default:                          conc = 2; break;
//         }
//     }

//     // Build a snapshot with the surrogate's predicted outcome for `g`, copying
//     // any unrelated fields from the real snapshot so downstream code is unchanged.
//     hrl::MetricsSnapshot predictedSnapshot(const hrl::MetricsSnapshot& base,
//                                            const RLAction& g) const {
//         double freq; bool gpu; double split; int conc;
//         configFromGenome(g, freq, gpu, split, conc);
//         SurrogatePrediction p = surrogate_.predict(freq, gpu, split, conc);
//         hrl::MetricsSnapshot s = base;   // preserve unrelated fields
//         s.fps             = p.fps;
//         s.avg_power_w_alg = p.power_w;
//         s.avg_latency_ms  = p.latency_ms;
//         s.cpu_temp_c      = p.temp_c;
//         return s;
//     }

//     void evaluateParetoPopulation(const hrl::MetricsSnapshot& snap,
//                                   bool creditPreviousAction,
//                                   bool previousActionStalled) {
//         // // One action -> one evidence window -> one surrogate credit update.
//         // if (cfg_.surrogateEnabled && hasLastApplied_) {
//         //     double freq; bool gpu; double split; int conc;
//         //     configFromGenome(lastAppliedGenome_, freq, gpu, split, conc);
//         //     if (previousActionStalled) {
//         //         surrogate_.observeStarvation(freq, gpu, split, conc);
//         //     } else if (creditPreviousAction) {
//         //         surrogate_.observe(freq, gpu, split, conc,
//         //                            snap.fps, snap.avg_power_w_alg,
//         //                            snap.avg_latency_ms, snap.cpu_temp_c);
//         //     }
//         // }
//         // One action -> one evidence window -> one surrogate credit update.
//         if (cfg_.surrogateEnabled && hasLastApplied_) {
//             double freq; bool gpu; double split; int conc;
//             configFromGenome(lastAppliedGenome_, freq, gpu, split, conc);
//             if (previousActionStalled) {
//                 // [FINAL FIX F2] A timeout is real feasibility evidence but it
//                 // is NOT an exact physical FPS/latency sample.  Fresh-frame poll
//                 // count is not guaranteed to equal processed-frame count, so do
//                 // not poison the physical EMA with a fabricated throughput.
//                 surrogate_.observeStarvation(freq, gpu, split, conc);
//             } else if (creditPreviousAction) {
//                 surrogate_.observe(freq, gpu, split, conc,
//                                    snap.fps, snap.avg_power_w_alg,
//                                    snap.avg_latency_ms, snap.cpu_temp_c);
//                 if (snap.fps > maxMeasuredFps_) maxMeasuredFps_ = snap.fps; // [FIX P2]
//             }
//         }

//         for (auto& ind : population_) {
//             canonicalizeGenomeForWorkload(ind.genome);
//             hrl::RuntimeControls simControls;
//             simControls.concurrency_level.store(2, std::memory_order_relaxed);
//             simControls.enable_gpu.store(true, std::memory_order_relaxed);
//             simulateRuntimeControlsFromAction(ind.genome, simControls);

//             // Preserve the exact counterfactual prediction used for THIS candidate.
//             hrl::MetricsSnapshot evalSnap = snap;
//             ind.surrogateUsed = false;
//             ind.surrogatePrediction = SurrogatePrediction{};
//             ind.surrogateExplorationBonus = 0.0;
//             ind.surrogateStallPenalty = 0.0;
//             ind.surrogateConfigFreqKhz = 0.0;
//             ind.surrogateConfigGpu = false;
//             ind.surrogateConfigGpuSplit = 0.0;
//             ind.surrogateConfigConcurrency = 0;
//             ind.feasible = true;
//             ind.constraintViolation = 0.0;

//             if (cfg_.surrogateEnabled) {
//                 double freq; bool gpu; double split; int conc;
//                 configFromGenome(ind.genome, freq, gpu, split, conc);

//                 ind.surrogateConfigFreqKhz = freq;
//                 ind.surrogateConfigGpu = gpu;
//                 ind.surrogateConfigGpuSplit = split;
//                 ind.surrogateConfigConcurrency = conc;

//                 // ind.surrogatePrediction = surrogate_.predict(freq, gpu, split, conc);
//                 // ind.surrogateUsed = true;
//                 // evalSnap.fps             = ind.surrogatePrediction.fps;

//                 ind.surrogatePrediction = surrogate_.predict(freq, gpu, split, conc);

//                 // [FIX P2] Reality gating: unverified optimism must not enter fitness.
//                 {
//                     SurrogatePrediction& p = ind.surrogatePrediction;
//                     const double fpsCap = (maxMeasuredFps_ > 0.0)
//                         ? cfg_.surrogatePredFpsCapRatio * maxMeasuredFps_
//                         : cfg_.surrogateColdFpsCeiling;
//                     const bool lowSupport = !p.from_data ||
//                         p.support_count < static_cast<uint32_t>(cfg_.surrogateMinSupport);
//                     if (lowSupport) {
//                         const double pessimistic = (maxMeasuredFps_ > 0.0)
//                             ? 0.5 * maxMeasuredFps_
//                             : 0.5 * cfg_.surrogateColdFpsCeiling;
//                         p.fps = std::min(p.fps, std::max(1.0, pessimistic));
//                     }
//                     if (p.fps > fpsCap) p.fps = fpsCap;   // unverified over-envelope claims are inadmissible

//                     // [FINAL FIX L1] DO NOT impose latency >= 1000/FPS.
//                     // 1000/FPS is the inter-frame period, not per-frame
//                     // processing/E2E latency.  Latency remains an independently
//                     // learned surrogate dimension.
//                 }

//                 ind.surrogateUsed = true;
//                 evalSnap.fps             = ind.surrogatePrediction.fps;

//                 evalSnap.avg_power_w_alg = ind.surrogatePrediction.power_w;
//                 evalSnap.avg_latency_ms  = ind.surrogatePrediction.latency_ms;
//                 evalSnap.cpu_temp_c      = ind.surrogatePrediction.temp_c;

//                 ind.surrogateExplorationBonus =
//                     surrogate_.explorationBonus(freq, gpu, split, conc);
//                 ind.surrogateStallPenalty =
//                     surrogate_.stallPenalty(freq, gpu, split, conc);

//                 // [FINAL FIX F1] Exact-bucket timeout feasibility.  Keep the
//                 // existing soft stall penalty for sparse evidence, but once an
//                 // exact configuration has enough attempts and a majority timeout
//                 // rate, constraint-domination removes it from the feasible front.
//                 if (cfg_.hardFeasibilityEnabled) {
//                     const SurrogateFeasibilityStats fs =
//                         surrogate_.feasibilityStats(freq, gpu, split, conc);
//                     if (fs.attempts >= cfg_.hardFeasibilityMinAttempts &&
//                         fs.timeout_ratio > cfg_.hardFeasibilityMaxTimeoutRatio) {
//                         ind.feasible = false;
//                         ind.constraintViolation += 1.0 + fs.timeout_ratio;
//                     }
//                 }
//             }

//             // [FINAL FIX H1] Performance recovery with resource headroom.
//             // If measured throughput is below the required band while power and
//             // temperatures are safely below their limits, LOW_POWER is not an
//             // admissible action.  This directly prevents the "2 W / 2 FPS while
//             // cool" trap observed in HistogramEqualization.
//             if (cfg_.performanceHeadroomConstraint &&
//                 ind.genome.mode == PolicyMode::LOW_POWER) {
//                 const double requiredFps =
//                     cfg_.performanceRecoveryRatio * cfg_.performanceTargetFps;
//                 const bool fpsDeficit = snap.fps < requiredFps;
//                 const bool powerHeadroom =
//                     PowerSanity::valid(snap.avg_power_w_alg) &&
//                     snap.avg_power_w_alg <
//                         (cfg_.awm_config.battery_critical_watts -
//                          cfg_.performancePowerHeadroomW);
//                 const bool thermalHeadroom =
//                     snap.cpu_temp_c <
//                         (cfg_.awm_config.thermal_warn_cpu_c -
//                          cfg_.performanceThermalHeadroomC) &&
//                     snap.gpu_temp_c <
//                         (cfg_.awm_config.thermal_warn_gpu_c -
//                          cfg_.performanceThermalHeadroomC);

//                 if (fpsDeficit && powerHeadroom && thermalHeadroom) {
//                     ind.feasible = false;
//                     const double deficitRatio =
//                         requiredFps > 0.0
//                             ? utils::local_clamp(
//                                   (requiredFps - snap.fps) / requiredFps,
//                                   0.0, 1.0)
//                             : 0.0;
//                     ind.constraintViolation += 1.0 + deficitRatio;
//                 }
//             }

//             // [FINAL FIX T1] Thermal safety is an admissibility rule, not just
//             // another scalar weight.  Preserve MAX genetically for later
//             // recovery, but prevent selection while the measured board is in
//             // the configured critical thermal band.
//             if (cfg_.thermalCriticalBlocksMax &&
//                 ind.genome.mode == PolicyMode::MAX_PERFORMANCE) {
//                 const double cpuOver = std::max(
//                     0.0, snap.cpu_temp_c - cfg_.awm_config.thermal_crit_cpu_c);
//                 const double gpuOver = std::max(
//                     0.0, snap.gpu_temp_c - cfg_.awm_config.thermal_crit_gpu_c);
//                 if (cpuOver > 0.0 || gpuOver > 0.0) {
//                     ind.feasible = false;
//                     ind.constraintViolation +=
//                         1.0 + (cpuOver + gpuOver) / 10.0;
//                 }
//             }

//             ind.objectives = calculateObjectives(evalSnap, ind.genome, simControls);
//             ind.fitness = calculateScalarizedFitness(ind.objectives, evalSnap,
//                                                       ind.genome, ind.age, &ind);

//             // Optimism under uncertainty is added AFTER scalar fitness/age decay,
//             // preserving the existing controller arithmetic exactly.
//             ind.fitness += ind.surrogateExplorationBonus;
//             ind.fitness -= ind.surrogateStallPenalty;
//             ind.fitFinal = ind.fitness;
//             ind.age++;
//         }
//     }


// //================================================================
//     //  Return vector of normalized objectives (all to be minimized)
//     std::vector<double> calculateObjectives(const hrl::MetricsSnapshot& snap,
//                                             const RLAction& action,
//                                             const RuntimeControls& controls) {
//         std::vector<double> objs(4);
//         // Objective 1: FPS (minimize negative FPS = maximize FPS)

//         const bool highPerformanceRegime =
//             weightManager_ && weightManager_->getCurrentRegime() == SystemRegime::HIGH_PERFORMANCE;
//         const double fpsScore = action.target_fps.has
//             ? (1.0 / (1.0 + std::abs(snap.fps - action.target_fps.value))) : 0.0;
//         const double directThroughput = cfg_.performanceTargetFps > 0.0
//             ? utils::local_clamp(snap.fps / cfg_.performanceTargetFps, 0.0, 2.0)
//             : 0.0;

//         // [P0] Frequency-aware energy model. The measured snapshot power reflects
//         // the CURRENT clock, but each candidate would run at its own gene frequency.
//         // Scale the energy estimate by (candidate_freq / max_freq) so a low-clock
//         // genome is correctly scored as cheaper and a high-clock genome as costlier.
//         // Without this, LOW_POWER and MAX_PERFORMANCE candidates score near-identical
//         // energy and the Pareto front collapses onto the idle low-clock corner.
//         // const double kMaxFreqKhz = 1479000.0;
//         // double freqRatio = action.cpu_max_freq_khz.has
//         //     ? (action.cpu_max_freq_khz.value / kMaxFreqKhz)
//         //     : 1.0;
//         // freqRatio = freqRatio < 0.05 ? 0.05 : (freqRatio > 1.0 ? 1.0 : freqRatio);

//         // double energy  = snap.avg_power_w_alg * freqRatio
//         //                  * (1.0 + 0.3 * (controls.concurrency_level.load() - 2));
//         // [FIX P4] With the surrogate on, snap.avg_power_w_alg is already the
//         // per-candidate PREDICTED power at this genome's own frequency/config;
//         // multiplying by freqRatio double-counted the clock and let 102 MHz
//         // genomes buy the energy objective ~15-20x below reality (Pareto rank-0
//         // then filled with LOW_POWER fictions). Keep the legacy discount only
//         // for the surrogate-off ablation baseline.
//         double energy = snap.avg_power_w_alg;
//         if (!cfg_.surrogateEnabled) {
//             const double kMaxFreqKhz = 1479000.0;
//             double freqRatio = action.cpu_max_freq_khz.has
//                 ? (action.cpu_max_freq_khz.value / kMaxFreqKhz)
//                 : 1.0;
//             freqRatio = freqRatio < 0.05 ? 0.05 : (freqRatio > 1.0 ? 1.0 : freqRatio);
//             energy *= freqRatio * (1.0 + 0.3 * (controls.concurrency_level.load() - 2));
//         }
//         // [P0-F17] Penalty onset uses the calibrated thresholds (default 57C
//         // via AdaptiveWeightManager::Config) instead of hard-coded 75C/80C,
//         // so the live ERL-vs-baseline thermal gradient (39-45C vs 61-65C)
//         // actually reaches the objective vector.
//         double tempPen = (snap.cpu_temp_c > cfg_.temp_onset_cpu_c ? snap.cpu_temp_c - cfg_.temp_onset_cpu_c : 0.0) +
//                          (snap.gpu_temp_c > cfg_.temp_onset_gpu_c ? snap.gpu_temp_c - cfg_.temp_onset_gpu_c : 0.0);
//         double latency = snap.avg_latency_ms;

//         // In HIGH_PERFORMANCE, maximize actual predicted throughput against a
//         // fixed reference. Other regimes preserve the legacy target-matching term.
//         objs[0] = highPerformanceRegime ? -directThroughput : -fpsScore;           // Maximize FPS // Objective 1: FPS (minimize negative FPS = maximize FPS)
//         objs[1] = energy / 10.0;       // Minimize energy // Objective 2: Energy/Power (minimize)
//         objs[2] = tempPen / 20.0;      // Minimize temperature  // Objective 3: Temperature penalty (minimize)
//         objs[3] = latency / 50.0;      // Minimize latency  // Objective 4: Latency (minimize)  // normalize
//         return objs;
//     }

// // ===================================================================
// // NSGA-II Core
// // ===================================================================
//     void nonDominatedSortAndCrowding();
//     void calculateCrowdingDistance(std::vector<int>& front);
//     const ParetoIndividual& tournamentSelect(int tournamentSize);
    
//     // Scalarized fitness: FPS=50%, Power=20%, Temp=20%, Latency=10% (PhD dual evaluation)
//     double calculateScalarizedFitness(const std::vector<double>& objs,
//                                                      const hrl::MetricsSnapshot& snap,
//                                                      const RLAction& action,
//                                                      int age,
//                                                      ParetoIndividual* auditOut) const ;

//     // ===================================================================
//     // Evolution Operators
//     // ===================================================================
//     ParetoIndividual crossover(const ParetoIndividual& p1, const ParetoIndividual& p2);
//     //void mutate(ParetoIndividual& ind);
//     void mutate(ParetoIndividual& ind, const hrl::MetricsSnapshot& snap);

//     std::vector<ParetoIndividual> selectEliteAndOffspring();

//     // [FINAL FIX D1] Hard population invariant: retain at least one
//     // MAX_PERFORMANCE, one BALANCED and one LOW_POWER genome.
//     void ensureModeDiversity(std::vector<ParetoIndividual>& pop);

//     void reducePopulation();
//     void injectDiversityIfNeeded();

//     // ===================================================================
//     // Runtime & Logging
//     // ===================================================================
//     // void applyBestParetoToRuntime(const hrl::MetricsSnapshot& snapshot);
//     // void seedRLReplayBuffer(const hrl::MetricsSnapshot& snapshot);
//     // void logParetoStats();
//     // void logDominanceAnalysis();

//     // bool isSnapshotStable(const hrl::MetricsSnapshot& snap);
//     // void initializePopulation();

//     // ===================================================================
//     // Runtime & Logging
//     // ===================================================================
//     void applyBestParetoToRuntime(const hrl::MetricsSnapshot& snapshot);

//     const ParetoIndividual* selectBestEvaluatedPareto() const;

//     double applySelectedParetoToRuntime(const ParetoIndividual& selected,
//                                         const hrl::MetricsSnapshot& snapshot);

//     void exportParetoEvidence(const hrl::MetricsSnapshot& snap,
//                               const ParetoIndividual* selected);

//     void exportSelectedActionEvidence(const hrl::MetricsSnapshot& snap,
//                                       const ParetoIndividual& selected);

//     void ensureEvidenceFiles();
//     static bool ensureParentDirectoryForFile(const std::string& filePath);

//     int countRank0() const;

//     void simulateRuntimeControlsFromAction(const RLAction& action,
//                                            RuntimeControls& simControls) const;

//     void seedRLReplayBuffer(const hrl::MetricsSnapshot& snapshot);
//     void logParetoStats();
//     void logDominanceAnalysis();

//     bool isSnapshotStable(const hrl::MetricsSnapshot& snap);
//     void initializePopulation();

//     // [Adaptive mutation] Update currentMutationRate_ from best-fitness trend.
//     // Pure rate control ? does not alter selection, so it is Pareto-safe.
//     double updateAdaptiveMutationRate(double currentBestFitness);
// };

// // ===================================================================
// // Implementations (add to .cpp or keep inline)
// // ===================================================================
// // ... (nonDominatedSortAndCrowding, calculateCrowdingDistance, tournamentSelect remain as in your GA_03/04)

// // Crossover & Mutate
// //ParetoIndividual GeneticAlgorithm::crossover(const ParetoIndividual& p1, const ParetoIndividual& p2) { /* same as before */ }


// // ====================== Inline Implementations ======================
// inline bool ParetoIndividual::dominates(const ParetoIndividual& other) const {
//     // [FINAL FIX F1/T1/H1] Deb-style constraint domination:
//     //   1) feasible always beats infeasible;
//     //   2) among infeasible candidates, smaller violation wins;
//     //   3) ties fall through to normal Pareto objective dominance.
//     if (feasible && !other.feasible) return true;
//     if (!feasible && other.feasible) return false;

//     if (!feasible && !other.feasible) {
//         constexpr double kConstraintEps = 1e-12;
//         if (constraintViolation + kConstraintEps < other.constraintViolation)
//             return true;
//         if (other.constraintViolation + kConstraintEps < constraintViolation)
//             return false;
//     }

//     if (objectives.size() != other.objectives.size()) return false;
//     bool strictlyBetter = false;
//     for (size_t i = 0; i < objectives.size(); ++i) {
//         if (objectives[i] > other.objectives[i]) return false;
//         if (objectives[i] < other.objectives[i]) strictlyBetter = true;
//     }
//     return strictlyBetter;
// }




//   // ===================================================================
//     // Evolution Operators (UPDATED for ParetoIndividual)
//     // ===================================================================
//     ParetoIndividual GeneticAlgorithm::crossover(const ParetoIndividual& p1, const ParetoIndividual& p2) {
//         ParetoIndividual child;
//         std::uniform_real_distribution<double> mix(0.0, 1.0);

//         child.genome.mode = mix(rng_) < 0.5 ? p1.genome.mode : p2.genome.mode;

//         if (p1.genome.target_fps.has && p2.genome.target_fps.has) {
//             double avg = (p1.genome.target_fps.value + p2.genome.target_fps.value) * 0.5;
//             child.genome.target_fps = MaybeDouble(utils::local_clamp(avg, cfg_.minTargetFps, cfg_.maxTargetFps));
//         }

//         if (p1.genome.prefer_gpu.has && p2.genome.prefer_gpu.has) {
//             child.genome.prefer_gpu = mix(rng_) < 0.6 ? p1.genome.prefer_gpu : p2.genome.prefer_gpu;
//         }

//         // [P0] Inherit the CPU frequency gene from one parent (discrete, so no averaging).
//         if (p1.genome.cpu_max_freq_khz.has || p2.genome.cpu_max_freq_khz.has) {
//             const RLAction& src = (mix(rng_) < 0.5 ? p1.genome : p2.genome);
//             child.genome.cpu_max_freq_khz = src.cpu_max_freq_khz.has
//                 ? src.cpu_max_freq_khz
//                 : (p1.genome.cpu_max_freq_khz.has ? p1.genome.cpu_max_freq_khz
//                                                   : p2.genome.cpu_max_freq_khz);
//         }

//         // [Micro-scheduler] Blend the continuous split gene. Unlike the discrete
//         // frequency gene, averaging is meaningful here and lets crossover explore
//         // intermediate load balances between two parents.
//         if (p1.genome.gpu_workload_split.has && p2.genome.gpu_workload_split.has) {
//             double avg = 0.5 * (p1.genome.gpu_workload_split.value +
//                                 p2.genome.gpu_workload_split.value);
//             child.genome.gpu_workload_split = MaybeDouble(utils::local_clamp(avg, 0.0, 1.0));
//         } else if (p1.genome.gpu_workload_split.has || p2.genome.gpu_workload_split.has) {
//             child.genome.gpu_workload_split = p1.genome.gpu_workload_split.has
//                 ? p1.genome.gpu_workload_split : p2.genome.gpu_workload_split;
//         }
//         canonicalizeGenomeForWorkload(child.genome);
//         return child;
//     }


// //void GeneticAlgorithm::mutate(ParetoIndividual& ind) { /* same as before */ }

//     void GeneticAlgorithm::mutate(ParetoIndividual& ind, const hrl::MetricsSnapshot& snap) {
//         (void)snap;
//         std::uniform_real_distribution<double> dist(0.0, 1.0);
//         if (dist(rng_) >= currentMutationRate_) return;

//         if (ind.genome.target_fps.has) {
//             std::normal_distribution<double> fpsMut(0.0, 4.0);
//             double newFps = ind.genome.target_fps.value + fpsMut(rng_);
//             ind.genome.target_fps = MaybeDouble(utils::local_clamp(newFps, cfg_.minTargetFps, cfg_.maxTargetFps));
//         }

//         if (ind.genome.prefer_gpu.has && dist(rng_) < 0.25) {
//             ind.genome.prefer_gpu = MaybeBool(!ind.genome.prefer_gpu.value);
//         }

//         // [Micro-scheduler] Mutate the continuous GPU workload fraction by a small
//         // Gaussian perturbation, clamped to [0,1]. This is the fine-grained load-
//         // balancing search that a binary toggle cannot express.
//         if (ind.genome.gpu_workload_split.has) {
//             std::normal_distribution<double> splitMut(0.0, 0.15);
//             double s = ind.genome.gpu_workload_split.value + splitMut(rng_);
//             ind.genome.gpu_workload_split = MaybeDouble(utils::local_clamp(s, 0.0, 1.0));
//         }

//         // [P0] Mutate the CPU frequency gene by stepping to a neighbouring DVFS level.
//         if (ind.genome.cpu_max_freq_khz.has) {
//             static const long kFreqSteps[] = {102000, 460800, 921600, 1190400, 1479000};
//             const int n = 5;
//             // Find nearest current index
//             long cur = static_cast<long>(ind.genome.cpu_max_freq_khz.value);
//             int idx = 0; long best = std::abs(kFreqSteps[0] - cur);
//             for (int k = 1; k < n; ++k) {
//                 long d = std::abs(kFreqSteps[k] - cur);
//                 if (d < best) { best = d; idx = k; }
//             }
//             int step = (dist(rng_) < 0.5) ? -1 : 1;
//             idx = utils::local_clamp(idx + step, 0, n - 1);
//             ind.genome.cpu_max_freq_khz = MaybeDouble(static_cast<double>(kFreqSteps[idx]));
//         }

//         if (dist(rng_) < 0.07) {
//             std::uniform_int_distribution<int> modeDist(1, 3);
//             ind.genome.mode = static_cast<PolicyMode>(modeDist(rng_));
//         }
//         canonicalizeGenomeForWorkload(ind.genome);
//     }




// // selectEliteAndOffspring, reducePopulation, injectDiversityIfNeeded, applyBestParetoToRuntime, etc. ? use the clean versions from GA_04



//     // ===================================================================
//     // Selection + Population Management
//     // ===================================================================
//     std::vector<ParetoIndividual> GeneticAlgorithm::selectEliteAndOffspring() {
//         std::vector<ParetoIndividual> newPop;

//         // [P0-F16] Defensive: tournamentSelect() draws indices in
//         // [0, population_.size()-1]; on an empty population that distribution
//         // is degenerate and indexing is UB. Self-heal instead of crashing.
//         if (population_.empty()) {
//             spdlog::error("[GA-Pareto] Population empty at selection - reinitializing");
//             initializePopulation();
//         }

//         // Elitism (population_ is sorted by rank/crowding - see [P0-F16])
//         for (size_t i = 0; i < std::min(cfg_.eliteCount, population_.size()); ++i) {
//             newPop.push_back(population_[i]);
//         }

//         // Offspring
//         while (newPop.size() < cfg_.popSize) {
//             const auto& p1 = tournamentSelect(3);
//             const auto& p2 = tournamentSelect(3);
//             auto child = crossover(p1, p2);
//             mutate(child, hrl::MetricsSnapshot{});   // snapshot not used in mutate anymore
//             newPop.push_back(child);
//         }
//         return newPop;
//     }


//     // ===================================================================
//     // [FINAL FIX D1] Mode-diversity floor
//     // ===================================================================
//     // The Aug-19 HistogramEqualization trace showed long 8/8 BALANCED and
//     // 8/8 MAX monocultures.  Mutation is too weak to guarantee recovery after
//     // a mode goes extinct, so diversity is enforced as a population invariant.
//     // This does NOT bypass feasibility: an unsafe representative can remain in
//     // the population while constraint-domination prevents its selection.
//     void GeneticAlgorithm::ensureModeDiversity(
//         std::vector<ParetoIndividual>& pop)
//     {
//         if (!cfg_.enforceModeDiversity || pop.size() < 3) return;

//         auto countMode = [&pop](PolicyMode mode) -> size_t {
//             return static_cast<size_t>(std::count_if(
//                 pop.begin(), pop.end(),
//                 [mode](const ParetoIndividual& x) {
//                     return x.genome.mode == mode;
//                 }));
//         };

//         const std::array<PolicyMode, 3> required{{
//             PolicyMode::MAX_PERFORMANCE,
//             PolicyMode::BALANCED,
//             PolicyMode::LOW_POWER
//         }};

//         for (PolicyMode missing : required) {
//             if (countMode(missing) > 0) continue;

//             // Replace a member belonging to a donor mode that still has >1
//             // representative, so fixing one missing mode cannot immediately
//             // delete another required mode.
//             size_t replaceIndex = pop.size() - 1;
//             bool donorFound = false;
//             for (size_t i = pop.size(); i-- > 0;) {
//                 if (countMode(pop[i].genome.mode) > 1) {
//                     replaceIndex = i;
//                     donorFound = true;
//                     break;
//                 }
//             }
//             if (!donorFound) replaceIndex = pop.size() - 1;

//             ParetoIndividual candidate = pop[replaceIndex];
//             candidate.genome.mode = missing;

//             // Give each injected representative a semantically valid clock
//             // anchor.  Other genes remain inherited to avoid resetting the
//             // entire search trajectory.
//             if (missing == PolicyMode::MAX_PERFORMANCE) {
//                 candidate.genome.target_fps =
//                     MaybeDouble(cfg_.performanceTargetFps);
//                 candidate.genome.cpu_max_freq_khz =
//                     MaybeDouble(1479000.0);
//             } else if (missing == PolicyMode::BALANCED) {
//                 candidate.genome.cpu_max_freq_khz =
//                     MaybeDouble(921600.0);
//             } else { // LOW_POWER
//                 candidate.genome.cpu_max_freq_khz =
//                     MaybeDouble(460800.0);
//             }

//             canonicalizeGenomeForWorkload(candidate.genome);

//             // This is a new, unevaluated representative for the NEXT
//             // generation. Reset stale donor decision-state fields.
//             candidate.objectives.clear();
//             candidate.fitness = -std::numeric_limits<double>::max();
//             candidate.rank = 0;
//             candidate.crowdingDistance = 0.0;
//             candidate.feasible = true;
//             candidate.constraintViolation = 0.0;
//             candidate.age = 0;
//             candidate.surrogateUsed = false;
//             candidate.surrogatePrediction = SurrogatePrediction{};
//             candidate.surrogateExplorationBonus = 0.0;
//             candidate.surrogateStallPenalty = 0.0;
//             candidate.fitFinal = -std::numeric_limits<double>::max();

//             pop[replaceIndex] = std::move(candidate);
//         }
//     }

//     void GeneticAlgorithm::reducePopulation() {
//         if (generation_ < cfg_.reductionStartGen) return;

//         // [P0-F16] population_ is sorted by (rank, crowding) at the end of
//         // nonDominatedSortAndCrowding(), so truncation keeps the best
//         // individuals with no duplicates. The old index-based fill
//         // (population_[survivors.size()]) could re-copy rank-0 members that
//         // were already in `survivors`, because rank-0 individuals were
//         // scattered through an unsorted array. Semantics preserved: keep at
//         // least minPopSize, and keep the whole rank-0 front even when it
//         // exceeds the reduction target.
//         size_t rank0 = 0;
//         for (const auto& ind : population_) {
//             if (ind.rank == 0) ++rank0;
//         }

//         size_t target = std::max(cfg_.minPopSize,
//                                  static_cast<size_t>(population_.size() * cfg_.reductionRatio));
//         target = std::max(target, rank0);

//         if (population_.size() > target) {
//             population_.resize(target);
//         }

//         spdlog::info("[GA-Pareto] Dynamic reduction -> size {} (gen {})", population_.size(), generation_);
//     }

//     void GeneticAlgorithm::injectDiversityIfNeeded() {
//         if (generation_ % 5 == 0 && population_.size() < cfg_.popSize) {
//             spdlog::debug("[GA-Pareto] Injecting diversity");
//             // Simple random injection (same logic as initializePopulation)
//             ParetoIndividual randomInd;
//             std::uniform_real_distribution<double> fpsDist(cfg_.minTargetFps, cfg_.maxTargetFps);
//             std::uniform_int_distribution<int> modeDist(1, 3);
//             std::bernoulli_distribution gpuBias(0.65);

//             randomInd.genome.mode = static_cast<PolicyMode>(modeDist(rng_));
//             randomInd.genome.target_fps = MaybeDouble(fpsDist(rng_));
//             randomInd.genome.prefer_gpu = MaybeBool(gpuBias(rng_));
//             canonicalizeGenomeForWorkload(randomInd.genome);
//             population_.push_back(randomInd);
//         }
//     }

//     // ===================================================================
//     // Runtime Application & Logging (PhD helpers)
//     // ===================================================================
//     // ===================================================================
//     // Runtime & Logging
//     // ===================================================================
//    // void applyBestParetoToRuntime(const hrl::MetricsSnapshot& snapshot);
//     //void seedRLReplayBuffer(const hrl::MetricsSnapshot& snapshot);
//     //void logParetoStats();
//     //void logDominanceAnalysis();

//     //bool isSnapshotStable(const hrl::MetricsSnapshot& snap);
//     // void initializePopulation();
//     // ===================================================================

//     // void hrl::GeneticAlgorithm::applyBestParetoToRuntime(const hrl::MetricsSnapshot& snapshot) {
//     //     if (population_.empty() || !runtimeControls_) return;
//     //     const auto& best = population_[0];
//     //     if (scheduler_) scheduler_->apply(best.genome, snapshot, *runtimeControls_);

//     //     spdlog::debug("[GeneticAlgorithm] Applied best: mode={}, target_fps={:.1f}, gpu={}",
//     //                   static_cast<int>(best.genome.mode),
//     //                   best.genome.target_fps.has ? best.genome.target_fps.value : 0.0,
//     //                   best.genome.prefer_gpu.has ? (best.genome.prefer_gpu.value ? "YES" : "NO") : "AUTO");
//     // }

// // // ===================================================================
// // // applyBestParetoToRuntime: apply Rank-0 individual with best fitness
// // // ===================================================================
// // void hrl::GeneticAlgorithm::applyBestParetoToRuntime(const hrl::MetricsSnapshot& snapshot) {
// //     if (population_.empty() || !runtimeControls_) return;

// //     // [P0-F17] Legacy actuation path uses the same feasibility-aware selector
// //     // as the primary evolve() path.  Never maintain two subtly different
// //     // definitions of "best" action.
// //     const ParetoIndividual* best = selectBestEvaluatedPareto();
// //     if (!best) {
// //         spdlog::warn("[GeneticAlgorithm] No selectable Pareto solution available");
// //         return;
// //     }

// //     if (scheduler_) scheduler_->apply(best->genome, snapshot, *runtimeControls_);
    

// //     spdlog::debug("[GeneticAlgorithm] Applied Rank-0 best: mode={}, fps={:.1f}, gpu={}, fitness={:.4f}",
// //                   static_cast<int>(best->genome.mode),
// //                   best->genome.target_fps.has ? best->genome.target_fps.value : 0.0,
// //                   best->genome.prefer_gpu.has ? (best->genome.prefer_gpu.value ? "YES" : "NO") : "AUTO",
// //                   best->fitness);
// // }

// // ===================================================================
// // applyBestParetoToRuntime:
// // apply the constraint-aware best evaluated individual.
// //
// // Normal:
// //   feasible Rank-0 -> highest scalar fitness.
// //
// // Degraded/all-infeasible:
// //   lowest constraint violation -> rank -> fitness.
// //
// // Never arbitrarily falls back to population_[0].
// // ===================================================================
// void hrl::GeneticAlgorithm::applyBestParetoToRuntime(
//     const hrl::MetricsSnapshot& snapshot)
// {
//     if (population_.empty() ||
//         !runtimeControls_)
//     {
//         return;
//     }

//     const ParetoIndividual* best =
//         selectBestEvaluatedPareto();

//     if (!best) {
//         // No evaluated individual is available. Do not push an
//         // unevaluated genome to hardware under the name "best Pareto".
//         spdlog::warn(
//             "[GeneticAlgorithm] "
//             "No evaluated Pareto solution available; "
//             "runtime controls left unchanged");
//         return;
//     }

//     // ---------------------------------------------------------------
//     // Diagnostic distinction is important for thesis telemetry:
//     // deployment from a feasible Pareto front is fundamentally
//     // different from degraded operation where every candidate violates
//     // at least one constraint.
//     // ---------------------------------------------------------------
//     if (!best->feasible) {
//         spdlog::warn(
//             "[GeneticAlgorithm] "
//             "ALL CANDIDATES INFEASIBLE: applying least-violation "
//             "candidate mode={}, violation={:.6f}, rank={}, fitness={:.4f}",
//             static_cast<int>(best->genome.mode),
//             best->constraintViolation,
//             best->rank,
//             best->fitness);
//     }

//     if (scheduler_) {
//         scheduler_->apply(
//             best->genome,
//             snapshot,
//             *runtimeControls_);
//     }

//     spdlog::debug(
//         "[GeneticAlgorithm] Applied Pareto candidate: "
//         "mode={}, fps={:.1f}, gpu={}, feasible={}, "
//         "violation={:.6f}, rank={}, fitness={:.4f}",
//         static_cast<int>(best->genome.mode),
//         best->genome.target_fps.has
//             ? best->genome.target_fps.value
//             : 0.0,
//         best->genome.prefer_gpu.has
//             ? (best->genome.prefer_gpu.value
//                 ? "YES"
//                 : "NO")
//             : "AUTO",
//         best->feasible ? 1 : 0,
//         best->constraintViolation,
//         best->rank,
//         best->fitness);
// }

// // ===================================================================
// // seedRLReplayBuffer: push Rank-0 solutions as RL experience (ERL)
// // ===================================================================
// void GeneticAlgorithm::seedRLReplayBuffer(const hrl::MetricsSnapshot& snapshot) {
//     // ERL hybridization: Rank-0 genomes serve as high-quality seeds for RL
//     // In a full ERL implementation these would be pushed to a shared replay buffer.
//     int rank0Count = 0;
//     for (const auto& ind : population_) {
//         if (ind.rank == 0) ++rank0Count;
//     }
//     spdlog::debug("[GeneticAlgorithm] ERL: {} Rank-0 solutions available for RL seeding (gen {})",
//                   rank0Count, generation_);
//     (void)snapshot;  // suppress unused warning until RL buffer is integrated
// }



// // ===================================================================
// // logParetoStats: generation summary for PhD documentation
// // ===================================================================
// void GeneticAlgorithm::logParetoStats() {
//     if (population_.empty()) return;

//     int rank0Count = 0;
//     double bestFitness = -std::numeric_limits<double>::max();
//     double avgFitness  = 0.0;

//     for (const auto& ind : population_) {
//         if (ind.rank == 0) ++rank0Count;
//         if (ind.fitness > bestFitness) bestFitness = ind.fitness;
//         avgFitness += ind.fitness;
//     }
//     avgFitness /= static_cast<double>(population_.size());

//     spdlog::info("[GA-Pareto] Gen {} | Pop={} | Rank-0={} | BestFit={:.4f} | AvgFit={:.4f}",
//                  generation_, population_.size(), rank0Count, bestFitness, avgFitness);
// }



//     bool GeneticAlgorithm::isSnapshotStable(const hrl::MetricsSnapshot& snap) {
//         if (!snap.valid || snap.fps < cfg_.minFpsForEvolution) {
//             stableFrames_ = 0;
//             return false;
//         }

//         if (cfg_.requirePowerForEvolution && snap.avg_power_w_alg < cfg_.minPowerForEvolution) {
//             stableFrames_ = 0;
//             return false;
//         }

//         stableFrames_++;
//         return stableFrames_ >= std::max(1, cfg_.stableFramesRequired);
//     }

//     void GeneticAlgorithm::initializePopulation() {
//         population_.resize(cfg_.popSize);
//         std::uniform_real_distribution<double> fpsDist(cfg_.minTargetFps, cfg_.maxTargetFps);
//         std::uniform_int_distribution<int> modeDist(1, 3);
//         std::bernoulli_distribution gpuBias(0.65);

//         // [P0] Discrete Jetson Nano CPU DVFS steps (kHz). Seeding the frequency
//         // gene across these lets the Pareto front span the full clock range
//         // instead of collapsing onto the 102 MHz floor via PolicyMode alone.
//         static const long kFreqSteps[] = {102000, 460800, 921600, 1190400, 1479000};
//         std::uniform_int_distribution<int> freqIdx(0, 4);
//         std::uniform_real_distribution<double> splitDist(0.0, 1.0);

//         for (size_t i = 0; i < cfg_.popSize; ++i) {
//             auto& ind = population_[i];

//             // [FINAL FIX D1] Seed generation zero with all three macro modes
//             // represented. Remaining individuals are random as before.
//             if (cfg_.enforceModeDiversity && cfg_.popSize >= 3 && i < 3) {
//                 static const PolicyMode kSeedModes[3] = {
//                     PolicyMode::MAX_PERFORMANCE,
//                     PolicyMode::BALANCED,
//                     PolicyMode::LOW_POWER
//                 };
//                 ind.genome.mode = kSeedModes[i];
//             } else {
//                 ind.genome.mode = static_cast<PolicyMode>(modeDist(rng_));
//             }
//             ind.genome.target_fps = MaybeDouble(fpsDist(rng_));
//             ind.genome.prefer_gpu = MaybeBool(gpuBias(rng_));
//             ind.genome.power_budget_watts = MaybeDouble(3.0 + rng_() % 5);
//             ind.genome.cpu_max_freq_khz =
//                 MaybeDouble(static_cast<double>(kFreqSteps[freqIdx(rng_)]));
//             // [Micro-scheduler] Continuous GPU workload fraction, uniform in [0,1].
//             ind.genome.gpu_workload_split = MaybeDouble(splitDist(rng_));
//             canonicalizeGenomeForWorkload(ind.genome);
//         }
//         spdlog::info("[GeneticAlgorithm] Initialized population of size {} "
//                      "(target_fps range {:.0f}-{:.0f}, freq gene enabled)",
//                      cfg_.popSize, cfg_.minTargetFps, cfg_.maxTargetFps);
//     }

//     // ===================================================================
//     // [Adaptive mutation] Plateau-driven mutation-rate control.
//     // Ramps mutation up when best fitness stagnates, eases it down when
//     // fitness improves. Operates only on the rate ? selection is untouched,
//     // so this is safe to run alongside NSGA-II non-dominated sorting.
//     // Directly targets the observed failure mode (best fitness drifting
//     // negative after ~gen 500 with no escape mechanism).
//     // ===================================================================
//     double GeneticAlgorithm::updateAdaptiveMutationRate(double currentBestFitness) {
//         if (!cfg_.adaptiveMutationEnabled) {
//             currentMutationRate_ = cfg_.mutationRate;
//             return currentMutationRate_;
//         }

//         // First call: just seed the baseline, no adjustment yet.
//         if (prevBestFitness_ == -std::numeric_limits<double>::max()) {
//             prevBestFitness_ = currentBestFitness;
//             currentMutationRate_ = cfg_.mutationRate;
//             return currentMutationRate_;
//         }

//         const double improvement = currentBestFitness - prevBestFitness_;
//         prevBestFitness_ = currentBestFitness;

//         // Relative threshold scaled to the fitness magnitude, with an absolute
//         // floor so near-zero fitness doesn't make the threshold collapse to 0.
//         const double scale = std::max(std::abs(currentBestFitness),
//                                       cfg_.mutationPlateauAbsFloor);
//         const double improveThreshold = cfg_.mutationPlateauRelThreshold * scale;

//         if (improvement > improveThreshold) {
//             // Improving -> exploit: ease mutation back toward (below) base.
//             mutationPlateauCounter_ = 0;
//             currentMutationRate_ = cfg_.mutationRate * cfg_.mutationImproveFactor;
//         } else {
//             // Stagnating -> explore: ramp mutation up, capped.
//             ++mutationPlateauCounter_;
//             const double factor = 1.0 + cfg_.mutationPlateauStep * mutationPlateauCounter_;
//             currentMutationRate_ = std::min(cfg_.mutationRate * factor,
//                                             cfg_.maxMutationRate);
//         }

//         // Guard rails
//         currentMutationRate_ = std::min(std::max(currentMutationRate_, 0.0),
//                                         cfg_.maxMutationRate);
//         return currentMutationRate_;
//     }

// // ===================================================================
// // IMPLEMENTATIONS (NSGA-II + Dominance)
// // ===================================================================
// //void GeneticAlgorithm::nonDominatedSortAndCrowding() { /* your existing correct implementation */ }
// //void GeneticAlgorithm::calculateCrowdingDistance(std::vector<int>& front) { /* your existing correct implementation */ }
// //const ParetoIndividual& GeneticAlgorithm::tournamentSelect(int tournamentSize) { /* your existing correct implementation */ }


// // IMPLEMENTATION DETAILS BELOW (can be moved to .cpp if desired)

// /*
// 1. Fast Non-Dominated Sort
// This function separates your population into "Fronts" (Rank 0 is the true Pareto front, Rank 1 is the next best, etc.).
//  It uses a helper lambda to determine if Individual A completely dominates Individual B.
// */
// void GeneticAlgorithm::nonDominatedSortAndCrowding() {
//     const size_t n = population_.size();
//     if (n == 0) return;

//     // S[i] contains a list of indices of individuals that individual 'i' dominates
//     std::vector<std::vector<int>> S(n);
//     // n_dom[i] is the domination count: how many individuals dominate individual 'i'
//     std::vector<int> n_dom(n, 0);
    
//     std::vector<std::vector<int>> fronts;
//     std::vector<int> currentFront;

//     // [P0-F17] CRITICAL: use the canonical ParetoIndividual::dominates().
//     // The old local lambda compared objectives only and therefore bypassed
//     // feasible/constraintViolation, allowing infeasible LOW_POWER candidates
//     // to receive Rank 0 and reach the scheduler.  One dominance definition is
//     // now shared by sorting, feasibility, and the deterministic audit trail.
//     auto dominates = [](const ParetoIndividual& a, const ParetoIndividual& b) {
//         return a.dominates(b);
//     };

//     // 1. Calculate domination counts and build the first front
//     for (size_t p = 0; p < n; ++p) {
//         S[p].clear();
//         n_dom[p] = 0;
//         for (size_t q = 0; q < n; ++q) {
//             if (p == q) continue;
            
//             if (dominates(population_[p], population_[q])) {
//                 S[p].push_back(q);
//             } else if (dominates(population_[q], population_[p])) {
//                 n_dom[p]++;
//             }
//         }
        
//         // If nothing dominates p, it belongs to the first Pareto front (Rank 0)
//         if (n_dom[p] == 0) {
//             population_[p].rank = 0;
//             currentFront.push_back(p);
//         }
//     }

//     fronts.push_back(currentFront);

//     // 2. Build subsequent fronts
//     int i = 0;
//     while (!fronts[i].empty()) {
//         std::vector<int> nextFront;
//         for (int p : fronts[i]) {
//             for (int q : S[p]) {
//                 n_dom[q]--; // Since p was in the previous front, remove its domination effect
//                 if (n_dom[q] == 0) {
//                     population_[q].rank = i + 1;
//                     nextFront.push_back(q);
//                 }
//             }
//         }
//         i++;
//         if (!nextFront.empty()) {
//             fronts.push_back(nextFront);
//         } else {
//             break; // All individuals sorted
//         }
//     }

//     // 3. Assign Crowding Distance for each front independently
//     for (auto& front : fronts) {
//         if (front.empty()) continue;
//         calculateCrowdingDistance(front); // Call our helper
//     }

//     // [P0-F16] CRITICAL: order the population by the NSGA-II crowded
//     // comparison (rank asc, crowding desc - ParetoIndividual::operator<).
//     // selectEliteAndOffspring() takes the first eliteCount entries as
//     // "elites" and reducePopulation() truncates from the back; both silently
//     // assumed this ordering. Without it, elites were arbitrary individuals
//     // and reduction could duplicate rank-0 members via index-based fill.
//     // (The per-front index vectors above are not used after this point, so
//     // invalidating them by reordering population_ is safe.)
//     std::sort(population_.begin(), population_.end());
// }

// /*
// 2. Crowding Distance Calculation
// I separated this into a helper function calculateCrowdingDistance(std::vector<int>& front). You should declare this as a private function in your header. 
// It ensures that boundary individuals (the absolute best at a specific objective) are always preserved.
// */

// // Add this declaration to your .h file:
// // void calculateCrowdingDistance(std::vector<int>& front);

// void GeneticAlgorithm::calculateCrowdingDistance(std::vector<int>& front) {
//     size_t l = front.size();
    
//     // Initialize crowding distance to 0 for everyone in the current front
//     for (int idx : front) {
//         population_[idx].crowdingDistance = 0.0;
//     }
    
//     // If 1 or 2 items, they are boundaries by default. Preserve them with infinity.
//     if (l <= 2) {
//         for (int idx : front) {
//             population_[idx].crowdingDistance = std::numeric_limits<double>::infinity();
//         }
//         return;
//     }

//     size_t numObjectives = population_[front[0]].objectives.size();
    
//     // Calculate density across each objective axis independently
//     for (size_t m = 0; m < numObjectives; ++m) {
//         // Sort the current front based solely on objective 'm'
//         std::sort(front.begin(), front.end(), [&](int a, int b) {
//             return population_[a].objectives[m] < population_[b].objectives[m];
//         });

//         // The extremes of the front get infinite distance (guaranteed survival)
//         population_[front[0]].crowdingDistance = std::numeric_limits<double>::infinity();
//         population_[front[l - 1]].crowdingDistance = std::numeric_limits<double>::infinity();

//         double objMin = population_[front[0]].objectives[m];
//         double objMax = population_[front[l - 1]].objectives[m];
//         double range = objMax - objMin;

//         if (range == 0.0) continue; // Prevent division by zero if all values are identical

//         // For all intermediate individuals, calculate normalized distance to neighbors
//         for (size_t j = 1; j < l - 1; ++j) {
//             if (population_[front[j]].crowdingDistance != std::numeric_limits<double>::infinity()) {
//                 double diff = population_[front[j + 1]].objectives[m] - population_[front[j - 1]].objectives[m];
//                 population_[front[j]].crowdingDistance += diff / range;
//             }
//         }
//     }
// }

// /*
// 3. NSGA-II Tournament Selection (Crowded-Comparison Operator)
// This selection logic acts on the operator< you already defined in ParetoIndividual, 
// ensuring the algorithm prefers lower ranks, but breaks ties by preferring higher crowding distances (exploring less-crowded areas of the Pareto front).
// */
// const ParetoIndividual& GeneticAlgorithm::tournamentSelect(int tournamentSize) {
//     std::uniform_int_distribution<size_t> dist(0, population_.size() - 1);
    
//     size_t bestIdx = dist(rng_);

//     for (int i = 1; i < tournamentSize; ++i) {
//         size_t contenderIdx = dist(rng_);
        
//         const auto& best = population_[bestIdx];
//         const auto& contender = population_[contenderIdx];

//         // NSGA-II Crowded-Comparison Operator:
//         // 1. Better Rank (Lower is better) wins.
//         if (contender.rank < best.rank) {
//             bestIdx = contenderIdx;
//         } 
//         // 2. If ranks are tied, pick the one in the less crowded region (Higher distance)
//         else if (contender.rank == best.rank) {
//             if (contender.crowdingDistance > best.crowdingDistance) {
//                 bestIdx = contenderIdx;
//             }
//         }
//     }
    
//     return population_[bestIdx];
// }

// // ===================================================================
// // Scalarized fitness: weighted combination of normalized objectives
// // Weights: FPS=50%, Power=20%, Temperature=20%, Latency=10%
// // ===================================================================
// double GeneticAlgorithm::calculateScalarizedFitness(const std::vector<double>& objs,
//                                                      const hrl::MetricsSnapshot& snap,
//                                                      const RLAction& action,
//                                                      int age,
//                                                      ParetoIndividual* auditOut) const {
//     if (objs.size() < 4) return -std::numeric_limits<double>::max();

//     // objs = [normFPS, normEnergy, normTemp, normLat] (all minimized).
//     // This exact authoritative weight vector is also persisted in the candidate.
//     std::array<double, 4> w = {cfg_.weight_fps, cfg_.weight_power,
//                                cfg_.weight_temp, cfg_.weight_latency};
//     if (weightManager_) {
//         w = weightManager_->getWeights();
//     }

//     const double fpsComponent   = -objs[0] * w[0];
//     const double powerComponent = -objs[1] * w[1];
//     const double tempComponent  = -objs[2] * w[2];
//     const double latComponent   = -objs[3] * w[3];

//     double gpuBonus = 0.0;
//     if (action.prefer_gpu.has && action.prefer_gpu.value &&
//         snap.gpu_util_avg > 15.0 && snap.gpu_util_avg < 85.0) {
//         gpuBonus = 0.05;
//     }

//     const double beforeAge = fpsComponent + powerComponent +
//                              tempComponent + latComponent + gpuBonus;
//     const double ageFactor = std::pow(0.985, static_cast<double>(age) / 50.0);
//     const double afterAge = beforeAge * ageFactor;

//     if (auditOut) {
//         auditOut->fitnessWeights = w;
//         auditOut->fitFpsComponent = fpsComponent;
//         auditOut->fitPowerComponent = powerComponent;
//         auditOut->fitTempComponent = tempComponent;
//         auditOut->fitLatencyComponent = latComponent;
//         auditOut->fitGpuBonus = gpuBonus;
//         auditOut->fitBeforeAge = beforeAge;
//         auditOut->fitAgeFactor = ageFactor;
//         auditOut->fitAfterAge = afterAge;
//         auditOut->fitFinal = afterAge; // exploration bonus is appended by caller
//     }

//     return afterAge;
// }


//     // double calculateScalarizedFitness(const hrl::MetricsSnapshot& snap,
//     //                                   const RLAction& action,
//     //                                   const RuntimeControls& /*controls*/) {
//     //     double f = 0.0;
//     //     if (action.target_fps.has) {
//     //         double err = std::abs(snap.fps - action.target_fps.value);
//     //         f += 0.50 * (1.0 / (1.0 + err));
//     //     }
//     //     f += -0.20 * snap.avg_power_w_alg;

//     //     if (snap.cpu_temp_c > 75.0) f += -2.0 * (snap.cpu_temp_c - 75.0);
//     //     if (snap.gpu_temp_c > 80.0) f += -2.0 * (snap.gpu_temp_c - 80.0);

//     //     f += -0.10 * snap.avg_latency_ms / 50.0;

//     //     if (action.prefer_gpu.has && action.prefer_gpu.value &&
//     //         snap.gpu_util_avg > 15.0 && snap.gpu_util_avg < 85.0) {
//     //         f += 0.20;
//     //     }

//     //     f *= std::pow(0.985, generation_ / 50.0);
//     //     return f;
//     // }


// //=======================================================================================================================================================================================
// void GeneticAlgorithm::logDominanceAnalysis() {
//     if (population_.size() < 2) return;
    
//     spdlog::info("\n===== PARETO FRONT ANALYSIS (Generation {}) =====", generation_);
    
//     std::vector<size_t> frontIndices;
//     for (size_t i = 0; i < population_.size(); ++i) {
//         if (population_[i].rank == 0) frontIndices.push_back(i);
//     }
    
//     for (size_t idx : frontIndices) {
//         const auto& ind = population_[idx];
//         spdlog::info("  [Rank-0] ID={} | Fitness={:.4f} | FPS Goal={:.1f} | Power={:.2f}W | "
//                      "Temp Penalty={:.2f} | Latency={:.2f}ms | Crowding Dist={:.4f}",
//                      idx, ind.fitness,
//                      ind.genome.target_fps.has ? ind.genome.target_fps.value : 0.0,
//                      ind.objectives[1] * 10.0,
//                      ind.objectives[2] * 20.0,
//                      ind.objectives[3] * 50.0,
//                      ind.crowdingDistance);
//     }
    
//     spdlog::info("\n  ===== DOMINANCE COMPARISON =====");
//     if (!frontIndices.empty() && population_.size() > frontIndices.size()) {
//         const auto& best = population_[frontIndices[0]];
//         for (size_t i = frontIndices.size(); i < std::min(size_t(3), population_.size()); ++i) {
//             const auto& challenger = population_[i];
//             spdlog::info("  [Rank-{}] vs [Rank-0]: {} dominates because:",
//                          challenger.rank, best.dominates(challenger) ? "Best" : "Trade-off");
            
//             const char* objName[] = {"FPS", "Energy", "Temperature", "Latency"};
//             for (size_t obj = 0; obj < best.objectives.size(); ++obj) {
//                 spdlog::info("    - {}: Best={:.3f} vs Challenger={:.3f} {}",
//                              objName[obj], best.objectives[obj], challenger.objectives[obj],
//                              best.objectives[obj] <= challenger.objectives[obj] ? "? Better" : "? Worse");
//             }
//         }
//     }
//     spdlog::info("\n");
// }

// // ===================================================================
// // Side-effect-free simulation of runtime controls for GA evaluation.
// // This avoids touching real sysfs/GPU/DVFS while evaluating candidates.
// // ===================================================================
// void GeneticAlgorithm::simulateRuntimeControlsFromAction(
//     const RLAction& action,
//     RuntimeControls& simControls) const
// {
//     int concurrency = 2;
//     bool enableGpu = true;
//     Affinity affinity = Affinity::Spread;

//     switch (action.mode) {
//         case PolicyMode::MAX_PERFORMANCE:
//             concurrency = 4;
//             enableGpu = true;
//             affinity = Affinity::Spread;
//             break;

//         case PolicyMode::LOW_POWER:
//             concurrency = 1;
//             enableGpu = false;
//             affinity = Affinity::Pack;
//             break;

//         case PolicyMode::BALANCED:
//             concurrency = 2;
//             enableGpu = action.prefer_gpu.has ? action.prefer_gpu.value : true;
//             affinity = Affinity::Spread;
//             break;

//         case PolicyMode::UNKNOWN:
//         default:
//             concurrency = 2;
//             enableGpu = true;
//             affinity = Affinity::Spread;
//             break;
//     }

//     if (action.prefer_gpu.has) {
//         enableGpu = action.prefer_gpu.value;
//     }

//     simControls.concurrency_level.store(concurrency, std::memory_order_relaxed);
//     simControls.enable_gpu.store(enableGpu, std::memory_order_relaxed);
//     simControls.affinity.store(affinity, std::memory_order_relaxed);
// }

// // ===================================================================
// // Select the best evaluated Rank-0 Pareto individual.
// // ===================================================================
// const ParetoIndividual* GeneticAlgorithm::selectBestEvaluatedPareto() const
// {
//     // [P0-F17] Defense-in-depth action gate.
//     // Primary path: only a FEASIBLE Rank-0 candidate may be deployed.
//     const ParetoIndividual* selected = nullptr;
//     for (const auto& ind : population_) {
//         if (ind.rank == 0 && ind.feasible &&
//             (!selected || ind.fitness > selected->fitness)) {
//             selected = &ind;
//         }
//     }
//     if (selected) return selected;

//     // If the entire evaluated population is infeasible, do not fall back to an
//     // arbitrary high-fitness point.  Select the smallest constraint violation;
//     // scalar fitness is only the tie-breaker between equally violating points.
//     constexpr double kConstraintEps = 1e-12;
//     for (const auto& ind : population_) {
//         if (ind.rank != 0) continue;
//         if (!selected ||
//             ind.constraintViolation + kConstraintEps < selected->constraintViolation ||
//             (std::abs(ind.constraintViolation - selected->constraintViolation) <= kConstraintEps &&
//              ind.fitness > selected->fitness)) {
//             selected = &ind;
//         }
//     }
//     if (selected) return selected;

//     // Pre-evaluation fallback only (e.g. public getBestAction before first
//     // non-dominated sort).  Prefer a feasible individual if one exists.
//     for (const auto& ind : population_) {
//         if (ind.feasible && (!selected || ind.fitness > selected->fitness)) {
//             selected = &ind;
//         }
//     }
//     if (selected) return selected;

//     return population_.empty() ? nullptr : &population_[0];
// }

// // ===================================================================
// // Apply selected evaluated Pareto individual to real runtime controls.
// // ===================================================================
// double GeneticAlgorithm::applySelectedParetoToRuntime(
//     const ParetoIndividual& selected,
//     const hrl::MetricsSnapshot& snapshot)
// {
//     if (!runtimeControls_) {
//         return 0.0;
//     }

//     // // [Surrogate] Remember what we are about to apply; next cycle's snapshot is
//     // // this action's measured outcome and will be fed to surrogate_.observe().
//     // lastAppliedGenome_ = selected.genome;
//     // hasLastApplied_    = true;

//     // if (scheduler_) {
//     //     scheduler_->apply(selected.genome, snapshot, *runtimeControls_);
//     // }

//     // [Surrogate] Remember what we are about to apply; next cycle's snapshot is
//     // this action's measured outcome and will be fed to surrogate_.observe().
//     lastAppliedGenome_ = selected.genome;
//     hasLastApplied_    = true;

//     // [PhD FIX] Diagnostic trace for HetGB continuous split gene crash
//     if (selected.genome.gpu_workload_split.has) {
//         double safe_split = utils::local_clamp(selected.genome.gpu_workload_split.value, 0.0, 1.0);
//         spdlog::info("[GA-Split-Trace] Dispatching continuous split gene: {:.3f}", safe_split);
//     }

//     double schedulerApplyMs = 0.0;
//     if (scheduler_) {
//         const auto schedulerStart = std::chrono::steady_clock::now();
//         scheduler_->apply(selected.genome, snapshot, *runtimeControls_);
//         const auto schedulerEnd = std::chrono::steady_clock::now();
//         schedulerApplyMs = std::chrono::duration<double, std::milli>(
//             schedulerEnd - schedulerStart).count();
//     }

//     // [PhD timing] Publish a 1-based action epoch only after all scheduler
//     // actuation has completed. AlgorithmConcrete timestamps the first frame
//     // that observes this epoch, yielding true action-to-effect response time.
//     const uint64_t applyEndNs = static_cast<uint64_t>(
//         std::chrono::duration_cast<std::chrono::nanoseconds>(
//             std::chrono::steady_clock::now().time_since_epoch()).count());
//     const uint64_t actionEpoch = static_cast<uint64_t>(generation_) + 1ULL;
//     runtimeControls_->action_apply_end_ns.store(applyEndNs, std::memory_order_relaxed);
//     runtimeControls_->action_generation.store(actionEpoch, std::memory_order_release);

//     spdlog::info(
//         "[ERL SELECTED] Gen={} | Rank={} | Fit={:.4f} | mode={} | targetFPS={:.1f} | gpu={} | "
//         "fps={:.2f} | power={:.3f}W | J/frame={:.6f} | latency={:.3f}ms",
//         generation_,
//         selected.rank,
//         selected.fitness,
//         static_cast<int>(selected.genome.mode),
//         selected.genome.target_fps.has ? selected.genome.target_fps.value : 0.0,
//         selected.genome.prefer_gpu.has
//             ? (selected.genome.prefer_gpu.value ? "YES" : "NO")
//             : "AUTO",
//         snapshot.fps,
//         snapshot.avg_power_w_alg,
//         snapshot.joulesPerFrame,
//         snapshot.avg_latency_ms
//     );

//     return schedulerApplyMs;
// }


// // ===================================================================
// // Ensure output directory and evidence CSV files exist immediately.
// // This creates headers even before the first successful ERL generation.
// // ===================================================================
// bool GeneticAlgorithm::ensureParentDirectoryForFile(const std::string& filePath)
// {
//     const std::size_t slash = filePath.find_last_of("/");
//     if (slash == std::string::npos) {
//         return true;
//     }

//     const std::string dir = filePath.substr(0, slash);
//     if (dir.empty()) {
//         return true;
//     }

//     std::string current;
//     for (char c : dir) {
//         current.push_back(c);
//         if (c == '/') {
//             if (current.size() > 1) {
//                 ::mkdir(current.c_str(), 0755);
//             }
//         }
//     }

//     if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
//         spdlog::warn("[GA-Pareto] Could not create directory '{}': {}", dir, std::strerror(errno));
//         return false;
//     }
//     return true;
// }

// void GeneticAlgorithm::ensureEvidenceFiles()
// {
//     // Telemetry schema v3 adds `feasible` and `constraint_violation` to both
//     // Pareto and selected-action evidence.  Bumping the row schema prevents
//     // downstream analysis scripts from silently interpreting the new columns
//     // as the older 91/67-column v2 layout.
//     std::lock_guard<std::mutex> guard(evidenceMutex_);

//     ensureParentDirectoryForFile(cfg_.pareto_csv_path);
//     if (!std::ifstream(cfg_.pareto_csv_path).good()) {
//         std::ofstream out(cfg_.pareto_csv_path, std::ios::out);
//         if (out.is_open()) {
//             out << "schema_version,generation,individual_id,rank,fitness,crowding_distance,obj_fps,obj_energy,obj_temp,obj_latency,target_fps_has,target_fps,power_budget_has,power_budget_watts,prefer_gpu_has,prefer_gpu,gpu_workload_split_has,gpu_workload_split,gpu_split_applicable,cpu_max_freq_has,cpu_max_freq_khz,policy_mode,awm_regime,awm_detected_regime,awm_w_fps,awm_w_power,awm_w_temp,awm_w_latency,awm_trigger_field,awm_trigger_value,awm_trigger_threshold,awm_regime_streak,awm_last_update_generation,surrogate_used,surrogate_source,surrogate_confidence,surrogate_from_data,surrogate_support_count,surrogate_neighbor_count,surrogate_nearest_distance,surrogate_exploration_bonus,surrogate_stall_penalty,surrogate_config_freq_khz,surrogate_config_gpu,surrogate_config_gpu_split,surrogate_config_concurrency,pred_fps,pred_power_w,pred_latency_ms,pred_temp_c,fit_fps_component,fit_power_component,fit_temp_component,fit_latency_component,fit_gpu_bonus,fit_before_age,fit_age_factor,fit_after_age,fit_final,feasible,constraint_violation,selected,measured_fps,measured_power_w,measured_joules_per_frame,algorithm_processing_ms,pipeline_e2e_ms,cpu_temp_c,gpu_temp_c\n";
//         } else {
//             spdlog::warn("[GA-Pareto] Could not create Pareto CSV: {}", cfg_.pareto_csv_path);
//         }
//     }

//     ensureParentDirectoryForFile(cfg_.action_csv_path);
//     if (!std::ifstream(cfg_.action_csv_path).good()) {
//         std::ofstream out(cfg_.action_csv_path, std::ios::out);
//         if (out.is_open()) {
//             out << "schema_version,generation,action_epoch,frame_id,rank0_count,selected_rank,selected_fitness,rank0_runnerup_fitness,fitness_margin,policy_mode,target_fps_has,target_fps,power_budget_has,power_budget_watts,prefer_gpu_has,prefer_gpu,gpu_workload_split_has,gpu_workload_split,gpu_split_applicable,cpu_max_freq_has,cpu_max_freq_gene_khz,cpu_max_freq_commanded_khz,runtime_enable_gpu,runtime_gpu_workload_split,runtime_concurrency,runtime_affinity,awm_regime,awm_detected_regime,awm_w_fps,awm_w_power,awm_w_temp,awm_w_latency,awm_trigger_field,awm_trigger_value,awm_trigger_threshold,awm_regime_streak,awm_last_update_generation,surrogate_used,surrogate_source,surrogate_confidence,surrogate_from_data,surrogate_support_count,surrogate_neighbor_count,surrogate_nearest_distance,surrogate_exploration_bonus,surrogate_stall_penalty,surrogate_config_freq_khz,surrogate_config_gpu,surrogate_config_gpu_split,surrogate_config_concurrency,pred_fps,pred_power_w,pred_latency_ms,pred_temp_c,obj_fps,obj_energy,obj_temp,obj_latency,fit_fps_component,fit_power_component,fit_temp_component,fit_latency_component,fit_gpu_bonus,fit_before_age,fit_age_factor,fit_after_age,fit_final,feasible,constraint_violation,measured_fps,measured_power_w,measured_joules_per_frame,algorithm_processing_ms,pipeline_e2e_ms,ga_compute_total_ms,ga_weight_update_ms,ga_evaluation_ms,ga_nsga_sort_ms,ga_selection_ms,ga_scheduler_apply_ms,ga_action_dispatch_overhead_ms,ga_pareto_export_ms,ga_diagnostics_ms,ga_mutation_ms,ga_offspring_ms,ga_postprocess_ms,action_apply_end_ns,algorithm_observed_epoch,algorithm_observed_time_ns,action_response_epoch,action_response_ms,cpu_temp_c,gpu_temp_c\n";
//         } else {
//             spdlog::warn("[GA-Pareto] Could not create action CSV: {}", cfg_.action_csv_path);
//         }
//     }
// }

// // ===================================================================
// // Count Rank-0 solutions.
// // ===================================================================
// int GeneticAlgorithm::countRank0() const
// {
//     int count = 0;

//     for (const auto& ind : population_) {
//         if (ind.rank == 0) {
//             ++count;
//         }
//     }

//     return count;
// }

// // ===================================================================
// // Export full Pareto front evidence.
// // ===================================================================
// void GeneticAlgorithm::exportParetoEvidence(
//     const hrl::MetricsSnapshot& snap,
//     const ParetoIndividual* selected)
// {
//     if (!cfg_.export_pareto_evidence) return;

//     std::lock_guard<std::mutex> guard(evidenceMutex_);
//     ensureParentDirectoryForFile(cfg_.pareto_csv_path);
//     const bool writeHeader = !std::ifstream(cfg_.pareto_csv_path).good();
//     std::ofstream out(cfg_.pareto_csv_path, std::ios::app);
//     if (!out.is_open()) {
//         spdlog::warn("[GA-Pareto] Could not open Pareto CSV: {}", cfg_.pareto_csv_path);
//         return;
//     }
//     if (writeHeader) out << "schema_version,generation,individual_id,rank,fitness,crowding_distance,obj_fps,obj_energy,obj_temp,obj_latency,target_fps_has,target_fps,power_budget_has,power_budget_watts,prefer_gpu_has,prefer_gpu,gpu_workload_split_has,gpu_workload_split,gpu_split_applicable,cpu_max_freq_has,cpu_max_freq_khz,policy_mode,awm_regime,awm_detected_regime,awm_w_fps,awm_w_power,awm_w_temp,awm_w_latency,awm_trigger_field,awm_trigger_value,awm_trigger_threshold,awm_regime_streak,awm_last_update_generation,surrogate_used,surrogate_source,surrogate_confidence,surrogate_from_data,surrogate_support_count,surrogate_neighbor_count,surrogate_nearest_distance,surrogate_exploration_bonus,surrogate_stall_penalty,surrogate_config_freq_khz,surrogate_config_gpu,surrogate_config_gpu_split,surrogate_config_concurrency,pred_fps,pred_power_w,pred_latency_ms,pred_temp_c,fit_fps_component,fit_power_component,fit_temp_component,fit_latency_component,fit_gpu_bonus,fit_before_age,fit_age_factor,fit_after_age,fit_final,feasible,constraint_violation,selected,measured_fps,measured_power_w,measured_joules_per_frame,algorithm_processing_ms,pipeline_e2e_ms,cpu_temp_c,gpu_temp_c\n";

//     const std::string awmRegime = weightManager_ ? weightManager_->getCurrentRegimeName() : "FIXED_WEIGHTS";
//     const std::string awmDetected = weightManager_ ? weightManager_->getLastDetectedRegimeName() : "FIXED_WEIGHTS";
//     const std::string awmTriggerField = weightManager_ ? weightManager_->getLastTriggerField() : "none";
//     const double awmTriggerValue = weightManager_ ? weightManager_->getLastTriggerValue() : 0.0;
//     const double awmTriggerThreshold = weightManager_ ? weightManager_->getLastTriggerThreshold() : 0.0;
//     const int awmStreak = weightManager_ ? weightManager_->getRegimeStreak() : 0;
//     const int awmLastUpdate = weightManager_ ? weightManager_->getLastUpdateGeneration() : -1;

//     for (size_t i = 0; i < population_.size(); ++i) {
//         const auto& ind = population_[i];
//         const bool isSelected = selected && (&ind == selected);

//         const double targetFps = ind.genome.target_fps.has ? ind.genome.target_fps.value : 0.0;
//         const double powerBudget = ind.genome.power_budget_watts.has ? ind.genome.power_budget_watts.value : 0.0;
//         const int preferGpu = ind.genome.prefer_gpu.has ? (ind.genome.prefer_gpu.value ? 1 : 0) : -1;
//         const double splitGene = ind.genome.gpu_workload_split.has ? ind.genome.gpu_workload_split.value : 0.0;
//         const double cpuFreqGene = ind.genome.cpu_max_freq_khz.has ? ind.genome.cpu_max_freq_khz.value : 0.0;

//         out << 3 << ","
//             << generation_ << "," << i << "," << ind.rank << ","
//             << ind.fitness << "," << ind.crowdingDistance << ",";

//         if (ind.objectives.size() >= 4) {
//             out << ind.objectives[0] << "," << ind.objectives[1] << ","
//                 << ind.objectives[2] << "," << ind.objectives[3] << ",";
//         } else {
//             out << "0,0,0,0,";
//         }

//         out << (ind.genome.target_fps.has ? 1 : 0) << "," << targetFps << ","
//             << (ind.genome.power_budget_watts.has ? 1 : 0) << "," << powerBudget << ","
//             << (ind.genome.prefer_gpu.has ? 1 : 0) << "," << preferGpu << ","
//             << (ind.genome.gpu_workload_split.has ? 1 : 0) << "," << splitGene << ","
//             << (cfg_.gpuSplitApplicable ? 1 : 0) << ","
//             << (ind.genome.cpu_max_freq_khz.has ? 1 : 0) << "," << cpuFreqGene << ","
//             << static_cast<int>(ind.genome.mode) << ","
//             << awmRegime << "," << awmDetected << ","
//             << ind.fitnessWeights[0] << "," << ind.fitnessWeights[1] << ","
//             << ind.fitnessWeights[2] << "," << ind.fitnessWeights[3] << ","
//             << awmTriggerField << "," << awmTriggerValue << "," << awmTriggerThreshold << ","
//             << awmStreak << "," << awmLastUpdate << ","
//             << (ind.surrogateUsed ? 1 : 0) << ","
//             << SurrogateModel::sourceName(ind.surrogatePrediction.source) << ","
//             << ind.surrogatePrediction.confidence << ","
//             << (ind.surrogatePrediction.from_data ? 1 : 0) << ","
//             << ind.surrogatePrediction.support_count << ","
//             << ind.surrogatePrediction.neighbor_count << ","
//             << ind.surrogatePrediction.nearest_distance << ","
//             << ind.surrogateExplorationBonus << ","
//             << ind.surrogateStallPenalty << ","
//             << ind.surrogateConfigFreqKhz << ","
//             << (ind.surrogateConfigGpu ? 1 : 0) << ","
//             << ind.surrogateConfigGpuSplit << ","
//             << ind.surrogateConfigConcurrency << ","
//             << ind.surrogatePrediction.fps << ","
//             << ind.surrogatePrediction.power_w << ","
//             << ind.surrogatePrediction.latency_ms << ","
//             << ind.surrogatePrediction.temp_c << ","
//             << ind.fitFpsComponent << "," << ind.fitPowerComponent << ","
//             << ind.fitTempComponent << "," << ind.fitLatencyComponent << ","
//             << ind.fitGpuBonus << "," << ind.fitBeforeAge << ","
//             << ind.fitAgeFactor << "," << ind.fitAfterAge << "," << ind.fitFinal << ","
//             << (ind.feasible ? 1 : 0) << "," << ind.constraintViolation << ","
//             << (isSelected ? 1 : 0) << ","
//             << snap.fps << "," << snap.avg_power_w_alg << "," << snap.joulesPerFrame << ","
//             << snap.avg_inference_ms << "," << snap.end_to_end_latency_ms << ","
//             << snap.cpu_temp_c << "," << snap.gpu_temp_c << "\n";
//     }
// }

// // ===================================================================
// // Export selected action trace.
// // ===================================================================
// void GeneticAlgorithm::exportSelectedActionEvidence(
//     const hrl::MetricsSnapshot& snap,
//     const ParetoIndividual& selected)
// {
//     if (!cfg_.export_pareto_evidence) return;

//     std::lock_guard<std::mutex> guard(evidenceMutex_);
//     ensureParentDirectoryForFile(cfg_.action_csv_path);
//     const bool writeHeader = !std::ifstream(cfg_.action_csv_path).good();
//     std::ofstream out(cfg_.action_csv_path, std::ios::app);
//     if (!out.is_open()) {
//         spdlog::warn("[GA-Pareto] Could not open action CSV: {}", cfg_.action_csv_path);
//         return;
//     }
//     if (writeHeader) out << "schema_version,generation,action_epoch,frame_id,rank0_count,selected_rank,selected_fitness,rank0_runnerup_fitness,fitness_margin,policy_mode,target_fps_has,target_fps,power_budget_has,power_budget_watts,prefer_gpu_has,prefer_gpu,gpu_workload_split_has,gpu_workload_split,gpu_split_applicable,cpu_max_freq_has,cpu_max_freq_gene_khz,cpu_max_freq_commanded_khz,runtime_enable_gpu,runtime_gpu_workload_split,runtime_concurrency,runtime_affinity,awm_regime,awm_detected_regime,awm_w_fps,awm_w_power,awm_w_temp,awm_w_latency,awm_trigger_field,awm_trigger_value,awm_trigger_threshold,awm_regime_streak,awm_last_update_generation,surrogate_used,surrogate_source,surrogate_confidence,surrogate_from_data,surrogate_support_count,surrogate_neighbor_count,surrogate_nearest_distance,surrogate_exploration_bonus,surrogate_stall_penalty,surrogate_config_freq_khz,surrogate_config_gpu,surrogate_config_gpu_split,surrogate_config_concurrency,pred_fps,pred_power_w,pred_latency_ms,pred_temp_c,obj_fps,obj_energy,obj_temp,obj_latency,fit_fps_component,fit_power_component,fit_temp_component,fit_latency_component,fit_gpu_bonus,fit_before_age,fit_age_factor,fit_after_age,fit_final,feasible,constraint_violation,measured_fps,measured_power_w,measured_joules_per_frame,algorithm_processing_ms,pipeline_e2e_ms,ga_compute_total_ms,ga_weight_update_ms,ga_evaluation_ms,ga_nsga_sort_ms,ga_selection_ms,ga_scheduler_apply_ms,ga_action_dispatch_overhead_ms,ga_pareto_export_ms,ga_diagnostics_ms,ga_mutation_ms,ga_offspring_ms,ga_postprocess_ms,action_apply_end_ns,algorithm_observed_epoch,algorithm_observed_time_ns,action_response_epoch,action_response_ms,cpu_temp_c,gpu_temp_c\n";

//     const int rtGpu = runtimeControls_
//         ? (runtimeControls_->enable_gpu.load(std::memory_order_relaxed) ? 1 : 0) : -1;
//     const double rtSplit = runtimeControls_
//         ? runtimeControls_->gpu_workload_split.load(std::memory_order_relaxed) : -1.0;
//     const int rtConcurrency = runtimeControls_
//         ? runtimeControls_->concurrency_level.load(std::memory_order_relaxed) : -1;
//     const int rtAffinity = runtimeControls_
//         ? static_cast<int>(runtimeControls_->affinity.load(std::memory_order_relaxed)) : -1;
//     const long appliedCpuKhz = runtimeControls_
//         ? runtimeControls_->commanded_cpu_max_freq_khz.load(std::memory_order_relaxed) : 0L;

//     const uint64_t actionEpoch = runtimeControls_
//         ? runtimeControls_->action_generation.load(std::memory_order_acquire)
//         : static_cast<uint64_t>(generation_) + 1ULL;
//     const uint64_t applyEndNs = runtimeControls_
//         ? runtimeControls_->action_apply_end_ns.load(std::memory_order_acquire) : 0ULL;
//     const uint64_t observedEpoch = runtimeControls_
//         ? runtimeControls_->algorithm_observed_generation.load(std::memory_order_acquire) : 0ULL;
//     const uint64_t observedNs = runtimeControls_
//         ? runtimeControls_->algorithm_observed_time_ns.load(std::memory_order_acquire) : 0ULL;
//     const uint64_t responseEpoch = runtimeControls_
//         ? runtimeControls_->action_response_generation.load(std::memory_order_acquire) : 0ULL;
//     const uint64_t responseNs = runtimeControls_
//         ? runtimeControls_->action_response_latency_ns.load(std::memory_order_acquire) : 0ULL;
//     const double actionResponseMs = responseEpoch > 0
//         ? static_cast<double>(responseNs) / 1.0e6 : -1.0;

//     const std::string awmRegime = weightManager_ ? weightManager_->getCurrentRegimeName() : "FIXED_WEIGHTS";
//     const std::string awmDetected = weightManager_ ? weightManager_->getLastDetectedRegimeName() : "FIXED_WEIGHTS";
//     const std::string awmTriggerField = weightManager_ ? weightManager_->getLastTriggerField() : "none";
//     const double awmTriggerValue = weightManager_ ? weightManager_->getLastTriggerValue() : 0.0;
//     const double awmTriggerThreshold = weightManager_ ? weightManager_->getLastTriggerThreshold() : 0.0;
//     const int awmStreak = weightManager_ ? weightManager_->getRegimeStreak() : 0;
//     const int awmLastUpdate = weightManager_ ? weightManager_->getLastUpdateGeneration() : -1;

//     const double targetFps = selected.genome.target_fps.has ? selected.genome.target_fps.value : 0.0;
//     const double powerBudget = selected.genome.power_budget_watts.has ? selected.genome.power_budget_watts.value : 0.0;
//     const int preferGpu = selected.genome.prefer_gpu.has ? (selected.genome.prefer_gpu.value ? 1 : 0) : -1;
//     const double splitGene = selected.genome.gpu_workload_split.has ? selected.genome.gpu_workload_split.value : 0.0;
//     const double cpuFreqGene = selected.genome.cpu_max_freq_khz.has ? selected.genome.cpu_max_freq_khz.value : 0.0;

//     const double obj0 = selected.objectives.size() > 0 ? selected.objectives[0] : 0.0;
//     const double obj1 = selected.objectives.size() > 1 ? selected.objectives[1] : 0.0;
//     const double obj2 = selected.objectives.size() > 2 ? selected.objectives[2] : 0.0;
//     const double obj3 = selected.objectives.size() > 3 ? selected.objectives[3] : 0.0;

//     out << 3 << ","
//         << generation_ << "," << actionEpoch << "," << snap.frameId << ","
//         << lastEvaluatedRank0Count_ << "," << selected.rank << "," << selected.fitness << ","
//         << lastRank0RunnerUpFitness_ << "," << lastSelectionMargin_ << ","
//         << static_cast<int>(selected.genome.mode) << ","
//         << (selected.genome.target_fps.has ? 1 : 0) << "," << targetFps << ","
//         << (selected.genome.power_budget_watts.has ? 1 : 0) << "," << powerBudget << ","
//         << (selected.genome.prefer_gpu.has ? 1 : 0) << "," << preferGpu << ","
//         << (selected.genome.gpu_workload_split.has ? 1 : 0) << "," << splitGene << ","
//         << (cfg_.gpuSplitApplicable ? 1 : 0) << ","
//         << (selected.genome.cpu_max_freq_khz.has ? 1 : 0) << "," << cpuFreqGene << ","
//         << appliedCpuKhz << "," << rtGpu << "," << rtSplit << ","
//         << rtConcurrency << "," << rtAffinity << ","
//         << awmRegime << "," << awmDetected << ","
//         << selected.fitnessWeights[0] << "," << selected.fitnessWeights[1] << ","
//         << selected.fitnessWeights[2] << "," << selected.fitnessWeights[3] << ","
//         << awmTriggerField << "," << awmTriggerValue << "," << awmTriggerThreshold << ","
//         << awmStreak << "," << awmLastUpdate << ","
//         << (selected.surrogateUsed ? 1 : 0) << ","
//         << SurrogateModel::sourceName(selected.surrogatePrediction.source) << ","
//         << selected.surrogatePrediction.confidence << ","
//         << (selected.surrogatePrediction.from_data ? 1 : 0) << ","
//         << selected.surrogatePrediction.support_count << ","
//         << selected.surrogatePrediction.neighbor_count << ","
//         << selected.surrogatePrediction.nearest_distance << ","
//         << selected.surrogateExplorationBonus << ","
//         << selected.surrogateStallPenalty << ","
//         << selected.surrogateConfigFreqKhz << ","
//         << (selected.surrogateConfigGpu ? 1 : 0) << ","
//         << selected.surrogateConfigGpuSplit << ","
//         << selected.surrogateConfigConcurrency << ","
//         << selected.surrogatePrediction.fps << ","
//         << selected.surrogatePrediction.power_w << ","
//         << selected.surrogatePrediction.latency_ms << ","
//         << selected.surrogatePrediction.temp_c << ","
//         << obj0 << "," << obj1 << "," << obj2 << "," << obj3 << ","
//         << selected.fitFpsComponent << "," << selected.fitPowerComponent << ","
//         << selected.fitTempComponent << "," << selected.fitLatencyComponent << ","
//         << selected.fitGpuBonus << "," << selected.fitBeforeAge << ","
//         << selected.fitAgeFactor << "," << selected.fitAfterAge << "," << selected.fitFinal << ","
//         << (selected.feasible ? 1 : 0) << "," << selected.constraintViolation << ","
//         << snap.fps << "," << snap.avg_power_w_alg << "," << snap.joulesPerFrame << ","
//         << snap.avg_inference_ms << "," << snap.end_to_end_latency_ms << ","
//         << lastTiming_.total_compute_ms << "," << lastTiming_.weight_update_ms << ","
//         << lastTiming_.evaluation_ms << "," << lastTiming_.nsga_sort_ms << ","
//         << lastTiming_.selection_ms << "," << lastTiming_.scheduler_apply_ms << ","
//         << lastTiming_.action_dispatch_overhead_ms << "," << lastTiming_.pareto_export_ms << ","
//         << lastTiming_.diagnostics_ms << "," << lastTiming_.mutation_ms << ","
//         << lastTiming_.offspring_ms << "," << lastTiming_.postprocess_ms << ","
//         << applyEndNs << "," << observedEpoch << "," << observedNs << ","
//         << responseEpoch << "," << actionResponseMs << ","
//         << snap.cpu_temp_c << "," << snap.gpu_temp_c << "\n";
// }

// } // namespace hrl


// //

// //================================================== GeneticAlgorithm_05.h ===========================================================
// // FINAL GOLD STANDARD ? PhD-Ready (Merged Best of GA_03 + GA_04)
// // Pareto + Scalar Hybrid | Clean | Complete | Jetson Nano Optimized
// // GeneticAlgorithm_05.h - FINAL GOLD STANDARD ? PhD-Ready
// //=====================================================================================================================================

// #pragma once

// #include <vector>
// #include <random>
// #include <algorithm>
// #include <limits>
// #include <memory>
// #include <cmath>
// #include <spdlog/spdlog.h>

// #include <array>
// #include <cstdint>
// #include <fstream>
// #include <iomanip>
// #include <mutex>
// #include <sstream>
// #include <cerrno>
// #include <cstring>
// #include <sys/stat.h>
// #include <sys/types.h>

// #include "RuntimeControls.h"
// #include "../Stage_01/SharedStructures/allModulesStatcs.h"
// #include "RuntimeControls.h"   // ? ADD THIS
// #include "IScheduler.h"
// #include "AdaptiveWeightManager.h"
// #include "PowerSanity.h"          // physical-plausibility check used by headroom recovery
// #include "SurrogateModel.h"   // online counterfactual fitness surrogate (thesis gap 2)

// #include "../Stage_01/Concretes/SystemMetricsAggregatorConcrete_v3_2.h"
// #include "../Stage_01/Others/utils.h"

// namespace hrl {

// // ===================================================================
// // GAConfig
// // ===================================================================
// struct GAConfig {
//     size_t   popSize             = 12;
//     size_t   minPopSize          = 6;
//     size_t   eliteCount          = 3;
//     double   crossoverRate       = 0.75;
//     double   mutationRate        = 0.18;   // Base / floor mutation rate
//     double   explorationDecay    = 0.96;
//     int      controlIntervalMs   = 800;
//     size_t   reductionStartGen   = 10;
//     double   reductionRatio      = 0.6;

//     // [Adaptive mutation] Plateau-driven mutation control (extracted from
//     // AdaptivePopulationManager, made Pareto-safe ? it scales the rate only,
//     // never touches selection, so it composes cleanly with NSGA-II).
//     // When best fitness improves, mutation eases toward exploit; when it
//     // plateaus, mutation ramps up toward maxMutationRate to escape stagnation.
//     bool   adaptiveMutationEnabled = true;
//     double maxMutationRate         = 0.35;   // Hard cap when fully plateaued
//     // Relative improvement threshold (fraction of |prev best fitness|) below
//     // which a generation counts as "stagnating". Scaled to the fitness signal
//     // (which sits in the ~±0.05?0.20 band) rather than the old absolute 0.01.
//     double mutationPlateauRelThreshold = 0.02;   // 2% relative improvement
//     double mutationPlateauAbsFloor     = 0.0005; // abs floor for near-zero fitness
//     double mutationImproveFactor       = 0.8;    // multiply base when improving
//     double mutationPlateauStep         = 0.10;   // +10% of base per plateau gen

//     // [Surrogate] Online counterfactual fitness (thesis gap 2). When enabled, all
//     // candidates are scored by a learned per-config surrogate instead of the shared
//     // measured snapshot, and under-explored configs receive an optimism bonus.
//     // Disable for the ablation baseline (surrogate-off vs surrogate-on).
//     bool   surrogateEnabled       = true;
//     double surrogateExploreWeight = 0.15;   // UCB bonus scale in fitness units

//     // [FIX P2] Reality gating for counterfactual fitness.
//     int    surrogateMinSupport      = 3;    // below this support, predictions are pessimised
//     double surrogateColdFpsCeiling  = 8.0;  // fps ceiling before any real measurement exists
//     double surrogatePredFpsCapRatio = 1.2;  // pred_fps <= ratio * best measured fps this run
//     // [FIX P7] Physics-prior FPS ceiling, per workload (was hard-coded 60 for all).
//     double surrogateFpsMax          = 60.0;

//     // [FINAL FIX L1] Processing latency is a separate physical quantity from
//     // frame period (1000/FPS).  The surrogate therefore needs an independent
//     // cold-start latency prior instead of deriving latency from throughput.
//     double surrogateLatencyPriorMs  = 10.0;

//     // [FINAL FIX F1] Exact-bucket feasibility gate.  Repeated bounded evidence
//     // timeouts are treated as a constraint violation, not merely a soft scalar
//     // penalty.  This prevents a low-power configuration that repeatedly stalls
//     // from remaining Rank-0 because of attractive power/temperature objectives.
//     bool     hardFeasibilityEnabled         = true;
//     uint32_t hardFeasibilityMinAttempts     = 3;
//     double   hardFeasibilityMaxTimeoutRatio = 0.50;

//     // [FINAL FIX D1] Preserve one representative of each macro policy mode in
//     // every offspring population.  Representation does NOT imply admissibility:
//     // a thermally unsafe MAX candidate may remain genetically available while
//     // constraint-domination prevents it from being selected.
//     bool enforceModeDiversity = true;

//     // [FINAL FIX H1] When measured FPS is below the performance threshold while
//     // physically-valid power and temperatures still have headroom, LOW_POWER is
//     // temporarily inadmissible.  This encodes the intended control semantics:
//     // "spend available resource headroom to recover required performance".
//     bool   performanceHeadroomConstraint = true;
//     double performanceRecoveryRatio       = 0.70;
//     double performancePowerHeadroomW      = 0.50;
//     double performanceThermalHeadroomC    = 2.0;

//     // [FINAL FIX T1] In a measured THERMAL_CRITICAL state, MAX_PERFORMANCE is
//     // a hard feasibility violation rather than only another weighted penalty.
//     bool thermalCriticalBlocksMax = true;

//     // Whether the active algorithm physically consumes the continuous GPU split.
//     // ConfigManager sets this true only for HeterogeneousGaussianBlur. For
//     // MedianFilter and other binary CPU/GPU algorithms, the surrogate uses the
//     // canonical split {0,1} implied by GPU state so identical hardware states
//     // cannot acquire different model buckets.
//     // Fail closed: continuous split is opt-in.  ConfigManager/EvolutionarySelector
//     // must set true explicitly for HeterogeneousGaussianBlur.  Binary workloads
//     // (HistogramEqualization, MedianFilter, SobelEdge, etc.) remain canonical {0,1}.
//     bool gpuSplitApplicable = false;

//     // Pareto evidence export for PhD validation.
//     // Disabled by default to preserve normal runtime behaviour.
//     bool        export_pareto_evidence = false;
//     std::string pareto_csv_path = "output/erl_pareto_front.csv";
//     std::string action_csv_path = "output/erl_action_trace.csv";

//     // Do not let missing Lynsyn/power block Pareto evidence generation.
//     // On Jetson runs the power stream often starts later than FPS/camera metrics.
//     int    stableFramesRequired = 1;
//     double minFpsForEvolution = 1.0;
//     double minPowerForEvolution = 0.0;
//     bool   requirePowerForEvolution = false;

//     // AdaptiveWeightManager (opt-in ? default preserves current behaviour)
//     bool                           adaptive_weights_enabled = false;
//     AdaptiveWeightManager::Config  awm_config               = {};

//     // Reproducibility: if rngSeed >= 0, the GA seeds std::mt19937 deterministically.
//     // If rngSeed < 0 (default), it falls back to std::random_device for entropy.
//     // Set this (e.g. from config "seed") so ERL runs are seed-stable for the thesis.
//     long rngSeed = -1;

//     // Search-space floor for target_fps gene. Raised from the old hardcoded 8.0
//     // so the population cannot collapse onto the ~8 fps low-clock corner.
//     // Applied in initializePopulation(), mutate(), and crossover().
//     double minTargetFps = 20.0;
//     double maxTargetFps = 60.0;

//     // Workload policy. SobelEdge is GPU-dominant on the validated Jetson Nano
//     // data; ConfigManager enables this flag only for SobelEdge by default.
//     // ThermalGovernor remains authoritative and may still revoke GPU access.
//     bool   forceGpuForWorkload = false;

//     // Fixed performance reference used by HIGH_PERFORMANCE AWM/objective logic.
//     // It is intentionally independent of the evolvable target_fps gene.
//     double performanceTargetFps = 60.0;

//     // [PhD FIX] Explicit scalarization weights for the fitness function.
//     // These bridge the gap between EvolutionarySelector config and the GA's scalar fitness.
//     double weight_fps     = 1.0;
//     double weight_power   = 0.5;
//     double weight_temp    = 0.5;
//     double weight_latency = 0.3;

//     // [P0-F17] Temperature-objective onset thresholds. The old hard-coded
//     // 75C/80C onsets in calculateObjectives() were unreachable on Jetson
//     // Nano (39-65C observed across ERL and baselines), so objs[2] was
//     // identically zero and temperature never influenced dominance, crowding,
//     // or fitness. Defaults track the calibrated AdaptiveWeightManager warn
//     // thresholds (57C) as the single source of truth; override per run via
//     // obj_temp_onset_cpu_c / obj_temp_onset_gpu_c in the Scheduler block.
//     double temp_onset_cpu_c = AdaptiveWeightManager::Config().thermal_warn_cpu_c;
//     double temp_onset_gpu_c = AdaptiveWeightManager::Config().thermal_warn_gpu_c;

// };

// // ===================================================================
// // PhD timing instrumentation: per-generation GA phase breakdown.
// // All values are wall-clock milliseconds measured with steady_clock.
// // total_compute_ms excludes the final selected-action CSV append; the outer
// // EvolutionarySelector separately measures complete controller active time.
// // ===================================================================
// struct GATimingMetrics {
//     double weight_update_ms{0.0};
//     double evaluation_ms{0.0};
//     double nsga_sort_ms{0.0};
//     double selection_ms{0.0};
//     double scheduler_apply_ms{0.0};
//     double action_dispatch_overhead_ms{0.0};
//     double pareto_export_ms{0.0};
//     double diagnostics_ms{0.0};
//     double mutation_ms{0.0};
//     double offspring_ms{0.0};
//     double postprocess_ms{0.0};
//     double total_compute_ms{0.0};
//     bool valid{false};
// };

// // ===================================================================
// // ParetoIndividual (Single struct ? Dual Evaluation)
// // ===================================================================
// struct ParetoIndividual {
//     RLAction genome;

//     // Multi-objective (all minimized)
//     std::vector<double> objectives;   // [normFPS, normEnergy, normTemp, normLatency]

//     // Scalarized single fitness (for ranking & elitism)
//     double fitness = -std::numeric_limits<double>::max();

//     // NSGA-II
//     double crowdingDistance = 0.0;
//     int    rank = 0;

//     // [FINAL FIX F1/T1/H1] Constraint-domination state.
//     // Feasible candidates always dominate infeasible candidates.  Among
//     // infeasible candidates, smaller violation is preferred before normal
//     // objective dominance is considered.
//     bool   feasible = true;
//     double constraintViolation = 0.0;

//     // History
//     int    age = 0;
//     int    generation = 0;

//     // [PhD deterministic telemetry] Prediction/provenance used for THIS exact
//     // candidate evaluation. These fields are copied with the selected individual,
//     // preserving the decision rationale even after the population is replaced.
//     SurrogatePrediction surrogatePrediction{};
//     bool   surrogateUsed = false;
//     double surrogateExplorationBonus = 0.0;
//     double surrogateStallPenalty = 0.0;
//     double surrogateConfigFreqKhz = 0.0;
//     bool   surrogateConfigGpu = false;
//     double surrogateConfigGpuSplit = 0.0;
//     int    surrogateConfigConcurrency = 0;

//     // Exact scalar-fitness decomposition used to rank this candidate.
//     std::array<double, 4> fitnessWeights{{0.0, 0.0, 0.0, 0.0}};
//     double fitFpsComponent = 0.0;
//     double fitPowerComponent = 0.0;
//     double fitTempComponent = 0.0;
//     double fitLatencyComponent = 0.0;
//     double fitGpuBonus = 0.0;
//     double fitBeforeAge = 0.0;
//     double fitAgeFactor = 1.0;
//     double fitAfterAge = 0.0;
//     double fitFinal = -std::numeric_limits<double>::max();

//     ParetoIndividual() = default;

//     bool operator<(const ParetoIndividual& other) const {
//         if (rank != other.rank) return rank < other.rank;
//         return crowdingDistance > other.crowdingDistance;
//     }

//     bool dominates(const ParetoIndividual& other) const;

//     // bool dominates(const ParetoIndividual& other) const {
//     //     if (objectives.size() != other.objectives.size()) return false;
//     //     bool strictlyBetter = false;
//     //     for (size_t i = 0; i < objectives.size(); ++i) {
//     //         if (objectives[i] > other.objectives[i]) return false;
//     //         if (objectives[i] < other.objectives[i]) strictlyBetter = true;
//     //     }
//     //     return strictlyBetter;
//     // }
// };

// // ===================================================================
// // GeneticAlgorithm
// // Description: 
// // ===================================================================
// class GeneticAlgorithm {
// public:
//     // Overloaded constructor accepting awmCfg  
//     // GeneticAlgorithm(const GAConfig& cfg,
//     //                  std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator,
//     //                  std::shared_ptr<hrl::RuntimeControls> runtimeControls,
//     //                  IScheduler* scheduler = nullptr,
//     //                  const hrl::AdaptiveWeightManager::Config& awmCfg)

//     GeneticAlgorithm(const GAConfig& cfg,
//                      std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator,
//                      std::shared_ptr<hrl::RuntimeControls> runtimeControls,
//                      IScheduler* scheduler = nullptr,
//                      const hrl::AdaptiveWeightManager::Config& awmCfg =
//                          hrl::AdaptiveWeightManager::Config{})
//         : cfg_(cfg),
//           aggregator_(std::move(aggregator)),
//           runtimeControls_(std::move(runtimeControls)),
//           rng_(cfg.rngSeed >= 0
//                    ? static_cast<std::mt19937::result_type>(cfg.rngSeed)
//                    : std::random_device{}()),
//           scheduler_(scheduler),
//           generation_(0),
//           stableFrames_(0),
//           surrogate_(makeSurrogateConfig(cfg))
//     {
//         // Single authoritative AWM configuration/instance. The production
//         // EvolutionarySelector passes gaCfg.awm_config explicitly as arg 5.
//         cfg_.awm_config = awmCfg;

//         if (cfg_.rngSeed >= 0) {
//             spdlog::info("[GeneticAlgorithm] Deterministic RNG seed = {}", cfg_.rngSeed);
//         } else {
//             spdlog::info("[GeneticAlgorithm] Non-deterministic RNG (random_device)");
//         }

//         currentMutationRate_ = cfg_.mutationRate;
//         initializePopulation();
//         if (cfg_.export_pareto_evidence) ensureEvidenceFiles();

//         spdlog::info("[GeneticAlgorithm] Pareto-ERL ready (pop={}, reduction_gen={}, evidence={}, force_gpu={}, perf_target={:.1f})",
//                      cfg_.popSize, cfg_.reductionStartGen,
//                      cfg_.export_pareto_evidence ? "ON" : "OFF",
//                      cfg_.forceGpuForWorkload ? "YES" : "NO",
//                      cfg_.performanceTargetFps);

//         if (cfg_.adaptive_weights_enabled) {
//             weightManager_ = std::make_unique<hrl::AdaptiveWeightManager>(awmCfg);
//             spdlog::info("[GeneticAlgorithm] AdaptiveWeightManager ENABLED (update_interval={} gens, smoothing={:.2f})",
//                          awmCfg.update_interval_gens, awmCfg.transition_smoothing);
//         }
//     }

// //================================================================================================
// // [FIX P5] Called when the selector force-applies a recovery configuration
//     // outside the GA's control: the next evidence window must not be credited
//     // to the previously selected genome (it did not produce those frames).
//     void notifyExternalOverride() { hasLastApplied_ = false; }


// void evolve(const hrl::MetricsSnapshot& snapshot,
//             bool creditPreviousAction = true,
//             bool previousActionStalled = false) {
//     // if (!isSnapshotStable(snapshot)) {
//     //     lastTiming_ = GATimingMetrics{};
//     //     return;
//     // }
//     // [FINAL FIX F2] A timeout must still reach evaluateParetoPopulation()
//     // so the exact applied bucket receives observeStarvation(). The timeout
//     // is feasibility evidence only; it is no longer injected into the physical
//     // FPS/latency EMA.
//     if (!previousActionStalled && !isSnapshotStable(snapshot)) {
//         lastTiming_ = GATimingMetrics{};
//         return;
//     }

//     using TimingClock = std::chrono::steady_clock;
//     const auto totalStart = TimingClock::now();
//     lastTiming_ = GATimingMetrics{};

//     auto elapsedMs = [](const TimingClock::time_point& a,
//                         const TimingClock::time_point& b) -> double {
//         return std::chrono::duration<double, std::milli>(b - a).count();
//     };

//     // ============================================================
//     // 0) Adaptive-weight update
//     // ============================================================
//     auto phaseStart = TimingClock::now();
//     // [FINAL FIX A1] Regime classification is a controller-state decision,
//     // not a surrogate-credit decision.  Even when the previous action timed out,
//     // the latest valid snapshot must still be allowed to trigger
//     // HIGH_PERFORMANCE / thermal recovery.  Only surrogate learning is gated by
//     // creditPreviousAction below.
//     if (weightManager_ && snapshot.valid) {
//         // HIGH_PERFORMANCE detection uses a fixed experimental target, never an
//         // evolvable low target_fps gene.
//         const double targetFps = cfg_.performanceTargetFps;

//         bool changed = weightManager_->update(
//             snapshot,
//             static_cast<int>(generation_),
//             targetFps
//         );

//         if (changed) {
//             spdlog::info(
//                 "[GeneticAlgorithm] Weight regime -> {} | weights=[{:.3f},{:.3f},{:.3f},{:.3f}]",
//                 weightManager_->getCurrentRegimeName(),
//                 weightManager_->getWeights()[0],
//                 weightManager_->getWeights()[1],
//                 weightManager_->getWeights()[2],
//                 weightManager_->getWeights()[3]
//             );
//         }
//     }
//     auto phaseEnd = TimingClock::now();
//     lastTiming_.weight_update_ms = elapsedMs(phaseStart, phaseEnd);

//     // ============================================================
//     // 1) Evaluate current population
//     // ============================================================
//     phaseStart = TimingClock::now();
//     evaluateParetoPopulation(snapshot, creditPreviousAction, previousActionStalled);
//     phaseEnd = TimingClock::now();
//     lastTiming_.evaluation_ms = elapsedMs(phaseStart, phaseEnd);

//     // ============================================================
//     // 2) NSGA-II ranking + crowding
//     // ============================================================
//     phaseStart = TimingClock::now();
//     nonDominatedSortAndCrowding();
//     phaseEnd = TimingClock::now();
//     lastTiming_.nsga_sort_ms = elapsedMs(phaseStart, phaseEnd);
//     lastEvaluatedRank0Count_ = countRank0();

//     // ============================================================
//     // 3) Select evaluated Rank-0 action
//     // ============================================================
//     phaseStart = TimingClock::now();
//     const ParetoIndividual* selected = selectBestEvaluatedPareto();
//     ParetoIndividual selectedCopy;
//     const bool haveSelected = (selected != nullptr);

//     // Capture how decisive the Rank-0 choice was BEFORE population replacement.
//     //lastRank0RunnerUpFitness_ = std::numeric_limits<double>::quiet_NaN();
//     lastSelectionMargin_ = std::numeric_limits<double>::quiet_NaN();
//     if (haveSelected) {
//         double runnerUp = -std::numeric_limits<double>::max();
//         bool haveRunnerUp = false;
//         for (const auto& ind : population_) {
//             if (ind.rank != 0 || &ind == selected) continue;
//             if (!haveRunnerUp || ind.fitness > runnerUp) {
//                 runnerUp = ind.fitness;
//                 haveRunnerUp = true;
//             }
//         }
//         if (haveRunnerUp) {
//             lastRank0RunnerUpFitness_ = runnerUp;
//             lastSelectionMargin_ = selected->fitness - runnerUp;
//         }
//         selectedCopy = *selected;  // Preserve evidence after population replacement.
//     }
//     phaseEnd = TimingClock::now();
//     lastTiming_.selection_ms = elapsedMs(phaseStart, phaseEnd);

//     // ============================================================
//     // 4) Scheduler / runtime actuation
//     // ============================================================
//     phaseStart = TimingClock::now();
//     if (haveSelected) {
//         lastTiming_.scheduler_apply_ms = applySelectedParetoToRuntime(selectedCopy, snapshot);
//     }
//     phaseEnd = TimingClock::now();
//     const double actionDispatchTotalMs = elapsedMs(phaseStart, phaseEnd);
//     lastTiming_.action_dispatch_overhead_ms = std::max(
//         0.0, actionDispatchTotalMs - lastTiming_.scheduler_apply_ms);

//     // ============================================================
//     // 5) Pareto-front evidence (kept before mutation/replacement)
//     // ============================================================
//     phaseStart = TimingClock::now();
//     exportParetoEvidence(snapshot, selected);
//     phaseEnd = TimingClock::now();
//     lastTiming_.pareto_export_ms = elapsedMs(phaseStart, phaseEnd);

//     phaseStart = TimingClock::now();
//     logDominanceAnalysis();
//     logParetoStats();
//     phaseEnd = TimingClock::now();
//     lastTiming_.diagnostics_ms = elapsedMs(phaseStart, phaseEnd);

//     // ============================================================
//     // 5b) Adaptive mutation
//     // ============================================================
//     phaseStart = TimingClock::now();
//     {
//         double bestFit = -std::numeric_limits<double>::max();
//         for (const auto& ind : population_) {
//             if (ind.rank == 0 && ind.fitness > bestFit) bestFit = ind.fitness;
//         }
//         if (bestFit == -std::numeric_limits<double>::max() && !population_.empty()) {
//             bestFit = population_[0].fitness;
//         }
//         double r = updateAdaptiveMutationRate(bestFit);
//         if (generation_ % 10 == 0) {
//             spdlog::info("[GA-Pareto] Adaptive mutation rate={:.3f} "
//                          "(plateau={}, bestFit={:.4f})",
//                          r, mutationPlateauCounter_, bestFit);
//         }
//     }
//     phaseEnd = TimingClock::now();
//     lastTiming_.mutation_ms = elapsedMs(phaseStart, phaseEnd);

//     // ============================================================
//     // 6) Generate next population
//     // ============================================================
//     phaseStart = TimingClock::now();
//     reducePopulation();
//     auto newPop = selectEliteAndOffspring();

//     // [FINAL FIX D1] Crossover/elitism can otherwise extinguish an entire
//     // policy mode (observed as 8/8 BALANCED and later 8/8 MAX populations).
//     // Keep one MAX, one BALANCED and one LOW representative available.
//     ensureModeDiversity(newPop);

//     population_ = std::move(newPop);
//     phaseEnd = TimingClock::now();
//     lastTiming_.offspring_ms = elapsedMs(phaseStart, phaseEnd);

//     // ============================================================
//     // 7) Replay/diversity post-processing
//     // ============================================================
//     phaseStart = TimingClock::now();
//     seedRLReplayBuffer(snapshot);
//     injectDiversityIfNeeded();
//     phaseEnd = TimingClock::now();
//     lastTiming_.postprocess_ms = elapsedMs(phaseStart, phaseEnd);

//     const auto computeEnd = TimingClock::now();
//     lastTiming_.total_compute_ms = elapsedMs(totalStart, computeEnd);
//     lastTiming_.valid = true;

//     // Export the selected-action row after phase timings are finalised. This
//     // deliberately leaves the row's own file-append time out of ga_compute_total_ms;
//     // EvolutionarySelector's outer erl_active_wall_ms includes it.
//     if (haveSelected) {
//         exportSelectedActionEvidence(snapshot, selectedCopy);
//     }

//     ++generation_;
// }
// //=========================================================================================
//     // void evolve(const hrl::MetricsSnapshot& snapshot) {
//     //     if (!isSnapshotStable(snapshot)) return;

//     //     // Update adaptive weights BEFORE fitness evaluation
//     //     if (weightManager_) {
//     //         double targetFps = population_.empty() ? 30.0 :
//     //             (population_[0].genome.target_fps.has ? population_[0].genome.target_fps.value : 30.0);
//     //         bool changed = weightManager_->update(snapshot, static_cast<int>(generation_), targetFps);
//     //         if (changed) {
//     //             spdlog::info("[GeneticAlgorithm] Weight regime ? {} | weights=[{:.3f},{:.3f},{:.3f},{:.3f}]",
//     //                          weightManager_->getCurrentRegimeName(),
//     //                          weightManager_->getWeights()[0],
//     //                          weightManager_->getWeights()[1],
//     //                          weightManager_->getWeights()[2],
//     //                          weightManager_->getWeights()[3]);
//     //         }
//     //     }
        
//     //     // 1) Evaluate the CURRENT population against the latest measured snapshot.
//     //     evaluateParetoPopulation(snapshot);      // Dual: objectives + fitness
//     //     // 2) Rank by non-dominated sorting and crowding distance.
//     //     nonDominatedSortAndCrowding();
//     //     // 3) Select the best evaluated Rank-0 individual BEFORE offspring generation.
//     //     //    This is the key thesis-proof fix: the applied action is from the evaluated Pareto front.
//     //     const ParetoIndividual* selected = selectBestEvaluatedPareto();

//     //     // 4) Apply the selected evaluated Pareto action to runtime controls.
//     //     if (selected) {
//     //         applySelectedParetoToRuntime(*selected, snapshot);
//     //     }

//     //     // 5) Export evidence before population mutation/replacement.
//     //     exportParetoEvidence(snapshot, selected);
//     //     if (selected) {
//     //         exportSelectedActionEvidence(snapshot, *selected);
//     //     }

//     //     logDominanceAnalysis();     // PhD thesis material
//     //     logParetoStats();
       
//     //     // 6) Generate the NEXT population only after applying/logging the current front.
//     //     reducePopulation();
//     //     auto newPop = selectEliteAndOffspring();
//     //     population_ = std::move(newPop);

//     //     ++generation_;
//     //     applyBestParetoToRuntime(snapshot);
//     //     seedRLReplayBuffer(snapshot);
//     //     injectDiversityIfNeeded();
       
//     // }

//     RLAction getBestAction() const {
//         if (population_.empty()) return RLAction{};
//         // Return best Rank-0 by scalar fitness.
//         // [P0-F16] Seed with a rank-0 individual, not population_[0]
//         // unconditionally: previously a high-fitness *dominated* individual
//         // at index 0 could shadow the entire Pareto front.
//         const ParetoIndividual* best = nullptr;
//         for (const auto& ind : population_) {
//             if (ind.rank == 0 && (!best || ind.fitness > best->fitness))
//                 best = &ind;
//         }
//         if (!best) best = &population_[0];   // no rank-0 yet (e.g. pre-evaluation)
//         return best->genome;
//     }

//     // Thesis data export ? CSV of weight history
//     std::string exportWeightHistory() const {
//         if (weightManager_) return weightManager_->exportHistoryCSV();
//         return "adaptive_weights_disabled\n";
//     }

//     // Current regime label for logging / thesis
//     std::string getCurrentRegimeName() const {
//         if (weightManager_) return weightManager_->getCurrentRegimeName();
//         return "FIXED_WEIGHTS";
//     }

//     GATimingMetrics getLastTimingMetrics() const { return lastTiming_; }
//     size_t getCurrentGeneration() const { return generation_; }

// private:
//     GAConfig cfg_;
//     std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator_;
//     std::shared_ptr<hrl::RuntimeControls> runtimeControls_;
//     std::vector<ParetoIndividual> population_;
//     std::mt19937 rng_;
//     IScheduler* scheduler_ = nullptr;
//     size_t generation_ = 0;
//     int stableFrames_ = 0;
//     std::unique_ptr<AdaptiveWeightManager> weightManager_;
//     mutable std::mutex evidenceMutex_;
//     GATimingMetrics lastTiming_{};
//     int lastEvaluatedRank0Count_{0};

//      // [FIX] Restored: declaration was lost while merging the trap patches.
//     // Written in evolve() (runner-up capture) and read by
//     // exportSelectedActionEvidence() -> rank0_runnerup_fitness CSV column.
//     double lastRank0RunnerUpFitness_{std::numeric_limits<double>::quiet_NaN()};

//     double lastSelectionMargin_{std::numeric_limits<double>::quiet_NaN()};

//     // [Adaptive mutation] runtime state
//     double currentMutationRate_ = 0.18;   // Initialised from cfg_.mutationRate in ctor
//     double prevBestFitness_     = -std::numeric_limits<double>::max();
//     int    mutationPlateauCounter_ = 0;

//     // [Surrogate] online counterfactual fitness model + last-applied tracking.
//     // snap on cycle N reflects the action applied on cycle N-1, so we observe the
//     // last-applied config against the current snapshot (temporal credit assignment).
//     SurrogateModel surrogate_;
//     RLAction       lastAppliedGenome_;
//     bool           hasLastApplied_ = false;

//     double         maxMeasuredFps_ = 0.0;   // [FIX P2] best hardware-verified fps this run

// //     // ===================================================================
// //     // Simulate simulateRuntimeControlsFromAction
// //     // ===================================================================
// //     void simulateRuntimeControlsFromAction(const RLAction& action,
// //                                        RuntimeControls& simControls) const {
// //     int concurrency = 2;
// //     bool enableGpu = true;
// //     Affinity affinity = Affinity::Spread;

// //     switch (action.mode) {
// //         case PolicyMode::MAX_PERFORMANCE:
// //             concurrency = 4;
// //             enableGpu = true;
// //             affinity = Affinity::Spread;
// //             break;

// //         case PolicyMode::LOW_POWER:
// //             concurrency = 1;
// //             enableGpu = false;
// //             affinity = Affinity::Pack;
// //             break;

// //         case PolicyMode::BALANCED:
// //             concurrency = 2;
// //             enableGpu = action.prefer_gpu.has ? action.prefer_gpu.value : true;
// //             affinity = Affinity::Spread;
// //             break;

// //         case PolicyMode::UNKNOWN:
// //         default:
// //             concurrency = 2;
// //             enableGpu = true;
// //             affinity = Affinity::Spread;
// //             break;
// //     }

// //     if (action.prefer_gpu.has) {
// //         enableGpu = action.prefer_gpu.value;
// //     }

// //     simControls.concurrency_level.store(concurrency, std::memory_order_relaxed);
// //     simControls.enable_gpu.store(enableGpu, std::memory_order_relaxed);
// //     simControls.affinity.store(affinity, std::memory_order_relaxed);
// // }

//     // ===================================================================
//     // Evaluation (Dual: Pareto + Scalar)
//     // ===================================================================
//     // ===================================================================
//     // [Surrogate] Build the surrogate model configuration from GAConfig.
//     // ===================================================================
//     // static SurrogateModel::Config makeSurrogateConfig(const GAConfig& cfg) {
//     //     SurrogateModel::Config sc;
//     //     sc.exploration_weight = cfg.surrogateExploreWeight;
//     //     return sc;
//     // }

//     static SurrogateModel::Config makeSurrogateConfig(const GAConfig& cfg) {
//         SurrogateModel::Config sc;
//         sc.exploration_weight   = cfg.surrogateExploreWeight;
//         sc.fps_max              = cfg.surrogateFpsMax;          // per-workload FPS envelope
//         sc.latency_prior_ms     = cfg.surrogateLatencyPriorMs;  // independent processing-latency prior
//         sc.gpu_split_applicable = cfg.gpuSplitApplicable;       // binary workloads collapse split to {0,1}
//         return sc;
//     }

//     void canonicalizeGenomeForWorkload(RLAction& g) const {
//         // UNKNOWN is never part of the evolutionary search space.  This guard
//         // also cleans legacy/replayed genomes before they reach the scheduler.
//         if (g.mode == PolicyMode::UNKNOWN) {
//             g.mode = PolicyMode::BALANCED;
//         }

//         // Workload-level GPU preference remains authoritative unless thermal
//         // safety later revokes GPU access in the Scheduler.
//         if (cfg_.forceGpuForWorkload) {
//             g.prefer_gpu = MaybeBool(true);
//         }

//         // [FINAL FIX S1] Binary CPU/GPU algorithms must never preserve a
//         // fictitious continuous split gene.  Canonicalising the genome itself
//         // (not just the surrogate key) makes action telemetry honest as well:
//         // CPU => split 0, GPU => split 1.
//         if (!cfg_.gpuSplitApplicable) {
//             const bool gpu = g.prefer_gpu.has
//                 ? g.prefer_gpu.value
//                 : (g.mode != PolicyMode::LOW_POWER);
//             g.gpu_workload_split = MaybeDouble(gpu ? 1.0 : 0.0);
//         } else if (g.gpu_workload_split.has) {
//             g.gpu_workload_split.value =
//                 utils::local_clamp(g.gpu_workload_split.value, 0.0, 1.0);
//         }

//         // MAX_PERFORMANCE has fixed semantic intent: do not let a low evolved
//         // target relabel a configuration as "high performance".
//         if (g.mode == PolicyMode::MAX_PERFORMANCE) {
//             g.target_fps = MaybeDouble(cfg_.performanceTargetFps);
//         }
//     }

//     // ===================================================================
//     // [Surrogate] Extract the discretised hardware config a genome represents.
//     // freq: explicit gene if present, else derived from PolicyMode.
//     // split: work-split gene if present (thesis gap 1), else GPU on=>0.5 / off=>0.
//     // conc: derived from mode/budget to mirror the Scheduler's mapping.
//     // ===================================================================
//     void configFromGenome(const RLAction& g, double& freq_khz, bool& gpu,
//                           double& split, int& conc) const {
//         gpu = g.prefer_gpu.has ? g.prefer_gpu.value
//                                : (g.mode != PolicyMode::LOW_POWER);
//         if (g.cpu_max_freq_khz.has) {
//             if (g.mode == PolicyMode::LOW_POWER) {
//                 freq_khz = utils::local_clamp(g.cpu_max_freq_khz.value, 460800.0, 1479000.0);
//             } else {
//                 freq_khz = utils::local_clamp(g.cpu_max_freq_khz.value, 102000.0, 1479000.0);
//             }
//         } else {
//             switch (g.mode) {
//                 case PolicyMode::MAX_PERFORMANCE: freq_khz = 1479000.0; break;
//                 case PolicyMode::LOW_POWER:       freq_khz = 1020000.0; break;
//                 default:                          freq_khz =  921600.0; break;
//             }
//         }
//         // [Micro-scheduler] Only algorithms that physically implement a
//         // continuous CPU/GPU partition may use the split gene as a surrogate
//         // dimension. Binary algorithms (e.g. MedianFilter) are canonicalised
//         // to 0=CPU or 1=GPU, preventing fictitious duplicate model states.
//         if (!cfg_.gpuSplitApplicable) {
//             split = gpu ? 1.0 : 0.0;
//         } else if (g.gpu_workload_split.has) {
//             split = g.gpu_workload_split.value;
//             if (split < 0.0) split = 0.0;
//             if (split > 1.0) split = 1.0;
//         } else {
//             split = gpu ? 1.0 : 0.0;
//         }
//         double budget = g.power_budget_watts.has ? g.power_budget_watts.value : 4.0;
//         switch (g.mode) {
//             case PolicyMode::MAX_PERFORMANCE: conc = 4; break;
//             case PolicyMode::LOW_POWER:       conc = (budget < 3.5) ? 1 : 2; break;
//             default:                          conc = 2; break;
//         }
//     }

//     // Build a snapshot with the surrogate's predicted outcome for `g`, copying
//     // any unrelated fields from the real snapshot so downstream code is unchanged.
//     hrl::MetricsSnapshot predictedSnapshot(const hrl::MetricsSnapshot& base,
//                                            const RLAction& g) const {
//         double freq; bool gpu; double split; int conc;
//         configFromGenome(g, freq, gpu, split, conc);
//         SurrogatePrediction p = surrogate_.predict(freq, gpu, split, conc);
//         hrl::MetricsSnapshot s = base;   // preserve unrelated fields
//         s.fps             = p.fps;
//         s.avg_power_w_alg = p.power_w;
//         s.avg_latency_ms  = p.latency_ms;
//         s.cpu_temp_c      = p.temp_c;
//         return s;
//     }

//     void evaluateParetoPopulation(const hrl::MetricsSnapshot& snap,
//                                   bool creditPreviousAction,
//                                   bool previousActionStalled) {
//         // // One action -> one evidence window -> one surrogate credit update.
//         // if (cfg_.surrogateEnabled && hasLastApplied_) {
//         //     double freq; bool gpu; double split; int conc;
//         //     configFromGenome(lastAppliedGenome_, freq, gpu, split, conc);
//         //     if (previousActionStalled) {
//         //         surrogate_.observeStarvation(freq, gpu, split, conc);
//         //     } else if (creditPreviousAction) {
//         //         surrogate_.observe(freq, gpu, split, conc,
//         //                            snap.fps, snap.avg_power_w_alg,
//         //                            snap.avg_latency_ms, snap.cpu_temp_c);
//         //     }
//         // }
//         // One action -> one evidence window -> one surrogate credit update.
//         if (cfg_.surrogateEnabled && hasLastApplied_) {
//             double freq; bool gpu; double split; int conc;
//             configFromGenome(lastAppliedGenome_, freq, gpu, split, conc);
//             if (previousActionStalled) {
//                 // [FINAL FIX F2] A timeout is real feasibility evidence but it
//                 // is NOT an exact physical FPS/latency sample.  Fresh-frame poll
//                 // count is not guaranteed to equal processed-frame count, so do
//                 // not poison the physical EMA with a fabricated throughput.
//                 surrogate_.observeStarvation(freq, gpu, split, conc);
//             } else if (creditPreviousAction) {
//                 surrogate_.observe(freq, gpu, split, conc,
//                                    snap.fps, snap.avg_power_w_alg,
//                                    snap.avg_latency_ms, snap.cpu_temp_c);
//                 if (snap.fps > maxMeasuredFps_) maxMeasuredFps_ = snap.fps; // [FIX P2]
//             }
//         }

//         for (auto& ind : population_) {
//             canonicalizeGenomeForWorkload(ind.genome);
//             hrl::RuntimeControls simControls;
//             simControls.concurrency_level.store(2, std::memory_order_relaxed);
//             simControls.enable_gpu.store(true, std::memory_order_relaxed);
//             simulateRuntimeControlsFromAction(ind.genome, simControls);

//             // Preserve the exact counterfactual prediction used for THIS candidate.
//             hrl::MetricsSnapshot evalSnap = snap;
//             ind.surrogateUsed = false;
//             ind.surrogatePrediction = SurrogatePrediction{};
//             ind.surrogateExplorationBonus = 0.0;
//             ind.surrogateStallPenalty = 0.0;
//             ind.surrogateConfigFreqKhz = 0.0;
//             ind.surrogateConfigGpu = false;
//             ind.surrogateConfigGpuSplit = 0.0;
//             ind.surrogateConfigConcurrency = 0;
//             ind.feasible = true;
//             ind.constraintViolation = 0.0;

//             if (cfg_.surrogateEnabled) {
//                 double freq; bool gpu; double split; int conc;
//                 configFromGenome(ind.genome, freq, gpu, split, conc);

//                 ind.surrogateConfigFreqKhz = freq;
//                 ind.surrogateConfigGpu = gpu;
//                 ind.surrogateConfigGpuSplit = split;
//                 ind.surrogateConfigConcurrency = conc;

//                 // ind.surrogatePrediction = surrogate_.predict(freq, gpu, split, conc);
//                 // ind.surrogateUsed = true;
//                 // evalSnap.fps             = ind.surrogatePrediction.fps;

//                 ind.surrogatePrediction = surrogate_.predict(freq, gpu, split, conc);

//                 // [FIX P2] Reality gating: unverified optimism must not enter fitness.
//                 {
//                     SurrogatePrediction& p = ind.surrogatePrediction;
//                     const double fpsCap = (maxMeasuredFps_ > 0.0)
//                         ? cfg_.surrogatePredFpsCapRatio * maxMeasuredFps_
//                         : cfg_.surrogateColdFpsCeiling;
//                     const bool lowSupport = !p.from_data ||
//                         p.support_count < static_cast<uint32_t>(cfg_.surrogateMinSupport);
//                     if (lowSupport) {
//                         const double pessimistic = (maxMeasuredFps_ > 0.0)
//                             ? 0.5 * maxMeasuredFps_
//                             : 0.5 * cfg_.surrogateColdFpsCeiling;
//                         p.fps = std::min(p.fps, std::max(1.0, pessimistic));
//                     }
//                     if (p.fps > fpsCap) p.fps = fpsCap;   // unverified over-envelope claims are inadmissible

//                     // [FINAL FIX L1] DO NOT impose latency >= 1000/FPS.
//                     // 1000/FPS is the inter-frame period, not per-frame
//                     // processing/E2E latency.  Latency remains an independently
//                     // learned surrogate dimension.
//                 }

//                 ind.surrogateUsed = true;
//                 evalSnap.fps             = ind.surrogatePrediction.fps;

//                 evalSnap.avg_power_w_alg = ind.surrogatePrediction.power_w;
//                 evalSnap.avg_latency_ms  = ind.surrogatePrediction.latency_ms;
//                 evalSnap.cpu_temp_c      = ind.surrogatePrediction.temp_c;

//                 ind.surrogateExplorationBonus =
//                     surrogate_.explorationBonus(freq, gpu, split, conc);
//                 ind.surrogateStallPenalty =
//                     surrogate_.stallPenalty(freq, gpu, split, conc);

//                 // [FINAL FIX F1] Exact-bucket timeout feasibility.  Keep the
//                 // existing soft stall penalty for sparse evidence, but once an
//                 // exact configuration has enough attempts and a majority timeout
//                 // rate, constraint-domination removes it from the feasible front.
//                 if (cfg_.hardFeasibilityEnabled) {
//                     const SurrogateFeasibilityStats fs =
//                         surrogate_.feasibilityStats(freq, gpu, split, conc);
//                     if (fs.attempts >= cfg_.hardFeasibilityMinAttempts &&
//                         fs.timeout_ratio > cfg_.hardFeasibilityMaxTimeoutRatio) {
//                         ind.feasible = false;
//                         ind.constraintViolation += 1.0 + fs.timeout_ratio;
//                     }
//                 }
//             }

//             // [FINAL FIX H1] Performance recovery with resource headroom.
//             // If measured throughput is below the required band while power and
//             // temperatures are safely below their limits, LOW_POWER is not an
//             // admissible action.  This directly prevents the "2 W / 2 FPS while
//             // cool" trap observed in HistogramEqualization.
//             if (cfg_.performanceHeadroomConstraint &&
//                 ind.genome.mode == PolicyMode::LOW_POWER) {
//                 const double requiredFps =
//                     cfg_.performanceRecoveryRatio * cfg_.performanceTargetFps;
//                 const bool fpsDeficit = snap.fps < requiredFps;
//                 const bool powerHeadroom =
//                     PowerSanity::valid(snap.avg_power_w_alg) &&
//                     snap.avg_power_w_alg <
//                         (cfg_.awm_config.battery_critical_watts -
//                          cfg_.performancePowerHeadroomW);
//                 const bool thermalHeadroom =
//                     snap.cpu_temp_c <
//                         (cfg_.awm_config.thermal_warn_cpu_c -
//                          cfg_.performanceThermalHeadroomC) &&
//                     snap.gpu_temp_c <
//                         (cfg_.awm_config.thermal_warn_gpu_c -
//                          cfg_.performanceThermalHeadroomC);

//                 if (fpsDeficit && powerHeadroom && thermalHeadroom) {
//                     ind.feasible = false;
//                     const double deficitRatio =
//                         requiredFps > 0.0
//                             ? utils::local_clamp(
//                                   (requiredFps - snap.fps) / requiredFps,
//                                   0.0, 1.0)
//                             : 0.0;
//                     ind.constraintViolation += 1.0 + deficitRatio;
//                 }
//             }

//             // [FINAL FIX T1] Thermal safety is an admissibility rule, not just
//             // another scalar weight.  Preserve MAX genetically for later
//             // recovery, but prevent selection while the measured board is in
//             // the configured critical thermal band.
//             if (cfg_.thermalCriticalBlocksMax &&
//                 ind.genome.mode == PolicyMode::MAX_PERFORMANCE) {
//                 const double cpuOver = std::max(
//                     0.0, snap.cpu_temp_c - cfg_.awm_config.thermal_crit_cpu_c);
//                 const double gpuOver = std::max(
//                     0.0, snap.gpu_temp_c - cfg_.awm_config.thermal_crit_gpu_c);
//                 if (cpuOver > 0.0 || gpuOver > 0.0) {
//                     ind.feasible = false;
//                     ind.constraintViolation +=
//                         1.0 + (cpuOver + gpuOver) / 10.0;
//                 }
//             }

//             ind.objectives = calculateObjectives(evalSnap, ind.genome, simControls);
//             ind.fitness = calculateScalarizedFitness(ind.objectives, evalSnap,
//                                                       ind.genome, ind.age, &ind);

//             // Optimism under uncertainty is added AFTER scalar fitness/age decay,
//             // preserving the existing controller arithmetic exactly.
//             ind.fitness += ind.surrogateExplorationBonus;
//             ind.fitness -= ind.surrogateStallPenalty;
//             ind.fitFinal = ind.fitness;
//             ind.age++;
//         }
//     }


// //================================================================
//     //  Return vector of normalized objectives (all to be minimized)
//     std::vector<double> calculateObjectives(const hrl::MetricsSnapshot& snap,
//                                             const RLAction& action,
//                                             const RuntimeControls& controls) {
//         std::vector<double> objs(4);
//         // Objective 1: FPS (minimize negative FPS = maximize FPS)

//         const bool highPerformanceRegime =
//             weightManager_ && weightManager_->getCurrentRegime() == SystemRegime::HIGH_PERFORMANCE;
//         const double fpsScore = action.target_fps.has
//             ? (1.0 / (1.0 + std::abs(snap.fps - action.target_fps.value))) : 0.0;
//         const double directThroughput = cfg_.performanceTargetFps > 0.0
//             ? utils::local_clamp(snap.fps / cfg_.performanceTargetFps, 0.0, 2.0)
//             : 0.0;

//         // [P0] Frequency-aware energy model. The measured snapshot power reflects
//         // the CURRENT clock, but each candidate would run at its own gene frequency.
//         // Scale the energy estimate by (candidate_freq / max_freq) so a low-clock
//         // genome is correctly scored as cheaper and a high-clock genome as costlier.
//         // Without this, LOW_POWER and MAX_PERFORMANCE candidates score near-identical
//         // energy and the Pareto front collapses onto the idle low-clock corner.
//         // const double kMaxFreqKhz = 1479000.0;
//         // double freqRatio = action.cpu_max_freq_khz.has
//         //     ? (action.cpu_max_freq_khz.value / kMaxFreqKhz)
//         //     : 1.0;
//         // freqRatio = freqRatio < 0.05 ? 0.05 : (freqRatio > 1.0 ? 1.0 : freqRatio);

//         // double energy  = snap.avg_power_w_alg * freqRatio
//         //                  * (1.0 + 0.3 * (controls.concurrency_level.load() - 2));
//         // [FIX P4] With the surrogate on, snap.avg_power_w_alg is already the
//         // per-candidate PREDICTED power at this genome's own frequency/config;
//         // multiplying by freqRatio double-counted the clock and let 102 MHz
//         // genomes buy the energy objective ~15-20x below reality (Pareto rank-0
//         // then filled with LOW_POWER fictions). Keep the legacy discount only
//         // for the surrogate-off ablation baseline.
//         double energy = snap.avg_power_w_alg;
//         if (!cfg_.surrogateEnabled) {
//             const double kMaxFreqKhz = 1479000.0;
//             double freqRatio = action.cpu_max_freq_khz.has
//                 ? (action.cpu_max_freq_khz.value / kMaxFreqKhz)
//                 : 1.0;
//             freqRatio = freqRatio < 0.05 ? 0.05 : (freqRatio > 1.0 ? 1.0 : freqRatio);
//             energy *= freqRatio * (1.0 + 0.3 * (controls.concurrency_level.load() - 2));
//         }
//         // [P0-F17] Penalty onset uses the calibrated thresholds (default 57C
//         // via AdaptiveWeightManager::Config) instead of hard-coded 75C/80C,
//         // so the live ERL-vs-baseline thermal gradient (39-45C vs 61-65C)
//         // actually reaches the objective vector.
//         double tempPen = (snap.cpu_temp_c > cfg_.temp_onset_cpu_c ? snap.cpu_temp_c - cfg_.temp_onset_cpu_c : 0.0) +
//                          (snap.gpu_temp_c > cfg_.temp_onset_gpu_c ? snap.gpu_temp_c - cfg_.temp_onset_gpu_c : 0.0);
//         double latency = snap.avg_latency_ms;

//         // In HIGH_PERFORMANCE, maximize actual predicted throughput against a
//         // fixed reference. Other regimes preserve the legacy target-matching term.
//         objs[0] = highPerformanceRegime ? -directThroughput : -fpsScore;           // Maximize FPS // Objective 1: FPS (minimize negative FPS = maximize FPS)
//         objs[1] = energy / 10.0;       // Minimize energy // Objective 2: Energy/Power (minimize)
//         objs[2] = tempPen / 20.0;      // Minimize temperature  // Objective 3: Temperature penalty (minimize)
//         objs[3] = latency / 50.0;      // Minimize latency  // Objective 4: Latency (minimize)  // normalize
//         return objs;
//     }

// // ===================================================================
// // NSGA-II Core
// // ===================================================================
//     void nonDominatedSortAndCrowding();
//     void calculateCrowdingDistance(std::vector<int>& front);
//     const ParetoIndividual& tournamentSelect(int tournamentSize);
    
//     // Scalarized fitness: FPS=50%, Power=20%, Temp=20%, Latency=10% (PhD dual evaluation)
//     double calculateScalarizedFitness(const std::vector<double>& objs,
//                                                      const hrl::MetricsSnapshot& snap,
//                                                      const RLAction& action,
//                                                      int age,
//                                                      ParetoIndividual* auditOut) const ;

//     // ===================================================================
//     // Evolution Operators
//     // ===================================================================
//     ParetoIndividual crossover(const ParetoIndividual& p1, const ParetoIndividual& p2);
//     //void mutate(ParetoIndividual& ind);
//     void mutate(ParetoIndividual& ind, const hrl::MetricsSnapshot& snap);

//     std::vector<ParetoIndividual> selectEliteAndOffspring();

//     // [FINAL FIX D1] Hard population invariant: retain at least one
//     // MAX_PERFORMANCE, one BALANCED and one LOW_POWER genome.
//     void ensureModeDiversity(std::vector<ParetoIndividual>& pop);

//     void reducePopulation();
//     void injectDiversityIfNeeded();

//     // ===================================================================
//     // Runtime & Logging
//     // ===================================================================
//     // void applyBestParetoToRuntime(const hrl::MetricsSnapshot& snapshot);
//     // void seedRLReplayBuffer(const hrl::MetricsSnapshot& snapshot);
//     // void logParetoStats();
//     // void logDominanceAnalysis();

//     // bool isSnapshotStable(const hrl::MetricsSnapshot& snap);
//     // void initializePopulation();

//     // ===================================================================
//     // Runtime & Logging
//     // ===================================================================
//     void applyBestParetoToRuntime(const hrl::MetricsSnapshot& snapshot);

//     const ParetoIndividual* selectBestEvaluatedPareto() const;

//     double applySelectedParetoToRuntime(const ParetoIndividual& selected,
//                                         const hrl::MetricsSnapshot& snapshot);

//     void exportParetoEvidence(const hrl::MetricsSnapshot& snap,
//                               const ParetoIndividual* selected);

//     void exportSelectedActionEvidence(const hrl::MetricsSnapshot& snap,
//                                       const ParetoIndividual& selected);

//     void ensureEvidenceFiles();
//     static bool ensureParentDirectoryForFile(const std::string& filePath);

//     int countRank0() const;

//     void simulateRuntimeControlsFromAction(const RLAction& action,
//                                            RuntimeControls& simControls) const;

//     void seedRLReplayBuffer(const hrl::MetricsSnapshot& snapshot);
//     void logParetoStats();
//     void logDominanceAnalysis();

//     bool isSnapshotStable(const hrl::MetricsSnapshot& snap);
//     void initializePopulation();

//     // [Adaptive mutation] Update currentMutationRate_ from best-fitness trend.
//     // Pure rate control ? does not alter selection, so it is Pareto-safe.
//     double updateAdaptiveMutationRate(double currentBestFitness);
// };

// // ===================================================================
// // Implementations (add to .cpp or keep inline)
// // ===================================================================
// // ... (nonDominatedSortAndCrowding, calculateCrowdingDistance, tournamentSelect remain as in your GA_03/04)

// // Crossover & Mutate
// //ParetoIndividual GeneticAlgorithm::crossover(const ParetoIndividual& p1, const ParetoIndividual& p2) { /* same as before */ }


// // ====================== Inline Implementations ======================
// inline bool ParetoIndividual::dominates(const ParetoIndividual& other) const {
//     // [FINAL FIX F1/T1/H1] Deb-style constraint domination:
//     //   1) feasible always beats infeasible;
//     //   2) among infeasible candidates, smaller violation wins;
//     //   3) ties fall through to normal Pareto objective dominance.
//     if (feasible && !other.feasible) return true;
//     if (!feasible && other.feasible) return false;

//     if (!feasible && !other.feasible) {
//         constexpr double kConstraintEps = 1e-12;
//         if (constraintViolation + kConstraintEps < other.constraintViolation)
//             return true;
//         if (other.constraintViolation + kConstraintEps < constraintViolation)
//             return false;
//     }

//     if (objectives.size() != other.objectives.size()) return false;
//     bool strictlyBetter = false;
//     for (size_t i = 0; i < objectives.size(); ++i) {
//         if (objectives[i] > other.objectives[i]) return false;
//         if (objectives[i] < other.objectives[i]) strictlyBetter = true;
//     }
//     return strictlyBetter;
// }




//   // ===================================================================
//     // Evolution Operators (UPDATED for ParetoIndividual)
//     // ===================================================================
//     ParetoIndividual GeneticAlgorithm::crossover(const ParetoIndividual& p1, const ParetoIndividual& p2) {
//         ParetoIndividual child;
//         std::uniform_real_distribution<double> mix(0.0, 1.0);

//         child.genome.mode = mix(rng_) < 0.5 ? p1.genome.mode : p2.genome.mode;

//         if (p1.genome.target_fps.has && p2.genome.target_fps.has) {
//             double avg = (p1.genome.target_fps.value + p2.genome.target_fps.value) * 0.5;
//             child.genome.target_fps = MaybeDouble(utils::local_clamp(avg, cfg_.minTargetFps, cfg_.maxTargetFps));
//         }

//         if (p1.genome.prefer_gpu.has && p2.genome.prefer_gpu.has) {
//             child.genome.prefer_gpu = mix(rng_) < 0.6 ? p1.genome.prefer_gpu : p2.genome.prefer_gpu;
//         }

//         // [P0] Inherit the CPU frequency gene from one parent (discrete, so no averaging).
//         if (p1.genome.cpu_max_freq_khz.has || p2.genome.cpu_max_freq_khz.has) {
//             const RLAction& src = (mix(rng_) < 0.5 ? p1.genome : p2.genome);
//             child.genome.cpu_max_freq_khz = src.cpu_max_freq_khz.has
//                 ? src.cpu_max_freq_khz
//                 : (p1.genome.cpu_max_freq_khz.has ? p1.genome.cpu_max_freq_khz
//                                                   : p2.genome.cpu_max_freq_khz);
//         }

//         // [Micro-scheduler] Blend the continuous split gene. Unlike the discrete
//         // frequency gene, averaging is meaningful here and lets crossover explore
//         // intermediate load balances between two parents.
//         if (p1.genome.gpu_workload_split.has && p2.genome.gpu_workload_split.has) {
//             double avg = 0.5 * (p1.genome.gpu_workload_split.value +
//                                 p2.genome.gpu_workload_split.value);
//             child.genome.gpu_workload_split = MaybeDouble(utils::local_clamp(avg, 0.0, 1.0));
//         } else if (p1.genome.gpu_workload_split.has || p2.genome.gpu_workload_split.has) {
//             child.genome.gpu_workload_split = p1.genome.gpu_workload_split.has
//                 ? p1.genome.gpu_workload_split : p2.genome.gpu_workload_split;
//         }
//         canonicalizeGenomeForWorkload(child.genome);
//         return child;
//     }


// //void GeneticAlgorithm::mutate(ParetoIndividual& ind) { /* same as before */ }

//     void GeneticAlgorithm::mutate(ParetoIndividual& ind, const hrl::MetricsSnapshot& snap) {
//         (void)snap;
//         std::uniform_real_distribution<double> dist(0.0, 1.0);
//         if (dist(rng_) >= currentMutationRate_) return;

//         if (ind.genome.target_fps.has) {
//             std::normal_distribution<double> fpsMut(0.0, 4.0);
//             double newFps = ind.genome.target_fps.value + fpsMut(rng_);
//             ind.genome.target_fps = MaybeDouble(utils::local_clamp(newFps, cfg_.minTargetFps, cfg_.maxTargetFps));
//         }

//         if (ind.genome.prefer_gpu.has && dist(rng_) < 0.25) {
//             ind.genome.prefer_gpu = MaybeBool(!ind.genome.prefer_gpu.value);
//         }

//         // [Micro-scheduler] Mutate the continuous GPU workload fraction by a small
//         // Gaussian perturbation, clamped to [0,1]. This is the fine-grained load-
//         // balancing search that a binary toggle cannot express.
//         if (ind.genome.gpu_workload_split.has) {
//             std::normal_distribution<double> splitMut(0.0, 0.15);
//             double s = ind.genome.gpu_workload_split.value + splitMut(rng_);
//             ind.genome.gpu_workload_split = MaybeDouble(utils::local_clamp(s, 0.0, 1.0));
//         }

//         // [P0] Mutate the CPU frequency gene by stepping to a neighbouring DVFS level.
//         if (ind.genome.cpu_max_freq_khz.has) {
//             static const long kFreqSteps[] = {102000, 460800, 921600, 1190400, 1479000};
//             const int n = 5;
//             // Find nearest current index
//             long cur = static_cast<long>(ind.genome.cpu_max_freq_khz.value);
//             int idx = 0; long best = std::abs(kFreqSteps[0] - cur);
//             for (int k = 1; k < n; ++k) {
//                 long d = std::abs(kFreqSteps[k] - cur);
//                 if (d < best) { best = d; idx = k; }
//             }
//             int step = (dist(rng_) < 0.5) ? -1 : 1;
//             idx = utils::local_clamp(idx + step, 0, n - 1);
//             ind.genome.cpu_max_freq_khz = MaybeDouble(static_cast<double>(kFreqSteps[idx]));
//         }

//         if (dist(rng_) < 0.07) {
//             std::uniform_int_distribution<int> modeDist(1, 3);
//             ind.genome.mode = static_cast<PolicyMode>(modeDist(rng_));
//         }
//         canonicalizeGenomeForWorkload(ind.genome);
//     }




// // selectEliteAndOffspring, reducePopulation, injectDiversityIfNeeded, applyBestParetoToRuntime, etc. ? use the clean versions from GA_04



//     // ===================================================================
//     // Selection + Population Management
//     // ===================================================================
//     std::vector<ParetoIndividual> GeneticAlgorithm::selectEliteAndOffspring() {
//         std::vector<ParetoIndividual> newPop;

//         // [P0-F16] Defensive: tournamentSelect() draws indices in
//         // [0, population_.size()-1]; on an empty population that distribution
//         // is degenerate and indexing is UB. Self-heal instead of crashing.
//         if (population_.empty()) {
//             spdlog::error("[GA-Pareto] Population empty at selection - reinitializing");
//             initializePopulation();
//         }

//         // Elitism (population_ is sorted by rank/crowding - see [P0-F16])
//         for (size_t i = 0; i < std::min(cfg_.eliteCount, population_.size()); ++i) {
//             newPop.push_back(population_[i]);
//         }

//         // Offspring
//         while (newPop.size() < cfg_.popSize) {
//             const auto& p1 = tournamentSelect(3);
//             const auto& p2 = tournamentSelect(3);
//             auto child = crossover(p1, p2);
//             mutate(child, hrl::MetricsSnapshot{});   // snapshot not used in mutate anymore
//             newPop.push_back(child);
//         }
//         return newPop;
//     }


//     // ===================================================================
//     // [FINAL FIX D1] Mode-diversity floor
//     // ===================================================================
//     // The Aug-19 HistogramEqualization trace showed long 8/8 BALANCED and
//     // 8/8 MAX monocultures.  Mutation is too weak to guarantee recovery after
//     // a mode goes extinct, so diversity is enforced as a population invariant.
//     // This does NOT bypass feasibility: an unsafe representative can remain in
//     // the population while constraint-domination prevents its selection.
//     void GeneticAlgorithm::ensureModeDiversity(
//         std::vector<ParetoIndividual>& pop)
//     {
//         if (!cfg_.enforceModeDiversity || pop.size() < 3) return;

//         auto countMode = [&pop](PolicyMode mode) -> size_t {
//             return static_cast<size_t>(std::count_if(
//                 pop.begin(), pop.end(),
//                 [mode](const ParetoIndividual& x) {
//                     return x.genome.mode == mode;
//                 }));
//         };

//         const std::array<PolicyMode, 3> required{{
//             PolicyMode::MAX_PERFORMANCE,
//             PolicyMode::BALANCED,
//             PolicyMode::LOW_POWER
//         }};

//         for (PolicyMode missing : required) {
//             if (countMode(missing) > 0) continue;

//             // Replace a member belonging to a donor mode that still has >1
//             // representative, so fixing one missing mode cannot immediately
//             // delete another required mode.
//             size_t replaceIndex = pop.size() - 1;
//             bool donorFound = false;
//             for (size_t i = pop.size(); i-- > 0;) {
//                 if (countMode(pop[i].genome.mode) > 1) {
//                     replaceIndex = i;
//                     donorFound = true;
//                     break;
//                 }
//             }
//             if (!donorFound) replaceIndex = pop.size() - 1;

//             ParetoIndividual candidate = pop[replaceIndex];
//             candidate.genome.mode = missing;

//             // Give each injected representative a semantically valid clock
//             // anchor.  Other genes remain inherited to avoid resetting the
//             // entire search trajectory.
//             if (missing == PolicyMode::MAX_PERFORMANCE) {
//                 candidate.genome.target_fps =
//                     MaybeDouble(cfg_.performanceTargetFps);
//                 candidate.genome.cpu_max_freq_khz =
//                     MaybeDouble(1479000.0);
//             } else if (missing == PolicyMode::BALANCED) {
//                 candidate.genome.cpu_max_freq_khz =
//                     MaybeDouble(921600.0);
//             } else { // LOW_POWER
//                 candidate.genome.cpu_max_freq_khz =
//                     MaybeDouble(460800.0);
//             }

//             canonicalizeGenomeForWorkload(candidate.genome);

//             // This is a new, unevaluated representative for the NEXT
//             // generation. Reset stale donor decision-state fields.
//             candidate.objectives.clear();
//             candidate.fitness = -std::numeric_limits<double>::max();
//             candidate.rank = 0;
//             candidate.crowdingDistance = 0.0;
//             candidate.feasible = true;
//             candidate.constraintViolation = 0.0;
//             candidate.age = 0;
//             candidate.surrogateUsed = false;
//             candidate.surrogatePrediction = SurrogatePrediction{};
//             candidate.surrogateExplorationBonus = 0.0;
//             candidate.surrogateStallPenalty = 0.0;
//             candidate.fitFinal = -std::numeric_limits<double>::max();

//             pop[replaceIndex] = std::move(candidate);
//         }
//     }

//     void GeneticAlgorithm::reducePopulation() {
//         if (generation_ < cfg_.reductionStartGen) return;

//         // [P0-F16] population_ is sorted by (rank, crowding) at the end of
//         // nonDominatedSortAndCrowding(), so truncation keeps the best
//         // individuals with no duplicates. The old index-based fill
//         // (population_[survivors.size()]) could re-copy rank-0 members that
//         // were already in `survivors`, because rank-0 individuals were
//         // scattered through an unsorted array. Semantics preserved: keep at
//         // least minPopSize, and keep the whole rank-0 front even when it
//         // exceeds the reduction target.
//         size_t rank0 = 0;
//         for (const auto& ind : population_) {
//             if (ind.rank == 0) ++rank0;
//         }

//         size_t target = std::max(cfg_.minPopSize,
//                                  static_cast<size_t>(population_.size() * cfg_.reductionRatio));
//         target = std::max(target, rank0);

//         if (population_.size() > target) {
//             population_.resize(target);
//         }

//         spdlog::info("[GA-Pareto] Dynamic reduction -> size {} (gen {})", population_.size(), generation_);
//     }

//     void GeneticAlgorithm::injectDiversityIfNeeded() {
//         if (generation_ % 5 == 0 && population_.size() < cfg_.popSize) {
//             spdlog::debug("[GA-Pareto] Injecting diversity");
//             // Simple random injection (same logic as initializePopulation)
//             ParetoIndividual randomInd;
//             std::uniform_real_distribution<double> fpsDist(cfg_.minTargetFps, cfg_.maxTargetFps);
//             std::uniform_int_distribution<int> modeDist(1, 3);
//             std::bernoulli_distribution gpuBias(0.65);

//             randomInd.genome.mode = static_cast<PolicyMode>(modeDist(rng_));
//             randomInd.genome.target_fps = MaybeDouble(fpsDist(rng_));
//             randomInd.genome.prefer_gpu = MaybeBool(gpuBias(rng_));
//             canonicalizeGenomeForWorkload(randomInd.genome);
//             population_.push_back(randomInd);
//         }
//     }

//     // ===================================================================
//     // Runtime Application & Logging (PhD helpers)
//     // ===================================================================
//     // ===================================================================
//     // Runtime & Logging
//     // ===================================================================
//    // void applyBestParetoToRuntime(const hrl::MetricsSnapshot& snapshot);
//     //void seedRLReplayBuffer(const hrl::MetricsSnapshot& snapshot);
//     //void logParetoStats();
//     //void logDominanceAnalysis();

//     //bool isSnapshotStable(const hrl::MetricsSnapshot& snap);
//     // void initializePopulation();
//     // ===================================================================

//     // void hrl::GeneticAlgorithm::applyBestParetoToRuntime(const hrl::MetricsSnapshot& snapshot) {
//     //     if (population_.empty() || !runtimeControls_) return;
//     //     const auto& best = population_[0];
//     //     if (scheduler_) scheduler_->apply(best.genome, snapshot, *runtimeControls_);

//     //     spdlog::debug("[GeneticAlgorithm] Applied best: mode={}, target_fps={:.1f}, gpu={}",
//     //                   static_cast<int>(best.genome.mode),
//     //                   best.genome.target_fps.has ? best.genome.target_fps.value : 0.0,
//     //                   best.genome.prefer_gpu.has ? (best.genome.prefer_gpu.value ? "YES" : "NO") : "AUTO");
//     // }

// // ===================================================================
// // applyBestParetoToRuntime: apply Rank-0 individual with best fitness
// // ===================================================================
// void hrl::GeneticAlgorithm::applyBestParetoToRuntime(const hrl::MetricsSnapshot& snapshot) {
//     if (population_.empty() || !runtimeControls_) return;

//     // Select Rank-0 individual with highest scalarized fitness
//     const ParetoIndividual* best = nullptr;
//     for (const auto& ind : population_) {
//         if (ind.rank == 0) {
//             if (!best || ind.fitness > best->fitness) best = &ind;
//         }
//     }
//     if (!best) {
//         spdlog::warn("[GeneticAlgorithm] No Rank-0 solutions found ? falling back to population[0]");
//         best = &population_[0];  // fallback to first
//     }

//     if (scheduler_) scheduler_->apply(best->genome, snapshot, *runtimeControls_);
    

//     spdlog::debug("[GeneticAlgorithm] Applied Rank-0 best: mode={}, fps={:.1f}, gpu={}, fitness={:.4f}",
//                   static_cast<int>(best->genome.mode),
//                   best->genome.target_fps.has ? best->genome.target_fps.value : 0.0,
//                   best->genome.prefer_gpu.has ? (best->genome.prefer_gpu.value ? "YES" : "NO") : "AUTO",
//                   best->fitness);
// }

// // ===================================================================
// // seedRLReplayBuffer: push Rank-0 solutions as RL experience (ERL)
// // ===================================================================
// void GeneticAlgorithm::seedRLReplayBuffer(const hrl::MetricsSnapshot& snapshot) {
//     // ERL hybridization: Rank-0 genomes serve as high-quality seeds for RL
//     // In a full ERL implementation these would be pushed to a shared replay buffer.
//     int rank0Count = 0;
//     for (const auto& ind : population_) {
//         if (ind.rank == 0) ++rank0Count;
//     }
//     spdlog::debug("[GeneticAlgorithm] ERL: {} Rank-0 solutions available for RL seeding (gen {})",
//                   rank0Count, generation_);
//     (void)snapshot;  // suppress unused warning until RL buffer is integrated
// }



// // ===================================================================
// // logParetoStats: generation summary for PhD documentation
// // ===================================================================
// void GeneticAlgorithm::logParetoStats() {
//     if (population_.empty()) return;

//     int rank0Count = 0;
//     double bestFitness = -std::numeric_limits<double>::max();
//     double avgFitness  = 0.0;

//     for (const auto& ind : population_) {
//         if (ind.rank == 0) ++rank0Count;
//         if (ind.fitness > bestFitness) bestFitness = ind.fitness;
//         avgFitness += ind.fitness;
//     }
//     avgFitness /= static_cast<double>(population_.size());

//     spdlog::info("[GA-Pareto] Gen {} | Pop={} | Rank-0={} | BestFit={:.4f} | AvgFit={:.4f}",
//                  generation_, population_.size(), rank0Count, bestFitness, avgFitness);
// }



//     bool GeneticAlgorithm::isSnapshotStable(const hrl::MetricsSnapshot& snap) {
//         if (!snap.valid || snap.fps < cfg_.minFpsForEvolution) {
//             stableFrames_ = 0;
//             return false;
//         }

//         if (cfg_.requirePowerForEvolution && snap.avg_power_w_alg < cfg_.minPowerForEvolution) {
//             stableFrames_ = 0;
//             return false;
//         }

//         stableFrames_++;
//         return stableFrames_ >= std::max(1, cfg_.stableFramesRequired);
//     }

//     void GeneticAlgorithm::initializePopulation() {
//         population_.resize(cfg_.popSize);
//         std::uniform_real_distribution<double> fpsDist(cfg_.minTargetFps, cfg_.maxTargetFps);
//         std::uniform_int_distribution<int> modeDist(1, 3);
//         std::bernoulli_distribution gpuBias(0.65);

//         // [P0] Discrete Jetson Nano CPU DVFS steps (kHz). Seeding the frequency
//         // gene across these lets the Pareto front span the full clock range
//         // instead of collapsing onto the 102 MHz floor via PolicyMode alone.
//         static const long kFreqSteps[] = {102000, 460800, 921600, 1190400, 1479000};
//         std::uniform_int_distribution<int> freqIdx(0, 4);
//         std::uniform_real_distribution<double> splitDist(0.0, 1.0);

//         for (size_t i = 0; i < cfg_.popSize; ++i) {
//             auto& ind = population_[i];

//             // [FINAL FIX D1] Seed generation zero with all three macro modes
//             // represented. Remaining individuals are random as before.
//             if (cfg_.enforceModeDiversity && cfg_.popSize >= 3 && i < 3) {
//                 static const PolicyMode kSeedModes[3] = {
//                     PolicyMode::MAX_PERFORMANCE,
//                     PolicyMode::BALANCED,
//                     PolicyMode::LOW_POWER
//                 };
//                 ind.genome.mode = kSeedModes[i];
//             } else {
//                 ind.genome.mode = static_cast<PolicyMode>(modeDist(rng_));
//             }
//             ind.genome.target_fps = MaybeDouble(fpsDist(rng_));
//             ind.genome.prefer_gpu = MaybeBool(gpuBias(rng_));
//             ind.genome.power_budget_watts = MaybeDouble(3.0 + rng_() % 5);
//             ind.genome.cpu_max_freq_khz =
//                 MaybeDouble(static_cast<double>(kFreqSteps[freqIdx(rng_)]));
//             // [Micro-scheduler] Continuous GPU workload fraction, uniform in [0,1].
//             ind.genome.gpu_workload_split = MaybeDouble(splitDist(rng_));
//             canonicalizeGenomeForWorkload(ind.genome);
//         }
//         spdlog::info("[GeneticAlgorithm] Initialized population of size {} "
//                      "(target_fps range {:.0f}-{:.0f}, freq gene enabled)",
//                      cfg_.popSize, cfg_.minTargetFps, cfg_.maxTargetFps);
//     }

//     // ===================================================================
//     // [Adaptive mutation] Plateau-driven mutation-rate control.
//     // Ramps mutation up when best fitness stagnates, eases it down when
//     // fitness improves. Operates only on the rate ? selection is untouched,
//     // so this is safe to run alongside NSGA-II non-dominated sorting.
//     // Directly targets the observed failure mode (best fitness drifting
//     // negative after ~gen 500 with no escape mechanism).
//     // ===================================================================
//     double GeneticAlgorithm::updateAdaptiveMutationRate(double currentBestFitness) {
//         if (!cfg_.adaptiveMutationEnabled) {
//             currentMutationRate_ = cfg_.mutationRate;
//             return currentMutationRate_;
//         }

//         // First call: just seed the baseline, no adjustment yet.
//         if (prevBestFitness_ == -std::numeric_limits<double>::max()) {
//             prevBestFitness_ = currentBestFitness;
//             currentMutationRate_ = cfg_.mutationRate;
//             return currentMutationRate_;
//         }

//         const double improvement = currentBestFitness - prevBestFitness_;
//         prevBestFitness_ = currentBestFitness;

//         // Relative threshold scaled to the fitness magnitude, with an absolute
//         // floor so near-zero fitness doesn't make the threshold collapse to 0.
//         const double scale = std::max(std::abs(currentBestFitness),
//                                       cfg_.mutationPlateauAbsFloor);
//         const double improveThreshold = cfg_.mutationPlateauRelThreshold * scale;

//         if (improvement > improveThreshold) {
//             // Improving -> exploit: ease mutation back toward (below) base.
//             mutationPlateauCounter_ = 0;
//             currentMutationRate_ = cfg_.mutationRate * cfg_.mutationImproveFactor;
//         } else {
//             // Stagnating -> explore: ramp mutation up, capped.
//             ++mutationPlateauCounter_;
//             const double factor = 1.0 + cfg_.mutationPlateauStep * mutationPlateauCounter_;
//             currentMutationRate_ = std::min(cfg_.mutationRate * factor,
//                                             cfg_.maxMutationRate);
//         }

//         // Guard rails
//         currentMutationRate_ = std::min(std::max(currentMutationRate_, 0.0),
//                                         cfg_.maxMutationRate);
//         return currentMutationRate_;
//     }

// // ===================================================================
// // IMPLEMENTATIONS (NSGA-II + Dominance)
// // ===================================================================
// //void GeneticAlgorithm::nonDominatedSortAndCrowding() { /* your existing correct implementation */ }
// //void GeneticAlgorithm::calculateCrowdingDistance(std::vector<int>& front) { /* your existing correct implementation */ }
// //const ParetoIndividual& GeneticAlgorithm::tournamentSelect(int tournamentSize) { /* your existing correct implementation */ }


// // IMPLEMENTATION DETAILS BELOW (can be moved to .cpp if desired)

// /*
// 1. Fast Non-Dominated Sort
// This function separates your population into "Fronts" (Rank 0 is the true Pareto front, Rank 1 is the next best, etc.).
//  It uses a helper lambda to determine if Individual A completely dominates Individual B.
// */
// void GeneticAlgorithm::nonDominatedSortAndCrowding() {
//     size_t n = population_.size();
    
//     // S[i] contains a list of indices of individuals that individual 'i' dominates
//     std::vector<std::vector<int>> S(n);
//     // n_dom[i] is the domination count: how many individuals dominate individual 'i'
//     std::vector<int> n_dom(n, 0);
    
//     std::vector<std::vector<int>> fronts;
//     std::vector<int> currentFront;

//     // Helper: A dominates B if A is <= B in all objectives AND A < B in at least one
//     auto dominates = [&](const ParetoIndividual& a, const ParetoIndividual& b) {
//         bool strictlyBetter = false;
//         for (size_t m = 0; m < a.objectives.size(); ++m) {
//             if (a.objectives[m] > b.objectives[m]) return false; // a is worse in at least one
//             if (a.objectives[m] < b.objectives[m]) strictlyBetter = true;
//         }
//         return strictlyBetter;
//     };

//     // 1. Calculate domination counts and build the first front
//     for (size_t p = 0; p < n; ++p) {
//         S[p].clear();
//         n_dom[p] = 0;
//         for (size_t q = 0; q < n; ++q) {
//             if (p == q) continue;
            
//             if (dominates(population_[p], population_[q])) {
//                 S[p].push_back(q);
//             } else if (dominates(population_[q], population_[p])) {
//                 n_dom[p]++;
//             }
//         }
        
//         // If nothing dominates p, it belongs to the first Pareto front (Rank 0)
//         if (n_dom[p] == 0) {
//             population_[p].rank = 0;
//             currentFront.push_back(p);
//         }
//     }

//     fronts.push_back(currentFront);

//     // 2. Build subsequent fronts
//     int i = 0;
//     while (!fronts[i].empty()) {
//         std::vector<int> nextFront;
//         for (int p : fronts[i]) {
//             for (int q : S[p]) {
//                 n_dom[q]--; // Since p was in the previous front, remove its domination effect
//                 if (n_dom[q] == 0) {
//                     population_[q].rank = i + 1;
//                     nextFront.push_back(q);
//                 }
//             }
//         }
//         i++;
//         if (!nextFront.empty()) {
//             fronts.push_back(nextFront);
//         } else {
//             break; // All individuals sorted
//         }
//     }

//     // 3. Assign Crowding Distance for each front independently
//     for (auto& front : fronts) {
//         if (front.empty()) continue;
//         calculateCrowdingDistance(front); // Call our helper
//     }

//     // [P0-F16] CRITICAL: order the population by the NSGA-II crowded
//     // comparison (rank asc, crowding desc - ParetoIndividual::operator<).
//     // selectEliteAndOffspring() takes the first eliteCount entries as
//     // "elites" and reducePopulation() truncates from the back; both silently
//     // assumed this ordering. Without it, elites were arbitrary individuals
//     // and reduction could duplicate rank-0 members via index-based fill.
//     // (The per-front index vectors above are not used after this point, so
//     // invalidating them by reordering population_ is safe.)
//     std::sort(population_.begin(), population_.end());
// }

// /*
// 2. Crowding Distance Calculation
// I separated this into a helper function calculateCrowdingDistance(std::vector<int>& front). You should declare this as a private function in your header. 
// It ensures that boundary individuals (the absolute best at a specific objective) are always preserved.
// */

// // Add this declaration to your .h file:
// // void calculateCrowdingDistance(std::vector<int>& front);

// void GeneticAlgorithm::calculateCrowdingDistance(std::vector<int>& front) {
//     size_t l = front.size();
    
//     // Initialize crowding distance to 0 for everyone in the current front
//     for (int idx : front) {
//         population_[idx].crowdingDistance = 0.0;
//     }
    
//     // If 1 or 2 items, they are boundaries by default. Preserve them with infinity.
//     if (l <= 2) {
//         for (int idx : front) {
//             population_[idx].crowdingDistance = std::numeric_limits<double>::infinity();
//         }
//         return;
//     }

//     size_t numObjectives = population_[front[0]].objectives.size();
    
//     // Calculate density across each objective axis independently
//     for (size_t m = 0; m < numObjectives; ++m) {
//         // Sort the current front based solely on objective 'm'
//         std::sort(front.begin(), front.end(), [&](int a, int b) {
//             return population_[a].objectives[m] < population_[b].objectives[m];
//         });

//         // The extremes of the front get infinite distance (guaranteed survival)
//         population_[front[0]].crowdingDistance = std::numeric_limits<double>::infinity();
//         population_[front[l - 1]].crowdingDistance = std::numeric_limits<double>::infinity();

//         double objMin = population_[front[0]].objectives[m];
//         double objMax = population_[front[l - 1]].objectives[m];
//         double range = objMax - objMin;

//         if (range == 0.0) continue; // Prevent division by zero if all values are identical

//         // For all intermediate individuals, calculate normalized distance to neighbors
//         for (size_t j = 1; j < l - 1; ++j) {
//             if (population_[front[j]].crowdingDistance != std::numeric_limits<double>::infinity()) {
//                 double diff = population_[front[j + 1]].objectives[m] - population_[front[j - 1]].objectives[m];
//                 population_[front[j]].crowdingDistance += diff / range;
//             }
//         }
//     }
// }

// /*
// 3. NSGA-II Tournament Selection (Crowded-Comparison Operator)
// This selection logic acts on the operator< you already defined in ParetoIndividual, 
// ensuring the algorithm prefers lower ranks, but breaks ties by preferring higher crowding distances (exploring less-crowded areas of the Pareto front).
// */
// const ParetoIndividual& GeneticAlgorithm::tournamentSelect(int tournamentSize) {
//     std::uniform_int_distribution<size_t> dist(0, population_.size() - 1);
    
//     size_t bestIdx = dist(rng_);

//     for (int i = 1; i < tournamentSize; ++i) {
//         size_t contenderIdx = dist(rng_);
        
//         const auto& best = population_[bestIdx];
//         const auto& contender = population_[contenderIdx];

//         // NSGA-II Crowded-Comparison Operator:
//         // 1. Better Rank (Lower is better) wins.
//         if (contender.rank < best.rank) {
//             bestIdx = contenderIdx;
//         } 
//         // 2. If ranks are tied, pick the one in the less crowded region (Higher distance)
//         else if (contender.rank == best.rank) {
//             if (contender.crowdingDistance > best.crowdingDistance) {
//                 bestIdx = contenderIdx;
//             }
//         }
//     }
    
//     return population_[bestIdx];
// }

// // ===================================================================
// // Scalarized fitness: weighted combination of normalized objectives
// // Weights: FPS=50%, Power=20%, Temperature=20%, Latency=10%
// // ===================================================================
// double GeneticAlgorithm::calculateScalarizedFitness(const std::vector<double>& objs,
//                                                      const hrl::MetricsSnapshot& snap,
//                                                      const RLAction& action,
//                                                      int age,
//                                                      ParetoIndividual* auditOut) const {
//     if (objs.size() < 4) return -std::numeric_limits<double>::max();

//     // objs = [normFPS, normEnergy, normTemp, normLat] (all minimized).
//     // This exact authoritative weight vector is also persisted in the candidate.
//     std::array<double, 4> w = {cfg_.weight_fps, cfg_.weight_power,
//                                cfg_.weight_temp, cfg_.weight_latency};
//     if (weightManager_) {
//         w = weightManager_->getWeights();
//     }

//     const double fpsComponent   = -objs[0] * w[0];
//     const double powerComponent = -objs[1] * w[1];
//     const double tempComponent  = -objs[2] * w[2];
//     const double latComponent   = -objs[3] * w[3];

//     double gpuBonus = 0.0;
//     if (action.prefer_gpu.has && action.prefer_gpu.value &&
//         snap.gpu_util_avg > 15.0 && snap.gpu_util_avg < 85.0) {
//         gpuBonus = 0.05;
//     }

//     const double beforeAge = fpsComponent + powerComponent +
//                              tempComponent + latComponent + gpuBonus;
//     const double ageFactor = std::pow(0.985, static_cast<double>(age) / 50.0);
//     const double afterAge = beforeAge * ageFactor;

//     if (auditOut) {
//         auditOut->fitnessWeights = w;
//         auditOut->fitFpsComponent = fpsComponent;
//         auditOut->fitPowerComponent = powerComponent;
//         auditOut->fitTempComponent = tempComponent;
//         auditOut->fitLatencyComponent = latComponent;
//         auditOut->fitGpuBonus = gpuBonus;
//         auditOut->fitBeforeAge = beforeAge;
//         auditOut->fitAgeFactor = ageFactor;
//         auditOut->fitAfterAge = afterAge;
//         auditOut->fitFinal = afterAge; // exploration bonus is appended by caller
//     }

//     return afterAge;
// }


//     // double calculateScalarizedFitness(const hrl::MetricsSnapshot& snap,
//     //                                   const RLAction& action,
//     //                                   const RuntimeControls& /*controls*/) {
//     //     double f = 0.0;
//     //     if (action.target_fps.has) {
//     //         double err = std::abs(snap.fps - action.target_fps.value);
//     //         f += 0.50 * (1.0 / (1.0 + err));
//     //     }
//     //     f += -0.20 * snap.avg_power_w_alg;

//     //     if (snap.cpu_temp_c > 75.0) f += -2.0 * (snap.cpu_temp_c - 75.0);
//     //     if (snap.gpu_temp_c > 80.0) f += -2.0 * (snap.gpu_temp_c - 80.0);

//     //     f += -0.10 * snap.avg_latency_ms / 50.0;

//     //     if (action.prefer_gpu.has && action.prefer_gpu.value &&
//     //         snap.gpu_util_avg > 15.0 && snap.gpu_util_avg < 85.0) {
//     //         f += 0.20;
//     //     }

//     //     f *= std::pow(0.985, generation_ / 50.0);
//     //     return f;
//     // }


// //=======================================================================================================================================================================================
// void GeneticAlgorithm::logDominanceAnalysis() {
//     if (population_.size() < 2) return;
    
//     spdlog::info("\n===== PARETO FRONT ANALYSIS (Generation {}) =====", generation_);
    
//     std::vector<size_t> frontIndices;
//     for (size_t i = 0; i < population_.size(); ++i) {
//         if (population_[i].rank == 0) frontIndices.push_back(i);
//     }
    
//     for (size_t idx : frontIndices) {
//         const auto& ind = population_[idx];
//         spdlog::info("  [Rank-0] ID={} | Fitness={:.4f} | FPS Goal={:.1f} | Power={:.2f}W | "
//                      "Temp Penalty={:.2f} | Latency={:.2f}ms | Crowding Dist={:.4f}",
//                      idx, ind.fitness,
//                      ind.genome.target_fps.has ? ind.genome.target_fps.value : 0.0,
//                      ind.objectives[1] * 10.0,
//                      ind.objectives[2] * 20.0,
//                      ind.objectives[3] * 50.0,
//                      ind.crowdingDistance);
//     }
    
//     spdlog::info("\n  ===== DOMINANCE COMPARISON =====");
//     if (!frontIndices.empty() && population_.size() > frontIndices.size()) {
//         const auto& best = population_[frontIndices[0]];
//         for (size_t i = frontIndices.size(); i < std::min(size_t(3), population_.size()); ++i) {
//             const auto& challenger = population_[i];
//             spdlog::info("  [Rank-{}] vs [Rank-0]: {} dominates because:",
//                          challenger.rank, best.dominates(challenger) ? "Best" : "Trade-off");
            
//             const char* objName[] = {"FPS", "Energy", "Temperature", "Latency"};
//             for (size_t obj = 0; obj < best.objectives.size(); ++obj) {
//                 spdlog::info("    - {}: Best={:.3f} vs Challenger={:.3f} {}",
//                              objName[obj], best.objectives[obj], challenger.objectives[obj],
//                              best.objectives[obj] <= challenger.objectives[obj] ? "? Better" : "? Worse");
//             }
//         }
//     }
//     spdlog::info("\n");
// }

// // ===================================================================
// // Side-effect-free simulation of runtime controls for GA evaluation.
// // This avoids touching real sysfs/GPU/DVFS while evaluating candidates.
// // ===================================================================
// void GeneticAlgorithm::simulateRuntimeControlsFromAction(
//     const RLAction& action,
//     RuntimeControls& simControls) const
// {
//     int concurrency = 2;
//     bool enableGpu = true;
//     Affinity affinity = Affinity::Spread;

//     switch (action.mode) {
//         case PolicyMode::MAX_PERFORMANCE:
//             concurrency = 4;
//             enableGpu = true;
//             affinity = Affinity::Spread;
//             break;

//         case PolicyMode::LOW_POWER:
//             concurrency = 1;
//             enableGpu = false;
//             affinity = Affinity::Pack;
//             break;

//         case PolicyMode::BALANCED:
//             concurrency = 2;
//             enableGpu = action.prefer_gpu.has ? action.prefer_gpu.value : true;
//             affinity = Affinity::Spread;
//             break;

//         case PolicyMode::UNKNOWN:
//         default:
//             concurrency = 2;
//             enableGpu = true;
//             affinity = Affinity::Spread;
//             break;
//     }

//     if (action.prefer_gpu.has) {
//         enableGpu = action.prefer_gpu.value;
//     }

//     simControls.concurrency_level.store(concurrency, std::memory_order_relaxed);
//     simControls.enable_gpu.store(enableGpu, std::memory_order_relaxed);
//     simControls.affinity.store(affinity, std::memory_order_relaxed);
// }

// // ===================================================================
// // Select the best evaluated Rank-0 Pareto individual.
// // ===================================================================
// const ParetoIndividual* GeneticAlgorithm::selectBestEvaluatedPareto() const
// {
//     const ParetoIndividual* selected = nullptr;

//     for (const auto& ind : population_) {
//         if (ind.rank == 0) {
//             if (!selected || ind.fitness > selected->fitness) {
//                 selected = &ind;
//             }
//         }
//     }

//     if (!selected && !population_.empty()) {
//         selected = &population_[0];
//     }

//     return selected;
// }

// // ===================================================================
// // Apply selected evaluated Pareto individual to real runtime controls.
// // ===================================================================
// double GeneticAlgorithm::applySelectedParetoToRuntime(
//     const ParetoIndividual& selected,
//     const hrl::MetricsSnapshot& snapshot)
// {
//     if (!runtimeControls_) {
//         return 0.0;
//     }

//     // // [Surrogate] Remember what we are about to apply; next cycle's snapshot is
//     // // this action's measured outcome and will be fed to surrogate_.observe().
//     // lastAppliedGenome_ = selected.genome;
//     // hasLastApplied_    = true;

//     // if (scheduler_) {
//     //     scheduler_->apply(selected.genome, snapshot, *runtimeControls_);
//     // }

//     // [Surrogate] Remember what we are about to apply; next cycle's snapshot is
//     // this action's measured outcome and will be fed to surrogate_.observe().
//     lastAppliedGenome_ = selected.genome;
//     hasLastApplied_    = true;

//     // [PhD FIX] Diagnostic trace for HetGB continuous split gene crash
//     if (selected.genome.gpu_workload_split.has) {
//         double safe_split = utils::local_clamp(selected.genome.gpu_workload_split.value, 0.0, 1.0);
//         spdlog::info("[GA-Split-Trace] Dispatching continuous split gene: {:.3f}", safe_split);
//     }

//     double schedulerApplyMs = 0.0;
//     if (scheduler_) {
//         const auto schedulerStart = std::chrono::steady_clock::now();
//         scheduler_->apply(selected.genome, snapshot, *runtimeControls_);
//         const auto schedulerEnd = std::chrono::steady_clock::now();
//         schedulerApplyMs = std::chrono::duration<double, std::milli>(
//             schedulerEnd - schedulerStart).count();
//     }

//     // [PhD timing] Publish a 1-based action epoch only after all scheduler
//     // actuation has completed. AlgorithmConcrete timestamps the first frame
//     // that observes this epoch, yielding true action-to-effect response time.
//     const uint64_t applyEndNs = static_cast<uint64_t>(
//         std::chrono::duration_cast<std::chrono::nanoseconds>(
//             std::chrono::steady_clock::now().time_since_epoch()).count());
//     const uint64_t actionEpoch = static_cast<uint64_t>(generation_) + 1ULL;
//     runtimeControls_->action_apply_end_ns.store(applyEndNs, std::memory_order_relaxed);
//     runtimeControls_->action_generation.store(actionEpoch, std::memory_order_release);

//     spdlog::info(
//         "[ERL SELECTED] Gen={} | Rank={} | Fit={:.4f} | mode={} | targetFPS={:.1f} | gpu={} | "
//         "fps={:.2f} | power={:.3f}W | J/frame={:.6f} | latency={:.3f}ms",
//         generation_,
//         selected.rank,
//         selected.fitness,
//         static_cast<int>(selected.genome.mode),
//         selected.genome.target_fps.has ? selected.genome.target_fps.value : 0.0,
//         selected.genome.prefer_gpu.has
//             ? (selected.genome.prefer_gpu.value ? "YES" : "NO")
//             : "AUTO",
//         snapshot.fps,
//         snapshot.avg_power_w_alg,
//         snapshot.joulesPerFrame,
//         snapshot.avg_latency_ms
//     );

//     return schedulerApplyMs;
// }


// // ===================================================================
// // Ensure output directory and evidence CSV files exist immediately.
// // This creates headers even before the first successful ERL generation.
// // ===================================================================
// bool GeneticAlgorithm::ensureParentDirectoryForFile(const std::string& filePath)
// {
//     const std::size_t slash = filePath.find_last_of("/");
//     if (slash == std::string::npos) {
//         return true;
//     }

//     const std::string dir = filePath.substr(0, slash);
//     if (dir.empty()) {
//         return true;
//     }

//     std::string current;
//     for (char c : dir) {
//         current.push_back(c);
//         if (c == '/') {
//             if (current.size() > 1) {
//                 ::mkdir(current.c_str(), 0755);
//             }
//         }
//     }

//     if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
//         spdlog::warn("[GA-Pareto] Could not create directory '{}': {}", dir, std::strerror(errno));
//         return false;
//     }
//     return true;
// }

// void GeneticAlgorithm::ensureEvidenceFiles()
// {
//     // Telemetry schema v3 adds `feasible` and `constraint_violation` to both
//     // Pareto and selected-action evidence.  Bumping the row schema prevents
//     // downstream analysis scripts from silently interpreting the new columns
//     // as the older 91/67-column v2 layout.
//     std::lock_guard<std::mutex> guard(evidenceMutex_);

//     ensureParentDirectoryForFile(cfg_.pareto_csv_path);
//     if (!std::ifstream(cfg_.pareto_csv_path).good()) {
//         std::ofstream out(cfg_.pareto_csv_path, std::ios::out);
//         if (out.is_open()) {
//             out << "schema_version,generation,individual_id,rank,fitness,crowding_distance,obj_fps,obj_energy,obj_temp,obj_latency,target_fps_has,target_fps,power_budget_has,power_budget_watts,prefer_gpu_has,prefer_gpu,gpu_workload_split_has,gpu_workload_split,gpu_split_applicable,cpu_max_freq_has,cpu_max_freq_khz,policy_mode,awm_regime,awm_detected_regime,awm_w_fps,awm_w_power,awm_w_temp,awm_w_latency,awm_trigger_field,awm_trigger_value,awm_trigger_threshold,awm_regime_streak,awm_last_update_generation,surrogate_used,surrogate_source,surrogate_confidence,surrogate_from_data,surrogate_support_count,surrogate_neighbor_count,surrogate_nearest_distance,surrogate_exploration_bonus,surrogate_stall_penalty,surrogate_config_freq_khz,surrogate_config_gpu,surrogate_config_gpu_split,surrogate_config_concurrency,pred_fps,pred_power_w,pred_latency_ms,pred_temp_c,fit_fps_component,fit_power_component,fit_temp_component,fit_latency_component,fit_gpu_bonus,fit_before_age,fit_age_factor,fit_after_age,fit_final,feasible,constraint_violation,selected,measured_fps,measured_power_w,measured_joules_per_frame,algorithm_processing_ms,pipeline_e2e_ms,cpu_temp_c,gpu_temp_c\n";
//         } else {
//             spdlog::warn("[GA-Pareto] Could not create Pareto CSV: {}", cfg_.pareto_csv_path);
//         }
//     }

//     ensureParentDirectoryForFile(cfg_.action_csv_path);
//     if (!std::ifstream(cfg_.action_csv_path).good()) {
//         std::ofstream out(cfg_.action_csv_path, std::ios::out);
//         if (out.is_open()) {
//             out << "schema_version,generation,action_epoch,frame_id,rank0_count,selected_rank,selected_fitness,rank0_runnerup_fitness,fitness_margin,policy_mode,target_fps_has,target_fps,power_budget_has,power_budget_watts,prefer_gpu_has,prefer_gpu,gpu_workload_split_has,gpu_workload_split,gpu_split_applicable,cpu_max_freq_has,cpu_max_freq_gene_khz,cpu_max_freq_commanded_khz,runtime_enable_gpu,runtime_gpu_workload_split,runtime_concurrency,runtime_affinity,awm_regime,awm_detected_regime,awm_w_fps,awm_w_power,awm_w_temp,awm_w_latency,awm_trigger_field,awm_trigger_value,awm_trigger_threshold,awm_regime_streak,awm_last_update_generation,surrogate_used,surrogate_source,surrogate_confidence,surrogate_from_data,surrogate_support_count,surrogate_neighbor_count,surrogate_nearest_distance,surrogate_exploration_bonus,surrogate_stall_penalty,surrogate_config_freq_khz,surrogate_config_gpu,surrogate_config_gpu_split,surrogate_config_concurrency,pred_fps,pred_power_w,pred_latency_ms,pred_temp_c,obj_fps,obj_energy,obj_temp,obj_latency,fit_fps_component,fit_power_component,fit_temp_component,fit_latency_component,fit_gpu_bonus,fit_before_age,fit_age_factor,fit_after_age,fit_final,feasible,constraint_violation,measured_fps,measured_power_w,measured_joules_per_frame,algorithm_processing_ms,pipeline_e2e_ms,ga_compute_total_ms,ga_weight_update_ms,ga_evaluation_ms,ga_nsga_sort_ms,ga_selection_ms,ga_scheduler_apply_ms,ga_action_dispatch_overhead_ms,ga_pareto_export_ms,ga_diagnostics_ms,ga_mutation_ms,ga_offspring_ms,ga_postprocess_ms,action_apply_end_ns,algorithm_observed_epoch,algorithm_observed_time_ns,action_response_epoch,action_response_ms,cpu_temp_c,gpu_temp_c\n";
//         } else {
//             spdlog::warn("[GA-Pareto] Could not create action CSV: {}", cfg_.action_csv_path);
//         }
//     }
// }

// // ===================================================================
// // Count Rank-0 solutions.
// // ===================================================================
// int GeneticAlgorithm::countRank0() const
// {
//     int count = 0;

//     for (const auto& ind : population_) {
//         if (ind.rank == 0) {
//             ++count;
//         }
//     }

//     return count;
// }

// // ===================================================================
// // Export full Pareto front evidence.
// // ===================================================================
// void GeneticAlgorithm::exportParetoEvidence(
//     const hrl::MetricsSnapshot& snap,
//     const ParetoIndividual* selected)
// {
//     if (!cfg_.export_pareto_evidence) return;

//     std::lock_guard<std::mutex> guard(evidenceMutex_);
//     ensureParentDirectoryForFile(cfg_.pareto_csv_path);
//     const bool writeHeader = !std::ifstream(cfg_.pareto_csv_path).good();
//     std::ofstream out(cfg_.pareto_csv_path, std::ios::app);
//     if (!out.is_open()) {
//         spdlog::warn("[GA-Pareto] Could not open Pareto CSV: {}", cfg_.pareto_csv_path);
//         return;
//     }
//     if (writeHeader) out << "schema_version,generation,individual_id,rank,fitness,crowding_distance,obj_fps,obj_energy,obj_temp,obj_latency,target_fps_has,target_fps,power_budget_has,power_budget_watts,prefer_gpu_has,prefer_gpu,gpu_workload_split_has,gpu_workload_split,gpu_split_applicable,cpu_max_freq_has,cpu_max_freq_khz,policy_mode,awm_regime,awm_detected_regime,awm_w_fps,awm_w_power,awm_w_temp,awm_w_latency,awm_trigger_field,awm_trigger_value,awm_trigger_threshold,awm_regime_streak,awm_last_update_generation,surrogate_used,surrogate_source,surrogate_confidence,surrogate_from_data,surrogate_support_count,surrogate_neighbor_count,surrogate_nearest_distance,surrogate_exploration_bonus,surrogate_stall_penalty,surrogate_config_freq_khz,surrogate_config_gpu,surrogate_config_gpu_split,surrogate_config_concurrency,pred_fps,pred_power_w,pred_latency_ms,pred_temp_c,fit_fps_component,fit_power_component,fit_temp_component,fit_latency_component,fit_gpu_bonus,fit_before_age,fit_age_factor,fit_after_age,fit_final,feasible,constraint_violation,selected,measured_fps,measured_power_w,measured_joules_per_frame,algorithm_processing_ms,pipeline_e2e_ms,cpu_temp_c,gpu_temp_c\n";

//     const std::string awmRegime = weightManager_ ? weightManager_->getCurrentRegimeName() : "FIXED_WEIGHTS";
//     const std::string awmDetected = weightManager_ ? weightManager_->getLastDetectedRegimeName() : "FIXED_WEIGHTS";
//     const std::string awmTriggerField = weightManager_ ? weightManager_->getLastTriggerField() : "none";
//     const double awmTriggerValue = weightManager_ ? weightManager_->getLastTriggerValue() : 0.0;
//     const double awmTriggerThreshold = weightManager_ ? weightManager_->getLastTriggerThreshold() : 0.0;
//     const int awmStreak = weightManager_ ? weightManager_->getRegimeStreak() : 0;
//     const int awmLastUpdate = weightManager_ ? weightManager_->getLastUpdateGeneration() : -1;

//     for (size_t i = 0; i < population_.size(); ++i) {
//         const auto& ind = population_[i];
//         const bool isSelected = selected && (&ind == selected);

//         const double targetFps = ind.genome.target_fps.has ? ind.genome.target_fps.value : 0.0;
//         const double powerBudget = ind.genome.power_budget_watts.has ? ind.genome.power_budget_watts.value : 0.0;
//         const int preferGpu = ind.genome.prefer_gpu.has ? (ind.genome.prefer_gpu.value ? 1 : 0) : -1;
//         const double splitGene = ind.genome.gpu_workload_split.has ? ind.genome.gpu_workload_split.value : 0.0;
//         const double cpuFreqGene = ind.genome.cpu_max_freq_khz.has ? ind.genome.cpu_max_freq_khz.value : 0.0;

//         out << 3 << ","
//             << generation_ << "," << i << "," << ind.rank << ","
//             << ind.fitness << "," << ind.crowdingDistance << ",";

//         if (ind.objectives.size() >= 4) {
//             out << ind.objectives[0] << "," << ind.objectives[1] << ","
//                 << ind.objectives[2] << "," << ind.objectives[3] << ",";
//         } else {
//             out << "0,0,0,0,";
//         }

//         out << (ind.genome.target_fps.has ? 1 : 0) << "," << targetFps << ","
//             << (ind.genome.power_budget_watts.has ? 1 : 0) << "," << powerBudget << ","
//             << (ind.genome.prefer_gpu.has ? 1 : 0) << "," << preferGpu << ","
//             << (ind.genome.gpu_workload_split.has ? 1 : 0) << "," << splitGene << ","
//             << (cfg_.gpuSplitApplicable ? 1 : 0) << ","
//             << (ind.genome.cpu_max_freq_khz.has ? 1 : 0) << "," << cpuFreqGene << ","
//             << static_cast<int>(ind.genome.mode) << ","
//             << awmRegime << "," << awmDetected << ","
//             << ind.fitnessWeights[0] << "," << ind.fitnessWeights[1] << ","
//             << ind.fitnessWeights[2] << "," << ind.fitnessWeights[3] << ","
//             << awmTriggerField << "," << awmTriggerValue << "," << awmTriggerThreshold << ","
//             << awmStreak << "," << awmLastUpdate << ","
//             << (ind.surrogateUsed ? 1 : 0) << ","
//             << SurrogateModel::sourceName(ind.surrogatePrediction.source) << ","
//             << ind.surrogatePrediction.confidence << ","
//             << (ind.surrogatePrediction.from_data ? 1 : 0) << ","
//             << ind.surrogatePrediction.support_count << ","
//             << ind.surrogatePrediction.neighbor_count << ","
//             << ind.surrogatePrediction.nearest_distance << ","
//             << ind.surrogateExplorationBonus << ","
//             << ind.surrogateStallPenalty << ","
//             << ind.surrogateConfigFreqKhz << ","
//             << (ind.surrogateConfigGpu ? 1 : 0) << ","
//             << ind.surrogateConfigGpuSplit << ","
//             << ind.surrogateConfigConcurrency << ","
//             << ind.surrogatePrediction.fps << ","
//             << ind.surrogatePrediction.power_w << ","
//             << ind.surrogatePrediction.latency_ms << ","
//             << ind.surrogatePrediction.temp_c << ","
//             << ind.fitFpsComponent << "," << ind.fitPowerComponent << ","
//             << ind.fitTempComponent << "," << ind.fitLatencyComponent << ","
//             << ind.fitGpuBonus << "," << ind.fitBeforeAge << ","
//             << ind.fitAgeFactor << "," << ind.fitAfterAge << "," << ind.fitFinal << ","
//             << (ind.feasible ? 1 : 0) << "," << ind.constraintViolation << ","
//             << (isSelected ? 1 : 0) << ","
//             << snap.fps << "," << snap.avg_power_w_alg << "," << snap.joulesPerFrame << ","
//             << snap.avg_inference_ms << "," << snap.end_to_end_latency_ms << ","
//             << snap.cpu_temp_c << "," << snap.gpu_temp_c << "\n";
//     }
// }

// // ===================================================================
// // Export selected action trace.
// // ===================================================================
// void GeneticAlgorithm::exportSelectedActionEvidence(
//     const hrl::MetricsSnapshot& snap,
//     const ParetoIndividual& selected)
// {
//     if (!cfg_.export_pareto_evidence) return;

//     std::lock_guard<std::mutex> guard(evidenceMutex_);
//     ensureParentDirectoryForFile(cfg_.action_csv_path);
//     const bool writeHeader = !std::ifstream(cfg_.action_csv_path).good();
//     std::ofstream out(cfg_.action_csv_path, std::ios::app);
//     if (!out.is_open()) {
//         spdlog::warn("[GA-Pareto] Could not open action CSV: {}", cfg_.action_csv_path);
//         return;
//     }
//     if (writeHeader) out << "schema_version,generation,action_epoch,frame_id,rank0_count,selected_rank,selected_fitness,rank0_runnerup_fitness,fitness_margin,policy_mode,target_fps_has,target_fps,power_budget_has,power_budget_watts,prefer_gpu_has,prefer_gpu,gpu_workload_split_has,gpu_workload_split,gpu_split_applicable,cpu_max_freq_has,cpu_max_freq_gene_khz,cpu_max_freq_commanded_khz,runtime_enable_gpu,runtime_gpu_workload_split,runtime_concurrency,runtime_affinity,awm_regime,awm_detected_regime,awm_w_fps,awm_w_power,awm_w_temp,awm_w_latency,awm_trigger_field,awm_trigger_value,awm_trigger_threshold,awm_regime_streak,awm_last_update_generation,surrogate_used,surrogate_source,surrogate_confidence,surrogate_from_data,surrogate_support_count,surrogate_neighbor_count,surrogate_nearest_distance,surrogate_exploration_bonus,surrogate_stall_penalty,surrogate_config_freq_khz,surrogate_config_gpu,surrogate_config_gpu_split,surrogate_config_concurrency,pred_fps,pred_power_w,pred_latency_ms,pred_temp_c,obj_fps,obj_energy,obj_temp,obj_latency,fit_fps_component,fit_power_component,fit_temp_component,fit_latency_component,fit_gpu_bonus,fit_before_age,fit_age_factor,fit_after_age,fit_final,feasible,constraint_violation,measured_fps,measured_power_w,measured_joules_per_frame,algorithm_processing_ms,pipeline_e2e_ms,ga_compute_total_ms,ga_weight_update_ms,ga_evaluation_ms,ga_nsga_sort_ms,ga_selection_ms,ga_scheduler_apply_ms,ga_action_dispatch_overhead_ms,ga_pareto_export_ms,ga_diagnostics_ms,ga_mutation_ms,ga_offspring_ms,ga_postprocess_ms,action_apply_end_ns,algorithm_observed_epoch,algorithm_observed_time_ns,action_response_epoch,action_response_ms,cpu_temp_c,gpu_temp_c\n";

//     const int rtGpu = runtimeControls_
//         ? (runtimeControls_->enable_gpu.load(std::memory_order_relaxed) ? 1 : 0) : -1;
//     const double rtSplit = runtimeControls_
//         ? runtimeControls_->gpu_workload_split.load(std::memory_order_relaxed) : -1.0;
//     const int rtConcurrency = runtimeControls_
//         ? runtimeControls_->concurrency_level.load(std::memory_order_relaxed) : -1;
//     const int rtAffinity = runtimeControls_
//         ? static_cast<int>(runtimeControls_->affinity.load(std::memory_order_relaxed)) : -1;
//     const long appliedCpuKhz = runtimeControls_
//         ? runtimeControls_->commanded_cpu_max_freq_khz.load(std::memory_order_relaxed) : 0L;

//     const uint64_t actionEpoch = runtimeControls_
//         ? runtimeControls_->action_generation.load(std::memory_order_acquire)
//         : static_cast<uint64_t>(generation_) + 1ULL;
//     const uint64_t applyEndNs = runtimeControls_
//         ? runtimeControls_->action_apply_end_ns.load(std::memory_order_acquire) : 0ULL;
//     const uint64_t observedEpoch = runtimeControls_
//         ? runtimeControls_->algorithm_observed_generation.load(std::memory_order_acquire) : 0ULL;
//     const uint64_t observedNs = runtimeControls_
//         ? runtimeControls_->algorithm_observed_time_ns.load(std::memory_order_acquire) : 0ULL;
//     const uint64_t responseEpoch = runtimeControls_
//         ? runtimeControls_->action_response_generation.load(std::memory_order_acquire) : 0ULL;
//     const uint64_t responseNs = runtimeControls_
//         ? runtimeControls_->action_response_latency_ns.load(std::memory_order_acquire) : 0ULL;
//     const double actionResponseMs = responseEpoch > 0
//         ? static_cast<double>(responseNs) / 1.0e6 : -1.0;

//     const std::string awmRegime = weightManager_ ? weightManager_->getCurrentRegimeName() : "FIXED_WEIGHTS";
//     const std::string awmDetected = weightManager_ ? weightManager_->getLastDetectedRegimeName() : "FIXED_WEIGHTS";
//     const std::string awmTriggerField = weightManager_ ? weightManager_->getLastTriggerField() : "none";
//     const double awmTriggerValue = weightManager_ ? weightManager_->getLastTriggerValue() : 0.0;
//     const double awmTriggerThreshold = weightManager_ ? weightManager_->getLastTriggerThreshold() : 0.0;
//     const int awmStreak = weightManager_ ? weightManager_->getRegimeStreak() : 0;
//     const int awmLastUpdate = weightManager_ ? weightManager_->getLastUpdateGeneration() : -1;

//     const double targetFps = selected.genome.target_fps.has ? selected.genome.target_fps.value : 0.0;
//     const double powerBudget = selected.genome.power_budget_watts.has ? selected.genome.power_budget_watts.value : 0.0;
//     const int preferGpu = selected.genome.prefer_gpu.has ? (selected.genome.prefer_gpu.value ? 1 : 0) : -1;
//     const double splitGene = selected.genome.gpu_workload_split.has ? selected.genome.gpu_workload_split.value : 0.0;
//     const double cpuFreqGene = selected.genome.cpu_max_freq_khz.has ? selected.genome.cpu_max_freq_khz.value : 0.0;

//     const double obj0 = selected.objectives.size() > 0 ? selected.objectives[0] : 0.0;
//     const double obj1 = selected.objectives.size() > 1 ? selected.objectives[1] : 0.0;
//     const double obj2 = selected.objectives.size() > 2 ? selected.objectives[2] : 0.0;
//     const double obj3 = selected.objectives.size() > 3 ? selected.objectives[3] : 0.0;

//     out << 3 << ","
//         << generation_ << "," << actionEpoch << "," << snap.frameId << ","
//         << lastEvaluatedRank0Count_ << "," << selected.rank << "," << selected.fitness << ","
//         << lastRank0RunnerUpFitness_ << "," << lastSelectionMargin_ << ","
//         << static_cast<int>(selected.genome.mode) << ","
//         << (selected.genome.target_fps.has ? 1 : 0) << "," << targetFps << ","
//         << (selected.genome.power_budget_watts.has ? 1 : 0) << "," << powerBudget << ","
//         << (selected.genome.prefer_gpu.has ? 1 : 0) << "," << preferGpu << ","
//         << (selected.genome.gpu_workload_split.has ? 1 : 0) << "," << splitGene << ","
//         << (cfg_.gpuSplitApplicable ? 1 : 0) << ","
//         << (selected.genome.cpu_max_freq_khz.has ? 1 : 0) << "," << cpuFreqGene << ","
//         << appliedCpuKhz << "," << rtGpu << "," << rtSplit << ","
//         << rtConcurrency << "," << rtAffinity << ","
//         << awmRegime << "," << awmDetected << ","
//         << selected.fitnessWeights[0] << "," << selected.fitnessWeights[1] << ","
//         << selected.fitnessWeights[2] << "," << selected.fitnessWeights[3] << ","
//         << awmTriggerField << "," << awmTriggerValue << "," << awmTriggerThreshold << ","
//         << awmStreak << "," << awmLastUpdate << ","
//         << (selected.surrogateUsed ? 1 : 0) << ","
//         << SurrogateModel::sourceName(selected.surrogatePrediction.source) << ","
//         << selected.surrogatePrediction.confidence << ","
//         << (selected.surrogatePrediction.from_data ? 1 : 0) << ","
//         << selected.surrogatePrediction.support_count << ","
//         << selected.surrogatePrediction.neighbor_count << ","
//         << selected.surrogatePrediction.nearest_distance << ","
//         << selected.surrogateExplorationBonus << ","
//         << selected.surrogateStallPenalty << ","
//         << selected.surrogateConfigFreqKhz << ","
//         << (selected.surrogateConfigGpu ? 1 : 0) << ","
//         << selected.surrogateConfigGpuSplit << ","
//         << selected.surrogateConfigConcurrency << ","
//         << selected.surrogatePrediction.fps << ","
//         << selected.surrogatePrediction.power_w << ","
//         << selected.surrogatePrediction.latency_ms << ","
//         << selected.surrogatePrediction.temp_c << ","
//         << obj0 << "," << obj1 << "," << obj2 << "," << obj3 << ","
//         << selected.fitFpsComponent << "," << selected.fitPowerComponent << ","
//         << selected.fitTempComponent << "," << selected.fitLatencyComponent << ","
//         << selected.fitGpuBonus << "," << selected.fitBeforeAge << ","
//         << selected.fitAgeFactor << "," << selected.fitAfterAge << "," << selected.fitFinal << ","
//         << (selected.feasible ? 1 : 0) << "," << selected.constraintViolation << ","
//         << snap.fps << "," << snap.avg_power_w_alg << "," << snap.joulesPerFrame << ","
//         << snap.avg_inference_ms << "," << snap.end_to_end_latency_ms << ","
//         << lastTiming_.total_compute_ms << "," << lastTiming_.weight_update_ms << ","
//         << lastTiming_.evaluation_ms << "," << lastTiming_.nsga_sort_ms << ","
//         << lastTiming_.selection_ms << "," << lastTiming_.scheduler_apply_ms << ","
//         << lastTiming_.action_dispatch_overhead_ms << "," << lastTiming_.pareto_export_ms << ","
//         << lastTiming_.diagnostics_ms << "," << lastTiming_.mutation_ms << ","
//         << lastTiming_.offspring_ms << "," << lastTiming_.postprocess_ms << ","
//         << applyEndNs << "," << observedEpoch << "," << observedNs << ","
//         << responseEpoch << "," << actionResponseMs << ","
//         << snap.cpu_temp_c << "," << snap.gpu_temp_c << "\n";
// }

// } // namespace hrl



// //================================================== GeneticAlgorithm_05.h ===========================================================
// // FINAL GOLD STANDARD ? PhD-Ready (Merged Best of GA_03 + GA_04)
// // Pareto + Scalar Hybrid | Clean | Complete | Jetson Nano Optimized
// // GeneticAlgorithm_05.h - FINAL GOLD STANDARD ? PhD-Ready
// //=====================================================================================================================================

// #pragma once

// #include <vector>
// #include <random>
// #include <algorithm>
// #include <limits>
// #include <memory>
// #include <cmath>
// #include <spdlog/spdlog.h>

// #include <array>
// #include <fstream>
// #include <iomanip>
// #include <mutex>
// #include <sstream>
// #include <cerrno>
// #include <cstring>
// #include <sys/stat.h>
// #include <sys/types.h>

// #include "RuntimeControls.h"
// #include "../Stage_01/SharedStructures/allModulesStatcs.h"
// #include "RuntimeControls.h"   // ? ADD THIS
// #include "IScheduler.h"
// #include "AdaptiveWeightManager.h"
// #include "SurrogateModel.h"   // online counterfactual fitness surrogate (thesis gap 2)

// #include "../Stage_01/Concretes/SystemMetricsAggregatorConcrete_v3_2.h"
// #include "../Stage_01/Others/utils.h"

// namespace hrl {

// // ===================================================================
// // GAConfig
// // ===================================================================
// struct GAConfig {
//     size_t   popSize             = 12;
//     size_t   minPopSize          = 6;
//     size_t   eliteCount          = 3;
//     double   crossoverRate       = 0.75;
//     double   mutationRate        = 0.18;   // Base / floor mutation rate
//     double   explorationDecay    = 0.96;
//     int      controlIntervalMs   = 800;
//     size_t   reductionStartGen   = 10;
//     double   reductionRatio      = 0.6;

//     // [Adaptive mutation] Plateau-driven mutation control (extracted from
//     // AdaptivePopulationManager, made Pareto-safe ? it scales the rate only,
//     // never touches selection, so it composes cleanly with NSGA-II).
//     // When best fitness improves, mutation eases toward exploit; when it
//     // plateaus, mutation ramps up toward maxMutationRate to escape stagnation.
//     bool   adaptiveMutationEnabled = true;
//     double maxMutationRate         = 0.35;   // Hard cap when fully plateaued
//     // Relative improvement threshold (fraction of |prev best fitness|) below
//     // which a generation counts as "stagnating". Scaled to the fitness signal
//     // (which sits in the ~±0.05?0.20 band) rather than the old absolute 0.01.
//     double mutationPlateauRelThreshold = 0.02;   // 2% relative improvement
//     double mutationPlateauAbsFloor     = 0.0005; // abs floor for near-zero fitness
//     double mutationImproveFactor       = 0.8;    // multiply base when improving
//     double mutationPlateauStep         = 0.10;   // +10% of base per plateau gen

//     // [Surrogate] Online counterfactual fitness (thesis gap 2). When enabled, all
//     // candidates are scored by a learned per-config surrogate instead of the shared
//     // measured snapshot, and under-explored configs receive an optimism bonus.
//     // Disable for the ablation baseline (surrogate-off vs surrogate-on).
//     bool   surrogateEnabled       = true;
//     double surrogateExploreWeight = 0.15;   // UCB bonus scale in fitness units

//     // [FIX P2] Reality gating for counterfactual fitness.
//     int    surrogateMinSupport      = 3;    // below this support, predictions are pessimised
//     double surrogateColdFpsCeiling  = 8.0;  // fps ceiling before any real measurement exists
//     double surrogatePredFpsCapRatio = 1.2;  // pred_fps <= ratio * best measured fps this run
//     // [FIX P7] Physics-prior fps ceiling, per workload (was hard-coded 60 for all).
//     double surrogateFpsMax          = 60.0;

//     // Whether the active algorithm physically consumes the continuous GPU split.
//     // ConfigManager sets this true only for HeterogeneousGaussianBlur. For
//     // MedianFilter and other binary CPU/GPU algorithms, the surrogate uses the
//     // canonical split {0,1} implied by GPU state so identical hardware states
//     // cannot acquire different model buckets.
//     bool gpuSplitApplicable = true;

//     // Pareto evidence export for PhD validation.
//     // Disabled by default to preserve normal runtime behaviour.
//     bool        export_pareto_evidence = false;
//     std::string pareto_csv_path = "output/erl_pareto_front.csv";
//     std::string action_csv_path = "output/erl_action_trace.csv";

//     // Do not let missing Lynsyn/power block Pareto evidence generation.
//     // On Jetson runs the power stream often starts later than FPS/camera metrics.
//     int    stableFramesRequired = 1;
//     double minFpsForEvolution = 1.0;
//     double minPowerForEvolution = 0.0;
//     bool   requirePowerForEvolution = false;

//     // AdaptiveWeightManager (opt-in ? default preserves current behaviour)
//     bool                           adaptive_weights_enabled = false;
//     AdaptiveWeightManager::Config  awm_config               = {};

//     // Reproducibility: if rngSeed >= 0, the GA seeds std::mt19937 deterministically.
//     // If rngSeed < 0 (default), it falls back to std::random_device for entropy.
//     // Set this (e.g. from config "seed") so ERL runs are seed-stable for the thesis.
//     long rngSeed = -1;

//     // Search-space floor for target_fps gene. Raised from the old hardcoded 8.0
//     // so the population cannot collapse onto the ~8 fps low-clock corner.
//     // Applied in initializePopulation(), mutate(), and crossover().
//     double minTargetFps = 20.0;
//     double maxTargetFps = 60.0;

//     // Workload policy. SobelEdge is GPU-dominant on the validated Jetson Nano
//     // data; ConfigManager enables this flag only for SobelEdge by default.
//     // ThermalGovernor remains authoritative and may still revoke GPU access.
//     bool   forceGpuForWorkload = false;

//     // Fixed performance reference used by HIGH_PERFORMANCE AWM/objective logic.
//     // It is intentionally independent of the evolvable target_fps gene.
//     double performanceTargetFps = 60.0;

//     // [PhD FIX] Explicit scalarization weights for the fitness function.
//     // These bridge the gap between EvolutionarySelector config and the GA's scalar fitness.
//     double weight_fps     = 1.0;
//     double weight_power   = 0.5;
//     double weight_temp    = 0.5;
//     double weight_latency = 0.3;

//     // [P0-F17] Temperature-objective onset thresholds. The old hard-coded
//     // 75C/80C onsets in calculateObjectives() were unreachable on Jetson
//     // Nano (39-65C observed across ERL and baselines), so objs[2] was
//     // identically zero and temperature never influenced dominance, crowding,
//     // or fitness. Defaults track the calibrated AdaptiveWeightManager warn
//     // thresholds (57C) as the single source of truth; override per run via
//     // obj_temp_onset_cpu_c / obj_temp_onset_gpu_c in the Scheduler block.
//     double temp_onset_cpu_c = AdaptiveWeightManager::Config().thermal_warn_cpu_c;
//     double temp_onset_gpu_c = AdaptiveWeightManager::Config().thermal_warn_gpu_c;

// };

// // ===================================================================
// // PhD timing instrumentation: per-generation GA phase breakdown.
// // All values are wall-clock milliseconds measured with steady_clock.
// // total_compute_ms excludes the final selected-action CSV append; the outer
// // EvolutionarySelector separately measures complete controller active time.
// // ===================================================================
// struct GATimingMetrics {
//     double weight_update_ms{0.0};
//     double evaluation_ms{0.0};
//     double nsga_sort_ms{0.0};
//     double selection_ms{0.0};
//     double scheduler_apply_ms{0.0};
//     double action_dispatch_overhead_ms{0.0};
//     double pareto_export_ms{0.0};
//     double diagnostics_ms{0.0};
//     double mutation_ms{0.0};
//     double offspring_ms{0.0};
//     double postprocess_ms{0.0};
//     double total_compute_ms{0.0};
//     bool valid{false};
// };

// // ===================================================================
// // ParetoIndividual (Single struct ? Dual Evaluation)
// // ===================================================================
// struct ParetoIndividual {
//     RLAction genome;

//     // Multi-objective (all minimized)
//     std::vector<double> objectives;   // [normFPS, normEnergy, normTemp, normLatency]

//     // Scalarized single fitness (for ranking & elitism)
//     double fitness = -std::numeric_limits<double>::max();

//     // NSGA-II
//     double crowdingDistance = 0.0;
//     int    rank = 0;

//     // History
//     int    age = 0;
//     int    generation = 0;

//     // [PhD deterministic telemetry] Prediction/provenance used for THIS exact
//     // candidate evaluation. These fields are copied with the selected individual,
//     // preserving the decision rationale even after the population is replaced.
//     SurrogatePrediction surrogatePrediction{};
//     bool   surrogateUsed = false;
//     double surrogateExplorationBonus = 0.0;
//     double surrogateStallPenalty = 0.0;
//     double surrogateConfigFreqKhz = 0.0;
//     bool   surrogateConfigGpu = false;
//     double surrogateConfigGpuSplit = 0.0;
//     int    surrogateConfigConcurrency = 0;

//     // Exact scalar-fitness decomposition used to rank this candidate.
//     std::array<double, 4> fitnessWeights{{0.0, 0.0, 0.0, 0.0}};
//     double fitFpsComponent = 0.0;
//     double fitPowerComponent = 0.0;
//     double fitTempComponent = 0.0;
//     double fitLatencyComponent = 0.0;
//     double fitGpuBonus = 0.0;
//     double fitBeforeAge = 0.0;
//     double fitAgeFactor = 1.0;
//     double fitAfterAge = 0.0;
//     double fitFinal = -std::numeric_limits<double>::max();

//     ParetoIndividual() = default;

//     bool operator<(const ParetoIndividual& other) const {
//         if (rank != other.rank) return rank < other.rank;
//         return crowdingDistance > other.crowdingDistance;
//     }

//     bool dominates(const ParetoIndividual& other) const;

//     // bool dominates(const ParetoIndividual& other) const {
//     //     if (objectives.size() != other.objectives.size()) return false;
//     //     bool strictlyBetter = false;
//     //     for (size_t i = 0; i < objectives.size(); ++i) {
//     //         if (objectives[i] > other.objectives[i]) return false;
//     //         if (objectives[i] < other.objectives[i]) strictlyBetter = true;
//     //     }
//     //     return strictlyBetter;
//     // }
// };

// // ===================================================================
// // GeneticAlgorithm
// // Description: 
// // ===================================================================
// class GeneticAlgorithm {
// public:
//     // Overloaded constructor accepting awmCfg  
//     // GeneticAlgorithm(const GAConfig& cfg,
//     //                  std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator,
//     //                  std::shared_ptr<hrl::RuntimeControls> runtimeControls,
//     //                  IScheduler* scheduler = nullptr,
//     //                  const hrl::AdaptiveWeightManager::Config& awmCfg)

//     GeneticAlgorithm(const GAConfig& cfg,
//                      std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator,
//                      std::shared_ptr<hrl::RuntimeControls> runtimeControls,
//                      IScheduler* scheduler = nullptr,
//                      const hrl::AdaptiveWeightManager::Config& awmCfg =
//                          hrl::AdaptiveWeightManager::Config{})
//         : cfg_(cfg),
//           aggregator_(std::move(aggregator)),
//           runtimeControls_(std::move(runtimeControls)),
//           rng_(cfg.rngSeed >= 0
//                    ? static_cast<std::mt19937::result_type>(cfg.rngSeed)
//                    : std::random_device{}()),
//           scheduler_(scheduler),
//           generation_(0),
//           stableFrames_(0),
//           surrogate_(makeSurrogateConfig(cfg))
//     {
//         // Single authoritative AWM configuration/instance. The production
//         // EvolutionarySelector passes gaCfg.awm_config explicitly as arg 5.
//         cfg_.awm_config = awmCfg;

//         if (cfg_.rngSeed >= 0) {
//             spdlog::info("[GeneticAlgorithm] Deterministic RNG seed = {}", cfg_.rngSeed);
//         } else {
//             spdlog::info("[GeneticAlgorithm] Non-deterministic RNG (random_device)");
//         }

//         currentMutationRate_ = cfg_.mutationRate;
//         initializePopulation();
//         if (cfg_.export_pareto_evidence) ensureEvidenceFiles();

//         spdlog::info("[GeneticAlgorithm] Pareto-ERL ready (pop={}, reduction_gen={}, evidence={}, force_gpu={}, perf_target={:.1f})",
//                      cfg_.popSize, cfg_.reductionStartGen,
//                      cfg_.export_pareto_evidence ? "ON" : "OFF",
//                      cfg_.forceGpuForWorkload ? "YES" : "NO",
//                      cfg_.performanceTargetFps);

//         if (cfg_.adaptive_weights_enabled) {
//             weightManager_ = std::make_unique<hrl::AdaptiveWeightManager>(awmCfg);
//             spdlog::info("[GeneticAlgorithm] AdaptiveWeightManager ENABLED (update_interval={} gens, smoothing={:.2f})",
//                          awmCfg.update_interval_gens, awmCfg.transition_smoothing);
//         }
//     }

// //================================================================================================
// // [FIX P5] Called when the selector force-applies a recovery configuration
//     // outside the GA's control: the next evidence window must not be credited
//     // to the previously selected genome (it did not produce those frames).
//     void notifyExternalOverride() { hasLastApplied_ = false; }


// void evolve(const hrl::MetricsSnapshot& snapshot,
//             bool creditPreviousAction = true,
//             bool previousActionStalled = false) {
//     // if (!isSnapshotStable(snapshot)) {
//     //     lastTiming_ = GATimingMetrics{};
//     //     return;
//     // }
//     // [FIX P1] A starved snapshot (fps ~0-1.5, synthesised on timeout) is the
//     // evidence we need; do not drop it as "unstable".
//     if (!previousActionStalled && !isSnapshotStable(snapshot)) {
//         lastTiming_ = GATimingMetrics{};
//         return;
//     }

//     using TimingClock = std::chrono::steady_clock;
//     const auto totalStart = TimingClock::now();
//     lastTiming_ = GATimingMetrics{};

//     auto elapsedMs = [](const TimingClock::time_point& a,
//                         const TimingClock::time_point& b) -> double {
//         return std::chrono::duration<double, std::milli>(b - a).count();
//     };

//     // ============================================================
//     // 0) Adaptive-weight update
//     // ============================================================
//     auto phaseStart = TimingClock::now();
//     if (weightManager_ && creditPreviousAction) {
//         // HIGH_PERFORMANCE detection uses a fixed experimental target, never an
//         // evolvable low target_fps gene.
//         const double targetFps = cfg_.performanceTargetFps;

//         bool changed = weightManager_->update(
//             snapshot,
//             static_cast<int>(generation_),
//             targetFps
//         );

//         if (changed) {
//             spdlog::info(
//                 "[GeneticAlgorithm] Weight regime -> {} | weights=[{:.3f},{:.3f},{:.3f},{:.3f}]",
//                 weightManager_->getCurrentRegimeName(),
//                 weightManager_->getWeights()[0],
//                 weightManager_->getWeights()[1],
//                 weightManager_->getWeights()[2],
//                 weightManager_->getWeights()[3]
//             );
//         }
//     }
//     auto phaseEnd = TimingClock::now();
//     lastTiming_.weight_update_ms = elapsedMs(phaseStart, phaseEnd);

//     // ============================================================
//     // 1) Evaluate current population
//     // ============================================================
//     phaseStart = TimingClock::now();
//     evaluateParetoPopulation(snapshot, creditPreviousAction, previousActionStalled);
//     phaseEnd = TimingClock::now();
//     lastTiming_.evaluation_ms = elapsedMs(phaseStart, phaseEnd);

//     // ============================================================
//     // 2) NSGA-II ranking + crowding
//     // ============================================================
//     phaseStart = TimingClock::now();
//     nonDominatedSortAndCrowding();
//     phaseEnd = TimingClock::now();
//     lastTiming_.nsga_sort_ms = elapsedMs(phaseStart, phaseEnd);
//     lastEvaluatedRank0Count_ = countRank0();

//     // ============================================================
//     // 3) Select evaluated Rank-0 action
//     // ============================================================
//     phaseStart = TimingClock::now();
//     const ParetoIndividual* selected = selectBestEvaluatedPareto();
//     ParetoIndividual selectedCopy;
//     const bool haveSelected = (selected != nullptr);

//     // Capture how decisive the Rank-0 choice was BEFORE population replacement.
//     //lastRank0RunnerUpFitness_ = std::numeric_limits<double>::quiet_NaN();
//     lastSelectionMargin_ = std::numeric_limits<double>::quiet_NaN();
//     if (haveSelected) {
//         double runnerUp = -std::numeric_limits<double>::max();
//         bool haveRunnerUp = false;
//         for (const auto& ind : population_) {
//             if (ind.rank != 0 || &ind == selected) continue;
//             if (!haveRunnerUp || ind.fitness > runnerUp) {
//                 runnerUp = ind.fitness;
//                 haveRunnerUp = true;
//             }
//         }
//         if (haveRunnerUp) {
//             lastRank0RunnerUpFitness_ = runnerUp;
//             lastSelectionMargin_ = selected->fitness - runnerUp;
//         }
//         selectedCopy = *selected;  // Preserve evidence after population replacement.
//     }
//     phaseEnd = TimingClock::now();
//     lastTiming_.selection_ms = elapsedMs(phaseStart, phaseEnd);

//     // ============================================================
//     // 4) Scheduler / runtime actuation
//     // ============================================================
//     phaseStart = TimingClock::now();
//     if (haveSelected) {
//         lastTiming_.scheduler_apply_ms = applySelectedParetoToRuntime(selectedCopy, snapshot);
//     }
//     phaseEnd = TimingClock::now();
//     const double actionDispatchTotalMs = elapsedMs(phaseStart, phaseEnd);
//     lastTiming_.action_dispatch_overhead_ms = std::max(
//         0.0, actionDispatchTotalMs - lastTiming_.scheduler_apply_ms);

//     // ============================================================
//     // 5) Pareto-front evidence (kept before mutation/replacement)
//     // ============================================================
//     phaseStart = TimingClock::now();
//     exportParetoEvidence(snapshot, selected);
//     phaseEnd = TimingClock::now();
//     lastTiming_.pareto_export_ms = elapsedMs(phaseStart, phaseEnd);

//     phaseStart = TimingClock::now();
//     logDominanceAnalysis();
//     logParetoStats();
//     phaseEnd = TimingClock::now();
//     lastTiming_.diagnostics_ms = elapsedMs(phaseStart, phaseEnd);

//     // ============================================================
//     // 5b) Adaptive mutation
//     // ============================================================
//     phaseStart = TimingClock::now();
//     {
//         double bestFit = -std::numeric_limits<double>::max();
//         for (const auto& ind : population_) {
//             if (ind.rank == 0 && ind.fitness > bestFit) bestFit = ind.fitness;
//         }
//         if (bestFit == -std::numeric_limits<double>::max() && !population_.empty()) {
//             bestFit = population_[0].fitness;
//         }
//         double r = updateAdaptiveMutationRate(bestFit);
//         if (generation_ % 10 == 0) {
//             spdlog::info("[GA-Pareto] Adaptive mutation rate={:.3f} "
//                          "(plateau={}, bestFit={:.4f})",
//                          r, mutationPlateauCounter_, bestFit);
//         }
//     }
//     phaseEnd = TimingClock::now();
//     lastTiming_.mutation_ms = elapsedMs(phaseStart, phaseEnd);

//     // ============================================================
//     // 6) Generate next population
//     // ============================================================
//     phaseStart = TimingClock::now();
//     reducePopulation();
//     auto newPop = selectEliteAndOffspring();
//     population_ = std::move(newPop);
//     phaseEnd = TimingClock::now();
//     lastTiming_.offspring_ms = elapsedMs(phaseStart, phaseEnd);

//     // ============================================================
//     // 7) Replay/diversity post-processing
//     // ============================================================
//     phaseStart = TimingClock::now();
//     seedRLReplayBuffer(snapshot);
//     injectDiversityIfNeeded();
//     phaseEnd = TimingClock::now();
//     lastTiming_.postprocess_ms = elapsedMs(phaseStart, phaseEnd);

//     const auto computeEnd = TimingClock::now();
//     lastTiming_.total_compute_ms = elapsedMs(totalStart, computeEnd);
//     lastTiming_.valid = true;

//     // Export the selected-action row after phase timings are finalised. This
//     // deliberately leaves the row's own file-append time out of ga_compute_total_ms;
//     // EvolutionarySelector's outer erl_active_wall_ms includes it.
//     if (haveSelected) {
//         exportSelectedActionEvidence(snapshot, selectedCopy);
//     }

//     ++generation_;
// }
// //=========================================================================================
//     // void evolve(const hrl::MetricsSnapshot& snapshot) {
//     //     if (!isSnapshotStable(snapshot)) return;

//     //     // Update adaptive weights BEFORE fitness evaluation
//     //     if (weightManager_) {
//     //         double targetFps = population_.empty() ? 30.0 :
//     //             (population_[0].genome.target_fps.has ? population_[0].genome.target_fps.value : 30.0);
//     //         bool changed = weightManager_->update(snapshot, static_cast<int>(generation_), targetFps);
//     //         if (changed) {
//     //             spdlog::info("[GeneticAlgorithm] Weight regime ? {} | weights=[{:.3f},{:.3f},{:.3f},{:.3f}]",
//     //                          weightManager_->getCurrentRegimeName(),
//     //                          weightManager_->getWeights()[0],
//     //                          weightManager_->getWeights()[1],
//     //                          weightManager_->getWeights()[2],
//     //                          weightManager_->getWeights()[3]);
//     //         }
//     //     }
        
//     //     // 1) Evaluate the CURRENT population against the latest measured snapshot.
//     //     evaluateParetoPopulation(snapshot);      // Dual: objectives + fitness
//     //     // 2) Rank by non-dominated sorting and crowding distance.
//     //     nonDominatedSortAndCrowding();
//     //     // 3) Select the best evaluated Rank-0 individual BEFORE offspring generation.
//     //     //    This is the key thesis-proof fix: the applied action is from the evaluated Pareto front.
//     //     const ParetoIndividual* selected = selectBestEvaluatedPareto();

//     //     // 4) Apply the selected evaluated Pareto action to runtime controls.
//     //     if (selected) {
//     //         applySelectedParetoToRuntime(*selected, snapshot);
//     //     }

//     //     // 5) Export evidence before population mutation/replacement.
//     //     exportParetoEvidence(snapshot, selected);
//     //     if (selected) {
//     //         exportSelectedActionEvidence(snapshot, *selected);
//     //     }

//     //     logDominanceAnalysis();     // PhD thesis material
//     //     logParetoStats();
       
//     //     // 6) Generate the NEXT population only after applying/logging the current front.
//     //     reducePopulation();
//     //     auto newPop = selectEliteAndOffspring();
//     //     population_ = std::move(newPop);

//     //     ++generation_;
//     //     applyBestParetoToRuntime(snapshot);
//     //     seedRLReplayBuffer(snapshot);
//     //     injectDiversityIfNeeded();
       
//     // }

//     RLAction getBestAction() const {
//         if (population_.empty()) return RLAction{};
//         // Return best Rank-0 by scalar fitness.
//         // [P0-F16] Seed with a rank-0 individual, not population_[0]
//         // unconditionally: previously a high-fitness *dominated* individual
//         // at index 0 could shadow the entire Pareto front.
//         const ParetoIndividual* best = nullptr;
//         for (const auto& ind : population_) {
//             if (ind.rank == 0 && (!best || ind.fitness > best->fitness))
//                 best = &ind;
//         }
//         if (!best) best = &population_[0];   // no rank-0 yet (e.g. pre-evaluation)
//         return best->genome;
//     }

//     // Thesis data export ? CSV of weight history
//     std::string exportWeightHistory() const {
//         if (weightManager_) return weightManager_->exportHistoryCSV();
//         return "adaptive_weights_disabled\n";
//     }

//     // Current regime label for logging / thesis
//     std::string getCurrentRegimeName() const {
//         if (weightManager_) return weightManager_->getCurrentRegimeName();
//         return "FIXED_WEIGHTS";
//     }

//     GATimingMetrics getLastTimingMetrics() const { return lastTiming_; }
//     size_t getCurrentGeneration() const { return generation_; }

// private:
//     GAConfig cfg_;
//     std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator_;
//     std::shared_ptr<hrl::RuntimeControls> runtimeControls_;
//     std::vector<ParetoIndividual> population_;
//     std::mt19937 rng_;
//     IScheduler* scheduler_ = nullptr;
//     size_t generation_ = 0;
//     int stableFrames_ = 0;
//     std::unique_ptr<AdaptiveWeightManager> weightManager_;
//     mutable std::mutex evidenceMutex_;
//     GATimingMetrics lastTiming_{};
//     int lastEvaluatedRank0Count_{0};

//      // [FIX] Restored: declaration was lost while merging the trap patches.
//     // Written in evolve() (runner-up capture) and read by
//     // exportSelectedActionEvidence() -> rank0_runnerup_fitness CSV column.
//     double lastRank0RunnerUpFitness_{std::numeric_limits<double>::quiet_NaN()};

//     double lastSelectionMargin_{std::numeric_limits<double>::quiet_NaN()};

//     // [Adaptive mutation] runtime state
//     double currentMutationRate_ = 0.18;   // Initialised from cfg_.mutationRate in ctor
//     double prevBestFitness_     = -std::numeric_limits<double>::max();
//     int    mutationPlateauCounter_ = 0;

//     // [Surrogate] online counterfactual fitness model + last-applied tracking.
//     // snap on cycle N reflects the action applied on cycle N-1, so we observe the
//     // last-applied config against the current snapshot (temporal credit assignment).
//     SurrogateModel surrogate_;
//     RLAction       lastAppliedGenome_;
//     bool           hasLastApplied_ = false;

//     double         maxMeasuredFps_ = 0.0;   // [FIX P2] best hardware-verified fps this run

// //     // ===================================================================
// //     // Simulate simulateRuntimeControlsFromAction
// //     // ===================================================================
// //     void simulateRuntimeControlsFromAction(const RLAction& action,
// //                                        RuntimeControls& simControls) const {
// //     int concurrency = 2;
// //     bool enableGpu = true;
// //     Affinity affinity = Affinity::Spread;

// //     switch (action.mode) {
// //         case PolicyMode::MAX_PERFORMANCE:
// //             concurrency = 4;
// //             enableGpu = true;
// //             affinity = Affinity::Spread;
// //             break;

// //         case PolicyMode::LOW_POWER:
// //             concurrency = 1;
// //             enableGpu = false;
// //             affinity = Affinity::Pack;
// //             break;

// //         case PolicyMode::BALANCED:
// //             concurrency = 2;
// //             enableGpu = action.prefer_gpu.has ? action.prefer_gpu.value : true;
// //             affinity = Affinity::Spread;
// //             break;

// //         case PolicyMode::UNKNOWN:
// //         default:
// //             concurrency = 2;
// //             enableGpu = true;
// //             affinity = Affinity::Spread;
// //             break;
// //     }

// //     if (action.prefer_gpu.has) {
// //         enableGpu = action.prefer_gpu.value;
// //     }

// //     simControls.concurrency_level.store(concurrency, std::memory_order_relaxed);
// //     simControls.enable_gpu.store(enableGpu, std::memory_order_relaxed);
// //     simControls.affinity.store(affinity, std::memory_order_relaxed);
// // }

//     // ===================================================================
//     // Evaluation (Dual: Pareto + Scalar)
//     // ===================================================================
//     // ===================================================================
//     // [Surrogate] Build the surrogate model configuration from GAConfig.
//     // ===================================================================
//     // static SurrogateModel::Config makeSurrogateConfig(const GAConfig& cfg) {
//     //     SurrogateModel::Config sc;
//     //     sc.exploration_weight = cfg.surrogateExploreWeight;
//     //     return sc;
//     // }

//     static SurrogateModel::Config makeSurrogateConfig(const GAConfig& cfg) {
//         SurrogateModel::Config sc;
//         sc.exploration_weight = cfg.surrogateExploreWeight;
//         sc.fps_max = cfg.surrogateFpsMax;   // [FIX P7] per-workload prior ceiling
//         return sc;
//     }

//     void canonicalizeGenomeForWorkload(RLAction& g) const {
//         // UNKNOWN is not part of the search space. Keep this defensive guard for
//         // legacy/replayed genomes.
//         if (g.mode == PolicyMode::UNKNOWN) g.mode = PolicyMode::BALANCED;lastEvaluatedRank0Count_;

//         if (cfg_.forceGpuForWorkload) {
//             g.prefer_gpu = MaybeBool(true);
//             if (!cfg_.gpuSplitApplicable) g.gpu_workload_split = MaybeDouble(1.0);
//         }

//         // MAX_PERFORMANCE has fixed semantic intent: do not let a low evolved
//         // target relabel a configuration as high-performance.
//         if (g.mode == PolicyMode::MAX_PERFORMANCE) {
//             g.target_fps = MaybeDouble(cfg_.performanceTargetFps);
//         }
//     }

//     // ===================================================================
//     // [Surrogate] Extract the discretised hardware config a genome represents.
//     // freq: explicit gene if present, else derived from PolicyMode.
//     // split: work-split gene if present (thesis gap 1), else GPU on=>0.5 / off=>0.
//     // conc: derived from mode/budget to mirror the Scheduler's mapping.
//     // ===================================================================
//     void configFromGenome(const RLAction& g, double& freq_khz, bool& gpu,
//                           double& split, int& conc) const {
//         gpu = g.prefer_gpu.has ? g.prefer_gpu.value
//                                : (g.mode != PolicyMode::LOW_POWER);
//         if (g.cpu_max_freq_khz.has) {
//             if (g.mode == PolicyMode::LOW_POWER) {
//                 freq_khz = utils::local_clamp(g.cpu_max_freq_khz.value, 460800.0, 1479000.0);
//             } else {
//                 freq_khz = utils::local_clamp(g.cpu_max_freq_khz.value, 102000.0, 1479000.0);
//             }
//         } else {
//             switch (g.mode) {
//                 case PolicyMode::MAX_PERFORMANCE: freq_khz = 1479000.0; break;
//                 case PolicyMode::LOW_POWER:       freq_khz = 1020000.0; break;
//                 default:                          freq_khz =  921600.0; break;
//             }
//         }
//         // [Micro-scheduler] Only algorithms that physically implement a
//         // continuous CPU/GPU partition may use the split gene as a surrogate
//         // dimension. Binary algorithms (e.g. MedianFilter) are canonicalised
//         // to 0=CPU or 1=GPU, preventing fictitious duplicate model states.
//         if (!cfg_.gpuSplitApplicable) {
//             split = gpu ? 1.0 : 0.0;
//         } else if (g.gpu_workload_split.has) {
//             split = g.gpu_workload_split.value;
//             if (split < 0.0) split = 0.0;
//             if (split > 1.0) split = 1.0;
//         } else {
//             split = gpu ? 1.0 : 0.0;
//         }
//         double budget = g.power_budget_watts.has ? g.power_budget_watts.value : 4.0;
//         switch (g.mode) {
//             case PolicyMode::MAX_PERFORMANCE: conc = 4; break;
//             case PolicyMode::LOW_POWER:       conc = (budget < 3.5) ? 1 : 2; break;
//             default:                          conc = 2; break;
//         }
//     }

//     // Build a snapshot with the surrogate's predicted outcome for `g`, copying
//     // any unrelated fields from the real snapshot so downstream code is unchanged.
//     hrl::MetricsSnapshot predictedSnapshot(const hrl::MetricsSnapshot& base,
//                                            const RLAction& g) const {
//         double freq; bool gpu; double split; int conc;
//         configFromGenome(g, freq, gpu, split, conc);
//         SurrogatePrediction p = surrogate_.predict(freq, gpu, split, conc);
//         hrl::MetricsSnapshot s = base;   // preserve unrelated fields
//         s.fps             = p.fps;
//         s.avg_power_w_alg = p.power_w;
//         s.avg_latency_ms  = p.latency_ms;
//         s.cpu_temp_c      = p.temp_c;
//         return s;
//     }

//     void evaluateParetoPopulation(const hrl::MetricsSnapshot& snap,
//                                   bool creditPreviousAction,
//                                   bool previousActionStalled) {
//         // // One action -> one evidence window -> one surrogate credit update.
//         // if (cfg_.surrogateEnabled && hasLastApplied_) {
//         //     double freq; bool gpu; double split; int conc;
//         //     configFromGenome(lastAppliedGenome_, freq, gpu, split, conc);
//         //     if (previousActionStalled) {
//         //         surrogate_.observeStarvation(freq, gpu, split, conc);
//         //     } else if (creditPreviousAction) {
//         //         surrogate_.observe(freq, gpu, split, conc,
//         //                            snap.fps, snap.avg_power_w_alg,
//         //                            snap.avg_latency_ms, snap.cpu_temp_c);
//         //     }
//         // }
//         // One action -> one evidence window -> one surrogate credit update.
//         if (cfg_.surrogateEnabled && hasLastApplied_) {
//             double freq; bool gpu; double split; int conc;
//             configFromGenome(lastAppliedGenome_, freq, gpu, split, conc);
//             if (previousActionStalled) {
//                 surrogate_.observeStarvation(freq, gpu, split, conc);
//                 // [FIX P1] The selector synthesised the starved measurement
//                 // (fps = fresh_frames / elapsed) into `snap`. Feed it to the
//                 // model: honest ~0-1.5 fps data replaces the optimistic prior
//                 // for exactly the config that failed to produce frames.
//                 surrogate_.observe(freq, gpu, split, conc,
//                                    snap.fps, snap.avg_power_w_alg,
//                                    snap.avg_latency_ms, snap.cpu_temp_c);
//             } else if (creditPreviousAction) {
//                 surrogate_.observe(freq, gpu, split, conc,
//                                    snap.fps, snap.avg_power_w_alg,
//                                    snap.avg_latency_ms, snap.cpu_temp_c);
//                 if (snap.fps > maxMeasuredFps_) maxMeasuredFps_ = snap.fps; // [FIX P2]
//             }
//         }

//         for (auto& ind : population_) {
//             canonicalizeGenomeForWorkload(ind.genome);
//             hrl::RuntimeControls simControls;
//             simControls.concurrency_level.store(2, std::memory_order_relaxed);
//             simControls.enable_gpu.store(true, std::memory_order_relaxed);
//             simulateRuntimeControlsFromAction(ind.genome, simControls);

//             // Preserve the exact counterfactual prediction used for THIS candidate.
//             hrl::MetricsSnapshot evalSnap = snap;
//             ind.surrogateUsed = false;
//             ind.surrogatePrediction = SurrogatePrediction{};
//             ind.surrogateExplorationBonus = 0.0;
//             ind.surrogateStallPenalty = 0.0;
//             ind.surrogateConfigFreqKhz = 0.0;
//             ind.surrogateConfigGpu = false;
//             ind.surrogateConfigGpuSplit = 0.0;
//             ind.surrogateConfigConcurrency = 0;

//             if (cfg_.surrogateEnabled) {
//                 double freq; bool gpu; double split; int conc;
//                 configFromGenome(ind.genome, freq, gpu, split, conc);

//                 ind.surrogateConfigFreqKhz = freq;
//                 ind.surrogateConfigGpu = gpu;
//                 ind.surrogateConfigGpuSplit = split;
//                 ind.surrogateConfigConcurrency = conc;

//                 // ind.surrogatePrediction = surrogate_.predict(freq, gpu, split, conc);
//                 // ind.surrogateUsed = true;
//                 // evalSnap.fps             = ind.surrogatePrediction.fps;

//                 ind.surrogatePrediction = surrogate_.predict(freq, gpu, split, conc);

//                 // [FIX P2] Reality gating: unverified optimism must not enter fitness.
//                 {
//                     SurrogatePrediction& p = ind.surrogatePrediction;
//                     const double fpsCap = (maxMeasuredFps_ > 0.0)
//                         ? cfg_.surrogatePredFpsCapRatio * maxMeasuredFps_
//                         : cfg_.surrogateColdFpsCeiling;
//                     const bool lowSupport = !p.from_data ||
//                         p.support_count < static_cast<uint32_t>(cfg_.surrogateMinSupport);
//                     if (lowSupport) {
//                         const double pessimistic = (maxMeasuredFps_ > 0.0)
//                             ? 0.5 * maxMeasuredFps_
//                             : 0.5 * cfg_.surrogateColdFpsCeiling;
//                         p.fps = std::min(p.fps, std::max(1.0, pessimistic));
//                     }
//                     if (p.fps > fpsCap) p.fps = fpsCap;   // 72-fps claims are inadmissible
//                     if (p.fps > 0.0)
//                         p.latency_ms = std::max(p.latency_ms, 1000.0 / p.fps);
//                 }

//                 ind.surrogateUsed = true;
//                 evalSnap.fps             = ind.surrogatePrediction.fps;

//                 evalSnap.avg_power_w_alg = ind.surrogatePrediction.power_w;
//                 evalSnap.avg_latency_ms  = ind.surrogatePrediction.latency_ms;
//                 evalSnap.cpu_temp_c      = ind.surrogatePrediction.temp_c;

//                 ind.surrogateExplorationBonus =
//                     surrogate_.explorationBonus(freq, gpu, split, conc);
//                 ind.surrogateStallPenalty =
//                     surrogate_.stallPenalty(freq, gpu, split, conc);
//             }

//             ind.objectives = calculateObjectives(evalSnap, ind.genome, simControls);
//             ind.fitness = calculateScalarizedFitness(ind.objectives, evalSnap,
//                                                       ind.genome, ind.age, &ind);

//             // Optimism under uncertainty is added AFTER scalar fitness/age decay,
//             // preserving the existing controller arithmetic exactly.
//             ind.fitness += ind.surrogateExplorationBonus;
//             ind.fitness -= ind.surrogateStallPenalty;
//             ind.fitFinal = ind.fitness;
//             ind.age++;
//         }
//     }


// //================================================================
//     //  Return vector of normalized objectives (all to be minimized)
//     std::vector<double> calculateObjectives(const hrl::MetricsSnapshot& snap,
//                                             const RLAction& action,
//                                             const RuntimeControls& controls) {
//         std::vector<double> objs(4);
//         // Objective 1: FPS (minimize negative FPS = maximize FPS)

//         const bool highPerformanceRegime =
//             weightManager_ && weightManager_->getCurrentRegime() == SystemRegime::HIGH_PERFORMANCE;
//         const double fpsScore = action.target_fps.has
//             ? (1.0 / (1.0 + std::abs(snap.fps - action.target_fps.value))) : 0.0;
//         const double directThroughput = cfg_.performanceTargetFps > 0.0
//             ? utils::local_clamp(snap.fps / cfg_.performanceTargetFps, 0.0, 2.0)
//             : 0.0;

//         // [P0] Frequency-aware energy model. The measured snapshot power reflects
//         // the CURRENT clock, but each candidate would run at its own gene frequency.
//         // Scale the energy estimate by (candidate_freq / max_freq) so a low-clock
//         // genome is correctly scored as cheaper and a high-clock genome as costlier.
//         // Without this, LOW_POWER and MAX_PERFORMANCE candidates score near-identical
//         // energy and the Pareto front collapses onto the idle low-clock corner.
//         // const double kMaxFreqKhz = 1479000.0;
//         // double freqRatio = action.cpu_max_freq_khz.has
//         //     ? (action.cpu_max_freq_khz.value / kMaxFreqKhz)
//         //     : 1.0;
//         // freqRatio = freqRatio < 0.05 ? 0.05 : (freqRatio > 1.0 ? 1.0 : freqRatio);

//         // double energy  = snap.avg_power_w_alg * freqRatio
//         //                  * (1.0 + 0.3 * (controls.concurrency_level.load() - 2));
//         // [FIX P4] With the surrogate on, snap.avg_power_w_alg is already the
//         // per-candidate PREDICTED power at this genome's own frequency/config;
//         // multiplying by freqRatio double-counted the clock and let 102 MHz
//         // genomes buy the energy objective ~15-20x below reality (Pareto rank-0
//         // then filled with LOW_POWER fictions). Keep the legacy discount only
//         // for the surrogate-off ablation baseline.
//         double energy = snap.avg_power_w_alg;
//         if (!cfg_.surrogateEnabled) {
//             const double kMaxFreqKhz = 1479000.0;
//             double freqRatio = action.cpu_max_freq_khz.has
//                 ? (action.cpu_max_freq_khz.value / kMaxFreqKhz)
//                 : 1.0;
//             freqRatio = freqRatio < 0.05 ? 0.05 : (freqRatio > 1.0 ? 1.0 : freqRatio);
//             energy *= freqRatio * (1.0 + 0.3 * (controls.concurrency_level.load() - 2));
//         }
//         // [P0-F17] Penalty onset uses the calibrated thresholds (default 57C
//         // via AdaptiveWeightManager::Config) instead of hard-coded 75C/80C,
//         // so the live ERL-vs-baseline thermal gradient (39-45C vs 61-65C)
//         // actually reaches the objective vector.
//         double tempPen = (snap.cpu_temp_c > cfg_.temp_onset_cpu_c ? snap.cpu_temp_c - cfg_.temp_onset_cpu_c : 0.0) +
//                          (snap.gpu_temp_c > cfg_.temp_onset_gpu_c ? snap.gpu_temp_c - cfg_.temp_onset_gpu_c : 0.0);
//         double latency = snap.avg_latency_ms;

//         // In HIGH_PERFORMANCE, maximize actual predicted throughput against a
//         // fixed reference. Other regimes preserve the legacy target-matching term.
//         objs[0] = highPerformanceRegime ? -directThroughput : -fpsScore;           // Maximize FPS // Objective 1: FPS (minimize negative FPS = maximize FPS)
//         objs[1] = energy / 10.0;       // Minimize energy // Objective 2: Energy/Power (minimize)
//         objs[2] = tempPen / 20.0;      // Minimize temperature  // Objective 3: Temperature penalty (minimize)
//         objs[3] = latency / 50.0;      // Minimize latency  // Objective 4: Latency (minimize)  // normalize
//         return objs;
//     }

// // ===================================================================
// // NSGA-II Core
// // ===================================================================
//     void nonDominatedSortAndCrowding();
//     void calculateCrowdingDistance(std::vector<int>& front);
//     const ParetoIndividual& tournamentSelect(int tournamentSize);
    
//     // Scalarized fitness: FPS=50%, Power=20%, Temp=20%, Latency=10% (PhD dual evaluation)
//     double calculateScalarizedFitness(const std::vector<double>& objs,
//                                                      const hrl::MetricsSnapshot& snap,
//                                                      const RLAction& action,
//                                                      int age,
//                                                      ParetoIndividual* auditOut) const ;

//     // ===================================================================
//     // Evolution Operators
//     // ===================================================================
//     ParetoIndividual crossover(const ParetoIndividual& p1, const ParetoIndividual& p2);
//     //void mutate(ParetoIndividual& ind);
//     void mutate(ParetoIndividual& ind, const hrl::MetricsSnapshot& snap);

//     std::vector<ParetoIndividual> selectEliteAndOffspring();
//     void reducePopulation();
//     void injectDiversityIfNeeded();

//     // ===================================================================
//     // Runtime & Logging
//     // ===================================================================
//     // void applyBestParetoToRuntime(const hrl::MetricsSnapshot& snapshot);
//     // void seedRLReplayBuffer(const hrl::MetricsSnapshot& snapshot);
//     // void logParetoStats();
//     // void logDominanceAnalysis();

//     // bool isSnapshotStable(const hrl::MetricsSnapshot& snap);
//     // void initializePopulation();

//     // ===================================================================
//     // Runtime & Logging
//     // ===================================================================
//     void applyBestParetoToRuntime(const hrl::MetricsSnapshot& snapshot);

//     const ParetoIndividual* selectBestEvaluatedPareto() const;

//     double applySelectedParetoToRuntime(const ParetoIndividual& selected,
//                                         const hrl::MetricsSnapshot& snapshot);

//     void exportParetoEvidence(const hrl::MetricsSnapshot& snap,
//                               const ParetoIndividual* selected);

//     void exportSelectedActionEvidence(const hrl::MetricsSnapshot& snap,
//                                       const ParetoIndividual& selected);

//     void ensureEvidenceFiles();
//     static bool ensureParentDirectoryForFile(const std::string& filePath);

//     int countRank0() const;

//     void simulateRuntimeControlsFromAction(const RLAction& action,
//                                            RuntimeControls& simControls) const;

//     void seedRLReplayBuffer(const hrl::MetricsSnapshot& snapshot);
//     void logParetoStats();
//     void logDominanceAnalysis();

//     bool isSnapshotStable(const hrl::MetricsSnapshot& snap);
//     void initializePopulation();

//     // [Adaptive mutation] Update currentMutationRate_ from best-fitness trend.
//     // Pure rate control ? does not alter selection, so it is Pareto-safe.
//     double updateAdaptiveMutationRate(double currentBestFitness);
// };

// // ===================================================================
// // Implementations (add to .cpp or keep inline)
// // ===================================================================
// // ... (nonDominatedSortAndCrowding, calculateCrowdingDistance, tournamentSelect remain as in your GA_03/04)

// // Crossover & Mutate
// //ParetoIndividual GeneticAlgorithm::crossover(const ParetoIndividual& p1, const ParetoIndividual& p2) { /* same as before */ }


// // ====================== Inline Implementations ======================
// inline bool ParetoIndividual::dominates(const ParetoIndividual& other) const {
//     if (objectives.size() != other.objectives.size()) return false;
//     bool strictlyBetter = false;
//     for (size_t i = 0; i < objectives.size(); ++i) {
//         if (objectives[i] > other.objectives[i]) return false;
//         if (objectives[i] < other.objectives[i]) strictlyBetter = true;
//     }
//     return strictlyBetter;
// }




//   // ===================================================================
//     // Evolution Operators (UPDATED for ParetoIndividual)
//     // ===================================================================
//     ParetoIndividual GeneticAlgorithm::crossover(const ParetoIndividual& p1, const ParetoIndividual& p2) {
//         ParetoIndividual child;
//         std::uniform_real_distribution<double> mix(0.0, 1.0);

//         child.genome.mode = mix(rng_) < 0.5 ? p1.genome.mode : p2.genome.mode;

//         if (p1.genome.target_fps.has && p2.genome.target_fps.has) {
//             double avg = (p1.genome.target_fps.value + p2.genome.target_fps.value) * 0.5;
//             child.genome.target_fps = MaybeDouble(utils::local_clamp(avg, cfg_.minTargetFps, cfg_.maxTargetFps));
//         }

//         if (p1.genome.prefer_gpu.has && p2.genome.prefer_gpu.has) {
//             child.genome.prefer_gpu = mix(rng_) < 0.6 ? p1.genome.prefer_gpu : p2.genome.prefer_gpu;
//         }

//         // [P0] Inherit the CPU frequency gene from one parent (discrete, so no averaging).
//         if (p1.genome.cpu_max_freq_khz.has || p2.genome.cpu_max_freq_khz.has) {
//             const RLAction& src = (mix(rng_) < 0.5 ? p1.genome : p2.genome);
//             child.genome.cpu_max_freq_khz = src.cpu_max_freq_khz.has
//                 ? src.cpu_max_freq_khz
//                 : (p1.genome.cpu_max_freq_khz.has ? p1.genome.cpu_max_freq_khz
//                                                   : p2.genome.cpu_max_freq_khz);
//         }

//         // [Micro-scheduler] Blend the continuous split gene. Unlike the discrete
//         // frequency gene, averaging is meaningful here and lets crossover explore
//         // intermediate load balances between two parents.
//         if (p1.genome.gpu_workload_split.has && p2.genome.gpu_workload_split.has) {
//             double avg = 0.5 * (p1.genome.gpu_workload_split.value +
//                                 p2.genome.gpu_workload_split.value);
//             child.genome.gpu_workload_split = MaybeDouble(utils::local_clamp(avg, 0.0, 1.0));
//         } else if (p1.genome.gpu_workload_split.has || p2.genome.gpu_workload_split.has) {
//             child.genome.gpu_workload_split = p1.genome.gpu_workload_split.has
//                 ? p1.genome.gpu_workload_split : p2.genome.gpu_workload_split;
//         }
//         canonicalizeGenomeForWorkload(child.genome);
//         return child;
//     }


// //void GeneticAlgorithm::mutate(ParetoIndividual& ind) { /* same as before */ }

//     void GeneticAlgorithm::mutate(ParetoIndividual& ind, const hrl::MetricsSnapshot& snap) {
//         (void)snap;
//         std::uniform_real_distribution<double> dist(0.0, 1.0);
//         if (dist(rng_) >= currentMutationRate_) return;

//         if (ind.genome.target_fps.has) {
//             std::normal_distribution<double> fpsMut(0.0, 4.0);
//             double newFps = ind.genome.target_fps.value + fpsMut(rng_);
//             ind.genome.target_fps = MaybeDouble(utils::local_clamp(newFps, cfg_.minTargetFps, cfg_.maxTargetFps));
//         }

//         if (ind.genome.prefer_gpu.has && dist(rng_) < 0.25) {
//             ind.genome.prefer_gpu = MaybeBool(!ind.genome.prefer_gpu.value);
//         }

//         // [Micro-scheduler] Mutate the continuous GPU workload fraction by a small
//         // Gaussian perturbation, clamped to [0,1]. This is the fine-grained load-
//         // balancing search that a binary toggle cannot express.
//         if (ind.genome.gpu_workload_split.has) {
//             std::normal_distribution<double> splitMut(0.0, 0.15);
//             double s = ind.genome.gpu_workload_split.value + splitMut(rng_);
//             ind.genome.gpu_workload_split = MaybeDouble(utils::local_clamp(s, 0.0, 1.0));
//         }

//         // [P0] Mutate the CPU frequency gene by stepping to a neighbouring DVFS level.
//         if (ind.genome.cpu_max_freq_khz.has) {
//             static const long kFreqSteps[] = {102000, 460800, 921600, 1190400, 1479000};
//             const int n = 5;
//             // Find nearest current index
//             long cur = static_cast<long>(ind.genome.cpu_max_freq_khz.value);
//             int idx = 0; long best = std::abs(kFreqSteps[0] - cur);
//             for (int k = 1; k < n; ++k) {
//                 long d = std::abs(kFreqSteps[k] - cur);
//                 if (d < best) { best = d; idx = k; }
//             }
//             int step = (dist(rng_) < 0.5) ? -1 : 1;
//             idx = utils::local_clamp(idx + step, 0, n - 1);
//             ind.genome.cpu_max_freq_khz = MaybeDouble(static_cast<double>(kFreqSteps[idx]));
//         }

//         if (dist(rng_) < 0.07) {
//             std::uniform_int_distribution<int> modeDist(1, 3);
//             ind.genome.mode = static_cast<PolicyMode>(modeDist(rng_));
//         }
//         canonicalizeGenomeForWorkload(ind.genome);
//     }




// // selectEliteAndOffspring, reducePopulation, injectDiversityIfNeeded, applyBestParetoToRuntime, etc. ? use the clean versions from GA_04



//     // ===================================================================
//     // Selection + Population Management
//     // ===================================================================
//     std::vector<ParetoIndividual> GeneticAlgorithm::selectEliteAndOffspring() {
//         std::vector<ParetoIndividual> newPop;

//         // [P0-F16] Defensive: tournamentSelect() draws indices in
//         // [0, population_.size()-1]; on an empty population that distribution
//         // is degenerate and indexing is UB. Self-heal instead of crashing.
//         if (population_.empty()) {
//             spdlog::error("[GA-Pareto] Population empty at selection - reinitializing");
//             initializePopulation();
//         }

//         // Elitism (population_ is sorted by rank/crowding - see [P0-F16])
//         for (size_t i = 0; i < std::min(cfg_.eliteCount, population_.size()); ++i) {
//             newPop.push_back(population_[i]);
//         }

//         // Offspring
//         while (newPop.size() < cfg_.popSize) {
//             const auto& p1 = tournamentSelect(3);
//             const auto& p2 = tournamentSelect(3);
//             auto child = crossover(p1, p2);
//             mutate(child, hrl::MetricsSnapshot{});   // snapshot not used in mutate anymore
//             newPop.push_back(child);
//         }
//         return newPop;
//     }

//     void GeneticAlgorithm::reducePopulation() {
//         if (generation_ < cfg_.reductionStartGen) return;

//         // [P0-F16] population_ is sorted by (rank, crowding) at the end of
//         // nonDominatedSortAndCrowding(), so truncation keeps the best
//         // individuals with no duplicates. The old index-based fill
//         // (population_[survivors.size()]) could re-copy rank-0 members that
//         // were already in `survivors`, because rank-0 individuals were
//         // scattered through an unsorted array. Semantics preserved: keep at
//         // least minPopSize, and keep the whole rank-0 front even when it
//         // exceeds the reduction target.
//         size_t rank0 = 0;
//         for (const auto& ind : population_) {
//             if (ind.rank == 0) ++rank0;
//         }

//         size_t target = std::max(cfg_.minPopSize,
//                                  static_cast<size_t>(population_.size() * cfg_.reductionRatio));
//         target = std::max(target, rank0);

//         if (population_.size() > target) {
//             population_.resize(target);
//         }

//         spdlog::info("[GA-Pareto] Dynamic reduction -> size {} (gen {})", population_.size(), generation_);
//     }

//     void GeneticAlgorithm::injectDiversityIfNeeded() {
//         if (generation_ % 5 == 0 && population_.size() < cfg_.popSize) {
//             spdlog::debug("[GA-Pareto] Injecting diversity");
//             // Simple random injection (same logic as initializePopulation)
//             ParetoIndividual randomInd;
//             std::uniform_real_distribution<double> fpsDist(cfg_.minTargetFps, cfg_.maxTargetFps);
//             std::uniform_int_distribution<int> modeDist(1, 3);
//             std::bernoulli_distribution gpuBias(0.65);

//             randomInd.genome.mode = static_cast<PolicyMode>(modeDist(rng_));
//             randomInd.genome.target_fps = MaybeDouble(fpsDist(rng_));
//             randomInd.genome.prefer_gpu = MaybeBool(gpuBias(rng_));
//             canonicalizeGenomeForWorkload(randomInd.genome);
//             population_.push_back(randomInd);
//         }
//     }

//     // ===================================================================
//     // Runtime Application & Logging (PhD helpers)
//     // ===================================================================
//     // ===================================================================
//     // Runtime & Logging
//     // ===================================================================
//    // void applyBestParetoToRuntime(const hrl::MetricsSnapshot& snapshot);
//     //void seedRLReplayBuffer(const hrl::MetricsSnapshot& snapshot);
//     //void logParetoStats();
//     //void logDominanceAnalysis();

//     //bool isSnapshotStable(const hrl::MetricsSnapshot& snap);
//     // void initializePopulation();
//     // ===================================================================

//     // void hrl::GeneticAlgorithm::applyBestParetoToRuntime(const hrl::MetricsSnapshot& snapshot) {
//     //     if (population_.empty() || !runtimeControls_) return;
//     //     const auto& best = population_[0];
//     //     if (scheduler_) scheduler_->apply(best.genome, snapshot, *runtimeControls_);

//     //     spdlog::debug("[GeneticAlgorithm] Applied best: mode={}, target_fps={:.1f}, gpu={}",
//     //                   static_cast<int>(best.genome.mode),
//     //                   best.genome.target_fps.has ? best.genome.target_fps.value : 0.0,
//     //                   best.genome.prefer_gpu.has ? (best.genome.prefer_gpu.value ? "YES" : "NO") : "AUTO");
//     // }

// // ===================================================================
// // applyBestParetoToRuntime: apply Rank-0 individual with best fitness
// // ===================================================================
// void hrl::GeneticAlgorithm::applyBestParetoToRuntime(const hrl::MetricsSnapshot& snapshot) {
//     if (population_.empty() || !runtimeControls_) return;

//     // Select Rank-0 individual with highest scalarized fitness
//     const ParetoIndividual* best = nullptr;
//     for (const auto& ind : population_) {
//         if (ind.rank == 0) {
//             if (!best || ind.fitness > best->fitness) best = &ind;
//         }
//     }
//     if (!best) {
//         spdlog::warn("[GeneticAlgorithm] No Rank-0 solutions found ? falling back to population[0]");
//         best = &population_[0];  // fallback to first
//     }

//     if (scheduler_) scheduler_->apply(best->genome, snapshot, *runtimeControls_);
    

//     spdlog::debug("[GeneticAlgorithm] Applied Rank-0 best: mode={}, fps={:.1f}, gpu={}, fitness={:.4f}",
//                   static_cast<int>(best->genome.mode),
//                   best->genome.target_fps.has ? best->genome.target_fps.value : 0.0,
//                   best->genome.prefer_gpu.has ? (best->genome.prefer_gpu.value ? "YES" : "NO") : "AUTO",
//                   best->fitness);
// }

// // ===================================================================
// // seedRLReplayBuffer: push Rank-0 solutions as RL experience (ERL)
// // ===================================================================
// void GeneticAlgorithm::seedRLReplayBuffer(const hrl::MetricsSnapshot& snapshot) {
//     // ERL hybridization: Rank-0 genomes serve as high-quality seeds for RL
//     // In a full ERL implementation these would be pushed to a shared replay buffer.
//     int rank0Count = 0;
//     for (const auto& ind : population_) {
//         if (ind.rank == 0) ++rank0Count;
//     }
//     spdlog::debug("[GeneticAlgorithm] ERL: {} Rank-0 solutions available for RL seeding (gen {})",
//                   rank0Count, generation_);
//     (void)snapshot;  // suppress unused warning until RL buffer is integrated
// }



// // ===================================================================
// // logParetoStats: generation summary for PhD documentation
// // ===================================================================
// void GeneticAlgorithm::logParetoStats() {
//     if (population_.empty()) return;

//     int rank0Count = 0;
//     double bestFitness = -std::numeric_limits<double>::max();
//     double avgFitness  = 0.0;

//     for (const auto& ind : population_) {
//         if (ind.rank == 0) ++rank0Count;
//         if (ind.fitness > bestFitness) bestFitness = ind.fitness;
//         avgFitness += ind.fitness;
//     }
//     avgFitness /= static_cast<double>(population_.size());

//     spdlog::info("[GA-Pareto] Gen {} | Pop={} | Rank-0={} | BestFit={:.4f} | AvgFit={:.4f}",
//                  generation_, population_.size(), rank0Count, bestFitness, avgFitness);
// }



//     bool GeneticAlgorithm::isSnapshotStable(const hrl::MetricsSnapshot& snap) {
//         if (!snap.valid || snap.fps < cfg_.minFpsForEvolution) {
//             stableFrames_ = 0;
//             return false;
//         }

//         if (cfg_.requirePowerForEvolution && snap.avg_power_w_alg < cfg_.minPowerForEvolution) {
//             stableFrames_ = 0;
//             return false;
//         }

//         stableFrames_++;
//         return stableFrames_ >= std::max(1, cfg_.stableFramesRequired);
//     }

//     void GeneticAlgorithm::initializePopulation() {
//         population_.resize(cfg_.popSize);
//         std::uniform_real_distribution<double> fpsDist(cfg_.minTargetFps, cfg_.maxTargetFps);
//         std::uniform_int_distribution<int> modeDist(1, 3);
//         std::bernoulli_distribution gpuBias(0.65);

//         // [P0] Discrete Jetson Nano CPU DVFS steps (kHz). Seeding the frequency
//         // gene across these lets the Pareto front span the full clock range
//         // instead of collapsing onto the 102 MHz floor via PolicyMode alone.
//         static const long kFreqSteps[] = {102000, 460800, 921600, 1190400, 1479000};
//         std::uniform_int_distribution<int> freqIdx(0, 4);
//         std::uniform_real_distribution<double> splitDist(0.0, 1.0);

//         for (size_t i = 0; i < cfg_.popSize; ++i) {
//             auto& ind = population_[i];
//             ind.genome.mode = static_cast<PolicyMode>(modeDist(rng_));
//             ind.genome.target_fps = MaybeDouble(fpsDist(rng_));
//             ind.genome.prefer_gpu = MaybeBool(gpuBias(rng_));
//             ind.genome.power_budget_watts = MaybeDouble(3.0 + rng_() % 5);
//             ind.genome.cpu_max_freq_khz =
//                 MaybeDouble(static_cast<double>(kFreqSteps[freqIdx(rng_)]));
//             // [Micro-scheduler] Continuous GPU workload fraction, uniform in [0,1].
//             ind.genome.gpu_workload_split = MaybeDouble(splitDist(rng_));
//             canonicalizeGenomeForWorkload(ind.genome);
//         }
//         spdlog::info("[GeneticAlgorithm] Initialized population of size {} "
//                      "(target_fps range {:.0f}-{:.0f}, freq gene enabled)",
//                      cfg_.popSize, cfg_.minTargetFps, cfg_.maxTargetFps);
//     }

//     // ===================================================================
//     // [Adaptive mutation] Plateau-driven mutation-rate control.
//     // Ramps mutation up when best fitness stagnates, eases it down when
//     // fitness improves. Operates only on the rate ? selection is untouched,
//     // so this is safe to run alongside NSGA-II non-dominated sorting.
//     // Directly targets the observed failure mode (best fitness drifting
//     // negative after ~gen 500 with no escape mechanism).
//     // ===================================================================
//     double GeneticAlgorithm::updateAdaptiveMutationRate(double currentBestFitness) {
//         if (!cfg_.adaptiveMutationEnabled) {
//             currentMutationRate_ = cfg_.mutationRate;
//             return currentMutationRate_;
//         }

//         // First call: just seed the baseline, no adjustment yet.
//         if (prevBestFitness_ == -std::numeric_limits<double>::max()) {
//             prevBestFitness_ = currentBestFitness;
//             currentMutationRate_ = cfg_.mutationRate;
//             return currentMutationRate_;
//         }

//         const double improvement = currentBestFitness - prevBestFitness_;
//         prevBestFitness_ = currentBestFitness;

//         // Relative threshold scaled to the fitness magnitude, with an absolute
//         // floor so near-zero fitness doesn't make the threshold collapse to 0.
//         const double scale = std::max(std::abs(currentBestFitness),
//                                       cfg_.mutationPlateauAbsFloor);
//         const double improveThreshold = cfg_.mutationPlateauRelThreshold * scale;

//         if (improvement > improveThreshold) {
//             // Improving -> exploit: ease mutation back toward (below) base.
//             mutationPlateauCounter_ = 0;
//             currentMutationRate_ = cfg_.mutationRate * cfg_.mutationImproveFactor;
//         } else {
//             // Stagnating -> explore: ramp mutation up, capped.
//             ++mutationPlateauCounter_;
//             const double factor = 1.0 + cfg_.mutationPlateauStep * mutationPlateauCounter_;
//             currentMutationRate_ = std::min(cfg_.mutationRate * factor,
//                                             cfg_.maxMutationRate);
//         }

//         // Guard rails
//         currentMutationRate_ = std::min(std::max(currentMutationRate_, 0.0),
//                                         cfg_.maxMutationRate);
//         return currentMutationRate_;
//     }

// // ===================================================================
// // IMPLEMENTATIONS (NSGA-II + Dominance)
// // ===================================================================
// //void GeneticAlgorithm::nonDominatedSortAndCrowding() { /* your existing correct implementation */ }
// //void GeneticAlgorithm::calculateCrowdingDistance(std::vector<int>& front) { /* your existing correct implementation */ }
// //const ParetoIndividual& GeneticAlgorithm::tournamentSelect(int tournamentSize) { /* your existing correct implementation */ }


// // IMPLEMENTATION DETAILS BELOW (can be moved to .cpp if desired)

// /*
// 1. Fast Non-Dominated Sort
// This function separates your population into "Fronts" (Rank 0 is the true Pareto front, Rank 1 is the next best, etc.).
//  It uses a helper lambda to determine if Individual A completely dominates Individual B.
// */
// void GeneticAlgorithm::nonDominatedSortAndCrowding() {
//     size_t n = population_.size();
    
//     // S[i] contains a list of indices of individuals that individual 'i' dominates
//     std::vector<std::vector<int>> S(n);
//     // n_dom[i] is the domination count: how many individuals dominate individual 'i'
//     std::vector<int> n_dom(n, 0);
    
//     std::vector<std::vector<int>> fronts;
//     std::vector<int> currentFront;

//     // Helper: A dominates B if A is <= B in all objectives AND A < B in at least one
//     auto dominates = [&](const ParetoIndividual& a, const ParetoIndividual& b) {
//         bool strictlyBetter = false;
//         for (size_t m = 0; m < a.objectives.size(); ++m) {
//             if (a.objectives[m] > b.objectives[m]) return false; // a is worse in at least one
//             if (a.objectives[m] < b.objectives[m]) strictlyBetter = true;
//         }
//         return strictlyBetter;
//     };

//     // 1. Calculate domination counts and build the first front
//     for (size_t p = 0; p < n; ++p) {
//         S[p].clear();
//         n_dom[p] = 0;
//         for (size_t q = 0; q < n; ++q) {
//             if (p == q) continue;
            
//             if (dominates(population_[p], population_[q])) {
//                 S[p].push_back(q);
//             } else if (dominates(population_[q], population_[p])) {
//                 n_dom[p]++;
//             }
//         }
        
//         // If nothing dominates p, it belongs to the first Pareto front (Rank 0)
//         if (n_dom[p] == 0) {
//             population_[p].rank = 0;
//             currentFront.push_back(p);
//         }
//     }

//     fronts.push_back(currentFront);

//     // 2. Build subsequent fronts
//     int i = 0;
//     while (!fronts[i].empty()) {
//         std::vector<int> nextFront;
//         for (int p : fronts[i]) {
//             for (int q : S[p]) {
//                 n_dom[q]--; // Since p was in the previous front, remove its domination effect
//                 if (n_dom[q] == 0) {
//                     population_[q].rank = i + 1;
//                     nextFront.push_back(q);
//                 }
//             }
//         }
//         i++;
//         if (!nextFront.empty()) {
//             fronts.push_back(nextFront);
//         } else {
//             break; // All individuals sorted
//         }
//     }

//     // 3. Assign Crowding Distance for each front independently
//     for (auto& front : fronts) {
//         if (front.empty()) continue;
//         calculateCrowdingDistance(front); // Call our helper
//     }

//     // [P0-F16] CRITICAL: order the population by the NSGA-II crowded
//     // comparison (rank asc, crowding desc - ParetoIndividual::operator<).
//     // selectEliteAndOffspring() takes the first eliteCount entries as
//     // "elites" and reducePopulation() truncates from the back; both silently
//     // assumed this ordering. Without it, elites were arbitrary individuals
//     // and reduction could duplicate rank-0 members via index-based fill.
//     // (The per-front index vectors above are not used after this point, so
//     // invalidating them by reordering population_ is safe.)
//     std::sort(population_.begin(), population_.end());
// }

// /*
// 2. Crowding Distance Calculation
// I separated this into a helper function calculateCrowdingDistance(std::vector<int>& front). You should declare this as a private function in your header. 
// It ensures that boundary individuals (the absolute best at a specific objective) are always preserved.
// */

// // Add this declaration to your .h file:
// // void calculateCrowdingDistance(std::vector<int>& front);

// void GeneticAlgorithm::calculateCrowdingDistance(std::vector<int>& front) {
//     size_t l = front.size();
    
//     // Initialize crowding distance to 0 for everyone in the current front
//     for (int idx : front) {
//         population_[idx].crowdingDistance = 0.0;
//     }
    
//     // If 1 or 2 items, they are boundaries by default. Preserve them with infinity.
//     if (l <= 2) {
//         for (int idx : front) {
//             population_[idx].crowdingDistance = std::numeric_limits<double>::infinity();
//         }
//         return;
//     }

//     size_t numObjectives = population_[front[0]].objectives.size();
    
//     // Calculate density across each objective axis independently
//     for (size_t m = 0; m < numObjectives; ++m) {
//         // Sort the current front based solely on objective 'm'
//         std::sort(front.begin(), front.end(), [&](int a, int b) {
//             return population_[a].objectives[m] < population_[b].objectives[m];
//         });

//         // The extremes of the front get infinite distance (guaranteed survival)
//         population_[front[0]].crowdingDistance = std::numeric_limits<double>::infinity();
//         population_[front[l - 1]].crowdingDistance = std::numeric_limits<double>::infinity();

//         double objMin = population_[front[0]].objectives[m];
//         double objMax = population_[front[l - 1]].objectives[m];
//         double range = objMax - objMin;

//         if (range == 0.0) continue; // Prevent division by zero if all values are identical

//         // For all intermediate individuals, calculate normalized distance to neighbors
//         for (size_t j = 1; j < l - 1; ++j) {
//             if (population_[front[j]].crowdingDistance != std::numeric_limits<double>::infinity()) {
//                 double diff = population_[front[j + 1]].objectives[m] - population_[front[j - 1]].objectives[m];
//                 population_[front[j]].crowdingDistance += diff / range;
//             }
//         }
//     }
// }

// /*
// 3. NSGA-II Tournament Selection (Crowded-Comparison Operator)
// This selection logic acts on the operator< you already defined in ParetoIndividual, 
// ensuring the algorithm prefers lower ranks, but breaks ties by preferring higher crowding distances (exploring less-crowded areas of the Pareto front).
// */
// const ParetoIndividual& GeneticAlgorithm::tournamentSelect(int tournamentSize) {
//     std::uniform_int_distribution<size_t> dist(0, population_.size() - 1);
    
//     size_t bestIdx = dist(rng_);

//     for (int i = 1; i < tournamentSize; ++i) {
//         size_t contenderIdx = dist(rng_);
        
//         const auto& best = population_[bestIdx];
//         const auto& contender = population_[contenderIdx];

//         // NSGA-II Crowded-Comparison Operator:
//         // 1. Better Rank (Lower is better) wins.
//         if (contender.rank < best.rank) {
//             bestIdx = contenderIdx;
//         } 
//         // 2. If ranks are tied, pick the one in the less crowded region (Higher distance)
//         else if (contender.rank == best.rank) {
//             if (contender.crowdingDistance > best.crowdingDistance) {
//                 bestIdx = contenderIdx;
//             }
//         }
//     }
    
//     return population_[bestIdx];
// }

// // ===================================================================
// // Scalarized fitness: weighted combination of normalized objectives
// // Weights: FPS=50%, Power=20%, Temperature=20%, Latency=10%
// // ===================================================================
// double GeneticAlgorithm::calculateScalarizedFitness(const std::vector<double>& objs,
//                                                      const hrl::MetricsSnapshot& snap,
//                                                      const RLAction& action,
//                                                      int age,
//                                                      ParetoIndividual* auditOut) const {
//     if (objs.size() < 4) return -std::numeric_limits<double>::max();

//     // objs = [normFPS, normEnergy, normTemp, normLat] (all minimized).
//     // This exact authoritative weight vector is also persisted in the candidate.
//     std::array<double, 4> w = {cfg_.weight_fps, cfg_.weight_power,
//                                cfg_.weight_temp, cfg_.weight_latency};
//     if (weightManager_) {
//         w = weightManager_->getWeights();
//     }

//     const double fpsComponent   = -objs[0] * w[0];
//     const double powerComponent = -objs[1] * w[1];
//     const double tempComponent  = -objs[2] * w[2];
//     const double latComponent   = -objs[3] * w[3];

//     double gpuBonus = 0.0;
//     if (action.prefer_gpu.has && action.prefer_gpu.value &&
//         snap.gpu_util_avg > 15.0 && snap.gpu_util_avg < 85.0) {
//         gpuBonus = 0.05;
//     }

//     const double beforeAge = fpsComponent + powerComponent +
//                              tempComponent + latComponent + gpuBonus;
//     const double ageFactor = std::pow(0.985, static_cast<double>(age) / 50.0);
//     const double afterAge = beforeAge * ageFactor;

//     if (auditOut) {
//         auditOut->fitnessWeights = w;
//         auditOut->fitFpsComponent = fpsComponent;
//         auditOut->fitPowerComponent = powerComponent;
//         auditOut->fitTempComponent = tempComponent;
//         auditOut->fitLatencyComponent = latComponent;
//         auditOut->fitGpuBonus = gpuBonus;
//         auditOut->fitBeforeAge = beforeAge;
//         auditOut->fitAgeFactor = ageFactor;
//         auditOut->fitAfterAge = afterAge;
//         auditOut->fitFinal = afterAge; // exploration bonus is appended by caller
//     }

//     return afterAge;
// }


//     // double calculateScalarizedFitness(const hrl::MetricsSnapshot& snap,
//     //                                   const RLAction& action,
//     //                                   const RuntimeControls& /*controls*/) {
//     //     double f = 0.0;
//     //     if (action.target_fps.has) {
//     //         double err = std::abs(snap.fps - action.target_fps.value);
//     //         f += 0.50 * (1.0 / (1.0 + err));
//     //     }
//     //     f += -0.20 * snap.avg_power_w_alg;

//     //     if (snap.cpu_temp_c > 75.0) f += -2.0 * (snap.cpu_temp_c - 75.0);
//     //     if (snap.gpu_temp_c > 80.0) f += -2.0 * (snap.gpu_temp_c - 80.0);

//     //     f += -0.10 * snap.avg_latency_ms / 50.0;

//     //     if (action.prefer_gpu.has && action.prefer_gpu.value &&
//     //         snap.gpu_util_avg > 15.0 && snap.gpu_util_avg < 85.0) {
//     //         f += 0.20;
//     //     }

//     //     f *= std::pow(0.985, generation_ / 50.0);
//     //     return f;
//     // }


// //=======================================================================================================================================================================================
// void GeneticAlgorithm::logDominanceAnalysis() {
//     if (population_.size() < 2) return;
    
//     spdlog::info("\n===== PARETO FRONT ANALYSIS (Generation {}) =====", generation_);
    
//     std::vector<size_t> frontIndices;
//     for (size_t i = 0; i < population_.size(); ++i) {
//         if (population_[i].rank == 0) frontIndices.push_back(i);
//     }
    
//     for (size_t idx : frontIndices) {
//         const auto& ind = population_[idx];
//         spdlog::info("  [Rank-0] ID={} | Fitness={:.4f} | FPS Goal={:.1f} | Power={:.2f}W | "
//                      "Temp Penalty={:.2f} | Latency={:.2f}ms | Crowding Dist={:.4f}",
//                      idx, ind.fitness,
//                      ind.genome.target_fps.has ? ind.genome.target_fps.value : 0.0,
//                      ind.objectives[1] * 10.0,
//                      ind.objectives[2] * 20.0,
//                      ind.objectives[3] * 50.0,
//                      ind.crowdingDistance);
//     }
    
//     spdlog::info("\n  ===== DOMINANCE COMPARISON =====");
//     if (!frontIndices.empty() && population_.size() > frontIndices.size()) {
//         const auto& best = population_[frontIndices[0]];
//         for (size_t i = frontIndices.size(); i < std::min(size_t(3), population_.size()); ++i) {
//             const auto& challenger = population_[i];
//             spdlog::info("  [Rank-{}] vs [Rank-0]: {} dominates because:",
//                          challenger.rank, best.dominates(challenger) ? "Best" : "Trade-off");
            
//             const char* objName[] = {"FPS", "Energy", "Temperature", "Latency"};
//             for (size_t obj = 0; obj < best.objectives.size(); ++obj) {
//                 spdlog::info("    - {}: Best={:.3f} vs Challenger={:.3f} {}",
//                              objName[obj], best.objectives[obj], challenger.objectives[obj],
//                              best.objectives[obj] <= challenger.objectives[obj] ? "? Better" : "? Worse");
//             }
//         }
//     }
//     spdlog::info("\n");
// }

// // ===================================================================
// // Side-effect-free simulation of runtime controls for GA evaluation.
// // This avoids touching real sysfs/GPU/DVFS while evaluating candidates.
// // ===================================================================
// void GeneticAlgorithm::simulateRuntimeControlsFromAction(
//     const RLAction& action,
//     RuntimeControls& simControls) const
// {
//     int concurrency = 2;
//     bool enableGpu = true;
//     Affinity affinity = Affinity::Spread;

//     switch (action.mode) {
//         case PolicyMode::MAX_PERFORMANCE:
//             concurrency = 4;
//             enableGpu = true;
//             affinity = Affinity::Spread;
//             break;

//         case PolicyMode::LOW_POWER:
//             concurrency = 1;
//             enableGpu = false;
//             affinity = Affinity::Pack;
//             break;

//         case PolicyMode::BALANCED:
//             concurrency = 2;
//             enableGpu = action.prefer_gpu.has ? action.prefer_gpu.value : true;
//             affinity = Affinity::Spread;
//             break;

//         case PolicyMode::UNKNOWN:
//         default:
//             concurrency = 2;
//             enableGpu = true;
//             affinity = Affinity::Spread;
//             break;
//     }

//     if (action.prefer_gpu.has) {
//         enableGpu = action.prefer_gpu.value;
//     }

//     simControls.concurrency_level.store(concurrency, std::memory_order_relaxed);
//     simControls.enable_gpu.store(enableGpu, std::memory_order_relaxed);
//     simControls.affinity.store(affinity, std::memory_order_relaxed);
// }

// // ===================================================================
// // Select the best evaluated Rank-0 Pareto individual.
// // ===================================================================
// const ParetoIndividual* GeneticAlgorithm::selectBestEvaluatedPareto() const
// {
//     const ParetoIndividual* selected = nullptr;

//     for (const auto& ind : population_) {
//         if (ind.rank == 0) {
//             if (!selected || ind.fitness > selected->fitness) {
//                 selected = &ind;
//             }
//         }
//     }

//     if (!selected && !population_.empty()) {
//         selected = &population_[0];
//     }

//     return selected;
// }

// // ===================================================================
// // Apply selected evaluated Pareto individual to real runtime controls.
// // ===================================================================
// double GeneticAlgorithm::applySelectedParetoToRuntime(
//     const ParetoIndividual& selected,
//     const hrl::MetricsSnapshot& snapshot)
// {
//     if (!runtimeControls_) {
//         return 0.0;
//     }

//     // // [Surrogate] Remember what we are about to apply; next cycle's snapshot is
//     // // this action's measured outcome and will be fed to surrogate_.observe().
//     // lastAppliedGenome_ = selected.genome;
//     // hasLastApplied_    = true;

//     // if (scheduler_) {
//     //     scheduler_->apply(selected.genome, snapshot, *runtimeControls_);
//     // }

//     // [Surrogate] Remember what we are about to apply; next cycle's snapshot is
//     // this action's measured outcome and will be fed to surrogate_.observe().
//     lastAppliedGenome_ = selected.genome;
//     hasLastApplied_    = true;

//     // [PhD FIX] Diagnostic trace for HetGB continuous split gene crash
//     if (selected.genome.gpu_workload_split.has) {
//         double safe_split = utils::local_clamp(selected.genome.gpu_workload_split.value, 0.0, 1.0);
//         spdlog::info("[GA-Split-Trace] Dispatching continuous split gene: {:.3f}", safe_split);
//     }

//     double schedulerApplyMs = 0.0;
//     if (scheduler_) {
//         const auto schedulerStart = std::chrono::steady_clock::now();
//         scheduler_->apply(selected.genome, snapshot, *runtimeControls_);
//         const auto schedulerEnd = std::chrono::steady_clock::now();
//         schedulerApplyMs = std::chrono::duration<double, std::milli>(
//             schedulerEnd - schedulerStart).count();
//     }

//     // [PhD timing] Publish a 1-based action epoch only after all scheduler
//     // actuation has completed. AlgorithmConcrete timestamps the first frame
//     // that observes this epoch, yielding true action-to-effect response time.
//     const uint64_t applyEndNs = static_cast<uint64_t>(
//         std::chrono::duration_cast<std::chrono::nanoseconds>(
//             std::chrono::steady_clock::now().time_since_epoch()).count());
//     const uint64_t actionEpoch = static_cast<uint64_t>(generation_) + 1ULL;
//     runtimeControls_->action_apply_end_ns.store(applyEndNs, std::memory_order_relaxed);
//     runtimeControls_->action_generation.store(actionEpoch, std::memory_order_release);

//     spdlog::info(
//         "[ERL SELECTED] Gen={} | Rank={} | Fit={:.4f} | mode={} | targetFPS={:.1f} | gpu={} | "
//         "fps={:.2f} | power={:.3f}W | J/frame={:.6f} | latency={:.3f}ms",
//         generation_,
//         selected.rank,
//         selected.fitness,
//         static_cast<int>(selected.genome.mode),
//         selected.genome.target_fps.has ? selected.genome.target_fps.value : 0.0,
//         selected.genome.prefer_gpu.has
//             ? (selected.genome.prefer_gpu.value ? "YES" : "NO")
//             : "AUTO",
//         snapshot.fps,
//         snapshot.avg_power_w_alg,
//         snapshot.joulesPerFrame,
//         snapshot.avg_latency_ms
//     );

//     return schedulerApplyMs;
// }


// // ===================================================================
// // Ensure output directory and evidence CSV files exist immediately.
// // This creates headers even before the first successful ERL generation.
// // ===================================================================
// bool GeneticAlgorithm::ensureParentDirectoryForFile(const std::string& filePath)
// {
//     const std::size_t slash = filePath.find_last_of("/");
//     if (slash == std::string::npos) {
//         return true;
//     }

//     const std::string dir = filePath.substr(0, slash);
//     if (dir.empty()) {
//         return true;
//     }

//     std::string current;
//     for (char c : dir) {
//         current.push_back(c);
//         if (c == '/') {
//             if (current.size() > 1) {
//                 ::mkdir(current.c_str(), 0755);
//             }
//         }
//     }

//     if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
//         spdlog::warn("[GA-Pareto] Could not create directory '{}': {}", dir, std::strerror(errno));
//         return false;
//     }
//     return true;
// }

// void GeneticAlgorithm::ensureEvidenceFiles()
// {
//     std::lock_guard<std::mutex> guard(evidenceMutex_);

//     ensureParentDirectoryForFile(cfg_.pareto_csv_path);
//     if (!std::ifstream(cfg_.pareto_csv_path).good()) {
//         std::ofstream out(cfg_.pareto_csv_path, std::ios::out);
//         if (out.is_open()) {
//             out << "schema_version,generation,individual_id,rank,fitness,crowding_distance,obj_fps,obj_energy,obj_temp,obj_latency,target_fps_has,target_fps,power_budget_has,power_budget_watts,prefer_gpu_has,prefer_gpu,gpu_workload_split_has,gpu_workload_split,gpu_split_applicable,cpu_max_freq_has,cpu_max_freq_khz,policy_mode,awm_regime,awm_detected_regime,awm_w_fps,awm_w_power,awm_w_temp,awm_w_latency,awm_trigger_field,awm_trigger_value,awm_trigger_threshold,awm_regime_streak,awm_last_update_generation,surrogate_used,surrogate_source,surrogate_confidence,surrogate_from_data,surrogate_support_count,surrogate_neighbor_count,surrogate_nearest_distance,surrogate_exploration_bonus,surrogate_stall_penalty,surrogate_config_freq_khz,surrogate_config_gpu,surrogate_config_gpu_split,surrogate_config_concurrency,pred_fps,pred_power_w,pred_latency_ms,pred_temp_c,fit_fps_component,fit_power_component,fit_temp_component,fit_latency_component,fit_gpu_bonus,fit_before_age,fit_age_factor,fit_after_age,fit_final,selected,measured_fps,measured_power_w,measured_joules_per_frame,algorithm_processing_ms,pipeline_e2e_ms,cpu_temp_c,gpu_temp_c\n";
//         } else {
//             spdlog::warn("[GA-Pareto] Could not create Pareto CSV: {}", cfg_.pareto_csv_path);
//         }
//     }

//     ensureParentDirectoryForFile(cfg_.action_csv_path);
//     if (!std::ifstream(cfg_.action_csv_path).good()) {
//         std::ofstream out(cfg_.action_csv_path, std::ios::out);
//         if (out.is_open()) {
//             out << "schema_version,generation,action_epoch,frame_id,rank0_count,selected_rank,selected_fitness,rank0_runnerup_fitness,fitness_margin,policy_mode,target_fps_has,target_fps,power_budget_has,power_budget_watts,prefer_gpu_has,prefer_gpu,gpu_workload_split_has,gpu_workload_split,gpu_split_applicable,cpu_max_freq_has,cpu_max_freq_gene_khz,cpu_max_freq_commanded_khz,runtime_enable_gpu,runtime_gpu_workload_split,runtime_concurrency,runtime_affinity,awm_regime,awm_detected_regime,awm_w_fps,awm_w_power,awm_w_temp,awm_w_latency,awm_trigger_field,awm_trigger_value,awm_trigger_threshold,awm_regime_streak,awm_last_update_generation,surrogate_used,surrogate_source,surrogate_confidence,surrogate_from_data,surrogate_support_count,surrogate_neighbor_count,surrogate_nearest_distance,surrogate_exploration_bonus,surrogate_stall_penalty,surrogate_config_freq_khz,surrogate_config_gpu,surrogate_config_gpu_split,surrogate_config_concurrency,pred_fps,pred_power_w,pred_latency_ms,pred_temp_c,obj_fps,obj_energy,obj_temp,obj_latency,fit_fps_component,fit_power_component,fit_temp_component,fit_latency_component,fit_gpu_bonus,fit_before_age,fit_age_factor,fit_after_age,fit_final,measured_fps,measured_power_w,measured_joules_per_frame,algorithm_processing_ms,pipeline_e2e_ms,ga_compute_total_ms,ga_weight_update_ms,ga_evaluation_ms,ga_nsga_sort_ms,ga_selection_ms,ga_scheduler_apply_ms,ga_action_dispatch_overhead_ms,ga_pareto_export_ms,ga_diagnostics_ms,ga_mutation_ms,ga_offspring_ms,ga_postprocess_ms,action_apply_end_ns,algorithm_observed_epoch,algorithm_observed_time_ns,action_response_epoch,action_response_ms,cpu_temp_c,gpu_temp_c\n";
//         } else {
//             spdlog::warn("[GA-Pareto] Could not create action CSV: {}", cfg_.action_csv_path);
//         }
//     }
// }

// // ===================================================================
// // Count Rank-0 solutions.
// // ===================================================================
// int GeneticAlgorithm::countRank0() const
// {
//     int count = 0;

//     for (const auto& ind : population_) {
//         if (ind.rank == 0) {
//             ++count;
//         }
//     }

//     return count;
// }

// // ===================================================================
// // Export full Pareto front evidence.
// // ===================================================================
// void GeneticAlgorithm::exportParetoEvidence(
//     const hrl::MetricsSnapshot& snap,
//     const ParetoIndividual* selected)
// {
//     if (!cfg_.export_pareto_evidence) return;

//     std::lock_guard<std::mutex> guard(evidenceMutex_);
//     ensureParentDirectoryForFile(cfg_.pareto_csv_path);
//     const bool writeHeader = !std::ifstream(cfg_.pareto_csv_path).good();
//     std::ofstream out(cfg_.pareto_csv_path, std::ios::app);
//     if (!out.is_open()) {
//         spdlog::warn("[GA-Pareto] Could not open Pareto CSV: {}", cfg_.pareto_csv_path);
//         return;
//     }
//     if (writeHeader) out << "schema_version,generation,individual_id,rank,fitness,crowding_distance,obj_fps,obj_energy,obj_temp,obj_latency,target_fps_has,target_fps,power_budget_has,power_budget_watts,prefer_gpu_has,prefer_gpu,gpu_workload_split_has,gpu_workload_split,gpu_split_applicable,cpu_max_freq_has,cpu_max_freq_khz,policy_mode,awm_regime,awm_detected_regime,awm_w_fps,awm_w_power,awm_w_temp,awm_w_latency,awm_trigger_field,awm_trigger_value,awm_trigger_threshold,awm_regime_streak,awm_last_update_generation,surrogate_used,surrogate_source,surrogate_confidence,surrogate_from_data,surrogate_support_count,surrogate_neighbor_count,surrogate_nearest_distance,surrogate_exploration_bonus,surrogate_stall_penalty,surrogate_config_freq_khz,surrogate_config_gpu,surrogate_config_gpu_split,surrogate_config_concurrency,pred_fps,pred_power_w,pred_latency_ms,pred_temp_c,fit_fps_component,fit_power_component,fit_temp_component,fit_latency_component,fit_gpu_bonus,fit_before_age,fit_age_factor,fit_after_age,fit_final,selected,measured_fps,measured_power_w,measured_joules_per_frame,algorithm_processing_ms,pipeline_e2e_ms,cpu_temp_c,gpu_temp_c\n";

//     const std::string awmRegime = weightManager_ ? weightManager_->getCurrentRegimeName() : "FIXED_WEIGHTS";
//     const std::string awmDetected = weightManager_ ? weightManager_->getLastDetectedRegimeName() : "FIXED_WEIGHTS";
//     const std::string awmTriggerField = weightManager_ ? weightManager_->getLastTriggerField() : "none";
//     const double awmTriggerValue = weightManager_ ? weightManager_->getLastTriggerValue() : 0.0;
//     const double awmTriggerThreshold = weightManager_ ? weightManager_->getLastTriggerThreshold() : 0.0;
//     const int awmStreak = weightManager_ ? weightManager_->getRegimeStreak() : 0;
//     const int awmLastUpdate = weightManager_ ? weightManager_->getLastUpdateGeneration() : -1;

//     for (size_t i = 0; i < population_.size(); ++i) {
//         const auto& ind = population_[i];
//         const bool isSelected = selected && (&ind == selected);

//         const double targetFps = ind.genome.target_fps.has ? ind.genome.target_fps.value : 0.0;
//         const double powerBudget = ind.genome.power_budget_watts.has ? ind.genome.power_budget_watts.value : 0.0;
//         const int preferGpu = ind.genome.prefer_gpu.has ? (ind.genome.prefer_gpu.value ? 1 : 0) : -1;
//         const double splitGene = ind.genome.gpu_workload_split.has ? ind.genome.gpu_workload_split.value : 0.0;
//         const double cpuFreqGene = ind.genome.cpu_max_freq_khz.has ? ind.genome.cpu_max_freq_khz.value : 0.0;

//         out << 2 << ","
//             << generation_ << "," << i << "," << ind.rank << ","
//             << ind.fitness << "," << ind.crowdingDistance << ",";

//         if (ind.objectives.size() >= 4) {
//             out << ind.objectives[0] << "," << ind.objectives[1] << ","
//                 << ind.objectives[2] << "," << ind.objectives[3] << ",";
//         } else {
//             out << "0,0,0,0,";
//         }

//         out << (ind.genome.target_fps.has ? 1 : 0) << "," << targetFps << ","
//             << (ind.genome.power_budget_watts.has ? 1 : 0) << "," << powerBudget << ","
//             << (ind.genome.prefer_gpu.has ? 1 : 0) << "," << preferGpu << ","
//             << (ind.genome.gpu_workload_split.has ? 1 : 0) << "," << splitGene << ","
//             << (cfg_.gpuSplitApplicable ? 1 : 0) << ","
//             << (ind.genome.cpu_max_freq_khz.has ? 1 : 0) << "," << cpuFreqGene << ","
//             << static_cast<int>(ind.genome.mode) << ","
//             << awmRegime << "," << awmDetected << ","
//             << ind.fitnessWeights[0] << "," << ind.fitnessWeights[1] << ","
//             << ind.fitnessWeights[2] << "," << ind.fitnessWeights[3] << ","
//             << awmTriggerField << "," << awmTriggerValue << "," << awmTriggerThreshold << ","
//             << awmStreak << "," << awmLastUpdate << ","
//             << (ind.surrogateUsed ? 1 : 0) << ","
//             << SurrogateModel::sourceName(ind.surrogatePrediction.source) << ","
//             << ind.surrogatePrediction.confidence << ","
//             << (ind.surrogatePrediction.from_data ? 1 : 0) << ","
//             << ind.surrogatePrediction.support_count << ","
//             << ind.surrogatePrediction.neighbor_count << ","
//             << ind.surrogatePrediction.nearest_distance << ","
//             << ind.surrogateExplorationBonus << ","
//             << ind.surrogateStallPenalty << ","
//             << ind.surrogateConfigFreqKhz << ","
//             << (ind.surrogateConfigGpu ? 1 : 0) << ","
//             << ind.surrogateConfigGpuSplit << ","
//             << ind.surrogateConfigConcurrency << ","
//             << ind.surrogatePrediction.fps << ","
//             << ind.surrogatePrediction.power_w << ","
//             << ind.surrogatePrediction.latency_ms << ","
//             << ind.surrogatePrediction.temp_c << ","
//             << ind.fitFpsComponent << "," << ind.fitPowerComponent << ","
//             << ind.fitTempComponent << "," << ind.fitLatencyComponent << ","
//             << ind.fitGpuBonus << "," << ind.fitBeforeAge << ","
//             << ind.fitAgeFactor << "," << ind.fitAfterAge << "," << ind.fitFinal << ","
//             << (isSelected ? 1 : 0) << ","
//             << snap.fps << "," << snap.avg_power_w_alg << "," << snap.joulesPerFrame << ","
//             << snap.avg_inference_ms << "," << snap.end_to_end_latency_ms << ","
//             << snap.cpu_temp_c << "," << snap.gpu_temp_c << "\n";
//     }
// }

// // ===================================================================
// // Export selected action trace.
// // ===================================================================
// void GeneticAlgorithm::exportSelectedActionEvidence(
//     const hrl::MetricsSnapshot& snap,
//     const ParetoIndividual& selected)
// {
//     if (!cfg_.export_pareto_evidence) return;

//     std::lock_guard<std::mutex> guard(evidenceMutex_);
//     ensureParentDirectoryForFile(cfg_.action_csv_path);
//     const bool writeHeader = !std::ifstream(cfg_.action_csv_path).good();
//     std::ofstream out(cfg_.action_csv_path, std::ios::app);
//     if (!out.is_open()) {
//         spdlog::warn("[GA-Pareto] Could not open action CSV: {}", cfg_.action_csv_path);
//         return;
//     }
//     if (writeHeader) out << "schema_version,generation,action_epoch,frame_id,rank0_count,selected_rank,selected_fitness,rank0_runnerup_fitness,fitness_margin,policy_mode,target_fps_has,target_fps,power_budget_has,power_budget_watts,prefer_gpu_has,prefer_gpu,gpu_workload_split_has,gpu_workload_split,gpu_split_applicable,cpu_max_freq_has,cpu_max_freq_gene_khz,cpu_max_freq_commanded_khz,runtime_enable_gpu,runtime_gpu_workload_split,runtime_concurrency,runtime_affinity,awm_regime,awm_detected_regime,awm_w_fps,awm_w_power,awm_w_temp,awm_w_latency,awm_trigger_field,awm_trigger_value,awm_trigger_threshold,awm_regime_streak,awm_last_update_generation,surrogate_used,surrogate_source,surrogate_confidence,surrogate_from_data,surrogate_support_count,surrogate_neighbor_count,surrogate_nearest_distance,surrogate_exploration_bonus,surrogate_stall_penalty,surrogate_config_freq_khz,surrogate_config_gpu,surrogate_config_gpu_split,surrogate_config_concurrency,pred_fps,pred_power_w,pred_latency_ms,pred_temp_c,obj_fps,obj_energy,obj_temp,obj_latency,fit_fps_component,fit_power_component,fit_temp_component,fit_latency_component,fit_gpu_bonus,fit_before_age,fit_age_factor,fit_after_age,fit_final,measured_fps,measured_power_w,measured_joules_per_frame,algorithm_processing_ms,pipeline_e2e_ms,ga_compute_total_ms,ga_weight_update_ms,ga_evaluation_ms,ga_nsga_sort_ms,ga_selection_ms,ga_scheduler_apply_ms,ga_action_dispatch_overhead_ms,ga_pareto_export_ms,ga_diagnostics_ms,ga_mutation_ms,ga_offspring_ms,ga_postprocess_ms,action_apply_end_ns,algorithm_observed_epoch,algorithm_observed_time_ns,action_response_epoch,action_response_ms,cpu_temp_c,gpu_temp_c\n";

//     const int rtGpu = runtimeControls_
//         ? (runtimeControls_->enable_gpu.load(std::memory_order_relaxed) ? 1 : 0) : -1;
//     const double rtSplit = runtimeControls_
//         ? runtimeControls_->gpu_workload_split.load(std::memory_order_relaxed) : -1.0;
//     const int rtConcurrency = runtimeControls_
//         ? runtimeControls_->concurrency_level.load(std::memory_order_relaxed) : -1;
//     const int rtAffinity = runtimeControls_
//         ? static_cast<int>(runtimeControls_->affinity.load(std::memory_order_relaxed)) : -1;
//     const long appliedCpuKhz = runtimeControls_
//         ? runtimeControls_->commanded_cpu_max_freq_khz.load(std::memory_order_relaxed) : 0L;

//     const uint64_t actionEpoch = runtimeControls_
//         ? runtimeControls_->action_generation.load(std::memory_order_acquire)
//         : static_cast<uint64_t>(generation_) + 1ULL;
//     const uint64_t applyEndNs = runtimeControls_
//         ? runtimeControls_->action_apply_end_ns.load(std::memory_order_acquire) : 0ULL;
//     const uint64_t observedEpoch = runtimeControls_
//         ? runtimeControls_->algorithm_observed_generation.load(std::memory_order_acquire) : 0ULL;
//     const uint64_t observedNs = runtimeControls_
//         ? runtimeControls_->algorithm_observed_time_ns.load(std::memory_order_acquire) : 0ULL;
//     const uint64_t responseEpoch = runtimeControls_
//         ? runtimeControls_->action_response_generation.load(std::memory_order_acquire) : 0ULL;
//     const uint64_t responseNs = runtimeControls_
//         ? runtimeControls_->action_response_latency_ns.load(std::memory_order_acquire) : 0ULL;
//     const double actionResponseMs = responseEpoch > 0
//         ? static_cast<double>(responseNs) / 1.0e6 : -1.0;

//     const std::string awmRegime = weightManager_ ? weightManager_->getCurrentRegimeName() : "FIXED_WEIGHTS";
//     const std::string awmDetected = weightManager_ ? weightManager_->getLastDetectedRegimeName() : "FIXED_WEIGHTS";
//     const std::string awmTriggerField = weightManager_ ? weightManager_->getLastTriggerField() : "none";
//     const double awmTriggerValue = weightManager_ ? weightManager_->getLastTriggerValue() : 0.0;
//     const double awmTriggerThreshold = weightManager_ ? weightManager_->getLastTriggerThreshold() : 0.0;
//     const int awmStreak = weightManager_ ? weightManager_->getRegimeStreak() : 0;
//     const int awmLastUpdate = weightManager_ ? weightManager_->getLastUpdateGeneration() : -1;

//     const double targetFps = selected.genome.target_fps.has ? selected.genome.target_fps.value : 0.0;
//     const double powerBudget = selected.genome.power_budget_watts.has ? selected.genome.power_budget_watts.value : 0.0;
//     const int preferGpu = selected.genome.prefer_gpu.has ? (selected.genome.prefer_gpu.value ? 1 : 0) : -1;
//     const double splitGene = selected.genome.gpu_workload_split.has ? selected.genome.gpu_workload_split.value : 0.0;
//     const double cpuFreqGene = selected.genome.cpu_max_freq_khz.has ? selected.genome.cpu_max_freq_khz.value : 0.0;

//     const double obj0 = selected.objectives.size() > 0 ? selected.objectives[0] : 0.0;
//     const double obj1 = selected.objectives.size() > 1 ? selected.objectives[1] : 0.0;
//     const double obj2 = selected.objectives.size() > 2 ? selected.objectives[2] : 0.0;
//     const double obj3 = selected.objectives.size() > 3 ? selected.objectives[3] : 0.0;

//     out << 2 << ","
//         << generation_ << "," << actionEpoch << "," << snap.frameId << ","
//         << lastEvaluatedRank0Count_ << "," << selected.rank << "," << selected.fitness << ","
//         << lastRank0RunnerUpFitness_ << "," << lastSelectionMargin_ << ","
//         << static_cast<int>(selected.genome.mode) << ","
//         << (selected.genome.target_fps.has ? 1 : 0) << "," << targetFps << ","
//         << (selected.genome.power_budget_watts.has ? 1 : 0) << "," << powerBudget << ","
//         << (selected.genome.prefer_gpu.has ? 1 : 0) << "," << preferGpu << ","
//         << (selected.genome.gpu_workload_split.has ? 1 : 0) << "," << splitGene << ","
//         << (cfg_.gpuSplitApplicable ? 1 : 0) << ","
//         << (selected.genome.cpu_max_freq_khz.has ? 1 : 0) << "," << cpuFreqGene << ","
//         << appliedCpuKhz << "," << rtGpu << "," << rtSplit << ","
//         << rtConcurrency << "," << rtAffinity << ","
//         << awmRegime << "," << awmDetected << ","
//         << selected.fitnessWeights[0] << "," << selected.fitnessWeights[1] << ","
//         << selected.fitnessWeights[2] << "," << selected.fitnessWeights[3] << ","
//         << awmTriggerField << "," << awmTriggerValue << "," << awmTriggerThreshold << ","
//         << awmStreak << "," << awmLastUpdate << ","
//         << (selected.surrogateUsed ? 1 : 0) << ","
//         << SurrogateModel::sourceName(selected.surrogatePrediction.source) << ","
//         << selected.surrogatePrediction.confidence << ","
//         << (selected.surrogatePrediction.from_data ? 1 : 0) << ","
//         << selected.surrogatePrediction.support_count << ","
//         << selected.surrogatePrediction.neighbor_count << ","
//         << selected.surrogatePrediction.nearest_distance << ","
//         << selected.surrogateExplorationBonus << ","
//         << selected.surrogateStallPenalty << ","
//         << selected.surrogateConfigFreqKhz << ","
//         << (selected.surrogateConfigGpu ? 1 : 0) << ","
//         << selected.surrogateConfigGpuSplit << ","
//         << selected.surrogateConfigConcurrency << ","
//         << selected.surrogatePrediction.fps << ","
//         << selected.surrogatePrediction.power_w << ","
//         << selected.surrogatePrediction.latency_ms << ","
//         << selected.surrogatePrediction.temp_c << ","
//         << obj0 << "," << obj1 << "," << obj2 << "," << obj3 << ","
//         << selected.fitFpsComponent << "," << selected.fitPowerComponent << ","
//         << selected.fitTempComponent << "," << selected.fitLatencyComponent << ","
//         << selected.fitGpuBonus << "," << selected.fitBeforeAge << ","
//         << selected.fitAgeFactor << "," << selected.fitAfterAge << "," << selected.fitFinal << ","
//         << snap.fps << "," << snap.avg_power_w_alg << "," << snap.joulesPerFrame << ","
//         << snap.avg_inference_ms << "," << snap.end_to_end_latency_ms << ","
//         << lastTiming_.total_compute_ms << "," << lastTiming_.weight_update_ms << ","
//         << lastTiming_.evaluation_ms << "," << lastTiming_.nsga_sort_ms << ","
//         << lastTiming_.selection_ms << "," << lastTiming_.scheduler_apply_ms << ","
//         << lastTiming_.action_dispatch_overhead_ms << "," << lastTiming_.pareto_export_ms << ","
//         << lastTiming_.diagnostics_ms << "," << lastTiming_.mutation_ms << ","
//         << lastTiming_.offspring_ms << "," << lastTiming_.postprocess_ms << ","
//         << applyEndNs << "," << observedEpoch << "," << observedNs << ","
//         << responseEpoch << "," << actionResponseMs << ","
//         << snap.cpu_temp_c << "," << snap.gpu_temp_c << "\n";
// }

// } // namespace hrl
