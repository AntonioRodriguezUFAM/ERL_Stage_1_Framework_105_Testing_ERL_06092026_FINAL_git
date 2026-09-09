

// EvolutionarySelector_02.h - FINAL PhD-Ready (Best of Both Worlds)
// Pareto + Scalar Hybrid | Strong Defensive Checks | Clean Orchestrator

// EvolutionarySelector_02.h - FINAL PhD-Ready (Best of Both Worlds)
// Pareto + Scalar Hybrid | Strong Defensive Checks | Clean Orchestrator
#pragma once

#include <atomic>
#include <algorithm>
#include <chrono>
#include <thread>
#include <memory>
#include <deque>
#include <string>
#include <mutex>
#include <spdlog/spdlog.h>

#include "IModule.h"
#include "PowerSanity.h"   // [P0-F15] physical-plausibility gate for power samples
#include "RuntimeControls.h"
#include "IScheduler.h"
#include "Scheduler.h"
#include "RuntimeControls.h"   // ? ADD THIS
#include "../Stage_01/Concretes/SystemMetricsAggregatorConcrete_v3_2.h"
#include "GeneticAlgorithm_05.h"
#include "WorkloadMission.h"   // ? Pareto core (single source of truth)
#include "../Stage_01/Others/utils.h"

using namespace std::chrono_literals;

class EvolutionarySelector : public IModule {

public:
    // EvolutionarySelector(const json& cfg,
    //                      Context& ctx,
    //                      std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator,
    //                      std::shared_ptr<hrl::RuntimeControls> runtimeControls)
    EvolutionarySelector(const json& cfg, Context& ctx,
                    std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator,
                    std::shared_ptr<hrl::RuntimeControls> runtimeControls,
                    const hrl::ThermalGovernor::Config& thermalCfg,
                    const hrl::AdaptiveWeightManager::Config& awmCfg)
        : cfg_(cfg),
          ctx_(ctx),
          aggregator_(std::move(aggregator)),
          runtimeControls_(std::move(runtimeControls)),
          //scheduler_(hrl::Scheduler::Limits{}),
          // AFTER
          scheduler_(hrl::Scheduler::Limits{}, thermalCfg),
          running_(false) {

             // ====== GA Configuration (supports both old and new JSON key names) ======
            hrl::GAConfig gaCfg;

            

            // // 1. Construct the AdaptiveWeightManager using the embedded config
            // bool adaptive_weights_enabled = jBool("adaptive_weights_enabled", nullptr, false);

            // if (adaptive_weights_enabled) {
            //     // Pass the parsed struct (gaCfg.awm_config), NOT the raw json object
            //     awm_ = std::make_unique<AdaptiveWeightManager>(gaCfg.awm_config);
            //     spdlog::info("[GeneticAlgorithm] AdaptiveWeightManager initialized.");
            // } else {
            //     spdlog::info("[GeneticAlgorithm] Adaptive weights disabled by config.");
            // }
            // if (cfg_.adaptive_weights_enabled) {
            //     awm_ = std::make_unique<AdaptiveWeightManager>(cfg_.awm_config);
            //     spdlog::info("[GeneticAlgorithm] AdaptiveWeightManager initialized.");
            // } else {
            //     spdlog::info("[GeneticAlgorithm] Adaptive weights disabled by config.");
            // }

        auto jDouble = [this](const char* primary, const char* alias, double def) -> double {
            try {
                if (cfg_.contains(primary) && !cfg_[primary].is_null()) return cfg_[primary].get<double>();
                if (alias && cfg_.contains(alias) && !cfg_[alias].is_null()) return cfg_[alias].get<double>();
            } catch (...) {}
            return def;
        };

        //auto jInt = [this](const char* primary, const char* alias, int def) -> int {
        auto jInt = [this, &jDouble](const char* primary, const char* alias, int def) -> int {
            return static_cast<int>(jDouble(primary, alias, static_cast<double>(def)));
        };

        auto jBool = [this](const char* primary, const char* alias, bool def) -> bool {
            try {
                const json* v = nullptr;
                if (cfg_.contains(primary) && !cfg_[primary].is_null()) v = &cfg_[primary];
                else if (alias && cfg_.contains(alias) && !cfg_[alias].is_null()) v = &cfg_[alias];
                if (!v) return def;
                if (v->is_boolean()) return v->get<bool>();
                if (v->is_number_integer()) return v->get<int>() != 0;
                if (v->is_string()) {
                    const std::string s = v->get<std::string>();
                    return s == "true" || s == "TRUE" || s == "1" || s == "yes" || s == "ON" || s == "on";
                }
            } catch (...) {}
            return def;
        };

        auto jString = [this](const char* primary, const char* alias, const std::string& def) -> std::string {
            try {
                if (cfg_.contains(primary) && !cfg_[primary].is_null()) return cfg_[primary].get<std::string>();
                if (alias && cfg_.contains(alias) && !cfg_[alias].is_null()) return cfg_[alias].get<std::string>();
            } catch (...) {}
            return def;
        };

        // [BUILD FIX / S1 WIRING] Resolve the active workload before using it
        // to derive continuous GPU-split capability.  Framework97 previously
        // referenced `activeAlgorithm` without declaring it, which caused:
        //   error: 'activeAlgorithm' was not declared in this scope
        // ConfigManager injects `active_algorithm`; the `algorithmType` alias
        // keeps standalone/legacy construction compatible.  Empty/unknown
        // values fail closed to binary split (false).
        const std::string activeAlgorithm =
            jString("active_algorithm", "algorithmType", std::string{});

        // AdaptiveWeightManager is owned only by GeneticAlgorithm. Keeping one
        // authoritative instance is essential for proving that its regime/history
        // survives workload transitions; do not construct a shadow AWM here.


        // // ====== GA Configuration (supports both old and new JSON key names) ======
        // hrl::GAConfig gaCfg;
        gaCfg.popSize             = utils::local_clamp<size_t>(static_cast<size_t>(jInt("ga_population_size", "population_size", 12)), 4u, 64u);
        gaCfg.minPopSize          = utils::local_clamp<size_t>(static_cast<size_t>(jInt("ga_min_pop_size", nullptr, 4)), 2u, gaCfg.popSize);
        gaCfg.eliteCount          = static_cast<size_t>(utils::local_clamp<int>(jInt("ga_elite_count", "elite_count", 2), 1, static_cast<int>(gaCfg.popSize)));
        gaCfg.crossoverRate       = utils::local_clamp<double>(jDouble("ga_crossover_rate", "crossover_rate", 0.70), 0.0, 1.0);
        gaCfg.mutationRate        = utils::local_clamp<double>(jDouble("ga_mutation_rate", "mutation_rate", 0.15), 0.0, 1.0);
        gaCfg.explorationDecay    = utils::local_clamp<double>(jDouble("ga_exploration_decay", nullptr, 0.96), 0.80, 1.0);
        gaCfg.controlIntervalMs   = utils::local_clamp<int>(jInt("control_interval_ms", nullptr, 500), 50, 10000);
        gaCfg.reductionStartGen   = static_cast<size_t>(utils::local_clamp<int>(jInt("ga_reduction_start_gen", nullptr, 10), 1, 100000));
        gaCfg.reductionRatio      = utils::local_clamp<double>(jDouble("ga_reduction_ratio", nullptr, 0.60), 0.10, 1.0);

        // [Adaptive mutation] plateau-driven mutation control
        gaCfg.adaptiveMutationEnabled       = jBool("ga_adaptive_mutation_enabled", nullptr, true);
        gaCfg.maxMutationRate               = utils::local_clamp<double>(jDouble("ga_max_mutation_rate", nullptr, 0.35), gaCfg.mutationRate, 1.0);
        gaCfg.mutationPlateauRelThreshold   = utils::local_clamp<double>(jDouble("ga_mutation_plateau_rel_threshold", nullptr, 0.02), 0.0, 1.0);
        gaCfg.mutationPlateauAbsFloor       = jDouble("ga_mutation_plateau_abs_floor", nullptr, 0.0005);
        gaCfg.mutationImproveFactor         = utils::local_clamp<double>(jDouble("ga_mutation_improve_factor", nullptr, 0.80), 0.0, 1.0);
        gaCfg.mutationPlateauStep           = utils::local_clamp<double>(jDouble("ga_mutation_plateau_step", nullptr, 0.10), 0.0, 1.0);

        // [Surrogate] online counterfactual fitness (thesis gap 2)
        gaCfg.surrogateEnabled       = jBool("ga_surrogate_enabled", nullptr, true);
        gaCfg.surrogateExploreWeight = utils::local_clamp<double>(jDouble("ga_surrogate_explore_weight", nullptr, 0.15), 0.0, 5.0);

        // Runtime workload identity/capability. WorkloadMission provides one
        // stable workloadId per label, which becomes the surrogate-memory key.
        // JSON may still override the physical FPS prior for an ablation.
        const std::string initialName = activeAlgorithm.empty()
            ? std::string("UNSPECIFIED") : activeAlgorithm;
        const double initialFpsMax = jDouble("surrogate_fps_max", nullptr, 60.0);
        const hrl::WorkloadProfile initialWorkload =
            hrl::makeWorkloadProfile(initialName, true, initialFpsMax);

        gaCfg.activeWorkload = initialWorkload.label;
        const int configuredWorkloadId = jInt(
            "workload_id", nullptr, static_cast<int>(initialWorkload.workloadId));
        gaCfg.activeWorkloadId = configuredWorkloadId > 0
            ? static_cast<uint32_t>(configuredWorkloadId)
            : initialWorkload.workloadId;
        gaCfg.workloadGpuCapable = initialWorkload.gpuCapable;
        gaCfg.gpuSplitApplicable =
            jBool("gpu_split_applicable", nullptr, initialWorkload.gpuSplitApplicable);
        gaCfg.forceGpuForWorkload =
            jBool("ga_force_gpu_for_workload", nullptr, false);
        gaCfg.surrogateFpsMax = initialWorkload.fpsMax;
        gaCfg.surrogateLatencyPriorMs =
            jDouble("surrogate_latency_prior_ms", nullptr, 10.0);

        spdlog::info(
            "[EvolutionarySelector] initial workload id={} '{}' | split={} force_gpu={} "
            "surrogate_fps_max={:.1f} latency_prior={:.1f}ms",
            gaCfg.activeWorkloadId, gaCfg.activeWorkload,
            gaCfg.gpuSplitApplicable ? "continuous" : "binary",
            gaCfg.forceGpuForWorkload ? "YES" : "NO",
            gaCfg.surrogateFpsMax, gaCfg.surrogateLatencyPriorMs);

        // ADD THESE FOUR LINES:
        gaCfg.weight_fps      = utils::local_clamp<double>(jDouble("obj_weight_fps", nullptr, 1.0), 0.0, 10.0);
        gaCfg.weight_power    = utils::local_clamp<double>(jDouble("obj_weight_power", nullptr, 0.5), 0.0, 10.0);
        gaCfg.weight_temp     = utils::local_clamp<double>(jDouble("obj_weight_temp", nullptr, 0.5), 0.0, 10.0);
        gaCfg.weight_latency  = utils::local_clamp<double>(jDouble("obj_weight_latency", nullptr, 0.3), 0.0, 10.0);

        // [P0-F17] Optional per-run override of the temperature-objective
        // onset; defaults come from the calibrated AdaptiveWeightManager
        // thresholds (see GAConfig).
        gaCfg.temp_onset_cpu_c = jDouble("obj_temp_onset_cpu_c", nullptr, gaCfg.temp_onset_cpu_c);
        gaCfg.temp_onset_gpu_c = jDouble("obj_temp_onset_gpu_c", nullptr, gaCfg.temp_onset_gpu_c);

        controlIntervalMs_        = gaCfg.controlIntervalMs;
        warmupMs_                 = utils::local_clamp<int>(jInt("warmup_ms", nullptr, 5000), 0, 60000);
        minFpsUseful_             = jDouble("min_fps_for_evolution", nullptr, 1.0);
        minPowerUseful_           = jDouble("min_power_for_evolution", nullptr, 0.0);
        requirePowerForEvolution_ = jBool("require_power_for_evolution", nullptr, false);

        gaCfg.stableFramesRequired     = utils::local_clamp<int>(jInt("stable_frames_required", nullptr, 1), 1, 120);
        gaCfg.minFpsForEvolution       = minFpsUseful_;
        gaCfg.minPowerForEvolution     = minPowerUseful_;
        gaCfg.requirePowerForEvolution = requirePowerForEvolution_;

        gaCfg.adaptive_weights_enabled = jBool("adaptive_weights_enabled", nullptr, false);

        // Reproducibility: wire "seed" from config into the GA RNG.
        // Default -1 keeps non-deterministic behaviour if the key is absent.
        gaCfg.rngSeed = static_cast<long>(jInt("seed", nullptr, -1));

        // Search-space floor for target_fps gene (raised from old hardcoded 8.0).
        gaCfg.minTargetFps = utils::local_clamp<double>(jDouble("ga_min_target_fps", nullptr, 20.0), 1.0, 60.0);
        gaCfg.maxTargetFps = utils::local_clamp<double>(jDouble("ga_max_target_fps", nullptr, 60.0), gaCfg.minTargetFps, 240.0);
        gaCfg.performanceTargetFps = utils::local_clamp<double>(
            jDouble("ga_performance_target_fps", nullptr, gaCfg.maxTargetFps),
            gaCfg.minTargetFps, gaCfg.maxTargetFps);

        // PhD evidence export: ERL/Pareto traceability.
        gaCfg.export_pareto_evidence = jBool("export_pareto_evidence", nullptr, true);
        gaCfg.pareto_csv_path        = jString("pareto_csv", nullptr, "output/erl_pareto_front.csv");
        gaCfg.action_csv_path        = jString("action_csv", nullptr, "output/erl_action_trace.csv");

        spdlog::info("[EvolutionarySelector] Effective ERL config: pop={} minPop={} elite={} cross={:.2f} mut={:.2f} interval={}ms export={} pareto_csv={} action_csv={} minFPS={:.2f} requirePower={}",
                     gaCfg.popSize, gaCfg.minPopSize, gaCfg.eliteCount, gaCfg.crossoverRate, gaCfg.mutationRate,
                     gaCfg.controlIntervalMs, gaCfg.export_pareto_evidence ? "ON" : "OFF",
                     gaCfg.pareto_csv_path, gaCfg.action_csv_path, minFpsUseful_,
                     requirePowerForEvolution_ ? "YES" : "NO");

        
        // Inside EvolutionarySelector constructor, where gaCfg.awm_config is populated:
        if (gaCfg.adaptive_weights_enabled) {
            // [P0-F14] Fallbacks now defer to AdaptiveWeightManager::Config's
            // Jetson-Nano-calibrated defaults (warn 50C, crit 55C, battery
            // 7.5W, latency SLA 30ms) instead of re-imposing the original
            // unreachable thresholds which disabled the thermal/battery regimes.
            // Explicit awm_* JSON keys still override (for ablations).
            const hrl::AdaptiveWeightManager::Config awmDefaults{}; 

            // gaCfg.awm_config.update_interval_gens   = jvalue<int>(cfg, "awm_update_interval_gens", awmDefaults.update_interval_gens);
            // gaCfg.awm_config.battery_critical_watts = jvalue<double>(cfg, "awm_battery_critical_watts", awmDefaults.battery_critical_watts);
            // gaCfg.awm_config.thermal_warn_cpu_c     = jvalue<double>(cfg, "awm_thermal_warn_cpu_c", awmDefaults.thermal_warn_cpu_c);
            // gaCfg.awm_config.thermal_warn_gpu_c     = jvalue<double>(cfg, "awm_thermal_warn_gpu_c", awmDefaults.thermal_warn_gpu_c);
            // gaCfg.awm_config.thermal_crit_cpu_c     = jvalue<double>(cfg, "awm_thermal_crit_cpu_c", awmDefaults.thermal_crit_cpu_c);
            // gaCfg.awm_config.thermal_crit_gpu_c     = jvalue<double>(cfg, "awm_thermal_crit_gpu_c", awmDefaults.thermal_crit_gpu_c);
            // gaCfg.awm_config.latency_sla_ms         = jvalue<double>(cfg, "awm_latency_sla_ms", awmDefaults.latency_sla_ms);
            // gaCfg.awm_config.fps_underperform_ratio = jvalue<double>(cfg, "awm_fps_underperform_ratio", awmDefaults.fps_underperform_ratio);
            // gaCfg.awm_config.transition_smoothing   = jvalue<double>(cfg, "awm_transition_smoothing", awmDefaults.transition_smoothing);

            // To this:
            gaCfg.awm_config.update_interval_gens   = jInt("awm_update_interval_gens", nullptr, awmDefaults.update_interval_gens);
            gaCfg.awm_config.battery_critical_watts = jDouble("awm_battery_critical_watts", nullptr, awmDefaults.battery_critical_watts);
            gaCfg.awm_config.thermal_warn_cpu_c     = jDouble("awm_thermal_warn_cpu_c", nullptr, awmDefaults.thermal_warn_cpu_c);
            gaCfg.awm_config.thermal_warn_gpu_c     = jDouble("awm_thermal_warn_gpu_c", nullptr, awmDefaults.thermal_warn_gpu_c);
            gaCfg.awm_config.thermal_crit_cpu_c     = jDouble("awm_thermal_crit_cpu_c", nullptr, awmDefaults.thermal_crit_cpu_c);
            gaCfg.awm_config.thermal_crit_gpu_c     = jDouble("awm_thermal_crit_gpu_c", nullptr, awmDefaults.thermal_crit_gpu_c);
            gaCfg.awm_config.latency_sla_ms         = jDouble("awm_latency_sla_ms", nullptr, awmDefaults.latency_sla_ms);
            gaCfg.awm_config.fps_underperform_ratio = jDouble("awm_fps_underperform_ratio", nullptr, awmDefaults.fps_underperform_ratio);
            gaCfg.awm_config.transition_smoothing   = jDouble("awm_transition_smoothing", nullptr, awmDefaults.transition_smoothing);

            spdlog::info("[EvolutionarySelector] AdaptiveWeightManager configured "
                        "(update every {} gens, thermal_crit={:.0f}°C/{:.0f}°C)",
                        gaCfg.awm_config.update_interval_gens,
                        gaCfg.awm_config.thermal_crit_cpu_c,
                        gaCfg.awm_config.thermal_crit_gpu_c);
        } else {
            spdlog::info("[EvolutionarySelector] Adaptive weights disabled by config.");
        }

        try {
            ga_ = std::make_unique<hrl::GeneticAlgorithm>(
                gaCfg, aggregator_, runtimeControls_, &scheduler_);

            // Initial workload epoch is zero. Publish it explicitly so the
            // AlgorithmConcrete provenance stamp and GA context begin aligned.
            if (runtimeControls_) {
                const uint64_t initialEpoch = ga_->getWorkloadEpoch();
                runtimeControls_->requested_workload_epoch.store(initialEpoch, std::memory_order_release);
                runtimeControls_->active_workload_epoch.store(initialEpoch, std::memory_order_release);
                runtimeControls_->algorithm_processed_workload_epoch.store(initialEpoch, std::memory_order_release);
                runtimeControls_->algorithm_processed_frame_id.store(0ULL, std::memory_order_release);
            }

            spdlog::info("[EvolutionarySelector] - Pareto-GA Hybrid Ready (pop={}-{}, reduction at gen {})",
                         gaCfg.popSize, gaCfg.minPopSize, gaCfg.reductionStartGen);
        } catch (const std::exception& e) {
            spdlog::error("[EvolutionarySelector] Failed to initialize GeneticAlgorithm: {}", e.what());
            throw;
        }
    }

    bool validate() override {
        spdlog::info("[EvolutionarySelector] Validating configuration...");
        if (!aggregator_) {
            spdlog::error("[EvolutionarySelector] Aggregator is null");
            return false;
        }
        if (!runtimeControls_) {
            spdlog::error("[EvolutionarySelector] RuntimeControls is null");
            return false;
        }
        spdlog::info("[EvolutionarySelector] Validation successful");
        return true;
        // return aggregator_ && runtimeControls_; // Alfer testing all code just used thise line
    }

    void start() override {
        spdlog::info("[EvolutionarySelector] Starting evolutionary control loop...");
        // // Initialize telemetry CSV if configured
        // std::string csv_path = cfg_.value("action_csv", "output/erl_telemetry.csv");
        // csv_stream_.open(csv_path, std::ios::out);
        // if (csv_stream_.is_open()) {
        //     csv_stream_ << "timestamp_ms," << hrl::ThermalGovernor::getCsvHeader() << "\n";
        // }
        //running_.store(true);
        running_.store(true, std::memory_order_release);
        controlThread_ = std::thread(&EvolutionarySelector::controlLoop, this);
    }

    void stop() override {
        spdlog::info("[EvolutionarySelector] Stopping...");
        //running_.store(false);
        running_.store(false, std::memory_order_release);
        if (controlThread_.joinable()) controlThread_.join();
        scheduler_.reset();
        spdlog::info("[EvolutionarySelector] Stopped");
        spdlog::info("[EvolutionarySelector] System settings restored to default (BALANCED, 60 FPS, GPU on)");
    }

    // Helper function to extract a JSON parameter with fallback support
    template <typename T>
    T get_config_param(const nlohmann::json& root_cfg, 
                        const std::string& key, 
                        T default_val) 
    {
        // 1. Check primary location: root_cfg["Scheduler"][key]
        if (root_cfg.contains("Scheduler") && root_cfg["Scheduler"].contains(key)) {
            return root_cfg["Scheduler"][key].get<T>();
        }
        // 2. Fallback location: root_cfg["EvolutionarySelector"][key]
        if (root_cfg.contains("EvolutionarySelector") && root_cfg["EvolutionarySelector"].contains(key)) {
            return root_cfg["EvolutionarySelector"][key].get<T>();
        }
        // 3. Return fallback default if key is missing in both blocks
        return default_val;
    }

    void configure(const nlohmann::json& root_cfg) {
        // If passed only the "Scheduler" sub-object, fall back gracefully
        const auto& jSched = root_cfg.contains("Scheduler") ? root_cfg["Scheduler"] : root_cfg;

        // GA parameters
        this->pop_size = jSched.value("ga_population_size", 8);
        this->crossover_rate = jSched.value("ga_crossover_rate", 0.70);
        this->mutation_rate = jSched.value("ga_mutation_rate", 0.15);

        // // Adaptive Weight Manager parameters
        // this->awm_battery_crit_w = jSched.value("awm_battery_critical_watts", 3.5);
        // this->awm_thermal_warn_cpu = jSched.value("awm_thermal_warn_cpu_c", 57.0);
        // this->awm_thermal_crit_cpu = jSched.value("awm_thermal_crit_cpu_c", 63.0);


        // New configure() snippet (use 7.5 W and 45/55°C defaults):
        this->awm_battery_crit_w = jSched.value("awm_battery_critical_watts", 7.5);
        this->awm_thermal_warn_cpu = jSched.value("awm_thermal_warn_cpu_c", 45.0);
        this->awm_thermal_crit_cpu = jSched.value("awm_thermal_crit_cpu_c", 55.0);
    }

    // ---------------------------------------------------------------------
    // Workload transition protocol.
    // ---------------------------------------------------------------------
    // beginWorkloadTransition() first raises an atomic pause flag, then acquires
    // gaMutex_. This acts as a barrier: any already-running evolve() completes,
    // but no new generation can start while the algorithm worker is replaced.
    size_t beginWorkloadTransition(const hrl::WorkloadProfile& target)
    {
        transitionInProgress_.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(gaMutex_);

        pendingWorkload_ = target;
        transitionBoundaryFrameId_ = 0;
        if (aggregator_) {
            try {
                const uint64_t rawFrame = aggregator_->getLatestSnapshot().frameId;
                const uint64_t evolvedFrame = lastEvolvedFrameId_.load(std::memory_order_acquire);
                transitionBoundaryFrameId_ = std::max(rawFrame, evolvedFrame);
            } catch (...) {
                transitionBoundaryFrameId_ = lastEvolvedFrameId_.load(std::memory_order_acquire);
            }
        }

        const uint64_t currentEpoch = ga_ ? ga_->getWorkloadEpoch() : 0ULL;
        pendingWorkloadEpoch_ = currentEpoch + 1ULL;

        // Request the next epoch while the GA barrier is held, but DO NOT make
        // it active yet. AlgorithmModule activates it only after the old worker
        // has stopped/joined, so an in-flight old-workload frame can never be
        // stamped with the new workload epoch.
        if (runtimeControls_) {
            runtimeControls_->requested_workload_epoch.store(
                pendingWorkloadEpoch_, std::memory_order_release);
        }

        const size_t boundaryGeneration = ga_ ? ga_->getCurrentGeneration() : 0u;
        spdlog::info(
            "[ERL-WORKLOAD] BEGIN target={} | generation={} | boundary_frame={} | "
            "epoch {} -> requested {} | GA/AWM/Scheduler objects preserved",
            target.label, boundaryGeneration, transitionBoundaryFrameId_,
            currentEpoch, pendingWorkloadEpoch_);
        return boundaryGeneration;
    }

    size_t commitWorkloadTransition(const hrl::WorkloadProfile& target)
    {
        std::lock_guard<std::mutex> lock(gaMutex_);
        if (!ga_) {
            transitionInProgress_.store(false, std::memory_order_release);
            return 0u;
        }

        // Validate the replacement worker epoch BEFORE mutating GA workload
        // context. If this fails, ConfigManager can roll the algorithm back while
        // GA/population/AWM are still exactly in the previous workload context.
        const uint64_t activeAlgorithmEpoch = runtimeControls_
            ? runtimeControls_->active_workload_epoch.load(std::memory_order_acquire)
            : pendingWorkloadEpoch_;
        if (activeAlgorithmEpoch != pendingWorkloadEpoch_) {
            throw std::runtime_error(
                "[ERL-WORKLOAD] algorithm epoch mismatch before GA commit: requested=" +
                std::to_string(pendingWorkloadEpoch_) + " algorithm=" +
                std::to_string(activeAlgorithmEpoch));
        }

        ga_->setWorkloadContext(target.workloadId,
                                target.label,
                                target.gpuCapable,
                                target.gpuSplitApplicable,
                                target.fpsMax);

        const uint64_t committedEpoch = ga_->getWorkloadEpoch();
        // setWorkloadContext increments exactly once under gaMutex_. This check
        // is diagnostic; the precondition above is the rollback-safe guard.
        if (committedEpoch != pendingWorkloadEpoch_) {
            spdlog::critical(
                "[ERL-WORKLOAD] INTERNAL EPOCH ERROR after GA commit: committed={} expected={}",
                committedEpoch, pendingWorkloadEpoch_);
        }

        requiredWorkloadEpoch_.store(committedEpoch, std::memory_order_release);

        // Reject pre-transition/in-flight aggregator rows. The first accepted
        // post-transition evolve() must be tied to a replacement-worker provenance
        // frame at/after this boundary; see controlLoop().
        minPostTransitionFrameId_.store(
            transitionBoundaryFrameId_ + 1ULL, std::memory_order_release);
        postTransitionProvenanceFloorFrameId_.store(0ULL, std::memory_order_release);

        pendingWorkload_ = target;

        const size_t boundaryGeneration = ga_->getCurrentGeneration();
        spdlog::info(
            "[ERL-WORKLOAD] COMMIT active={} | workload_epoch={} | algorithm_epoch={} | "
            "generation={} PRESERVED | AWM={} PRESERVED | surrogate_workload_observations={}",
            ga_->getCurrentWorkload(), committedEpoch, activeAlgorithmEpoch,
            boundaryGeneration, ga_->getCurrentRegimeName(),
            ga_->getCurrentWorkloadSurrogateObservations());

        transitionInProgress_.store(false, std::memory_order_release);
        return boundaryGeneration;
    }

    // External mission warmup hold. Unlike a workload transition this does not
    // alter workload context or frame boundaries; it merely prevents GA evolve()
    // from running while camera/CUDA/Lynsyn/telemetry settle. The dynamic harness
    // enables this before startAll() and releases it at mission t=0, guaranteeing
    // that the measured experiment begins with generation 0.
    void pauseEvolutionForMissionWarmup()
    {
        transitionInProgress_.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(gaMutex_);
        spdlog::info(
            "[ERL-WARMUP] External evolution hold engaged at generation={}",
            ga_ ? ga_->getCurrentGeneration() : 0u);
    }

    size_t resumeEvolutionAfterMissionWarmup()
    {
        std::lock_guard<std::mutex> lock(gaMutex_);
        const size_t generation = ga_ ? ga_->getCurrentGeneration() : 0u;
        transitionInProgress_.store(false, std::memory_order_release);
        spdlog::info(
            "[ERL-WARMUP] External evolution hold released at mission t=0; generation={}",
            generation);
        return generation;
    }

    // If the replacement algorithm cannot be configured, the old algorithm is
    // still active (AlgorithmModule is transactional). Resume the existing ERL
    // context without changing GA/AWM state.
    void cancelWorkloadTransition()
    {
        std::lock_guard<std::mutex> lock(gaMutex_);
        const uint64_t currentEpoch = ga_ ? ga_->getWorkloadEpoch() : 0ULL;
        pendingWorkloadEpoch_ = currentEpoch;
        requiredWorkloadEpoch_.store(currentEpoch, std::memory_order_release);
        minPostTransitionFrameId_.store(0ULL, std::memory_order_release);
        postTransitionProvenanceFloorFrameId_.store(0ULL, std::memory_order_release);

        if (runtimeControls_) {
            runtimeControls_->requested_workload_epoch.store(currentEpoch, std::memory_order_release);
            runtimeControls_->active_workload_epoch.store(currentEpoch, std::memory_order_release);
            runtimeControls_->algorithm_processed_frame_id.store(0ULL, std::memory_order_relaxed);
            runtimeControls_->algorithm_processed_workload_epoch.store(currentEpoch, std::memory_order_release);
        }

        transitionInProgress_.store(false, std::memory_order_release);
        spdlog::warn("[ERL-WORKLOAD] Transition cancelled; previous workload/controller state retained");
    }

    bool isWorkloadTransitionInProgress() const {
        return transitionInProgress_.load(std::memory_order_acquire);
    }

    hrl::RLAction getBestAction() const {
        std::lock_guard<std::mutex> lock(gaMutex_);
        return ga_ ? ga_->getBestAction() : hrl::RLAction{};
    }

    size_t getCurrentGeneration() const {
        std::lock_guard<std::mutex> lock(gaMutex_);
        return ga_ ? ga_->getCurrentGeneration() : 0u;
    }

    std::string getCurrentRegimeName() const {
        std::lock_guard<std::mutex> lock(gaMutex_);
        return ga_ ? ga_->getCurrentRegimeName() : std::string("NO_GA");
    }

    std::string getCurrentWorkload() const {
        std::lock_guard<std::mutex> lock(gaMutex_);
        return ga_ ? ga_->getCurrentWorkload() : std::string("NO_GA");
    }

    uint32_t getCurrentWorkloadId() const {
        std::lock_guard<std::mutex> lock(gaMutex_);
        return ga_ ? ga_->getCurrentWorkloadId() : 0U;
    }

    uint64_t getWorkloadEpoch() const {
        std::lock_guard<std::mutex> lock(gaMutex_);
        return ga_ ? ga_->getWorkloadEpoch() : 0ULL;
    }

    uint64_t getCurrentWorkloadSurrogateObservations() const {
        std::lock_guard<std::mutex> lock(gaMutex_);
        return ga_ ? ga_->getCurrentWorkloadSurrogateObservations() : 0ULL;
    }

    std::string exportSurrogateCSV() const {
        std::lock_guard<std::mutex> lock(gaMutex_);
        return ga_ ? ga_->exportSurrogateCSV() : std::string("workload\n");
    }

    std::string exportWeightHistoryCSV() const {
        std::lock_guard<std::mutex> lock(gaMutex_);
        return ga_ ? ga_->exportWeightHistory() : std::string("adaptive_weights_disabled\n");
    }

private:
    json cfg_;
    Context& ctx_;
    // Add missing member declarations:
    size_t pop_size = 8;
    double crossover_rate = 0.70;
    double mutation_rate = 0.15;

    // double awm_battery_crit_w = 3.5;
    // double awm_thermal_warn_cpu = 57.0;
    // double awm_thermal_crit_cpu = 63.0;
    // New configure() snippet (use 7.5 W and 45/55°C defaults):

    double awm_battery_crit_w = 7.5;
    double awm_thermal_warn_cpu = 45.0;
    double awm_thermal_crit_cpu = 55.0;


    std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator_;
    std::shared_ptr<hrl::RuntimeControls> runtimeControls_;
    hrl::Scheduler scheduler_;
    std::unique_ptr<hrl::GeneticAlgorithm> ga_;

    std::atomic<bool> running_{false};
    std::thread controlThread_;

    // Serialises GA evolution against workload-context commits. The mutex does
    // NOT reset the controller; it only guarantees an atomic experimental boundary.
    mutable std::mutex gaMutex_;
    std::atomic<bool> transitionInProgress_{false};
    std::atomic<uint64_t> lastEvolvedFrameId_{0};
    std::atomic<uint64_t> minPostTransitionFrameId_{0};
    std::atomic<uint64_t> requiredWorkloadEpoch_{0};
    std::atomic<uint64_t> postTransitionProvenanceFloorFrameId_{0};
    std::atomic<uint64_t> staleSnapshotSkips_{0};
    std::atomic<uint64_t> provenanceSkips_{0};
    uint64_t transitionBoundaryFrameId_{0};
    uint64_t pendingWorkloadEpoch_{0};
    hrl::WorkloadProfile pendingWorkload_{};

    int loopCount_ = 0;  // Control loop iteration counter (for periodic regime logging)
    int controlIntervalMs_ = 500;
    int warmupMs_ = 5000;
    double minFpsUseful_ = 1.0;
    double minPowerUseful_ = 0.0;
    bool requirePowerForEvolution_ = false;

    // ADD THESE TWO LINES:
    double lastValidPower_ = 0.0;          
    std::deque<double> powerHistory_;
    uint64_t powerRejectCount_ = 0;   // [P0-F15] PowerSanity rejects (throttled logging)

//=========================================================================
void controlLoop() {
        spdlog::debug("[EvolutionarySelector] Control loop started");

        // Pipeline readiness wait
        spdlog::info("[EvolutionarySelector] Waiting for pipeline to become READY...");
        while (!ctx_.pipelineReady.load(std::memory_order_acquire)) {
            if (ctx_.shutdown_flag.load()) {
                spdlog::info("[EvolutionarySelector] Shutdown requested during wait");
                std::this_thread::sleep_for(50ms);
                return;
            }
            std::this_thread::sleep_for(50ms);
        }

        // Extended warmup (Assuming warmupMs_ is defined in your class)
        spdlog::info("[EvolutionarySelector] Pipeline READY. Warming up...");
        //std::this_thread::sleep_for(std::chrono::milliseconds(5000));
        if (warmupMs_ > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(warmupMs_));
        }

        spdlog::info("[EvolutionarySelector] Engaging control loop.");

        running_.store(true);
        int consecutiveSkips = 0;
        const int MAX_SKIPS = 20; // 20 skips at 500ms = 10 seconds to trigger emergency recovery
        uint64_t loopCount = 0;   // Local counter for the 10-iteration logging

        while (running_.load() && !ctx_.shutdown_flag.load()) {
            auto loopStart = std::chrono::steady_clock::now();

            // Workload hot-swap barrier: telemetry/camera remain active, but GA
            // evolution is deliberately paused for the short algorithm-only swap.
            if (transitionInProgress_.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            try {
                auto snapshot = getLatestSnapshot();

                if (!snapshot.valid || !hasUsefulData(snapshot)) {
                    consecutiveSkips++;        
                    // Fix 3 (EvolutionarySelector_02.h): Increase starvation sleep 
                    // interval from 80ms to 500ms to reduce CPU contention.
                    if (consecutiveSkips >= MAX_SKIPS) {
                        spdlog::warn("[EvolutionarySelector] EMERGENCY RECOVERY: resetting to MAX PERFORMANCE");
                        scheduler_.reset(); // Force high-performance profile
                        consecutiveSkips = 0;
                    }
                    spdlog::warn("[EvolutionarySelector] Snapshot not ready, skipping iteration");
                    std::this_thread::sleep_for(500ms);
                    continue;
                }

                // === Pareto + Scalar GA Evolution (Single Source of Truth) ===
                consecutiveSkips = 0;

                bool evolutionSkippedForTransition = false;
                {
                    std::lock_guard<std::mutex> gaLock(gaMutex_);

                    // Re-check after taking the lock: beginWorkloadTransition() may
                    // have raised the flag between the outer check and this point.
                    if (transitionInProgress_.load(std::memory_order_acquire)) {
                        evolutionSkippedForTransition = true;
                    } else {
                        // ---------------------------------------------------------
                        // GATE 1: strict stale-snapshot rejection.
                        // ---------------------------------------------------------
                        // One finalized frame may feed AT MOST one GA generation.
                        // This prevents a dead/stalled data plane from training the
                        // surrogate hundreds of times on an unchanged snapshot.
                        const uint64_t lastFrame =
                            lastEvolvedFrameId_.load(std::memory_order_acquire);
                        if (snapshot.frameId == 0 || snapshot.frameId <= lastFrame) {
                            evolutionSkippedForTransition = true;
                            const uint64_t skips = staleSnapshotSkips_.fetch_add(1) + 1ULL;
                            if (skips <= 3ULL || (skips % 100ULL) == 0ULL) {
                                spdlog::warn(
                                    "[ERL-EVIDENCE] stale snapshot rejected: frame={} last_evolved={} skips={}",
                                    snapshot.frameId, lastFrame, skips);
                            }
                        } else {
                            // -----------------------------------------------------
                            // GATE 2: workload provenance.
                            // -----------------------------------------------------
                            // AlgorithmConcrete publishes these values only after a
                            // successful frame execution and before mergeAlgorithm().
                            const uint64_t requiredEpoch = ga_->getWorkloadEpoch();
                            const uint64_t processedEpoch = runtimeControls_
                                ? runtimeControls_->algorithm_processed_workload_epoch.load(
                                      std::memory_order_acquire)
                                : requiredEpoch;
                            const uint64_t processedFrame = runtimeControls_
                                ? runtimeControls_->algorithm_processed_frame_id.load(
                                      std::memory_order_acquire)
                                : snapshot.frameId;

                            if (processedEpoch != requiredEpoch ||
                                processedFrame == 0ULL ||
                                processedFrame < snapshot.frameId) {
                                evolutionSkippedForTransition = true;
                                const uint64_t skips = provenanceSkips_.fetch_add(1) + 1ULL;
                                if (skips <= 3ULL || (skips % 100ULL) == 0ULL) {
                                    spdlog::warn(
                                        "[ERL-EVIDENCE] provenance gate waiting: snapshot_frame={} "
                                        "processed_frame={} processed_epoch={} required_epoch={} skips={}",
                                        snapshot.frameId, processedFrame, processedEpoch, requiredEpoch, skips);
                                }
                            } else {
                                const uint64_t minFrame =
                                    minPostTransitionFrameId_.load(std::memory_order_acquire);
                                uint64_t provenanceFloor =
                                    postTransitionProvenanceFloorFrameId_.load(std::memory_order_acquire);

                                // Latch the first frame proven to have been processed
                                // by the replacement workload. An older in-flight row
                                // that finalizes after the swap cannot pass this floor.
                                if (minFrame > 0ULL && provenanceFloor == 0ULL &&
                                    processedFrame >= minFrame) {
                                    provenanceFloor = processedFrame;
                                    postTransitionProvenanceFloorFrameId_.store(
                                        provenanceFloor, std::memory_order_release);
                                    spdlog::info(
                                        "[ERL-WORKLOAD] Replacement provenance latched: "
                                        "epoch={} frame={} boundary_min={}",
                                        requiredEpoch, provenanceFloor, minFrame);
                                }

                                if (minFrame > 0ULL &&
                                    (provenanceFloor == 0ULL || snapshot.frameId < provenanceFloor)) {
                                    evolutionSkippedForTransition = true;
                                } else {
                                    ga_->evolve(snapshot);
                                    lastEvolvedFrameId_.store(snapshot.frameId, std::memory_order_release);
                                    if (minFrame > 0ULL) {
                                        minPostTransitionFrameId_.store(0ULL, std::memory_order_release);
                                        postTransitionProvenanceFloorFrameId_.store(0ULL, std::memory_order_release);
                                        spdlog::info(
                                            "[ERL-WORKLOAD] First provenance-safe post-transition "
                                            "snapshot accepted: frame={} epoch={}",
                                            snapshot.frameId, requiredEpoch);
                                    }
                                }
                            }
                        }
                    }
                }

                if (evolutionSkippedForTransition) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    continue;
                }

                // Log current weight regime every 10 iterations for thesis data
                if (++loopCount % 10 == 0) {
                    spdlog::info("[EvolutionarySelector] Regime={} | fps={:.1f} | power={:.2f}W | "
                                 "cpu_temp={:.1f}°C | gpu_temp={:.1f}°C | latency={:.1f}ms",
                                 getCurrentRegimeName(),
                                 snapshot.fps, snapshot.avg_power_w_alg,
                                 snapshot.cpu_temp_c, snapshot.gpu_temp_c,
                                 snapshot.avg_latency_ms);
                }

            } catch (const std::exception& e) {
                spdlog::error("[EvolutionarySelector] Error in control loop: {}", e.what());
            } catch (...) {
                spdlog::error("[EvolutionarySelector] Unknown exception in control loop");
            }

            // Precise timing (Defaults to 2000ms standard ERL tick rate)
            auto elapsed = std::chrono::steady_clock::now() - loopStart;
            //auto sleepTime = std::chrono::milliseconds(2000) - elapsed; 
            auto sleepTime = std::chrono::milliseconds(controlIntervalMs_) - elapsed;
            if (sleepTime > 0ms) {
                std::this_thread::sleep_for(sleepTime);
            }
        }

        spdlog::info("[EvolutionarySelector] Control loop ended cleanly");
    }

    //==================================================================
    // void controlLoop() {
    //     spdlog::debug("[EvolutionarySelector] Control loop started");

    //     // Pipeline readiness wait
    //     spdlog::info("[EvolutionarySelector] Waiting for pipeline to become READY...");
    //     while (!ctx_.pipelineReady.load(std::memory_order_acquire)) {
    //         if (ctx_.shutdown_flag.load()) {
    //             spdlog::info("[EvolutionarySelector] Shutdown requested during wait");
    //             std::this_thread::sleep_for(50ms);
    //             return;
    //         }
    //         std::this_thread::sleep_for(50ms);
    //     }

    //     // Extended warmup
    //     spdlog::info("[EvolutionarySelector] Pipeline READY. Warming up for {} ms...", warmupMs_);
    //     if (warmupMs_ > 0) {
    //         std::this_thread::sleep_for(std::chrono::milliseconds(warmupMs_));
    //     }

    //     spdlog::info("[EvolutionarySelector] Engaging control loop.");

    //     // Inside the main evolutionary control loop:
    //     void run() {
    //         running_ = true;
    //         int consecutiveSkips = 0;
    //         const int MAX_SKIPS = 10; // Example threshold

    //         // while (running_ && !ctx_.shutdown_flag.load()) {
    //         while (running_.load() && !ctx_.shutdown_flag.load()) {
    //             auto loopStart = std::chrono::steady_clock::now();

    //             try {
    //                 auto snapshot = getLatestSnapshot();

    //                 if (!snapshot.valid || !hasUsefulData(snapshot)) {
    //                     consecutiveSkips++;        
    //                     // Fix 3 (EvolutionarySelector_02.h): Increase starvation sleep 
    //                     // interval from 80ms to 500ms to reduce CPU contention.
    //                     if (consecutiveSkips >= MAX_SKIPS) {
    //                         spdlog::warn("[EvolutionarySelector] EMERGENCY RECOVERY: resetting to MAX PERFORMANCE");
    //                         scheduler_.reset(); // Force high-performance profile
    //                         consecutiveSkips = 0;
    //                     }
    //                     spdlog::warn("[EvolutionarySelector] Snapshot not ready, skipping iteration");
    //                     std::this_thread::sleep_for(500ms);
    //                     continue;
    //                 }


    //                 // === Pareto + Scalar GA Evolution (Single Source of Truth) ===
    //                 // === Pareto + Scalar GA Evolution (Single Source of Truth) ===
    //                 consecutiveSkips = 0;   

    //                 ga_->evolve(snapshot);

    //                 // Log current weight regime every 10 iterations for thesis data
    //                 if (++loopCount_ % 10 == 0) {
    //                     spdlog::info("[EvolutionarySelector] Regime={} | fps={:.1f} | power={:.2f}W | "
    //                                 "cpu_temp={:.1f}°""C | gpu_temp={:.1f}°""C | latency={:.1f}ms",
    //                                 ga_->getCurrentRegimeName(),
    //                                 snapshot.fps, snapshot.avg_power_w_alg,
    //                                 snapshot.cpu_temp_c, snapshot.gpu_temp_c,
    //                                 snapshot.avg_latency_ms);
    //                 }

    //             } catch (const std::exception& e) {
    //                 spdlog::error("[EvolutionarySelector] Error in control loop: {}", e.what());
    //             } catch (...) {
    //                 spdlog::error("[EvolutionarySelector] Unknown exception in control loop");
    //             }

    //             // Precise timing
    //             auto elapsed = std::chrono::steady_clock::now() - loopStart;
    //             auto sleepTime = std::chrono::milliseconds(controlIntervalMs_) - elapsed;
    //             if (sleepTime > 0ms) {
    //                 std::this_thread::sleep_for(sleepTime);
    //             }
    //         }
    //     }

    //     spdlog::info("[EvolutionarySelector] Control loop ended cleanly");
    // }

//======================================================================================
hrl::MetricsSnapshot getLatestSnapshot() {
        hrl::MetricsSnapshot snap{};
        snap.valid = false;

        if (!aggregator_) {
            spdlog::warn("[EvolutionarySelector] Aggregator is NULL");
            return snap;
        }

        try {
            auto raw = aggregator_->getLatestSnapshot();
            if (!raw.isValid()) {
                spdlog::debug("[EvolutionarySelector] Raw snapshot not valid yet");
                return snap;
            }

            // FIX 1: Map frame_id (Closes the zeroed frame_id column in trace)
            snap.frameId = raw.frameId;

            
            snap.fps = raw.algorithmStats.fps;

            // [TELEMETRY-FIX1] Algorithm-only processing time.
            // GeneticAlgorithm::exportSelectedActionEvidence() already exports
            // snap.avg_inference_ms as actions.algorithm_processing_ms.
            snap.avg_inference_ms =
                raw.algorithmStats.inferenceTimeMs;

            // [P0-F15] Power mapping rewritten.
            //  - The measured, window-integrated aggregator power is used
            //    directly. The old re-derivation from joulesPerFrame was
            //    circular (the aggregator computes J/F FROM power) and used
            //    a different time base.
            //  - The old "mW to W" auto-fix multiplied any reading in
            //    (0, 0.5) W by 1000, turning a real 0.4 W idle sample into
            //    400 W - exactly the class of poisoning documented in the
            //    2026-07-11 PowerSanity postmortem. Deleted.
            //  - The fabricated 5.0 W default is deleted: absence of a
            //    measurement is reported honestly as 0.0 (and can pause
            //    evolution via require_power_for_evolution), never invented.
            //  - Every sample must pass PowerSanity before it can reach the
            //    GA fitness, the AdaptiveWeightManager, or the surrogate.
            double instantPower = (raw.avg_power_w_alg > 0.0)
                ? raw.avg_power_w_alg
                : raw.powerStats.totalPower();

            if (hrl::PowerSanity::valid(instantPower)) {
                lastValidPower_ = instantPower;
                // 5-sample moving average of ACCEPTED samples only.
                powerHistory_.push_back(instantPower);
                if (powerHistory_.size() > 5) {
                    powerHistory_.pop_front();
                }
            } else if (instantPower != 0.0) {
                // 0.0 means "no power module / no data yet" - not worth a log.
                if (++powerRejectCount_ % 10 == 1) {
                    spdlog::warn("[EvolutionarySelector] PowerSanity rejected "
                                 "{:.3f}W ({}) - {} rejected so far",
                                 instantPower,
                                 hrl::PowerSanity::reject_reason(instantPower),
                                 powerRejectCount_);
                }
            }

            double avgPower = 0.0;
            if (!powerHistory_.empty()) {
                for (double p : powerHistory_) {
                    avgPower += p;
                }
                avgPower /= static_cast<double>(powerHistory_.size());
            }

            snap.avg_power_w_alg = avgPower;

            // Resource mapping
            snap.cpu_util_avg = (raw.socInfo.CPU1_Utilization_Percent +
                                 raw.socInfo.CPU2_Utilization_Percent +
                                 raw.socInfo.CPU3_Utilization_Percent +
                                 raw.socInfo.CPU4_Utilization_Percent) / 4.0;
            snap.gpu_util_avg   = raw.socInfo.GR3D_Frequency_Percent;

            // FIX 3: Thermal mapping (Closes the obj_temp = 0 bug)
            // Tegrastats parses can output millidegrees. Normalize to Celsius.
            double c_temp = raw.socInfo.CPU_Temperature_C;
            double g_temp = raw.socInfo.GPU_Temperature_C;
            
            if (c_temp > 1000.0) c_temp /= 1000.0;
            if (g_temp > 1000.0) g_temp /= 1000.0;

            snap.cpu_temp_c = c_temp;
            snap.gpu_temp_c = g_temp;

            snap.end_to_end_latency_ms = raw.endToEndLatencyMs;
            snap.avg_latency_ms = raw.endToEndLatencyMs;
            
            // [P0-F15] Single definition of energy-per-frame: the aggregator's
            // measured value (power integrated over the algorithm window).
            // The old re-derivation avg_power * end_to_end_latency used a
            // different time base and disagreed with the exported metrics.
            snap.joulesPerFrame   = raw.joulesPerFrame;
            snap.joules_per_frame = raw.joulesPerFrame;

            snap.valid = true;
            spdlog::debug("[EvolutionarySelector] Valid snapshot (fps={:.1f}, pwr={:.2f}W, temp={:.1f}C)", 
                          snap.fps, snap.avg_power_w_alg, snap.cpu_temp_c);
            return snap;

        } catch (const std::exception& e) {
            spdlog::warn("[EvolutionarySelector] Exception in getLatestSnapshot: {}", e.what());
        } catch (...) {
            spdlog::warn("[EvolutionarySelector] Unknown exception in getLatestSnapshot");
        }

        return snap;
    }
//======================================================================================

// hrl::MetricsSnapshot getLatestSnapshot() {
//         hrl::MetricsSnapshot snap{};
//         snap.valid = false;

//         if (!aggregator_) {
//             spdlog::warn("[EvolutionarySelector] Aggregator is NULL");
//             return snap;
//         }

//         try {
//             auto raw = aggregator_->getLatestSnapshot();
//             if (!raw.isValid()) {
//                 spdlog::debug("[EvolutionarySelector] Raw snapshot not valid yet");
//                 return snap;
//             }

//             // FIX 1: Map frame_id (Closes the zeroed frame_id column in trace)
//             snap.frameId = raw.frameId;

//             snap.fps = raw.algorithmStats.fps;

//             // FIX 2: Power mapping (Closes the 0.09W blind energy optimizer)
//             // If pulling from raw.powerStats.averagePower() yielded 0.09, we have a unit/rail bug.
//             // We scale up milliWatt-range errors or fallback to a safe 5.0W baseline to ensure 
//             // the GA has a valid gradient to optimize against.
//             double power_val = raw.powerStats.averagePower();
//             if (raw.joulesPerFrame > 0 && raw.algorithmStats.totalProcTimeMs > 0) {
//                 power_val = raw.joulesPerFrame / (raw.algorithmStats.totalProcTimeMs / 1000.0);
//             }
            
//             if (power_val > 0.0 && power_val < 0.5) {
//                 power_val *= 1000.0; // Correct mW to W scale error
//             } else if (power_val <= 0.0) {
//                 power_val = 5.0; // Sane Jetson Nano default
//             }
//             snap.avg_power_w_alg = power_val;

//             snap.cpu_util_avg = (raw.socInfo.CPU1_Utilization_Percent +
//                                  raw.socInfo.CPU2_Utilization_Percent +
//                                  raw.socInfo.CPU3_Utilization_Percent +
//                                  raw.socInfo.CPU4_Utilization_Percent) / 4.0;
//             snap.gpu_util_avg   = raw.socInfo.GR3D_Frequency_Percent;

//             // FIX 3: Thermal mapping (Closes the obj_temp = 0 bug)
//             // Tegrastats parses can output millidegrees. Normalize to Celsius.
//             double c_temp = raw.socInfo.CPU_Temperature_C;
//             double g_temp = raw.socInfo.GPU_Temperature_C;
            
//             if (c_temp > 1000.0) c_temp /= 1000.0;
//             if (g_temp > 1000.0) g_temp /= 1000.0;

//             snap.cpu_temp_c = c_temp;
//             snap.gpu_temp_c = g_temp;

//             snap.end_to_end_latency_ms = raw.endToEndLatencyMs;
//             snap.avg_latency_ms = raw.endToEndLatencyMs;
            
//             // Re-sync Joules per frame with the corrected power value
//             snap.joulesPerFrame = snap.avg_power_w_alg * (snap.avg_latency_ms / 1000.0);
//             snap.joules_per_frame = snap.joulesPerFrame;

//             snap.valid = true;
//             spdlog::debug("[EvolutionarySelector] Valid snapshot (fps={:.1f}, pwr={:.2f}W, temp={:.1f}C)", 
//                           snap.fps, snap.avg_power_w_alg, snap.cpu_temp_c);
//             return snap;

//         } catch (const std::exception& e) {
//             spdlog::warn("[EvolutionarySelector] Exception in getLatestSnapshot: {}", e.what());
//         } catch (...) {
//             spdlog::warn("[EvolutionarySelector] Unknown exception in getLatestSnapshot");
//         }

//         return snap;
//     }
//================================================================================
// //================================================================================

//     hrl::MetricsSnapshot getLatestSnapshot() {
//         hrl::MetricsSnapshot snap{};
//         snap.valid = false;

//         if (!aggregator_) {
//             spdlog::warn("[EvolutionarySelector] Aggregator is NULL");
//             return snap;
//         }

//         try {
//             auto raw = aggregator_->getLatestSnapshot();
//             if (!raw.isValid()) {
//                 spdlog::debug("[EvolutionarySelector] Raw snapshot not valid yet");
//                 return snap;
//             }

//             // FIX 1: Map frame_id (Closes the zeroed frame_id column in trace)
//             snap.frameId = raw.frameId;

//             // Safe conversion with joulesPerFrame priority
//             snap.fps = raw.algorithmStats.fps;


//             snap.avg_power_w_alg = (raw.joulesPerFrame > 0 && raw.algorithmStats.totalProcTimeMs > 0)
//                 ? raw.joulesPerFrame / (raw.algorithmStats.totalProcTimeMs / 1000.0)
//                 : raw.powerStats.averagePower();

//             snap.cpu_util_avg = (raw.socInfo.CPU1_Utilization_Percent +
//                                  raw.socInfo.CPU2_Utilization_Percent +
//                                  raw.socInfo.CPU3_Utilization_Percent +
//                                  raw.socInfo.CPU4_Utilization_Percent) / 4.0;

//             snap.gpu_util_avg   = raw.socInfo.GR3D_Frequency_Percent;
//             snap.cpu_temp_c     = raw.socInfo.CPU_Temperature_C;
//             snap.gpu_temp_c     = raw.socInfo.GPU_Temperature_C;
//             snap.avg_latency_ms = raw.endToEndLatencyMs;
//             snap.joulesPerFrame = raw.joulesPerFrame;   // Important for ERL reward

//             snap.valid = true;
//             spdlog::debug("[EvolutionarySelector] Valid snapshot (fps={:.1f})", snap.fps);
//             return snap;

//         } catch (const std::exception& e) {
//             spdlog::warn("[EvolutionarySelector] Exception in getLatestSnapshot: {}", e.what());
//         } catch (...) {
//             spdlog::warn("[EvolutionarySelector] Unknown exception in getLatestSnapshot");
//         }

//         return snap;  // invalid
//     }

//================================================================================

bool hasUsefulData(const hrl::MetricsSnapshot& s) const {
        if (s.fps < minFpsUseful_) {
            return false;
        }
        if (requirePowerForEvolution_ && s.avg_power_w_alg < minPowerUseful_) {
            return false;
        }
        return true;
    }
};


//==============================================================================
// // EvolutionarySelector_02.h - FINAL PhD-Ready (Best of Both Worlds)
// // Pareto + Scalar Hybrid | Strong Defensive Checks | Clean Orchestrator

// // EvolutionarySelector_02.h - FINAL PhD-Ready (Best of Both Worlds)
// // Pareto + Scalar Hybrid | Strong Defensive Checks | Clean Orchestrator
// #pragma once

// #include <atomic>
#include <algorithm>
// #include <chrono>
// #include <thread>
// #include <memory>
// #include <spdlog/spdlog.h>

// #include "IModule.h"
// #include "RuntimeControls.h"
// #include "IScheduler.h"
// #include "Scheduler.h"
// #include "RuntimeControls.h"   // ? ADD THIS
// #include "../Stage_01/Concretes/SystemMetricsAggregatorConcrete_v3_2.h"
// #include "GeneticAlgorithm_05.h"   // ? Pareto core (single source of truth)
// #include "../Stage_01/Others/utils.h"

// using namespace std::chrono_literals;

// class EvolutionarySelector : public IModule {
// public:
//     EvolutionarySelector(const json& cfg,
//                          Context& ctx,
//                          std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator,
//                          std::shared_ptr<hrl::RuntimeControls> runtimeControls)
//         : cfg_(cfg),
//           ctx_(ctx),
//           aggregator_(std::move(aggregator)),
//           runtimeControls_(std::move(runtimeControls)),
//           scheduler_(hrl::Scheduler::Limits{}),
//           running_(false) {

//         // ====== GA Configuration (with Local clamping) ======
//         hrl::GAConfig gaCfg;
//         gaCfg.popSize             = utils::local_clamp<size_t>(cfg_.value("ga_population_size", 12), 4u, 16u);
//         gaCfg.minPopSize          = utils::local_clamp<size_t>(cfg_.value("ga_min_pop_size", 6), 2u, gaCfg.popSize);
//         gaCfg.eliteCount          = cfg_.value("ga_elite_count", 3);
//         gaCfg.crossoverRate       = utils::local_clamp<double>(cfg_.value("ga_crossover_rate", 0.75), 0.5, 0.95);
//         gaCfg.mutationRate        = utils::local_clamp<double>(cfg_.value("ga_mutation_rate", 0.18), 0.01, 0.5);
//         gaCfg.explorationDecay    = cfg_.value("ga_exploration_decay", 0.96);
//         gaCfg.controlIntervalMs   = cfg_.value("control_interval_ms", 800);
//         gaCfg.reductionStartGen   = cfg_.value("ga_reduction_start_gen", 10);
//         gaCfg.reductionRatio      = utils::local_clamp<double>(cfg_.value("ga_reduction_ratio", 0.6), 0.3, 0.9);

//         gaCfg.adaptive_weights_enabled = cfg_.value("adaptive_weights_enabled", false);


//         // PhD evidence export: ERL/Pareto traceability.
//         gaCfg.export_pareto_evidence = cfg_.value("export_pareto_evidence", false);
//         gaCfg.pareto_csv_path        = cfg_.value("pareto_csv", std::string("output/erl_pareto_front.csv"));
//         gaCfg.action_csv_path        = cfg_.value("action_csv", std::string("output/erl_action_trace.csv"));
        

//         if (gaCfg.adaptive_weights_enabled) {
//             gaCfg.awm_config.update_interval_gens   = cfg_.value("awm_update_interval_gens", 5);
//             gaCfg.awm_config.battery_critical_watts = cfg_.value("awm_battery_critical_watts", 8.0);
//             gaCfg.awm_config.thermal_warn_cpu_c     = cfg_.value("awm_thermal_warn_cpu_c", 75.0);
//             gaCfg.awm_config.thermal_warn_gpu_c     = cfg_.value("awm_thermal_warn_gpu_c", 78.0);
//             gaCfg.awm_config.thermal_crit_cpu_c     = cfg_.value("awm_thermal_crit_cpu_c", 80.0);
//             gaCfg.awm_config.thermal_crit_gpu_c     = cfg_.value("awm_thermal_crit_gpu_c", 83.0);
//             gaCfg.awm_config.latency_sla_ms         = cfg_.value("awm_latency_sla_ms", 45.0);
//             gaCfg.awm_config.fps_underperform_ratio = cfg_.value("awm_fps_underperform_ratio", 0.70);
//             gaCfg.awm_config.transition_smoothing   = cfg_.value("awm_transition_smoothing", 0.3);

//             spdlog::info("[EvolutionarySelector] AdaptiveWeightManager configured "
//                          "(update every {} gens, thermal_crit={:.0f}°""C/{:.0f}°""C)",
//                          gaCfg.awm_config.update_interval_gens,
//                          gaCfg.awm_config.thermal_crit_cpu_c,
//                          gaCfg.awm_config.thermal_crit_gpu_c);
//         }

//         try {
//             ga_ = std::make_unique<hrl::GeneticAlgorithm>(
//                 gaCfg, aggregator_, runtimeControls_, &scheduler_);

//             spdlog::info("[EvolutionarySelector] - Pareto-GA Hybrid Ready (pop={}-{}, reduction at gen {})",
//                          gaCfg.popSize, gaCfg.minPopSize, gaCfg.reductionStartGen);
//         } catch (const std::exception& e) {
//             spdlog::error("[EvolutionarySelector] Failed to initialize GeneticAlgorithm: {}", e.what());
//             throw;
//         }
//     }

//     bool validate() override {
//         spdlog::info("[EvolutionarySelector] Validating configuration...");
//         if (!aggregator_) {
//             spdlog::error("[EvolutionarySelector] Aggregator is null");
//             return false;
//         }
//         if (!runtimeControls_) {
//             spdlog::error("[EvolutionarySelector] RuntimeControls is null");
//             return false;
//         }
//         spdlog::info("[EvolutionarySelector] Validation successful");
//         return true;
//         // return aggregator_ && runtimeControls_; // Alfer testing all code just used thise line
//     }

//     void start() override {
//         spdlog::info("[EvolutionarySelector] Starting evolutionary control loop...");
//         //running_.store(true);
//         running_.store(true, std::memory_order_release);
//         controlThread_ = std::thread(&EvolutionarySelector::controlLoop, this);
//     }

//     void stop() override {
//         spdlog::info("[EvolutionarySelector] Stopping...");
//         //running_.store(false);
//         running_.store(false, std::memory_order_release);
//         if (controlThread_.joinable()) controlThread_.join();
//         spdlog::info("[EvolutionarySelector] Stopped");
//     }

//     // Optional helper for external modules (e.g. logging or RL)
//     hrl::RLAction getBestAction() const {
//         return ga_ ? ga_->getBestAction() : hrl::RLAction{};
//     }

// private:
//     json cfg_;
//     Context& ctx_;
//     std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator_;
//     std::shared_ptr<hrl::RuntimeControls> runtimeControls_;
//     hrl::Scheduler scheduler_;
//     std::unique_ptr<hrl::GeneticAlgorithm> ga_;
//     std::atomic<bool> running_{false};
//     std::thread controlThread_;
//     int loopCount_ = 0;  // Control loop iteration counter (for periodic regime logging)
    // int controlIntervalMs_ = 500;
    // int warmupMs_ = 5000;
    // double minFpsUseful_ = 1.0;
    // double minPowerUseful_ = 0.0;
    // bool requirePowerForEvolution_ = false;

//     void controlLoop() {
//         spdlog::debug("[EvolutionarySelector] Control loop started");

//         // Pipeline readiness wait
//         spdlog::info("[EvolutionarySelector] Waiting for pipeline to become READY...");
//         while (!ctx_.pipelineReady.load(std::memory_order_acquire)) {
//             if (ctx_.shutdown_flag.load()) {
//                 spdlog::info("[EvolutionarySelector] Shutdown requested during wait");
//                 std::this_thread::sleep_for(50ms);
//                 return;
//             }
//             std::this_thread::sleep_for(50ms);
//         }

//         // Extended warmup
//         spdlog::info("[EvolutionarySelector] Pipeline READY. Warming up for 5 seconds...");
//         std::this_thread::sleep_for(5s);    // Warmup

//         spdlog::info("[EvolutionarySelector] Engaging control loop.");

//         // while (running_ && !ctx_.shutdown_flag.load()) {
//         while (running_.load() && !ctx_.shutdown_flag.load()) {
//             auto loopStart = std::chrono::steady_clock::now();

//             try {
//                 auto snapshot = getLatestSnapshot();

//                 if (!snapshot.valid || !hasUsefulData(snapshot)) {
//                     spdlog::warn("[EvolutionarySelector] Snapshot not ready, skipping iteration");
//                     std::this_thread::sleep_for(80ms);
//                     continue;
//                 }

//                 // === Pareto + Scalar GA Evolution (Single Source of Truth) ===
//                 // === Pareto + Scalar GA Evolution (Single Source of Truth) ===
//                 ga_->evolve(snapshot);

//                 // Log current weight regime every 10 iterations for thesis data
//                 if (++loopCount_ % 10 == 0) {
//                     spdlog::info("[EvolutionarySelector] Regime={} | fps={:.1f} | power={:.2f}W | "
//                                  "cpu_temp={:.1f}°""C | gpu_temp={:.1f}°""C | latency={:.1f}ms",
//                                  ga_->getCurrentRegimeName(),
//                                  snapshot.fps, snapshot.avg_power_w_alg,
//                                  snapshot.cpu_temp_c, snapshot.gpu_temp_c,
//                                  snapshot.avg_latency_ms);
//                 }

//             } catch (const std::exception& e) {
//                 spdlog::error("[EvolutionarySelector] Error in control loop: {}", e.what());
//             } catch (...) {
//                 spdlog::error("[EvolutionarySelector] Unknown exception in control loop");
//             }

//             // Precise timing
//             auto elapsed = std::chrono::steady_clock::now() - loopStart;
//             auto sleepTime = std::chrono::milliseconds(controlIntervalMs_) - elapsed;
//             if (sleepTime > 0ms) {
//                 std::this_thread::sleep_for(sleepTime);
//             }
//         }

//         spdlog::info("[EvolutionarySelector] Control loop ended cleanly");
//     }

//     hrl::MetricsSnapshot getLatestSnapshot() {
//         hrl::MetricsSnapshot snap{};
//         snap.valid = false;

//         if (!aggregator_) {
//             spdlog::warn("[EvolutionarySelector] Aggregator is NULL");
//             return snap;
//         }

//         try {
//             auto raw = aggregator_->getLatestSnapshot();
//             if (!raw.isValid()) {
//                 spdlog::debug("[EvolutionarySelector] Raw snapshot not valid yet");
//                 return snap;
//             }

//             // Safe conversion with joulesPerFrame priority
//             snap.fps = raw.algorithmStats.fps;

//             snap.avg_power_w_alg = (raw.joulesPerFrame > 0 && raw.algorithmStats.totalProcTimeMs > 0)
//                 ? raw.joulesPerFrame / (raw.algorithmStats.totalProcTimeMs / 1000.0)
//                 : raw.powerStats.averagePower();

//             snap.cpu_util_avg = (raw.socInfo.CPU1_Utilization_Percent +
//                                  raw.socInfo.CPU2_Utilization_Percent +
//                                  raw.socInfo.CPU3_Utilization_Percent +
//                                  raw.socInfo.CPU4_Utilization_Percent) / 4.0;

//             snap.gpu_util_avg   = raw.socInfo.GR3D_Frequency_Percent;
//             snap.cpu_temp_c     = raw.socInfo.CPU_Temperature_C;
//             snap.gpu_temp_c     = raw.socInfo.GPU_Temperature_C;
//             snap.avg_latency_ms = raw.endToEndLatencyMs;
//             snap.joulesPerFrame = raw.joulesPerFrame;   // Important for ERL reward

//             snap.valid = true;
//             spdlog::debug("[EvolutionarySelector] Valid snapshot (fps={:.1f})", snap.fps);
//             return snap;

//         } catch (const std::exception& e) {
//             spdlog::warn("[EvolutionarySelector] Exception in getLatestSnapshot: {}", e.what());
//         } catch (...) {
//             spdlog::warn("[EvolutionarySelector] Unknown exception in getLatestSnapshot");
//         }

//         return snap;  // invalid
//     }

//     static bool hasUsefulData(const hrl::MetricsSnapshot& s) {
//         return s.fps > 5.0 && s.avg_power_w_alg > 0.5;
//     }
// };




//===============================================================================================
// // EvolutionarySelector_02.h - FINAL PhD-Ready (Best of Both Worlds)
// // Pareto + Scalar Hybrid | Strong Defensive Checks | Clean Orchestrator

// // EvolutionarySelector_02.h - FINAL PhD-Ready (Best of Both Worlds)
// // Pareto + Scalar Hybrid | Strong Defensive Checks | Clean Orchestrator
// #pragma once

// #include <atomic>
#include <algorithm>
// #include <chrono>
// #include <thread>
// #include <memory>
// #include <spdlog/spdlog.h>

// #include "IModule.h"
// #include "RuntimeControls.h"
// #include "IScheduler.h"
// #include "Scheduler.h"
// #include "RuntimeControls.h"   // ? ADD THIS
// #include "../Stage_01/Concretes/SystemMetricsAggregatorConcrete_v3_2.h"
// #include "GeneticAlgorithm_05.h"   // ? Pareto core (single source of truth)
// #include "../Stage_01/Others/utils.h"

// using namespace std::chrono_literals;

// class EvolutionarySelector : public IModule {
// public:
//     EvolutionarySelector(const json& cfg,
//                          Context& ctx,
//                          std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator,
//                          std::shared_ptr<hrl::RuntimeControls> runtimeControls)
//         : cfg_(cfg),
//           ctx_(ctx),
//           aggregator_(std::move(aggregator)),
//           runtimeControls_(std::move(runtimeControls)),
//           scheduler_(hrl::Scheduler::Limits{}),
//           running_(false) {

//         // ====== GA Configuration (with Local clamping) ======
//         hrl::GAConfig gaCfg;
//         gaCfg.popSize             = utils::local_clamp<size_t>(cfg_.value("ga_population_size", 12), 4u, 16u);
//         gaCfg.minPopSize          = utils::local_clamp<size_t>(cfg_.value("ga_min_pop_size", 6), 2u, gaCfg.popSize);
//         gaCfg.eliteCount          = cfg_.value("ga_elite_count", 3);
//         gaCfg.crossoverRate       = utils::local_clamp<double>(cfg_.value("ga_crossover_rate", 0.75), 0.5, 0.95);
//         gaCfg.mutationRate        = utils::local_clamp<double>(cfg_.value("ga_mutation_rate", 0.18), 0.01, 0.5);
//         gaCfg.explorationDecay    = cfg_.value("ga_exploration_decay", 0.96);
//         gaCfg.controlIntervalMs   = cfg_.value("control_interval_ms", 800);
//         gaCfg.reductionStartGen   = cfg_.value("ga_reduction_start_gen", 10);
//         gaCfg.reductionRatio      = utils::local_clamp<double>(cfg_.value("ga_reduction_ratio", 0.6), 0.3, 0.9);

//         gaCfg.adaptive_weights_enabled = cfg_.value("adaptive_weights_enabled", false);

//         // PhD evidence export: ERL/Pareto traceability.
//         gaCfg.export_pareto_evidence = cfg_.value("export_pareto_evidence", true);
//         gaCfg.pareto_csv_path        = cfg_.value("pareto_csv", std::string("output/erl_pareto_front.csv"));
//         gaCfg.action_csv_path        = cfg_.value("action_csv", std::string("output/erl_action_trace.csv"));


//         if (gaCfg.adaptive_weights_enabled) {
//             gaCfg.awm_config.update_interval_gens   = cfg_.value("awm_update_interval_gens", 5);
//             gaCfg.awm_config.battery_critical_watts = cfg_.value("awm_battery_critical_watts", 8.0);
//             gaCfg.awm_config.thermal_warn_cpu_c     = cfg_.value("awm_thermal_warn_cpu_c", 75.0);
//             gaCfg.awm_config.thermal_warn_gpu_c     = cfg_.value("awm_thermal_warn_gpu_c", 78.0);
//             gaCfg.awm_config.thermal_crit_cpu_c     = cfg_.value("awm_thermal_crit_cpu_c", 80.0);
//             gaCfg.awm_config.thermal_crit_gpu_c     = cfg_.value("awm_thermal_crit_gpu_c", 83.0);
//             gaCfg.awm_config.latency_sla_ms         = cfg_.value("awm_latency_sla_ms", 45.0);
//             gaCfg.awm_config.fps_underperform_ratio = cfg_.value("awm_fps_underperform_ratio", 0.70);
//             gaCfg.awm_config.transition_smoothing   = cfg_.value("awm_transition_smoothing", 0.3);

//             spdlog::info("[EvolutionarySelector] AdaptiveWeightManager configured "
//                          "(update every {} gens, thermal_crit={:.0f}°""C/{:.0f}°""C)",
//                          gaCfg.awm_config.update_interval_gens,
//                          gaCfg.awm_config.thermal_crit_cpu_c,
//                          gaCfg.awm_config.thermal_crit_gpu_c);
//         }

//         try {
//             ga_ = std::make_unique<hrl::GeneticAlgorithm>(
//                 gaCfg, aggregator_, runtimeControls_, &scheduler_);

//             spdlog::info("[EvolutionarySelector] - Pareto-GA Hybrid Ready (pop={}-{}, reduction at gen {})",
//                          gaCfg.popSize, gaCfg.minPopSize, gaCfg.reductionStartGen);
//         } catch (const std::exception& e) {
//             spdlog::error("[EvolutionarySelector] Failed to initialize GeneticAlgorithm: {}", e.what());
//             throw;
//         }
//     }

//     bool validate() override {
//         spdlog::info("[EvolutionarySelector] Validating configuration...");
//         if (!aggregator_) {
//             spdlog::error("[EvolutionarySelector] Aggregator is null");
//             return false;
//         }
//         if (!runtimeControls_) {
//             spdlog::error("[EvolutionarySelector] RuntimeControls is null");
//             return false;
//         }
//         spdlog::info("[EvolutionarySelector] Validation successful");
//         return true;
//         // return aggregator_ && runtimeControls_; // Alfer testing all code just used thise line
//     }

//     void start() override {
//         spdlog::info("[EvolutionarySelector] Starting evolutionary control loop...");
//         //running_.store(true);
//         running_.store(true, std::memory_order_release);
//         controlThread_ = std::thread(&EvolutionarySelector::controlLoop, this);
//     }

//     void stop() override {
//         spdlog::info("[EvolutionarySelector] Stopping...");
//         //running_.store(false);
//         running_.store(false, std::memory_order_release);
//         if (controlThread_.joinable()) controlThread_.join();
//         spdlog::info("[EvolutionarySelector] Stopped");
//     }

//     // Optional helper for external modules (e.g. logging or RL)
//     hrl::RLAction getBestAction() const {
//         return ga_ ? ga_->getBestAction() : hrl::RLAction{};
//     }

// private:
//     json cfg_;
//     Context& ctx_;
//     std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator_;
//     std::shared_ptr<hrl::RuntimeControls> runtimeControls_;
//     hrl::Scheduler scheduler_;
//     std::unique_ptr<hrl::GeneticAlgorithm> ga_;
//     std::atomic<bool> running_{false};
//     std::thread controlThread_;
//     int loopCount_ = 0;  // Control loop iteration counter (for periodic regime logging)

//     void controlLoop() {
//         spdlog::debug("[EvolutionarySelector] Control loop started");

//         // Pipeline readiness wait
//         spdlog::info("[EvolutionarySelector] Waiting for pipeline to become READY...");
//         while (!ctx_.pipelineReady.load(std::memory_order_acquire)) {
//             if (ctx_.shutdown_flag.load()) {
//                 spdlog::info("[EvolutionarySelector] Shutdown requested during wait");
//                 std::this_thread::sleep_for(50ms);
//                 return;
//             }
//             std::this_thread::sleep_for(50ms);
//         }

//         // Extended warmup
//         spdlog::info("[EvolutionarySelector] Pipeline READY. Warming up for 5 seconds...");
//         std::this_thread::sleep_for(5s);    // Warmup

//         spdlog::info("[EvolutionarySelector] Engaging control loop.");

//         // while (running_ && !ctx_.shutdown_flag.load()) {
//         while (running_.load() && !ctx_.shutdown_flag.load()) {
//             auto loopStart = std::chrono::steady_clock::now();

//             try {
//                 auto snapshot = getLatestSnapshot();

//                 if (!snapshot.valid || !hasUsefulData(snapshot)) {
//                     spdlog::warn("[EvolutionarySelector] Snapshot not ready, skipping iteration");
//                     std::this_thread::sleep_for(80ms);
//                     continue;
//                 }

//                 // === Pareto + Scalar GA Evolution (Single Source of Truth) ===
//                 // === Pareto + Scalar GA Evolution (Single Source of Truth) ===
//                 ga_->evolve(snapshot);

//                 // Log current weight regime every 10 iterations for thesis data
//                 if (++loopCount_ % 10 == 0) {
//                     spdlog::info("[EvolutionarySelector] Regime={} | fps={:.1f} | power={:.2f}W | "
//                                  "cpu_temp={:.1f}°""C | gpu_temp={:.1f}°""C | latency={:.1f}ms",
//                                  ga_->getCurrentRegimeName(),
//                                  snapshot.fps, snapshot.avg_power_w_alg,
//                                  snapshot.cpu_temp_c, snapshot.gpu_temp_c,
//                                  snapshot.avg_latency_ms);
//                 }

//             } catch (const std::exception& e) {
//                 spdlog::error("[EvolutionarySelector] Error in control loop: {}", e.what());
//             } catch (...) {
//                 spdlog::error("[EvolutionarySelector] Unknown exception in control loop");
//             }

//             // Precise timing
//             auto elapsed = std::chrono::steady_clock::now() - loopStart;
//             auto sleepTime = std::chrono::milliseconds(800) - elapsed;
//             if (sleepTime > 0ms) {
//                 std::this_thread::sleep_for(sleepTime);
//             }
//         }

//         spdlog::info("[EvolutionarySelector] Control loop ended cleanly");
//     }

//     hrl::MetricsSnapshot getLatestSnapshot() {
//         hrl::MetricsSnapshot snap{};
//         snap.valid = false;

//         if (!aggregator_) {
//             spdlog::warn("[EvolutionarySelector] Aggregator is NULL");
//             return snap;
//         }

//         try {
//             auto raw = aggregator_->getLatestSnapshot();
//             if (!raw.isValid()) {
//                 spdlog::debug("[EvolutionarySelector] Raw snapshot not valid yet");
//                 return snap;
//             }

//             // Safe conversion with joulesPerFrame priority
//             snap.fps = raw.algorithmStats.fps;

//             snap.avg_power_w_alg = (raw.joulesPerFrame > 0 && raw.algorithmStats.totalProcTimeMs > 0)
//                 ? raw.joulesPerFrame / (raw.algorithmStats.totalProcTimeMs / 1000.0)
//                 : raw.powerStats.averagePower();

//             snap.cpu_util_avg = (raw.socInfo.CPU1_Utilization_Percent +
//                                  raw.socInfo.CPU2_Utilization_Percent +
//                                  raw.socInfo.CPU3_Utilization_Percent +
//                                  raw.socInfo.CPU4_Utilization_Percent) / 4.0;

//             snap.gpu_util_avg   = raw.socInfo.GR3D_Frequency_Percent;
//             snap.cpu_temp_c     = raw.socInfo.CPU_Temperature_C;
//             snap.gpu_temp_c     = raw.socInfo.GPU_Temperature_C;
//             snap.avg_latency_ms = raw.endToEndLatencyMs;
//             snap.joulesPerFrame = raw.joulesPerFrame;   // Important for ERL reward

//             snap.valid = true;
//             spdlog::debug("[EvolutionarySelector] Valid snapshot (fps={:.1f})", snap.fps);
//             return snap;

//         } catch (const std::exception& e) {
//             spdlog::warn("[EvolutionarySelector] Exception in getLatestSnapshot: {}", e.what());
//         } catch (...) {
//             spdlog::warn("[EvolutionarySelector] Unknown exception in getLatestSnapshot");
//         }

//         return snap;  // invalid
//     }

//     static bool hasUsefulData(const hrl::MetricsSnapshot& s) {
//         return s.fps > 5.0 && s.avg_power_w_alg > 0.5;
//     }
// };


// //==============================================================================
// // // EvolutionarySelector_02.h - FINAL PhD-Ready (Best of Both Worlds)
// // // Pareto + Scalar Hybrid | Strong Defensive Checks | Clean Orchestrator

// // // EvolutionarySelector_02.h - FINAL PhD-Ready (Best of Both Worlds)
// // // Pareto + Scalar Hybrid | Strong Defensive Checks | Clean Orchestrator
// // #pragma once

// // #include <atomic>
#include <algorithm>
// // #include <chrono>
// // #include <thread>
// // #include <memory>
// // #include <spdlog/spdlog.h>

// // #include "IModule.h"
// // #include "RuntimeControls.h"
// // #include "IScheduler.h"
// // #include "Scheduler.h"
// // #include "RuntimeControls.h"   // ? ADD THIS
// // #include "../Stage_01/Concretes/SystemMetricsAggregatorConcrete_v3_2.h"
// // #include "GeneticAlgorithm_05.h"   // ? Pareto core (single source of truth)
// // #include "../Stage_01/Others/utils.h"

// // using namespace std::chrono_literals;

// // class EvolutionarySelector : public IModule {
// // public:
// //     EvolutionarySelector(const json& cfg,
// //                          Context& ctx,
// //                          std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator,
// //                          std::shared_ptr<hrl::RuntimeControls> runtimeControls)
// //         : cfg_(cfg),
// //           ctx_(ctx),
// //           aggregator_(std::move(aggregator)),
// //           runtimeControls_(std::move(runtimeControls)),
// //           scheduler_(hrl::Scheduler::Limits{}),
// //           running_(false) {

// //         // ====== GA Configuration (with Local clamping) ======
// //         hrl::GAConfig gaCfg;
// //         gaCfg.popSize             = utils::local_clamp<size_t>(cfg_.value("ga_population_size", 12), 4u, 16u);
// //         gaCfg.minPopSize          = utils::local_clamp<size_t>(cfg_.value("ga_min_pop_size", 6), 2u, gaCfg.popSize);
// //         gaCfg.eliteCount          = cfg_.value("ga_elite_count", 3);
// //         gaCfg.crossoverRate       = utils::local_clamp<double>(cfg_.value("ga_crossover_rate", 0.75), 0.5, 0.95);
// //         gaCfg.mutationRate        = utils::local_clamp<double>(cfg_.value("ga_mutation_rate", 0.18), 0.01, 0.5);
// //         gaCfg.explorationDecay    = cfg_.value("ga_exploration_decay", 0.96);
// //         gaCfg.controlIntervalMs   = cfg_.value("control_interval_ms", 800);
// //         gaCfg.reductionStartGen   = cfg_.value("ga_reduction_start_gen", 10);
// //         gaCfg.reductionRatio      = utils::local_clamp<double>(cfg_.value("ga_reduction_ratio", 0.6), 0.3, 0.9);

// //         gaCfg.adaptive_weights_enabled = cfg_.value("adaptive_weights_enabled", false);


// //         // PhD evidence export: ERL/Pareto traceability.
// //         gaCfg.export_pareto_evidence = cfg_.value("export_pareto_evidence", false);
// //         gaCfg.pareto_csv_path        = cfg_.value("pareto_csv", std::string("output/erl_pareto_front.csv"));
// //         gaCfg.action_csv_path        = cfg_.value("action_csv", std::string("output/erl_action_trace.csv"));
        

// //         if (gaCfg.adaptive_weights_enabled) {
// //             gaCfg.awm_config.update_interval_gens   = cfg_.value("awm_update_interval_gens", 5);
// //             gaCfg.awm_config.battery_critical_watts = cfg_.value("awm_battery_critical_watts", 8.0);
// //             gaCfg.awm_config.thermal_warn_cpu_c     = cfg_.value("awm_thermal_warn_cpu_c", 75.0);
// //             gaCfg.awm_config.thermal_warn_gpu_c     = cfg_.value("awm_thermal_warn_gpu_c", 78.0);
// //             gaCfg.awm_config.thermal_crit_cpu_c     = cfg_.value("awm_thermal_crit_cpu_c", 80.0);
// //             gaCfg.awm_config.thermal_crit_gpu_c     = cfg_.value("awm_thermal_crit_gpu_c", 83.0);
// //             gaCfg.awm_config.latency_sla_ms         = cfg_.value("awm_latency_sla_ms", 45.0);
// //             gaCfg.awm_config.fps_underperform_ratio = cfg_.value("awm_fps_underperform_ratio", 0.70);
// //             gaCfg.awm_config.transition_smoothing   = cfg_.value("awm_transition_smoothing", 0.3);

// //             spdlog::info("[EvolutionarySelector] AdaptiveWeightManager configured "
// //                          "(update every {} gens, thermal_crit={:.0f}°""C/{:.0f}°""C)",
// //                          gaCfg.awm_config.update_interval_gens,
// //                          gaCfg.awm_config.thermal_crit_cpu_c,
// //                          gaCfg.awm_config.thermal_crit_gpu_c);
// //         }

// //         try {
// //             ga_ = std::make_unique<hrl::GeneticAlgorithm>(
// //                 gaCfg, aggregator_, runtimeControls_, &scheduler_);

// //             spdlog::info("[EvolutionarySelector] - Pareto-GA Hybrid Ready (pop={}-{}, reduction at gen {})",
// //                          gaCfg.popSize, gaCfg.minPopSize, gaCfg.reductionStartGen);
// //         } catch (const std::exception& e) {
// //             spdlog::error("[EvolutionarySelector] Failed to initialize GeneticAlgorithm: {}", e.what());
// //             throw;
// //         }
// //     }

// //     bool validate() override {
// //         spdlog::info("[EvolutionarySelector] Validating configuration...");
// //         if (!aggregator_) {
// //             spdlog::error("[EvolutionarySelector] Aggregator is null");
// //             return false;
// //         }
// //         if (!runtimeControls_) {
// //             spdlog::error("[EvolutionarySelector] RuntimeControls is null");
// //             return false;
// //         }
// //         spdlog::info("[EvolutionarySelector] Validation successful");
// //         return true;
// //         // return aggregator_ && runtimeControls_; // Alfer testing all code just used thise line
// //     }

// //     void start() override {
// //         spdlog::info("[EvolutionarySelector] Starting evolutionary control loop...");
// //         //running_.store(true);
// //         running_.store(true, std::memory_order_release);
// //         controlThread_ = std::thread(&EvolutionarySelector::controlLoop, this);
// //     }

// //     void stop() override {
// //         spdlog::info("[EvolutionarySelector] Stopping...");
// //         //running_.store(false);
// //         running_.store(false, std::memory_order_release);
// //         if (controlThread_.joinable()) controlThread_.join();
// //         spdlog::info("[EvolutionarySelector] Stopped");
// //     }

// //     // Optional helper for external modules (e.g. logging or RL)
// //     hrl::RLAction getBestAction() const {
// //         return ga_ ? ga_->getBestAction() : hrl::RLAction{};
// //     }

// // private:
// //     json cfg_;
// //     Context& ctx_;
// //     std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator_;
// //     std::shared_ptr<hrl::RuntimeControls> runtimeControls_;
// //     hrl::Scheduler scheduler_;
// //     std::unique_ptr<hrl::GeneticAlgorithm> ga_;
// //     std::atomic<bool> running_{false};
// //     std::thread controlThread_;
// //     int loopCount_ = 0;  // Control loop iteration counter (for periodic regime logging)

// //     void controlLoop() {
// //         spdlog::debug("[EvolutionarySelector] Control loop started");

// //         // Pipeline readiness wait
// //         spdlog::info("[EvolutionarySelector] Waiting for pipeline to become READY...");
// //         while (!ctx_.pipelineReady.load(std::memory_order_acquire)) {
// //             if (ctx_.shutdown_flag.load()) {
// //                 spdlog::info("[EvolutionarySelector] Shutdown requested during wait");
// //                 std::this_thread::sleep_for(50ms);
// //                 return;
// //             }
// //             std::this_thread::sleep_for(50ms);
// //         }

// //         // Extended warmup
// //         spdlog::info("[EvolutionarySelector] Pipeline READY. Warming up for 5 seconds...");
// //         std::this_thread::sleep_for(5s);    // Warmup

// //         spdlog::info("[EvolutionarySelector] Engaging control loop.");

// //         // while (running_ && !ctx_.shutdown_flag.load()) {
// //         while (running_.load() && !ctx_.shutdown_flag.load()) {
// //             auto loopStart = std::chrono::steady_clock::now();

// //             try {
// //                 auto snapshot = getLatestSnapshot();

// //                 if (!snapshot.valid || !hasUsefulData(snapshot)) {
// //                     spdlog::warn("[EvolutionarySelector] Snapshot not ready, skipping iteration");
// //                     std::this_thread::sleep_for(80ms);
// //                     continue;
// //                 }

// //                 // === Pareto + Scalar GA Evolution (Single Source of Truth) ===
// //                 // === Pareto + Scalar GA Evolution (Single Source of Truth) ===
// //                 ga_->evolve(snapshot);

// //                 // Log current weight regime every 10 iterations for thesis data
// //                 if (++loopCount_ % 10 == 0) {
// //                     spdlog::info("[EvolutionarySelector] Regime={} | fps={:.1f} | power={:.2f}W | "
// //                                  "cpu_temp={:.1f}°""C | gpu_temp={:.1f}°""C | latency={:.1f}ms",
// //                                  ga_->getCurrentRegimeName(),
// //                                  snapshot.fps, snapshot.avg_power_w_alg,
// //                                  snapshot.cpu_temp_c, snapshot.gpu_temp_c,
// //                                  snapshot.avg_latency_ms);
// //                 }

// //             } catch (const std::exception& e) {
// //                 spdlog::error("[EvolutionarySelector] Error in control loop: {}", e.what());
// //             } catch (...) {
// //                 spdlog::error("[EvolutionarySelector] Unknown exception in control loop");
// //             }

// //             // Precise timing
// //             auto elapsed = std::chrono::steady_clock::now() - loopStart;
// //             auto sleepTime = std::chrono::milliseconds(800) - elapsed;
// //             if (sleepTime > 0ms) {
// //                 std::this_thread::sleep_for(sleepTime);
// //             }
// //         }

// //         spdlog::info("[EvolutionarySelector] Control loop ended cleanly");
// //     }

// //     hrl::MetricsSnapshot getLatestSnapshot() {
// //         hrl::MetricsSnapshot snap{};
// //         snap.valid = false;

// //         if (!aggregator_) {
// //             spdlog::warn("[EvolutionarySelector] Aggregator is NULL");
// //             return snap;
// //         }

// //         try {
// //             auto raw = aggregator_->getLatestSnapshot();
// //             if (!raw.isValid()) {
// //                 spdlog::debug("[EvolutionarySelector] Raw snapshot not valid yet");
// //                 return snap;
// //             }

// //             // Safe conversion with joulesPerFrame priority
// //             snap.fps = raw.algorithmStats.fps;

// //             snap.avg_power_w_alg = (raw.joulesPerFrame > 0 && raw.algorithmStats.totalProcTimeMs > 0)
// //                 ? raw.joulesPerFrame / (raw.algorithmStats.totalProcTimeMs / 1000.0)
// //                 : raw.powerStats.averagePower();

// //             snap.cpu_util_avg = (raw.socInfo.CPU1_Utilization_Percent +
// //                                  raw.socInfo.CPU2_Utilization_Percent +
// //                                  raw.socInfo.CPU3_Utilization_Percent +
// //                                  raw.socInfo.CPU4_Utilization_Percent) / 4.0;

// //             snap.gpu_util_avg   = raw.socInfo.GR3D_Frequency_Percent;
// //             snap.cpu_temp_c     = raw.socInfo.CPU_Temperature_C;
// //             snap.gpu_temp_c     = raw.socInfo.GPU_Temperature_C;
// //             snap.avg_latency_ms = raw.endToEndLatencyMs;
// //             snap.joulesPerFrame = raw.joulesPerFrame;   // Important for ERL reward

// //             snap.valid = true;
// //             spdlog::debug("[EvolutionarySelector] Valid snapshot (fps={:.1f})", snap.fps);
// //             return snap;

// //         } catch (const std::exception& e) {
// //             spdlog::warn("[EvolutionarySelector] Exception in getLatestSnapshot: {}", e.what());
// //         } catch (...) {
// //             spdlog::warn("[EvolutionarySelector] Unknown exception in getLatestSnapshot");
// //         }

// //         return snap;  // invalid
// //     }

// //     static bool hasUsefulData(const hrl::MetricsSnapshot& s) {
// //         return s.fps > 5.0 && s.avg_power_w_alg > 0.5;
// //     }
// // };

// // EvolutionarySelector_02.h - FINAL PhD-Ready (Best of Both Worlds)
// // Pareto + Scalar Hybrid | Strong Defensive Checks | Clean Orchestrator

// // EvolutionarySelector_02.h - FINAL PhD-Ready (Best of Both Worlds)
// // Pareto + Scalar Hybrid | Strong Defensive Checks | Clean Orchestrator
// #pragma once

// #include <atomic>
#include <algorithm>
// #include <chrono>
// #include <thread>
// #include <memory>
// #include <spdlog/spdlog.h>

// #include "IModule.h"
// #include "RuntimeControls.h"
// #include "IScheduler.h"
// #include "Scheduler.h"
// #include "RuntimeControls.h"   // ? ADD THIS
// #include "../Stage_01/Concretes/SystemMetricsAggregatorConcrete_v3_2.h"
// #include "GeneticAlgorithm_05.h"   // ? Pareto core (single source of truth)
// #include "../Stage_01/Others/utils.h"

// using namespace std::chrono_literals;

// class EvolutionarySelector : public IModule {
// public:
//     EvolutionarySelector(const json& cfg,
//                          Context& ctx,
//                          std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator,
//                          std::shared_ptr<hrl::RuntimeControls> runtimeControls)
//         : cfg_(cfg),
//           ctx_(ctx),
//           aggregator_(std::move(aggregator)),
//           runtimeControls_(std::move(runtimeControls)),
//           scheduler_(hrl::Scheduler::Limits{}),
//           running_(false) {

//         auto jDouble = [this](const char* primary, const char* alias, double def) -> double {
//             try {
//                 if (cfg_.contains(primary) && !cfg_[primary].is_null()) return cfg_[primary].get<double>();
//                 if (alias && cfg_.contains(alias) && !cfg_[alias].is_null()) return cfg_[alias].get<double>();
//             } catch (...) {}
//             return def;
//         };

//         //auto jInt = [this](const char* primary, const char* alias, int def) -> int {
//         auto jInt = [this, &jDouble](const char* primary, const char* alias, int def) -> int {
//             return static_cast<int>(jDouble(primary, alias, static_cast<double>(def)));
//         };

//         auto jBool = [this](const char* primary, const char* alias, bool def) -> bool {
//             try {
//                 const json* v = nullptr;
//                 if (cfg_.contains(primary) && !cfg_[primary].is_null()) v = &cfg_[primary];
//                 else if (alias && cfg_.contains(alias) && !cfg_[alias].is_null()) v = &cfg_[alias];
//                 if (!v) return def;
//                 if (v->is_boolean()) return v->get<bool>();
//                 if (v->is_number_integer()) return v->get<int>() != 0;
//                 if (v->is_string()) {
//                     const std::string s = v->get<std::string>();
//                     return s == "true" || s == "TRUE" || s == "1" || s == "yes" || s == "ON" || s == "on";
//                 }
//             } catch (...) {}
//             return def;
//         };

//         auto jString = [this](const char* primary, const char* alias, const std::string& def) -> std::string {
//             try {
//                 if (cfg_.contains(primary) && !cfg_[primary].is_null()) return cfg_[primary].get<std::string>();
//                 if (alias && cfg_.contains(alias) && !cfg_[alias].is_null()) return cfg_[alias].get<std::string>();
//             } catch (...) {}
//             return def;
//         };

//         // ====== GA Configuration (supports both old and new JSON key names) ======
//         hrl::GAConfig gaCfg;
//         gaCfg.popSize             = utils::local_clamp<size_t>(static_cast<size_t>(jInt("ga_population_size", "population_size", 12)), 4u, 64u);
//         gaCfg.minPopSize          = utils::local_clamp<size_t>(static_cast<size_t>(jInt("ga_min_pop_size", nullptr, 4)), 2u, gaCfg.popSize);
//         gaCfg.eliteCount          = static_cast<size_t>(utils::local_clamp<int>(jInt("ga_elite_count", "elite_count", 2), 1, static_cast<int>(gaCfg.popSize)));
//         gaCfg.crossoverRate       = utils::local_clamp<double>(jDouble("ga_crossover_rate", "crossover_rate", 0.70), 0.0, 1.0);
//         gaCfg.mutationRate        = utils::local_clamp<double>(jDouble("ga_mutation_rate", "mutation_rate", 0.15), 0.0, 1.0);
//         gaCfg.explorationDecay    = utils::local_clamp<double>(jDouble("ga_exploration_decay", nullptr, 0.96), 0.80, 1.0);
//         gaCfg.controlIntervalMs   = utils::local_clamp<int>(jInt("control_interval_ms", nullptr, 500), 50, 10000);
//         gaCfg.reductionStartGen   = static_cast<size_t>(utils::local_clamp<int>(jInt("ga_reduction_start_gen", nullptr, 10), 1, 100000));
//         gaCfg.reductionRatio      = utils::local_clamp<double>(jDouble("ga_reduction_ratio", nullptr, 0.60), 0.10, 1.0);

//         // [Adaptive mutation] plateau-driven mutation control
//         gaCfg.adaptiveMutationEnabled       = jBool("ga_adaptive_mutation_enabled", nullptr, true);
//         gaCfg.maxMutationRate               = utils::local_clamp<double>(jDouble("ga_max_mutation_rate", nullptr, 0.35), gaCfg.mutationRate, 1.0);
//         gaCfg.mutationPlateauRelThreshold   = utils::local_clamp<double>(jDouble("ga_mutation_plateau_rel_threshold", nullptr, 0.02), 0.0, 1.0);
//         gaCfg.mutationPlateauAbsFloor       = jDouble("ga_mutation_plateau_abs_floor", nullptr, 0.0005);
//         gaCfg.mutationImproveFactor         = utils::local_clamp<double>(jDouble("ga_mutation_improve_factor", nullptr, 0.80), 0.0, 1.0);
//         gaCfg.mutationPlateauStep           = utils::local_clamp<double>(jDouble("ga_mutation_plateau_step", nullptr, 0.10), 0.0, 1.0);

//         // [Surrogate] online counterfactual fitness (thesis gap 2)
//         gaCfg.surrogateEnabled       = jBool("ga_surrogate_enabled", nullptr, true);
//         gaCfg.surrogateExploreWeight = utils::local_clamp<double>(jDouble("ga_surrogate_explore_weight", nullptr, 0.15), 0.0, 5.0);

//         // ADD THESE FOUR LINES:
//         gaCfg.weight_fps      = utils::local_clamp<double>(jDouble("obj_weight_fps", nullptr, 1.0), 0.0, 10.0);
//         gaCfg.weight_power    = utils::local_clamp<double>(jDouble("obj_weight_power", nullptr, 0.5), 0.0, 10.0);
//         gaCfg.weight_temp     = utils::local_clamp<double>(jDouble("obj_weight_temp", nullptr, 0.5), 0.0, 10.0);
//         gaCfg.weight_latency  = utils::local_clamp<double>(jDouble("obj_weight_latency", nullptr, 0.3), 0.0, 10.0);

//         controlIntervalMs_        = gaCfg.controlIntervalMs;
//         warmupMs_                 = utils::local_clamp<int>(jInt("warmup_ms", nullptr, 5000), 0, 60000);
//         minFpsUseful_             = jDouble("min_fps_for_evolution", nullptr, 1.0);
//         minPowerUseful_           = jDouble("min_power_for_evolution", nullptr, 0.0);
//         requirePowerForEvolution_ = jBool("require_power_for_evolution", nullptr, false);

//         gaCfg.stableFramesRequired     = utils::local_clamp<int>(jInt("stable_frames_required", nullptr, 1), 1, 120);
//         gaCfg.minFpsForEvolution       = minFpsUseful_;
//         gaCfg.minPowerForEvolution     = minPowerUseful_;
//         gaCfg.requirePowerForEvolution = requirePowerForEvolution_;

//         gaCfg.adaptive_weights_enabled = jBool("adaptive_weights_enabled", nullptr, false);

//         // Reproducibility: wire "seed" from config into the GA RNG.
//         // Default -1 keeps non-deterministic behaviour if the key is absent.
//         gaCfg.rngSeed = static_cast<long>(jInt("seed", nullptr, -1));

//         // Search-space floor for target_fps gene (raised from old hardcoded 8.0).
//         gaCfg.minTargetFps = utils::local_clamp<double>(jDouble("ga_min_target_fps", nullptr, 20.0), 1.0, 60.0);
//         gaCfg.maxTargetFps = utils::local_clamp<double>(jDouble("ga_max_target_fps", nullptr, 60.0), gaCfg.minTargetFps, 240.0);

//         // PhD evidence export: ERL/Pareto traceability.
//         gaCfg.export_pareto_evidence = jBool("export_pareto_evidence", nullptr, true);
//         gaCfg.pareto_csv_path        = jString("pareto_csv", nullptr, "output/erl_pareto_front.csv");
//         gaCfg.action_csv_path        = jString("action_csv", nullptr, "output/erl_action_trace.csv");

//         spdlog::info("[EvolutionarySelector] Effective ERL config: pop={} minPop={} elite={} cross={:.2f} mut={:.2f} interval={}ms export={} pareto_csv={} action_csv={} minFPS={:.2f} requirePower={}",
//                      gaCfg.popSize, gaCfg.minPopSize, gaCfg.eliteCount, gaCfg.crossoverRate, gaCfg.mutationRate,
//                      gaCfg.controlIntervalMs, gaCfg.export_pareto_evidence ? "ON" : "OFF",
//                      gaCfg.pareto_csv_path, gaCfg.action_csv_path, minFpsUseful_,
//                      requirePowerForEvolution_ ? "YES" : "NO");


//         if (gaCfg.adaptive_weights_enabled) {
//             gaCfg.awm_config.update_interval_gens   = jInt("awm_update_interval_gens", nullptr, 5);
//             gaCfg.awm_config.battery_critical_watts = jDouble("awm_battery_critical_watts", nullptr, 8.0);
//             gaCfg.awm_config.thermal_warn_cpu_c     = jDouble("awm_thermal_warn_cpu_c", nullptr, 75.0);
//             gaCfg.awm_config.thermal_warn_gpu_c     = jDouble("awm_thermal_warn_gpu_c", nullptr, 78.0);
//             gaCfg.awm_config.thermal_crit_cpu_c     = jDouble("awm_thermal_crit_cpu_c", nullptr, 80.0);
//             gaCfg.awm_config.thermal_crit_gpu_c     = jDouble("awm_thermal_crit_gpu_c", nullptr, 83.0);
//             gaCfg.awm_config.latency_sla_ms         = jDouble("awm_latency_sla_ms", nullptr, 45.0);
//             gaCfg.awm_config.fps_underperform_ratio = jDouble("awm_fps_underperform_ratio", nullptr, 0.70);
//             gaCfg.awm_config.transition_smoothing   = jDouble("awm_transition_smoothing", nullptr, 0.3);

//             spdlog::info("[EvolutionarySelector] AdaptiveWeightManager configured "
//                          "(update every {} gens, thermal_crit={:.0f}°""C/{:.0f}°""C)",
//                          gaCfg.awm_config.update_interval_gens,
//                          gaCfg.awm_config.thermal_crit_cpu_c,
//                          gaCfg.awm_config.thermal_crit_gpu_c);
//         }

//         try {
//             ga_ = std::make_unique<hrl::GeneticAlgorithm>(
//                 gaCfg, aggregator_, runtimeControls_, &scheduler_);

//             spdlog::info("[EvolutionarySelector] - Pareto-GA Hybrid Ready (pop={}-{}, reduction at gen {})",
//                          gaCfg.popSize, gaCfg.minPopSize, gaCfg.reductionStartGen);
//         } catch (const std::exception& e) {
//             spdlog::error("[EvolutionarySelector] Failed to initialize GeneticAlgorithm: {}", e.what());
//             throw;
//         }
//     }

//     bool validate() override {
//         spdlog::info("[EvolutionarySelector] Validating configuration...");
//         if (!aggregator_) {
//             spdlog::error("[EvolutionarySelector] Aggregator is null");
//             return false;
//         }
//         if (!runtimeControls_) {
//             spdlog::error("[EvolutionarySelector] RuntimeControls is null");
//             return false;
//         }
//         spdlog::info("[EvolutionarySelector] Validation successful");
//         return true;
//         // return aggregator_ && runtimeControls_; // Alfer testing all code just used thise line
//     }

//     void start() override {
//         spdlog::info("[EvolutionarySelector] Starting evolutionary control loop...");
//         //running_.store(true);
//         running_.store(true, std::memory_order_release);
//         controlThread_ = std::thread(&EvolutionarySelector::controlLoop, this);
//     }

//     void stop() override {
//         spdlog::info("[EvolutionarySelector] Stopping...");
//         //running_.store(false);
//         running_.store(false, std::memory_order_release);
//         if (controlThread_.joinable()) controlThread_.join();
//         scheduler_.reset();
//         spdlog::info("[EvolutionarySelector] Stopped");
//         spdlog::info("[EvolutionarySelector] System settings restored to default (BALANCED, 60 FPS, GPU on)");
//     }

//     // Optional helper for external modules (e.g. logging or RL)
//     hrl::RLAction getBestAction() const {
//         return ga_ ? ga_->getBestAction() : hrl::RLAction{};
//     }

// private:
//     json cfg_;
//     Context& ctx_;
//     std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator_;
//     std::shared_ptr<hrl::RuntimeControls> runtimeControls_;
//     hrl::Scheduler scheduler_;
//     std::unique_ptr<hrl::GeneticAlgorithm> ga_;
//     std::atomic<bool> running_{false};
//     std::thread controlThread_;
//     int loopCount_ = 0;  // Control loop iteration counter (for periodic regime logging)
//     int controlIntervalMs_ = 500;
//     int warmupMs_ = 5000;
//     double minFpsUseful_ = 1.0;
//     double minPowerUseful_ = 0.0;
//     bool requirePowerForEvolution_ = false;

//     // ADD THESE TWO LINES:
//     double lastValidPower_ = 0.0;          
//     std::deque<double> powerHistory_;

// //=========================================================================
// void controlLoop() {
//         spdlog::debug("[EvolutionarySelector] Control loop started");

//         // Pipeline readiness wait
//         spdlog::info("[EvolutionarySelector] Waiting for pipeline to become READY...");
//         while (!ctx_.pipelineReady.load(std::memory_order_acquire)) {
//             if (ctx_.shutdown_flag.load()) {
//                 spdlog::info("[EvolutionarySelector] Shutdown requested during wait");
//                 std::this_thread::sleep_for(50ms);
//                 return;
//             }
//             std::this_thread::sleep_for(50ms);
//         }

//         // Extended warmup (Assuming warmupMs_ is defined in your class)
//         spdlog::info("[EvolutionarySelector] Pipeline READY. Warming up...");
//         //std::this_thread::sleep_for(std::chrono::milliseconds(5000));
//         if (warmupMs_ > 0) {
//             std::this_thread::sleep_for(std::chrono::milliseconds(warmupMs_));
//         }

//         spdlog::info("[EvolutionarySelector] Engaging control loop.");

//         running_.store(true);
//         int consecutiveSkips = 0;
//         const int MAX_SKIPS = 20; // 20 skips at 500ms = 10 seconds to trigger emergency recovery
//         uint64_t loopCount = 0;   // Local counter for the 10-iteration logging

//         while (running_.load() && !ctx_.shutdown_flag.load()) {
//             auto loopStart = std::chrono::steady_clock::now();

//             try {
//                 auto snapshot = getLatestSnapshot();

//                 if (!snapshot.valid || !hasUsefulData(snapshot)) {
//                     consecutiveSkips++;        
//                     // Fix 3 (EvolutionarySelector_02.h): Increase starvation sleep 
//                     // interval from 80ms to 500ms to reduce CPU contention.
//                     if (consecutiveSkips >= MAX_SKIPS) {
//                         spdlog::warn("[EvolutionarySelector] EMERGENCY RECOVERY: resetting to MAX PERFORMANCE");
//                         scheduler_.reset(); // Force high-performance profile
//                         consecutiveSkips = 0;
//                     }
//                     spdlog::warn("[EvolutionarySelector] Snapshot not ready, skipping iteration");
//                     std::this_thread::sleep_for(500ms);
//                     continue;
//                 }

//                 // === Pareto + Scalar GA Evolution (Single Source of Truth) ===
//                 consecutiveSkips = 0;   
//                 ga_->evolve(snapshot);

//                 // Log current weight regime every 10 iterations for thesis data
//                 if (++loopCount % 10 == 0) {
//                     spdlog::info("[EvolutionarySelector] Regime={} | fps={:.1f} | power={:.2f}W | "
//                                  "cpu_temp={:.1f}°C | gpu_temp={:.1f}°C | latency={:.1f}ms",
//                                  ga_->getCurrentRegimeName(),
//                                  snapshot.fps, snapshot.avg_power_w_alg,
//                                  snapshot.cpu_temp_c, snapshot.gpu_temp_c,
//                                  snapshot.avg_latency_ms);
//                 }

//             } catch (const std::exception& e) {
//                 spdlog::error("[EvolutionarySelector] Error in control loop: {}", e.what());
//             } catch (...) {
//                 spdlog::error("[EvolutionarySelector] Unknown exception in control loop");
//             }

//             // Precise timing (Defaults to 2000ms standard ERL tick rate)
//             auto elapsed = std::chrono::steady_clock::now() - loopStart;
//             //auto sleepTime = std::chrono::milliseconds(2000) - elapsed; 
//             auto sleepTime = std::chrono::milliseconds(controlIntervalMs_) - elapsed;
//             if (sleepTime > 0ms) {
//                 std::this_thread::sleep_for(sleepTime);
//             }
//         }

//         spdlog::info("[EvolutionarySelector] Control loop ended cleanly");
//     }

//     //==================================================================
//     // void controlLoop() {
//     //     spdlog::debug("[EvolutionarySelector] Control loop started");

//     //     // Pipeline readiness wait
//     //     spdlog::info("[EvolutionarySelector] Waiting for pipeline to become READY...");
//     //     while (!ctx_.pipelineReady.load(std::memory_order_acquire)) {
//     //         if (ctx_.shutdown_flag.load()) {
//     //             spdlog::info("[EvolutionarySelector] Shutdown requested during wait");
//     //             std::this_thread::sleep_for(50ms);
//     //             return;
//     //         }
//     //         std::this_thread::sleep_for(50ms);
//     //     }

//     //     // Extended warmup
//     //     spdlog::info("[EvolutionarySelector] Pipeline READY. Warming up for {} ms...", warmupMs_);
//     //     if (warmupMs_ > 0) {
//     //         std::this_thread::sleep_for(std::chrono::milliseconds(warmupMs_));
//     //     }

//     //     spdlog::info("[EvolutionarySelector] Engaging control loop.");

//     //     // Inside the main evolutionary control loop:
//     //     void run() {
//     //         running_ = true;
//     //         int consecutiveSkips = 0;
//     //         const int MAX_SKIPS = 10; // Example threshold

//     //         // while (running_ && !ctx_.shutdown_flag.load()) {
//     //         while (running_.load() && !ctx_.shutdown_flag.load()) {
//     //             auto loopStart = std::chrono::steady_clock::now();

//     //             try {
//     //                 auto snapshot = getLatestSnapshot();

//     //                 if (!snapshot.valid || !hasUsefulData(snapshot)) {
//     //                     consecutiveSkips++;        
//     //                     // Fix 3 (EvolutionarySelector_02.h): Increase starvation sleep 
//     //                     // interval from 80ms to 500ms to reduce CPU contention.
//     //                     if (consecutiveSkips >= MAX_SKIPS) {
//     //                         spdlog::warn("[EvolutionarySelector] EMERGENCY RECOVERY: resetting to MAX PERFORMANCE");
//     //                         scheduler_.reset(); // Force high-performance profile
//     //                         consecutiveSkips = 0;
//     //                     }
//     //                     spdlog::warn("[EvolutionarySelector] Snapshot not ready, skipping iteration");
//     //                     std::this_thread::sleep_for(500ms);
//     //                     continue;
//     //                 }


//     //                 // === Pareto + Scalar GA Evolution (Single Source of Truth) ===
//     //                 // === Pareto + Scalar GA Evolution (Single Source of Truth) ===
//     //                 consecutiveSkips = 0;   

//     //                 ga_->evolve(snapshot);

//     //                 // Log current weight regime every 10 iterations for thesis data
//     //                 if (++loopCount_ % 10 == 0) {
//     //                     spdlog::info("[EvolutionarySelector] Regime={} | fps={:.1f} | power={:.2f}W | "
//     //                                 "cpu_temp={:.1f}°""C | gpu_temp={:.1f}°""C | latency={:.1f}ms",
//     //                                 ga_->getCurrentRegimeName(),
//     //                                 snapshot.fps, snapshot.avg_power_w_alg,
//     //                                 snapshot.cpu_temp_c, snapshot.gpu_temp_c,
//     //                                 snapshot.avg_latency_ms);
//     //                 }

//     //             } catch (const std::exception& e) {
//     //                 spdlog::error("[EvolutionarySelector] Error in control loop: {}", e.what());
//     //             } catch (...) {
//     //                 spdlog::error("[EvolutionarySelector] Unknown exception in control loop");
//     //             }

//     //             // Precise timing
//     //             auto elapsed = std::chrono::steady_clock::now() - loopStart;
//     //             auto sleepTime = std::chrono::milliseconds(controlIntervalMs_) - elapsed;
//     //             if (sleepTime > 0ms) {
//     //                 std::this_thread::sleep_for(sleepTime);
//     //             }
//     //         }
//     //     }

//     //     spdlog::info("[EvolutionarySelector] Control loop ended cleanly");
//     // }

// //======================================================================================
// hrl::MetricsSnapshot getLatestSnapshot() {
//         hrl::MetricsSnapshot snap{};
//         snap.valid = false;

//         if (!aggregator_) {
//             spdlog::warn("[EvolutionarySelector] Aggregator is NULL");
//             return snap;
//         }

//         try {
//             auto raw = aggregator_->getLatestSnapshot();
//             if (!raw.isValid()) {
//                 spdlog::debug("[EvolutionarySelector] Raw snapshot not valid yet");
//                 return snap;
//             }

//             // FIX 1: Map frame_id (Closes the zeroed frame_id column in trace)
//             snap.frameId = raw.frameId;

//             snap.fps = raw.algorithmStats.fps;

//             // FIX 2: Power mapping with Moving Average Fallback (Closes the blind energy optimizer)
//             double instantPower = raw.powerStats.averagePower();
//             if (raw.joulesPerFrame > 0 && raw.algorithmStats.totalProcTimeMs > 0) {
//                 instantPower = raw.joulesPerFrame / (raw.algorithmStats.totalProcTimeMs / 1000.0);
//             }
            
//             // Correct mW to W scale error
//             if (instantPower > 0.0 && instantPower < 0.5) {
//                 instantPower *= 1000.0; 
//             }
            
//             // Guard against 0.0W hardware polling drops
//             if (instantPower > 0.0) {
//                 lastValidPower_ = instantPower;
//             } else if (lastValidPower_ <= 0.0) {
//                 lastValidPower_ = 5.0; // Sane Jetson Nano default
//             }

//             // Apply 5-sample moving average to smooth out jitter
//             powerHistory_.push_back(lastValidPower_);
//             if (powerHistory_.size() > 5) {
//                 powerHistory_.pop_front();
//             }
            
//             double avgPower = 0.0;
//             for (double p : powerHistory_) {
//                 avgPower += p;
//             }
//             avgPower /= static_cast<double>(powerHistory_.size());

//             snap.avg_power_w_alg = avgPower;

//             // Resource mapping
//             snap.cpu_util_avg = (raw.socInfo.CPU1_Utilization_Percent +
//                                  raw.socInfo.CPU2_Utilization_Percent +
//                                  raw.socInfo.CPU3_Utilization_Percent +
//                                  raw.socInfo.CPU4_Utilization_Percent) / 4.0;
//             snap.gpu_util_avg   = raw.socInfo.GR3D_Frequency_Percent;

//             // FIX 3: Thermal mapping (Closes the obj_temp = 0 bug)
//             // Tegrastats parses can output millidegrees. Normalize to Celsius.
//             double c_temp = raw.socInfo.CPU_Temperature_C;
//             double g_temp = raw.socInfo.GPU_Temperature_C;
            
//             if (c_temp > 1000.0) c_temp /= 1000.0;
//             if (g_temp > 1000.0) g_temp /= 1000.0;

//             snap.cpu_temp_c = c_temp;
//             snap.gpu_temp_c = g_temp;

//             snap.end_to_end_latency_ms = raw.endToEndLatencyMs;
//             snap.avg_latency_ms = raw.endToEndLatencyMs;
            
//             // Re-sync Joules per frame with the smoothed power value
//             snap.joulesPerFrame = snap.avg_power_w_alg * (snap.avg_latency_ms / 1000.0);
//             snap.joules_per_frame = snap.joulesPerFrame;

//             snap.valid = true;
//             spdlog::debug("[EvolutionarySelector] Valid snapshot (fps={:.1f}, pwr={:.2f}W, temp={:.1f}C)", 
//                           snap.fps, snap.avg_power_w_alg, snap.cpu_temp_c);
//             return snap;

//         } catch (const std::exception& e) {
//             spdlog::warn("[EvolutionarySelector] Exception in getLatestSnapshot: {}", e.what());
//         } catch (...) {
//             spdlog::warn("[EvolutionarySelector] Unknown exception in getLatestSnapshot");
//         }

//         return snap;
//     }
// //======================================================================================

// // hrl::MetricsSnapshot getLatestSnapshot() {
// //         hrl::MetricsSnapshot snap{};
// //         snap.valid = false;

// //         if (!aggregator_) {
// //             spdlog::warn("[EvolutionarySelector] Aggregator is NULL");
// //             return snap;
// //         }

// //         try {
// //             auto raw = aggregator_->getLatestSnapshot();
// //             if (!raw.isValid()) {
// //                 spdlog::debug("[EvolutionarySelector] Raw snapshot not valid yet");
// //                 return snap;
// //             }

// //             // FIX 1: Map frame_id (Closes the zeroed frame_id column in trace)
// //             snap.frameId = raw.frameId;

// //             snap.fps = raw.algorithmStats.fps;

// //             // FIX 2: Power mapping (Closes the 0.09W blind energy optimizer)
// //             // If pulling from raw.powerStats.averagePower() yielded 0.09, we have a unit/rail bug.
// //             // We scale up milliWatt-range errors or fallback to a safe 5.0W baseline to ensure 
// //             // the GA has a valid gradient to optimize against.
// //             double power_val = raw.powerStats.averagePower();
// //             if (raw.joulesPerFrame > 0 && raw.algorithmStats.totalProcTimeMs > 0) {
// //                 power_val = raw.joulesPerFrame / (raw.algorithmStats.totalProcTimeMs / 1000.0);
// //             }
            
// //             if (power_val > 0.0 && power_val < 0.5) {
// //                 power_val *= 1000.0; // Correct mW to W scale error
// //             } else if (power_val <= 0.0) {
// //                 power_val = 5.0; // Sane Jetson Nano default
// //             }
// //             snap.avg_power_w_alg = power_val;

// //             snap.cpu_util_avg = (raw.socInfo.CPU1_Utilization_Percent +
// //                                  raw.socInfo.CPU2_Utilization_Percent +
// //                                  raw.socInfo.CPU3_Utilization_Percent +
// //                                  raw.socInfo.CPU4_Utilization_Percent) / 4.0;
// //             snap.gpu_util_avg   = raw.socInfo.GR3D_Frequency_Percent;

// //             // FIX 3: Thermal mapping (Closes the obj_temp = 0 bug)
// //             // Tegrastats parses can output millidegrees. Normalize to Celsius.
// //             double c_temp = raw.socInfo.CPU_Temperature_C;
// //             double g_temp = raw.socInfo.GPU_Temperature_C;
            
// //             if (c_temp > 1000.0) c_temp /= 1000.0;
// //             if (g_temp > 1000.0) g_temp /= 1000.0;

// //             snap.cpu_temp_c = c_temp;
// //             snap.gpu_temp_c = g_temp;

// //             snap.end_to_end_latency_ms = raw.endToEndLatencyMs;
// //             snap.avg_latency_ms = raw.endToEndLatencyMs;
            
// //             // Re-sync Joules per frame with the corrected power value
// //             snap.joulesPerFrame = snap.avg_power_w_alg * (snap.avg_latency_ms / 1000.0);
// //             snap.joules_per_frame = snap.joulesPerFrame;

// //             snap.valid = true;
// //             spdlog::debug("[EvolutionarySelector] Valid snapshot (fps={:.1f}, pwr={:.2f}W, temp={:.1f}C)", 
// //                           snap.fps, snap.avg_power_w_alg, snap.cpu_temp_c);
// //             return snap;

// //         } catch (const std::exception& e) {
// //             spdlog::warn("[EvolutionarySelector] Exception in getLatestSnapshot: {}", e.what());
// //         } catch (...) {
// //             spdlog::warn("[EvolutionarySelector] Unknown exception in getLatestSnapshot");
// //         }

// //         return snap;
// //     }
// //================================================================================
// // //================================================================================

// //     hrl::MetricsSnapshot getLatestSnapshot() {
// //         hrl::MetricsSnapshot snap{};
// //         snap.valid = false;

// //         if (!aggregator_) {
// //             spdlog::warn("[EvolutionarySelector] Aggregator is NULL");
// //             return snap;
// //         }

// //         try {
// //             auto raw = aggregator_->getLatestSnapshot();
// //             if (!raw.isValid()) {
// //                 spdlog::debug("[EvolutionarySelector] Raw snapshot not valid yet");
// //                 return snap;
// //             }

// //             // FIX 1: Map frame_id (Closes the zeroed frame_id column in trace)
// //             snap.frameId = raw.frameId;

// //             // Safe conversion with joulesPerFrame priority
// //             snap.fps = raw.algorithmStats.fps;


// //             snap.avg_power_w_alg = (raw.joulesPerFrame > 0 && raw.algorithmStats.totalProcTimeMs > 0)
// //                 ? raw.joulesPerFrame / (raw.algorithmStats.totalProcTimeMs / 1000.0)
// //                 : raw.powerStats.averagePower();

// //             snap.cpu_util_avg = (raw.socInfo.CPU1_Utilization_Percent +
// //                                  raw.socInfo.CPU2_Utilization_Percent +
// //                                  raw.socInfo.CPU3_Utilization_Percent +
// //                                  raw.socInfo.CPU4_Utilization_Percent) / 4.0;

// //             snap.gpu_util_avg   = raw.socInfo.GR3D_Frequency_Percent;
// //             snap.cpu_temp_c     = raw.socInfo.CPU_Temperature_C;
// //             snap.gpu_temp_c     = raw.socInfo.GPU_Temperature_C;
// //             snap.avg_latency_ms = raw.endToEndLatencyMs;
// //             snap.joulesPerFrame = raw.joulesPerFrame;   // Important for ERL reward

// //             snap.valid = true;
// //             spdlog::debug("[EvolutionarySelector] Valid snapshot (fps={:.1f})", snap.fps);
// //             return snap;

// //         } catch (const std::exception& e) {
// //             spdlog::warn("[EvolutionarySelector] Exception in getLatestSnapshot: {}", e.what());
// //         } catch (...) {
// //             spdlog::warn("[EvolutionarySelector] Unknown exception in getLatestSnapshot");
// //         }

// //         return snap;  // invalid
// //     }

// //================================================================================

// bool hasUsefulData(const hrl::MetricsSnapshot& s) const {
//         if (s.fps < minFpsUseful_) {
//             return false;
//         }
//         if (requirePowerForEvolution_ && s.avg_power_w_alg < minPowerUseful_) {
//             return false;
//         }
//         return true;
//     }
// };


// //==============================================================================
// // // EvolutionarySelector_02.h - FINAL PhD-Ready (Best of Both Worlds)
// // // Pareto + Scalar Hybrid | Strong Defensive Checks | Clean Orchestrator

// // // EvolutionarySelector_02.h - FINAL PhD-Ready (Best of Both Worlds)
// // // Pareto + Scalar Hybrid | Strong Defensive Checks | Clean Orchestrator
// // #pragma once

// // #include <atomic>
#include <algorithm>
// // #include <chrono>
// // #include <thread>
// // #include <memory>
// // #include <spdlog/spdlog.h>

// // #include "IModule.h"
// // #include "RuntimeControls.h"
// // #include "IScheduler.h"
// // #include "Scheduler.h"
// // #include "RuntimeControls.h"   // ? ADD THIS
// // #include "../Stage_01/Concretes/SystemMetricsAggregatorConcrete_v3_2.h"
// // #include "GeneticAlgorithm_05.h"   // ? Pareto core (single source of truth)
// // #include "../Stage_01/Others/utils.h"

// // using namespace std::chrono_literals;

// // class EvolutionarySelector : public IModule {
// // public:
// //     EvolutionarySelector(const json& cfg,
// //                          Context& ctx,
// //                          std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator,
// //                          std::shared_ptr<hrl::RuntimeControls> runtimeControls)
// //         : cfg_(cfg),
// //           ctx_(ctx),
// //           aggregator_(std::move(aggregator)),
// //           runtimeControls_(std::move(runtimeControls)),
// //           scheduler_(hrl::Scheduler::Limits{}),
// //           running_(false) {

// //         // ====== GA Configuration (with Local clamping) ======
// //         hrl::GAConfig gaCfg;
// //         gaCfg.popSize             = utils::local_clamp<size_t>(cfg_.value("ga_population_size", 12), 4u, 16u);
// //         gaCfg.minPopSize          = utils::local_clamp<size_t>(cfg_.value("ga_min_pop_size", 6), 2u, gaCfg.popSize);
// //         gaCfg.eliteCount          = cfg_.value("ga_elite_count", 3);
// //         gaCfg.crossoverRate       = utils::local_clamp<double>(cfg_.value("ga_crossover_rate", 0.75), 0.5, 0.95);
// //         gaCfg.mutationRate        = utils::local_clamp<double>(cfg_.value("ga_mutation_rate", 0.18), 0.01, 0.5);
// //         gaCfg.explorationDecay    = cfg_.value("ga_exploration_decay", 0.96);
// //         gaCfg.controlIntervalMs   = cfg_.value("control_interval_ms", 800);
// //         gaCfg.reductionStartGen   = cfg_.value("ga_reduction_start_gen", 10);
// //         gaCfg.reductionRatio      = utils::local_clamp<double>(cfg_.value("ga_reduction_ratio", 0.6), 0.3, 0.9);

// //         gaCfg.adaptive_weights_enabled = cfg_.value("adaptive_weights_enabled", false);


// //         // PhD evidence export: ERL/Pareto traceability.
// //         gaCfg.export_pareto_evidence = cfg_.value("export_pareto_evidence", false);
// //         gaCfg.pareto_csv_path        = cfg_.value("pareto_csv", std::string("output/erl_pareto_front.csv"));
// //         gaCfg.action_csv_path        = cfg_.value("action_csv", std::string("output/erl_action_trace.csv"));
        

// //         if (gaCfg.adaptive_weights_enabled) {
// //             gaCfg.awm_config.update_interval_gens   = cfg_.value("awm_update_interval_gens", 5);
// //             gaCfg.awm_config.battery_critical_watts = cfg_.value("awm_battery_critical_watts", 8.0);
// //             gaCfg.awm_config.thermal_warn_cpu_c     = cfg_.value("awm_thermal_warn_cpu_c", 75.0);
// //             gaCfg.awm_config.thermal_warn_gpu_c     = cfg_.value("awm_thermal_warn_gpu_c", 78.0);
// //             gaCfg.awm_config.thermal_crit_cpu_c     = cfg_.value("awm_thermal_crit_cpu_c", 80.0);
// //             gaCfg.awm_config.thermal_crit_gpu_c     = cfg_.value("awm_thermal_crit_gpu_c", 83.0);
// //             gaCfg.awm_config.latency_sla_ms         = cfg_.value("awm_latency_sla_ms", 45.0);
// //             gaCfg.awm_config.fps_underperform_ratio = cfg_.value("awm_fps_underperform_ratio", 0.70);
// //             gaCfg.awm_config.transition_smoothing   = cfg_.value("awm_transition_smoothing", 0.3);

// //             spdlog::info("[EvolutionarySelector] AdaptiveWeightManager configured "
// //                          "(update every {} gens, thermal_crit={:.0f}°""C/{:.0f}°""C)",
// //                          gaCfg.awm_config.update_interval_gens,
// //                          gaCfg.awm_config.thermal_crit_cpu_c,
// //                          gaCfg.awm_config.thermal_crit_gpu_c);
// //         }

// //         try {
// //             ga_ = std::make_unique<hrl::GeneticAlgorithm>(
// //                 gaCfg, aggregator_, runtimeControls_, &scheduler_);

// //             spdlog::info("[EvolutionarySelector] - Pareto-GA Hybrid Ready (pop={}-{}, reduction at gen {})",
// //                          gaCfg.popSize, gaCfg.minPopSize, gaCfg.reductionStartGen);
// //         } catch (const std::exception& e) {
// //             spdlog::error("[EvolutionarySelector] Failed to initialize GeneticAlgorithm: {}", e.what());
// //             throw;
// //         }
// //     }

// //     bool validate() override {
// //         spdlog::info("[EvolutionarySelector] Validating configuration...");
// //         if (!aggregator_) {
// //             spdlog::error("[EvolutionarySelector] Aggregator is null");
// //             return false;
// //         }
// //         if (!runtimeControls_) {
// //             spdlog::error("[EvolutionarySelector] RuntimeControls is null");
// //             return false;
// //         }
// //         spdlog::info("[EvolutionarySelector] Validation successful");
// //         return true;
// //         // return aggregator_ && runtimeControls_; // Alfer testing all code just used thise line
// //     }

// //     void start() override {
// //         spdlog::info("[EvolutionarySelector] Starting evolutionary control loop...");
// //         //running_.store(true);
// //         running_.store(true, std::memory_order_release);
// //         controlThread_ = std::thread(&EvolutionarySelector::controlLoop, this);
// //     }

// //     void stop() override {
// //         spdlog::info("[EvolutionarySelector] Stopping...");
// //         //running_.store(false);
// //         running_.store(false, std::memory_order_release);
// //         if (controlThread_.joinable()) controlThread_.join();
// //         spdlog::info("[EvolutionarySelector] Stopped");
// //     }

// //     // Optional helper for external modules (e.g. logging or RL)
// //     hrl::RLAction getBestAction() const {
// //         return ga_ ? ga_->getBestAction() : hrl::RLAction{};
// //     }

// // private:
// //     json cfg_;
// //     Context& ctx_;
// //     std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator_;
// //     std::shared_ptr<hrl::RuntimeControls> runtimeControls_;
// //     hrl::Scheduler scheduler_;
// //     std::unique_ptr<hrl::GeneticAlgorithm> ga_;
// //     std::atomic<bool> running_{false};
// //     std::thread controlThread_;
// //     int loopCount_ = 0;  // Control loop iteration counter (for periodic regime logging)
//     int controlIntervalMs_ = 500;
//     int warmupMs_ = 5000;
//     double minFpsUseful_ = 1.0;
//     double minPowerUseful_ = 0.0;
//     bool requirePowerForEvolution_ = false;

// //     void controlLoop() {
// //         spdlog::debug("[EvolutionarySelector] Control loop started");

// //         // Pipeline readiness wait
// //         spdlog::info("[EvolutionarySelector] Waiting for pipeline to become READY...");
// //         while (!ctx_.pipelineReady.load(std::memory_order_acquire)) {
// //             if (ctx_.shutdown_flag.load()) {
// //                 spdlog::info("[EvolutionarySelector] Shutdown requested during wait");
// //                 std::this_thread::sleep_for(50ms);
// //                 return;
// //             }
// //             std::this_thread::sleep_for(50ms);
// //         }

// //         // Extended warmup
// //         spdlog::info("[EvolutionarySelector] Pipeline READY. Warming up for 5 seconds...");
// //         std::this_thread::sleep_for(5s);    // Warmup

// //         spdlog::info("[EvolutionarySelector] Engaging control loop.");

// //         // while (running_ && !ctx_.shutdown_flag.load()) {
// //         while (running_.load() && !ctx_.shutdown_flag.load()) {
// //             auto loopStart = std::chrono::steady_clock::now();

// //             try {
// //                 auto snapshot = getLatestSnapshot();

// //                 if (!snapshot.valid || !hasUsefulData(snapshot)) {
// //                     spdlog::warn("[EvolutionarySelector] Snapshot not ready, skipping iteration");
// //                     std::this_thread::sleep_for(80ms);
// //                     continue;
// //                 }

// //                 // === Pareto + Scalar GA Evolution (Single Source of Truth) ===
// //                 // === Pareto + Scalar GA Evolution (Single Source of Truth) ===
// //                 ga_->evolve(snapshot);

// //                 // Log current weight regime every 10 iterations for thesis data
// //                 if (++loopCount_ % 10 == 0) {
// //                     spdlog::info("[EvolutionarySelector] Regime={} | fps={:.1f} | power={:.2f}W | "
// //                                  "cpu_temp={:.1f}°""C | gpu_temp={:.1f}°""C | latency={:.1f}ms",
// //                                  ga_->getCurrentRegimeName(),
// //                                  snapshot.fps, snapshot.avg_power_w_alg,
// //                                  snapshot.cpu_temp_c, snapshot.gpu_temp_c,
// //                                  snapshot.avg_latency_ms);
// //                 }

// //             } catch (const std::exception& e) {
// //                 spdlog::error("[EvolutionarySelector] Error in control loop: {}", e.what());
// //             } catch (...) {
// //                 spdlog::error("[EvolutionarySelector] Unknown exception in control loop");
// //             }

// //             // Precise timing
// //             auto elapsed = std::chrono::steady_clock::now() - loopStart;
// //             auto sleepTime = std::chrono::milliseconds(controlIntervalMs_) - elapsed;
// //             if (sleepTime > 0ms) {
// //                 std::this_thread::sleep_for(sleepTime);
// //             }
// //         }

// //         spdlog::info("[EvolutionarySelector] Control loop ended cleanly");
// //     }

// //     hrl::MetricsSnapshot getLatestSnapshot() {
// //         hrl::MetricsSnapshot snap{};
// //         snap.valid = false;

// //         if (!aggregator_) {
// //             spdlog::warn("[EvolutionarySelector] Aggregator is NULL");
// //             return snap;
// //         }

// //         try {
// //             auto raw = aggregator_->getLatestSnapshot();
// //             if (!raw.isValid()) {
// //                 spdlog::debug("[EvolutionarySelector] Raw snapshot not valid yet");
// //                 return snap;
// //             }

// //             // Safe conversion with joulesPerFrame priority
// //             snap.fps = raw.algorithmStats.fps;

// //             snap.avg_power_w_alg = (raw.joulesPerFrame > 0 && raw.algorithmStats.totalProcTimeMs > 0)
// //                 ? raw.joulesPerFrame / (raw.algorithmStats.totalProcTimeMs / 1000.0)
// //                 : raw.powerStats.averagePower();

// //             snap.cpu_util_avg = (raw.socInfo.CPU1_Utilization_Percent +
// //                                  raw.socInfo.CPU2_Utilization_Percent +
// //                                  raw.socInfo.CPU3_Utilization_Percent +
// //                                  raw.socInfo.CPU4_Utilization_Percent) / 4.0;

// //             snap.gpu_util_avg   = raw.socInfo.GR3D_Frequency_Percent;
// //             snap.cpu_temp_c     = raw.socInfo.CPU_Temperature_C;
// //             snap.gpu_temp_c     = raw.socInfo.GPU_Temperature_C;
// //             snap.avg_latency_ms = raw.endToEndLatencyMs;
// //             snap.joulesPerFrame = raw.joulesPerFrame;   // Important for ERL reward

// //             snap.valid = true;
// //             spdlog::debug("[EvolutionarySelector] Valid snapshot (fps={:.1f})", snap.fps);
// //             return snap;

// //         } catch (const std::exception& e) {
// //             spdlog::warn("[EvolutionarySelector] Exception in getLatestSnapshot: {}", e.what());
// //         } catch (...) {
// //             spdlog::warn("[EvolutionarySelector] Unknown exception in getLatestSnapshot");
// //         }

// //         return snap;  // invalid
// //     }

// //     static bool hasUsefulData(const hrl::MetricsSnapshot& s) {
// //         return s.fps > 5.0 && s.avg_power_w_alg > 0.5;
// //     }
// // };




// //===============================================================================================
// // // EvolutionarySelector_02.h - FINAL PhD-Ready (Best of Both Worlds)
// // // Pareto + Scalar Hybrid | Strong Defensive Checks | Clean Orchestrator

// // // EvolutionarySelector_02.h - FINAL PhD-Ready (Best of Both Worlds)
// // // Pareto + Scalar Hybrid | Strong Defensive Checks | Clean Orchestrator
// // #pragma once

// // #include <atomic>
#include <algorithm>
// // #include <chrono>
// // #include <thread>
// // #include <memory>
// // #include <spdlog/spdlog.h>

// // #include "IModule.h"
// // #include "RuntimeControls.h"
// // #include "IScheduler.h"
// // #include "Scheduler.h"
// // #include "RuntimeControls.h"   // ? ADD THIS
// // #include "../Stage_01/Concretes/SystemMetricsAggregatorConcrete_v3_2.h"
// // #include "GeneticAlgorithm_05.h"   // ? Pareto core (single source of truth)
// // #include "../Stage_01/Others/utils.h"

// // using namespace std::chrono_literals;

// // class EvolutionarySelector : public IModule {
// // public:
// //     EvolutionarySelector(const json& cfg,
// //                          Context& ctx,
// //                          std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator,
// //                          std::shared_ptr<hrl::RuntimeControls> runtimeControls)
// //         : cfg_(cfg),
// //           ctx_(ctx),
// //           aggregator_(std::move(aggregator)),
// //           runtimeControls_(std::move(runtimeControls)),
// //           scheduler_(hrl::Scheduler::Limits{}),
// //           running_(false) {

// //         // ====== GA Configuration (with Local clamping) ======
// //         hrl::GAConfig gaCfg;
// //         gaCfg.popSize             = utils::local_clamp<size_t>(cfg_.value("ga_population_size", 12), 4u, 16u);
// //         gaCfg.minPopSize          = utils::local_clamp<size_t>(cfg_.value("ga_min_pop_size", 6), 2u, gaCfg.popSize);
// //         gaCfg.eliteCount          = cfg_.value("ga_elite_count", 3);
// //         gaCfg.crossoverRate       = utils::local_clamp<double>(cfg_.value("ga_crossover_rate", 0.75), 0.5, 0.95);
// //         gaCfg.mutationRate        = utils::local_clamp<double>(cfg_.value("ga_mutation_rate", 0.18), 0.01, 0.5);
// //         gaCfg.explorationDecay    = cfg_.value("ga_exploration_decay", 0.96);
// //         gaCfg.controlIntervalMs   = cfg_.value("control_interval_ms", 800);
// //         gaCfg.reductionStartGen   = cfg_.value("ga_reduction_start_gen", 10);
// //         gaCfg.reductionRatio      = utils::local_clamp<double>(cfg_.value("ga_reduction_ratio", 0.6), 0.3, 0.9);

// //         gaCfg.adaptive_weights_enabled = cfg_.value("adaptive_weights_enabled", false);

// //         // PhD evidence export: ERL/Pareto traceability.
// //         gaCfg.export_pareto_evidence = cfg_.value("export_pareto_evidence", true);
// //         gaCfg.pareto_csv_path        = cfg_.value("pareto_csv", std::string("output/erl_pareto_front.csv"));
// //         gaCfg.action_csv_path        = cfg_.value("action_csv", std::string("output/erl_action_trace.csv"));


// //         if (gaCfg.adaptive_weights_enabled) {
// //             gaCfg.awm_config.update_interval_gens   = cfg_.value("awm_update_interval_gens", 5);
// //             gaCfg.awm_config.battery_critical_watts = cfg_.value("awm_battery_critical_watts", 8.0);
// //             gaCfg.awm_config.thermal_warn_cpu_c     = cfg_.value("awm_thermal_warn_cpu_c", 75.0);
// //             gaCfg.awm_config.thermal_warn_gpu_c     = cfg_.value("awm_thermal_warn_gpu_c", 78.0);
// //             gaCfg.awm_config.thermal_crit_cpu_c     = cfg_.value("awm_thermal_crit_cpu_c", 80.0);
// //             gaCfg.awm_config.thermal_crit_gpu_c     = cfg_.value("awm_thermal_crit_gpu_c", 83.0);
// //             gaCfg.awm_config.latency_sla_ms         = cfg_.value("awm_latency_sla_ms", 45.0);
// //             gaCfg.awm_config.fps_underperform_ratio = cfg_.value("awm_fps_underperform_ratio", 0.70);
// //             gaCfg.awm_config.transition_smoothing   = cfg_.value("awm_transition_smoothing", 0.3);

// //             spdlog::info("[EvolutionarySelector] AdaptiveWeightManager configured "
// //                          "(update every {} gens, thermal_crit={:.0f}°""C/{:.0f}°""C)",
// //                          gaCfg.awm_config.update_interval_gens,
// //                          gaCfg.awm_config.thermal_crit_cpu_c,
// //                          gaCfg.awm_config.thermal_crit_gpu_c);
// //         }

// //         try {
// //             ga_ = std::make_unique<hrl::GeneticAlgorithm>(
// //                 gaCfg, aggregator_, runtimeControls_, &scheduler_);

// //             spdlog::info("[EvolutionarySelector] - Pareto-GA Hybrid Ready (pop={}-{}, reduction at gen {})",
// //                          gaCfg.popSize, gaCfg.minPopSize, gaCfg.reductionStartGen);
// //         } catch (const std::exception& e) {
// //             spdlog::error("[EvolutionarySelector] Failed to initialize GeneticAlgorithm: {}", e.what());
// //             throw;
// //         }
// //     }

// //     bool validate() override {
// //         spdlog::info("[EvolutionarySelector] Validating configuration...");
// //         if (!aggregator_) {
// //             spdlog::error("[EvolutionarySelector] Aggregator is null");
// //             return false;
// //         }
// //         if (!runtimeControls_) {
// //             spdlog::error("[EvolutionarySelector] RuntimeControls is null");
// //             return false;
// //         }
// //         spdlog::info("[EvolutionarySelector] Validation successful");
// //         return true;
// //         // return aggregator_ && runtimeControls_; // Alfer testing all code just used thise line
// //     }

// //     void start() override {
// //         spdlog::info("[EvolutionarySelector] Starting evolutionary control loop...");
// //         //running_.store(true);
// //         running_.store(true, std::memory_order_release);
// //         controlThread_ = std::thread(&EvolutionarySelector::controlLoop, this);
// //     }

// //     void stop() override {
// //         spdlog::info("[EvolutionarySelector] Stopping...");
// //         //running_.store(false);
// //         running_.store(false, std::memory_order_release);
// //         if (controlThread_.joinable()) controlThread_.join();
// //         spdlog::info("[EvolutionarySelector] Stopped");
// //     }

// //     // Optional helper for external modules (e.g. logging or RL)
// //     hrl::RLAction getBestAction() const {
// //         return ga_ ? ga_->getBestAction() : hrl::RLAction{};
// //     }

// // private:
// //     json cfg_;
// //     Context& ctx_;
// //     std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator_;
// //     std::shared_ptr<hrl::RuntimeControls> runtimeControls_;
// //     hrl::Scheduler scheduler_;
// //     std::unique_ptr<hrl::GeneticAlgorithm> ga_;
// //     std::atomic<bool> running_{false};
// //     std::thread controlThread_;
// //     int loopCount_ = 0;  // Control loop iteration counter (for periodic regime logging)

// //     void controlLoop() {
// //         spdlog::debug("[EvolutionarySelector] Control loop started");

// //         // Pipeline readiness wait
// //         spdlog::info("[EvolutionarySelector] Waiting for pipeline to become READY...");
// //         while (!ctx_.pipelineReady.load(std::memory_order_acquire)) {
// //             if (ctx_.shutdown_flag.load()) {
// //                 spdlog::info("[EvolutionarySelector] Shutdown requested during wait");
// //                 std::this_thread::sleep_for(50ms);
// //                 return;
// //             }
// //             std::this_thread::sleep_for(50ms);
// //         }

// //         // Extended warmup
// //         spdlog::info("[EvolutionarySelector] Pipeline READY. Warming up for 5 seconds...");
// //         std::this_thread::sleep_for(5s);    // Warmup

// //         spdlog::info("[EvolutionarySelector] Engaging control loop.");

// //         // while (running_ && !ctx_.shutdown_flag.load()) {
// //         while (running_.load() && !ctx_.shutdown_flag.load()) {
// //             auto loopStart = std::chrono::steady_clock::now();

// //             try {
// //                 auto snapshot = getLatestSnapshot();

// //                 if (!snapshot.valid || !hasUsefulData(snapshot)) {
// //                     spdlog::warn("[EvolutionarySelector] Snapshot not ready, skipping iteration");
// //                     std::this_thread::sleep_for(80ms);
// //                     continue;
// //                 }

// //                 // === Pareto + Scalar GA Evolution (Single Source of Truth) ===
// //                 // === Pareto + Scalar GA Evolution (Single Source of Truth) ===
// //                 ga_->evolve(snapshot);

// //                 // Log current weight regime every 10 iterations for thesis data
// //                 if (++loopCount_ % 10 == 0) {
// //                     spdlog::info("[EvolutionarySelector] Regime={} | fps={:.1f} | power={:.2f}W | "
// //                                  "cpu_temp={:.1f}°""C | gpu_temp={:.1f}°""C | latency={:.1f}ms",
// //                                  ga_->getCurrentRegimeName(),
// //                                  snapshot.fps, snapshot.avg_power_w_alg,
// //                                  snapshot.cpu_temp_c, snapshot.gpu_temp_c,
// //                                  snapshot.avg_latency_ms);
// //                 }

// //             } catch (const std::exception& e) {
// //                 spdlog::error("[EvolutionarySelector] Error in control loop: {}", e.what());
// //             } catch (...) {
// //                 spdlog::error("[EvolutionarySelector] Unknown exception in control loop");
// //             }

// //             // Precise timing
// //             auto elapsed = std::chrono::steady_clock::now() - loopStart;
// //             auto sleepTime = std::chrono::milliseconds(800) - elapsed;
// //             if (sleepTime > 0ms) {
// //                 std::this_thread::sleep_for(sleepTime);
// //             }
// //         }

// //         spdlog::info("[EvolutionarySelector] Control loop ended cleanly");
// //     }

// //     hrl::MetricsSnapshot getLatestSnapshot() {
// //         hrl::MetricsSnapshot snap{};
// //         snap.valid = false;

// //         if (!aggregator_) {
// //             spdlog::warn("[EvolutionarySelector] Aggregator is NULL");
// //             return snap;
// //         }

// //         try {
// //             auto raw = aggregator_->getLatestSnapshot();
// //             if (!raw.isValid()) {
// //                 spdlog::debug("[EvolutionarySelector] Raw snapshot not valid yet");
// //                 return snap;
// //             }

// //             // Safe conversion with joulesPerFrame priority
// //             snap.fps = raw.algorithmStats.fps;

// //             snap.avg_power_w_alg = (raw.joulesPerFrame > 0 && raw.algorithmStats.totalProcTimeMs > 0)
// //                 ? raw.joulesPerFrame / (raw.algorithmStats.totalProcTimeMs / 1000.0)
// //                 : raw.powerStats.averagePower();

// //             snap.cpu_util_avg = (raw.socInfo.CPU1_Utilization_Percent +
// //                                  raw.socInfo.CPU2_Utilization_Percent +
// //                                  raw.socInfo.CPU3_Utilization_Percent +
// //                                  raw.socInfo.CPU4_Utilization_Percent) / 4.0;

// //             snap.gpu_util_avg   = raw.socInfo.GR3D_Frequency_Percent;
// //             snap.cpu_temp_c     = raw.socInfo.CPU_Temperature_C;
// //             snap.gpu_temp_c     = raw.socInfo.GPU_Temperature_C;
// //             snap.avg_latency_ms = raw.endToEndLatencyMs;
// //             snap.joulesPerFrame = raw.joulesPerFrame;   // Important for ERL reward

// //             snap.valid = true;
// //             spdlog::debug("[EvolutionarySelector] Valid snapshot (fps={:.1f})", snap.fps);
// //             return snap;

// //         } catch (const std::exception& e) {
// //             spdlog::warn("[EvolutionarySelector] Exception in getLatestSnapshot: {}", e.what());
// //         } catch (...) {
// //             spdlog::warn("[EvolutionarySelector] Unknown exception in getLatestSnapshot");
// //         }

// //         return snap;  // invalid
// //     }

// //     static bool hasUsefulData(const hrl::MetricsSnapshot& s) {
// //         return s.fps > 5.0 && s.avg_power_w_alg > 0.5;
// //     }
// // };


// // //==============================================================================
// // // // EvolutionarySelector_02.h - FINAL PhD-Ready (Best of Both Worlds)
// // // // Pareto + Scalar Hybrid | Strong Defensive Checks | Clean Orchestrator

// // // // EvolutionarySelector_02.h - FINAL PhD-Ready (Best of Both Worlds)
// // // // Pareto + Scalar Hybrid | Strong Defensive Checks | Clean Orchestrator
// // // #pragma once

// // // #include <atomic>
#include <algorithm>
// // // #include <chrono>
// // // #include <thread>
// // // #include <memory>
// // // #include <spdlog/spdlog.h>

// // // #include "IModule.h"
// // // #include "RuntimeControls.h"
// // // #include "IScheduler.h"
// // // #include "Scheduler.h"
// // // #include "RuntimeControls.h"   // ? ADD THIS
// // // #include "../Stage_01/Concretes/SystemMetricsAggregatorConcrete_v3_2.h"
// // // #include "GeneticAlgorithm_05.h"   // ? Pareto core (single source of truth)
// // // #include "../Stage_01/Others/utils.h"

// // // using namespace std::chrono_literals;

// // // class EvolutionarySelector : public IModule {
// // // public:
// // //     EvolutionarySelector(const json& cfg,
// // //                          Context& ctx,
// // //                          std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator,
// // //                          std::shared_ptr<hrl::RuntimeControls> runtimeControls)
// // //         : cfg_(cfg),
// // //           ctx_(ctx),
// // //           aggregator_(std::move(aggregator)),
// // //           runtimeControls_(std::move(runtimeControls)),
// // //           scheduler_(hrl::Scheduler::Limits{}),
// // //           running_(false) {

// // //         // ====== GA Configuration (with Local clamping) ======
// // //         hrl::GAConfig gaCfg;
// // //         gaCfg.popSize             = utils::local_clamp<size_t>(cfg_.value("ga_population_size", 12), 4u, 16u);
// // //         gaCfg.minPopSize          = utils::local_clamp<size_t>(cfg_.value("ga_min_pop_size", 6), 2u, gaCfg.popSize);
// // //         gaCfg.eliteCount          = cfg_.value("ga_elite_count", 3);
// // //         gaCfg.crossoverRate       = utils::local_clamp<double>(cfg_.value("ga_crossover_rate", 0.75), 0.5, 0.95);
// // //         gaCfg.mutationRate        = utils::local_clamp<double>(cfg_.value("ga_mutation_rate", 0.18), 0.01, 0.5);
// // //         gaCfg.explorationDecay    = cfg_.value("ga_exploration_decay", 0.96);
// // //         gaCfg.controlIntervalMs   = cfg_.value("control_interval_ms", 800);
// // //         gaCfg.reductionStartGen   = cfg_.value("ga_reduction_start_gen", 10);
// // //         gaCfg.reductionRatio      = utils::local_clamp<double>(cfg_.value("ga_reduction_ratio", 0.6), 0.3, 0.9);

// // //         gaCfg.adaptive_weights_enabled = cfg_.value("adaptive_weights_enabled", false);


// // //         // PhD evidence export: ERL/Pareto traceability.
// // //         gaCfg.export_pareto_evidence = cfg_.value("export_pareto_evidence", false);
// // //         gaCfg.pareto_csv_path        = cfg_.value("pareto_csv", std::string("output/erl_pareto_front.csv"));
// // //         gaCfg.action_csv_path        = cfg_.value("action_csv", std::string("output/erl_action_trace.csv"));
        

// // //         if (gaCfg.adaptive_weights_enabled) {
// // //             gaCfg.awm_config.update_interval_gens   = cfg_.value("awm_update_interval_gens", 5);
// // //             gaCfg.awm_config.battery_critical_watts = cfg_.value("awm_battery_critical_watts", 8.0);
// // //             gaCfg.awm_config.thermal_warn_cpu_c     = cfg_.value("awm_thermal_warn_cpu_c", 75.0);
// // //             gaCfg.awm_config.thermal_warn_gpu_c     = cfg_.value("awm_thermal_warn_gpu_c", 78.0);
// // //             gaCfg.awm_config.thermal_crit_cpu_c     = cfg_.value("awm_thermal_crit_cpu_c", 80.0);
// // //             gaCfg.awm_config.thermal_crit_gpu_c     = cfg_.value("awm_thermal_crit_gpu_c", 83.0);
// // //             gaCfg.awm_config.latency_sla_ms         = cfg_.value("awm_latency_sla_ms", 45.0);
// // //             gaCfg.awm_config.fps_underperform_ratio = cfg_.value("awm_fps_underperform_ratio", 0.70);
// // //             gaCfg.awm_config.transition_smoothing   = cfg_.value("awm_transition_smoothing", 0.3);

// // //             spdlog::info("[EvolutionarySelector] AdaptiveWeightManager configured "
// // //                          "(update every {} gens, thermal_crit={:.0f}°""C/{:.0f}°""C)",
// // //                          gaCfg.awm_config.update_interval_gens,
// // //                          gaCfg.awm_config.thermal_crit_cpu_c,
// // //                          gaCfg.awm_config.thermal_crit_gpu_c);
// // //         }

// // //         try {
// // //             ga_ = std::make_unique<hrl::GeneticAlgorithm>(
// // //                 gaCfg, aggregator_, runtimeControls_, &scheduler_);

// // //             spdlog::info("[EvolutionarySelector] - Pareto-GA Hybrid Ready (pop={}-{}, reduction at gen {})",
// // //                          gaCfg.popSize, gaCfg.minPopSize, gaCfg.reductionStartGen);
// // //         } catch (const std::exception& e) {
// // //             spdlog::error("[EvolutionarySelector] Failed to initialize GeneticAlgorithm: {}", e.what());
// // //             throw;
// // //         }
// // //     }

// // //     bool validate() override {
// // //         spdlog::info("[EvolutionarySelector] Validating configuration...");
// // //         if (!aggregator_) {
// // //             spdlog::error("[EvolutionarySelector] Aggregator is null");
// // //             return false;
// // //         }
// // //         if (!runtimeControls_) {
// // //             spdlog::error("[EvolutionarySelector] RuntimeControls is null");
// // //             return false;
// // //         }
// // //         spdlog::info("[EvolutionarySelector] Validation successful");
// // //         return true;
// // //         // return aggregator_ && runtimeControls_; // Alfer testing all code just used thise line
// // //     }

// // //     void start() override {
// // //         spdlog::info("[EvolutionarySelector] Starting evolutionary control loop...");
// // //         //running_.store(true);
// // //         running_.store(true, std::memory_order_release);
// // //         controlThread_ = std::thread(&EvolutionarySelector::controlLoop, this);
// // //     }

// // //     void stop() override {
// // //         spdlog::info("[EvolutionarySelector] Stopping...");
// // //         //running_.store(false);
// // //         running_.store(false, std::memory_order_release);
// // //         if (controlThread_.joinable()) controlThread_.join();
// // //         spdlog::info("[EvolutionarySelector] Stopped");
// // //     }

// // //     // Optional helper for external modules (e.g. logging or RL)
// // //     hrl::RLAction getBestAction() const {
// // //         return ga_ ? ga_->getBestAction() : hrl::RLAction{};
// // //     }

// // // private:
// // //     json cfg_;
// // //     Context& ctx_;
// // //     std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator_;
// // //     std::shared_ptr<hrl::RuntimeControls> runtimeControls_;
// // //     hrl::Scheduler scheduler_;
// // //     std::unique_ptr<hrl::GeneticAlgorithm> ga_;
// // //     std::atomic<bool> running_{false};
// // //     std::thread controlThread_;
// // //     int loopCount_ = 0;  // Control loop iteration counter (for periodic regime logging)

// // //     void controlLoop() {
// // //         spdlog::debug("[EvolutionarySelector] Control loop started");

// // //         // Pipeline readiness wait
// // //         spdlog::info("[EvolutionarySelector] Waiting for pipeline to become READY...");
// // //         while (!ctx_.pipelineReady.load(std::memory_order_acquire)) {
// // //             if (ctx_.shutdown_flag.load()) {
// // //                 spdlog::info("[EvolutionarySelector] Shutdown requested during wait");
// // //                 std::this_thread::sleep_for(50ms);
// // //                 return;
// // //             }
// // //             std::this_thread::sleep_for(50ms);
// // //         }

// // //         // Extended warmup
// // //         spdlog::info("[EvolutionarySelector] Pipeline READY. Warming up for 5 seconds...");
// // //         std::this_thread::sleep_for(5s);    // Warmup

// // //         spdlog::info("[EvolutionarySelector] Engaging control loop.");

// // //         // while (running_ && !ctx_.shutdown_flag.load()) {
// // //         while (running_.load() && !ctx_.shutdown_flag.load()) {
// // //             auto loopStart = std::chrono::steady_clock::now();

// // //             try {
// // //                 auto snapshot = getLatestSnapshot();

// // //                 if (!snapshot.valid || !hasUsefulData(snapshot)) {
// // //                     spdlog::warn("[EvolutionarySelector] Snapshot not ready, skipping iteration");
// // //                     std::this_thread::sleep_for(80ms);
// // //                     continue;
// // //                 }

// // //                 // === Pareto + Scalar GA Evolution (Single Source of Truth) ===
// // //                 // === Pareto + Scalar GA Evolution (Single Source of Truth) ===
// // //                 ga_->evolve(snapshot);

// // //                 // Log current weight regime every 10 iterations for thesis data
// // //                 if (++loopCount_ % 10 == 0) {
// // //                     spdlog::info("[EvolutionarySelector] Regime={} | fps={:.1f} | power={:.2f}W | "
// // //                                  "cpu_temp={:.1f}°""C | gpu_temp={:.1f}°""C | latency={:.1f}ms",
// // //                                  ga_->getCurrentRegimeName(),
// // //                                  snapshot.fps, snapshot.avg_power_w_alg,
// // //                                  snapshot.cpu_temp_c, snapshot.gpu_temp_c,
// // //                                  snapshot.avg_latency_ms);
// // //                 }

// // //             } catch (const std::exception& e) {
// // //                 spdlog::error("[EvolutionarySelector] Error in control loop: {}", e.what());
// // //             } catch (...) {
// // //                 spdlog::error("[EvolutionarySelector] Unknown exception in control loop");
// // //             }

// // //             // Precise timing
// // //             auto elapsed = std::chrono::steady_clock::now() - loopStart;
// // //             auto sleepTime = std::chrono::milliseconds(800) - elapsed;
// // //             if (sleepTime > 0ms) {
// // //                 std::this_thread::sleep_for(sleepTime);
// // //             }
// // //         }

// // //         spdlog::info("[EvolutionarySelector] Control loop ended cleanly");
// // //     }

// // //     hrl::MetricsSnapshot getLatestSnapshot() {
// // //         hrl::MetricsSnapshot snap{};
// // //         snap.valid = false;

// // //         if (!aggregator_) {
// // //             spdlog::warn("[EvolutionarySelector] Aggregator is NULL");
// // //             return snap;
// // //         }

// // //         try {
// // //             auto raw = aggregator_->getLatestSnapshot();
// // //             if (!raw.isValid()) {
// // //                 spdlog::debug("[EvolutionarySelector] Raw snapshot not valid yet");
// // //                 return snap;
// // //             }

// // //             // Safe conversion with joulesPerFrame priority
// // //             snap.fps = raw.algorithmStats.fps;

// // //             snap.avg_power_w_alg = (raw.joulesPerFrame > 0 && raw.algorithmStats.totalProcTimeMs > 0)
// // //                 ? raw.joulesPerFrame / (raw.algorithmStats.totalProcTimeMs / 1000.0)
// // //                 : raw.powerStats.averagePower();

// // //             snap.cpu_util_avg = (raw.socInfo.CPU1_Utilization_Percent +
// // //                                  raw.socInfo.CPU2_Utilization_Percent +
// // //                                  raw.socInfo.CPU3_Utilization_Percent +
// // //                                  raw.socInfo.CPU4_Utilization_Percent) / 4.0;

// // //             snap.gpu_util_avg   = raw.socInfo.GR3D_Frequency_Percent;
// // //             snap.cpu_temp_c     = raw.socInfo.CPU_Temperature_C;
// // //             snap.gpu_temp_c     = raw.socInfo.GPU_Temperature_C;
// // //             snap.avg_latency_ms = raw.endToEndLatencyMs;
// // //             snap.joulesPerFrame = raw.joulesPerFrame;   // Important for ERL reward

// // //             snap.valid = true;
// // //             spdlog::debug("[EvolutionarySelector] Valid snapshot (fps={:.1f})", snap.fps);
// // //             return snap;

// // //         } catch (const std::exception& e) {
// // //             spdlog::warn("[EvolutionarySelector] Exception in getLatestSnapshot: {}", e.what());
// // //         } catch (...) {
// // //             spdlog::warn("[EvolutionarySelector] Unknown exception in getLatestSnapshot");
// // //         }

// // //         return snap;  // invalid
// // //     }

// // //     static bool hasUsefulData(const hrl::MetricsSnapshot& s) {
// // //         return s.fps > 5.0 && s.avg_power_w_alg > 0.5;
// // //     }
// // // };




//====================================================================================
// // EvolutionarySelector_02.h - FINAL PhD-Ready (Best of Both Worlds)
// // Pareto + Scalar Hybrid | Strong Defensive Checks | Clean Orchestrator

// // EvolutionarySelector_02.h - FINAL PhD-Ready (Best of Both Worlds)
// // Pareto + Scalar Hybrid | Strong Defensive Checks | Clean Orchestrator
// #pragma once

// #include <atomic>
#include <algorithm>
// #include <chrono>
// #include <thread>
// #include <memory>
// #include <deque>
// #include <fstream>
// #include <mutex>
// #include <ctime>
// #include <time.h>
// #include <iomanip>
// #include <cmath>
// #include <algorithm>

// #include <spdlog/spdlog.h>

// #include "IModule.h"
// #include "PowerSanity.h"   // [P0-F15] physical-plausibility gate for power samples
// #include "RuntimeControls.h"
// #include "IScheduler.h"
// #include "Scheduler.h"
// #include "RuntimeControls.h"   // ? ADD THIS
// #include "../Stage_01/Concretes/SystemMetricsAggregatorConcrete_v3_2.h"
// #include "GeneticAlgorithm_05.h"   // ? Pareto core (single source of truth)
// #include "../Stage_01/Others/utils.h"

// using namespace std::chrono_literals;

// class EvolutionarySelector : public IModule {

// public:
//     // EvolutionarySelector(const json& cfg,
//     //                      Context& ctx,
//     //                      std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator,
//     //                      std::shared_ptr<hrl::RuntimeControls> runtimeControls)
//     EvolutionarySelector(const json& cfg, Context& ctx,
//                     std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator,
//                     std::shared_ptr<hrl::RuntimeControls> runtimeControls,
//                     const hrl::ThermalGovernor::Config& thermalCfg,
//                     const hrl::AdaptiveWeightManager::Config& awmCfg)
//         : cfg_(cfg),
//           ctx_(ctx),
//           aggregator_(std::move(aggregator)),
//           runtimeControls_(std::move(runtimeControls)),
//           scheduler_(hrl::Scheduler::Limits{}, thermalCfg),
//           running_(false) {

//              // ====== GA Configuration (supports both old and new JSON key names) ======
//             hrl::GAConfig gaCfg;

            

//             // // 1. Construct the AdaptiveWeightManager using the embedded config
//             // bool adaptive_weights_enabled = jBool("adaptive_weights_enabled", nullptr, false);

//             // if (adaptive_weights_enabled) {
//             //     // Pass the parsed struct (gaCfg.awm_config), NOT the raw json object
//             //     awm_ = std::make_unique<AdaptiveWeightManager>(gaCfg.awm_config);
//             //     spdlog::info("[GeneticAlgorithm] AdaptiveWeightManager initialized.");
//             // } else {
//             //     spdlog::info("[GeneticAlgorithm] Adaptive weights disabled by config.");
//             // }
//             // if (cfg_.adaptive_weights_enabled) {
//             //     awm_ = std::make_unique<AdaptiveWeightManager>(cfg_.awm_config);
//             //     spdlog::info("[GeneticAlgorithm] AdaptiveWeightManager initialized.");
//             // } else {
//             //     spdlog::info("[GeneticAlgorithm] Adaptive weights disabled by config.");
//             // }

//         auto jDouble = [this](const char* primary, const char* alias, double def) -> double {
//             try {
//                 if (cfg_.contains(primary) && !cfg_[primary].is_null()) return cfg_[primary].get<double>();
//                 if (alias && cfg_.contains(alias) && !cfg_[alias].is_null()) return cfg_[alias].get<double>();
//             } catch (...) {}
//             return def;
//         };

//         //auto jInt = [this](const char* primary, const char* alias, int def) -> int {
//         auto jInt = [this, &jDouble](const char* primary, const char* alias, int def) -> int {
//             return static_cast<int>(jDouble(primary, alias, static_cast<double>(def)));
//         };

//         auto jBool = [this](const char* primary, const char* alias, bool def) -> bool {
//             try {
//                 const json* v = nullptr;
//                 if (cfg_.contains(primary) && !cfg_[primary].is_null()) v = &cfg_[primary];
//                 else if (alias && cfg_.contains(alias) && !cfg_[alias].is_null()) v = &cfg_[alias];
//                 if (!v) return def;
//                 if (v->is_boolean()) return v->get<bool>();
//                 if (v->is_number_integer()) return v->get<int>() != 0;
//                 if (v->is_string()) {
//                     const std::string s = v->get<std::string>();
//                     return s == "true" || s == "TRUE" || s == "1" || s == "yes" || s == "ON" || s == "on";
//                 }
//             } catch (...) {}
//             return def;
//         };

//         auto jString = [this](const char* primary, const char* alias, const std::string& def) -> std::string {
//             try {
//                 if (cfg_.contains(primary) && !cfg_[primary].is_null()) return cfg_[primary].get<std::string>();
//                 if (alias && cfg_.contains(alias) && !cfg_[alias].is_null()) return cfg_[alias].get<std::string>();
//             } catch (...) {}
//             return def;
//         };

//         // AdaptiveWeightManager is owned only by GeneticAlgorithm::weightManager_.
//         // EvolutionarySelector does not instantiate a shadow AWM object.

//         // // ====== GA Configuration (supports both old and new JSON key names) ======
//         // hrl::GAConfig gaCfg;
//         gaCfg.popSize             = utils::local_clamp<size_t>(static_cast<size_t>(jInt("ga_population_size", "population_size", 12)), 4u, 64u);
//         gaCfg.minPopSize          = utils::local_clamp<size_t>(static_cast<size_t>(jInt("ga_min_pop_size", nullptr, 4)), 2u, gaCfg.popSize);
//         gaCfg.eliteCount          = static_cast<size_t>(utils::local_clamp<int>(jInt("ga_elite_count", "elite_count", 2), 1, static_cast<int>(gaCfg.popSize)));
//         gaCfg.crossoverRate       = utils::local_clamp<double>(jDouble("ga_crossover_rate", "crossover_rate", 0.70), 0.0, 1.0);
//         gaCfg.mutationRate        = utils::local_clamp<double>(jDouble("ga_mutation_rate", "mutation_rate", 0.15), 0.0, 1.0);
//         gaCfg.explorationDecay    = utils::local_clamp<double>(jDouble("ga_exploration_decay", nullptr, 0.96), 0.80, 1.0);
//         gaCfg.controlIntervalMs   = utils::local_clamp<int>(jInt("control_interval_ms", nullptr, 500), 50, 10000);
//         gaCfg.reductionStartGen   = static_cast<size_t>(utils::local_clamp<int>(jInt("ga_reduction_start_gen", nullptr, 10), 1, 100000));
//         gaCfg.reductionRatio      = utils::local_clamp<double>(jDouble("ga_reduction_ratio", nullptr, 0.60), 0.10, 1.0);

//         // [Adaptive mutation] plateau-driven mutation control
//         gaCfg.adaptiveMutationEnabled       = jBool("ga_adaptive_mutation_enabled", nullptr, true);
//         gaCfg.maxMutationRate               = utils::local_clamp<double>(jDouble("ga_max_mutation_rate", nullptr, 0.35), gaCfg.mutationRate, 1.0);
//         gaCfg.mutationPlateauRelThreshold   = utils::local_clamp<double>(jDouble("ga_mutation_plateau_rel_threshold", nullptr, 0.02), 0.0, 1.0);
//         gaCfg.mutationPlateauAbsFloor       = jDouble("ga_mutation_plateau_abs_floor", nullptr, 0.0005);
//         gaCfg.mutationImproveFactor         = utils::local_clamp<double>(jDouble("ga_mutation_improve_factor", nullptr, 0.80), 0.0, 1.0);
//         gaCfg.mutationPlateauStep           = utils::local_clamp<double>(jDouble("ga_mutation_plateau_step", nullptr, 0.10), 0.0, 1.0);

//         // [Surrogate] online counterfactual fitness (thesis gap 2)
//         gaCfg.surrogateEnabled       = jBool("ga_surrogate_enabled", nullptr, true);
//         gaCfg.surrogateExploreWeight = utils::local_clamp<double>(jDouble("ga_surrogate_explore_weight", nullptr, 0.15), 0.0, 5.0);

//         // ADD THESE FOUR LINES:
//         gaCfg.weight_fps      = utils::local_clamp<double>(jDouble("obj_weight_fps", nullptr, 1.0), 0.0, 10.0);
//         gaCfg.weight_power    = utils::local_clamp<double>(jDouble("obj_weight_power", nullptr, 0.5), 0.0, 10.0);
//         gaCfg.weight_temp     = utils::local_clamp<double>(jDouble("obj_weight_temp", nullptr, 0.5), 0.0, 10.0);
//         gaCfg.weight_latency  = utils::local_clamp<double>(jDouble("obj_weight_latency", nullptr, 0.3), 0.0, 10.0);

//         // [P0-F17] Optional per-run override of the temperature-objective
//         // onset; defaults come from the calibrated AdaptiveWeightManager
//         // thresholds (see GAConfig).
//         gaCfg.temp_onset_cpu_c = jDouble("obj_temp_onset_cpu_c", nullptr, gaCfg.temp_onset_cpu_c);
//         gaCfg.temp_onset_gpu_c = jDouble("obj_temp_onset_gpu_c", nullptr, gaCfg.temp_onset_gpu_c);

//         controlIntervalMs_        = gaCfg.controlIntervalMs;
//         warmupMs_                 = utils::local_clamp<int>(jInt("warmup_ms", nullptr, 5000), 0, 60000);
//         minFpsUseful_             = jDouble("min_fps_for_evolution", nullptr, 1.0);
//         minPowerUseful_           = jDouble("min_power_for_evolution", nullptr, 0.0);
//         requirePowerForEvolution_ = jBool("require_power_for_evolution", nullptr, false);

//         gaCfg.stableFramesRequired     = utils::local_clamp<int>(jInt("stable_frames_required", nullptr, 1), 1, 120);
//         gaCfg.minFpsForEvolution       = minFpsUseful_;
//         gaCfg.minPowerForEvolution     = minPowerUseful_;
//         gaCfg.requirePowerForEvolution = requirePowerForEvolution_;

//         gaCfg.adaptive_weights_enabled = jBool("adaptive_weights_enabled", nullptr, false);
//         gaCfg.awm_config = awmCfg;  // single typed source from ConfigManager
        
//         //gaCfg.gpuSplitApplicable = jBool("gpu_split_applicable", nullptr, true);
//         // Then HistEq, Median and Sobel fail closed automatically.
//         const bool continuousSplit = activeAlgorithm == "HeterogeneousGaussianBlur";

//         gaCfg.gpuSplitApplicable = jBool("gpu_split_applicable",nullptr,continuousSplit);


//         gaCfg.forceGpuForWorkload = jBool("ga_force_gpu_for_workload", nullptr, false);

//         // Reproducibility: wire "seed" from config into the GA RNG.
//         // Default -1 keeps non-deterministic behaviour if the key is absent.
//         gaCfg.rngSeed = static_cast<long>(jInt("seed", nullptr, -1));

//         // Search-space floor for target_fps gene (raised from old hardcoded 8.0).
//         gaCfg.minTargetFps = utils::local_clamp<double>(jDouble("ga_min_target_fps", nullptr, 20.0), 1.0, 60.0);
//         gaCfg.maxTargetFps = utils::local_clamp<double>(jDouble("ga_max_target_fps", nullptr, 60.0), gaCfg.minTargetFps, 240.0);
//         gaCfg.performanceTargetFps = utils::local_clamp<double>(
//             jDouble("ga_performance_target_fps", nullptr, gaCfg.maxTargetFps),
//             gaCfg.minTargetFps, 240.0);
        
//         // [FIX] New surrogate-gating knobs (Patch 2a). Adapt jDouble to your helper name.
//         gaCfg.surrogateMinSupport      = utils::local_clamp<int>(jInt("surrogate_min_support", nullptr, 3), 1, 20);
//         gaCfg.surrogateColdFpsCeiling  = jDouble("surrogate_cold_fps_ceiling",  nullptr, 8.0);
//         gaCfg.surrogatePredFpsCapRatio = jDouble("surrogate_pred_fps_cap_ratio", nullptr, 1.2);
//         gaCfg.surrogateFpsMax          = jDouble("surrogate_fps_max",           nullptr, 60.0); // per-workload!

//         evidenceGateEnabled_ = jBool("evidence_gate_enabled", nullptr, true);
//         evidenceFreshFramesRequired_ = utils::local_clamp<int>(
//             jInt("evidence_fresh_frames", nullptr, 3), 1, 30);
//         evidenceTimeoutMs_ = utils::local_clamp<int>(
//             jInt("evidence_timeout_ms", nullptr, 2000), 250, 30000);

//         // PhD evidence export: ERL/Pareto traceability.
//         gaCfg.export_pareto_evidence = jBool("export_pareto_evidence", nullptr, true);
//         gaCfg.pareto_csv_path        = jString("pareto_csv", nullptr, "output/erl_pareto_front.csv");
//         gaCfg.action_csv_path        = jString("action_csv", nullptr, "output/erl_action_trace.csv");
//         erlTimingCsvPath_            = jString("erl_timing_csv", nullptr, gaCfg.action_csv_path + ".timing.csv");

//         spdlog::info("[EvolutionarySelector] Effective ERL config: pop={} minPop={} elite={} cross={:.2f} mut={:.2f} interval={}ms export={} pareto_csv={} action_csv={} minFPS={:.2f} requirePower={}",
//                      gaCfg.popSize, gaCfg.minPopSize, gaCfg.eliteCount, gaCfg.crossoverRate, gaCfg.mutationRate,
//                      gaCfg.controlIntervalMs, gaCfg.export_pareto_evidence ? "ON" : "OFF",
//                      gaCfg.pareto_csv_path, gaCfg.action_csv_path, minFpsUseful_,
//                      requirePowerForEvolution_ ? "YES" : "NO");
//         spdlog::info("[EvolutionarySelector] ERL timing CSV: {}", erlTimingCsvPath_);

        
//         if (gaCfg.adaptive_weights_enabled) {
//             spdlog::info("[EvolutionarySelector] Authoritative AWM config forwarded "
//                          "(update every {} gens, warn={:.1f}/{:.1f}C, crit={:.1f}/{:.1f}C, "
//                          "latency_sla={:.1f}ms)",
//                          gaCfg.awm_config.update_interval_gens,
//                          gaCfg.awm_config.thermal_warn_cpu_c,
//                          gaCfg.awm_config.thermal_warn_gpu_c,
//                          gaCfg.awm_config.thermal_crit_cpu_c,
//                          gaCfg.awm_config.thermal_crit_gpu_c,
//                          gaCfg.awm_config.latency_sla_ms);
//         } else {
//             spdlog::info("[EvolutionarySelector] Adaptive weights disabled by config.");
//         }
//         spdlog::info("[EvolutionarySelector] Workload policy: gpu_split_applicable={} force_gpu={} perf_target={:.1f} | evidence_gate={} {} fresh frames / {} ms",
//                      gaCfg.gpuSplitApplicable ? "YES" : "NO",
//                      gaCfg.forceGpuForWorkload ? "YES" : "NO",
//                      gaCfg.performanceTargetFps,
//                      evidenceGateEnabled_ ? "ON" : "OFF",
//                      evidenceFreshFramesRequired_, evidenceTimeoutMs_);

//         try {
//             ga_ = std::make_unique<hrl::GeneticAlgorithm>(
//                 gaCfg, aggregator_, runtimeControls_, &scheduler_, gaCfg.awm_config);

//             spdlog::info("[EvolutionarySelector] - Pareto-GA Hybrid Ready (pop={}-{}, reduction at gen {})",
//                          gaCfg.popSize, gaCfg.minPopSize, gaCfg.reductionStartGen);
//         } catch (const std::exception& e) {
//             spdlog::error("[EvolutionarySelector] Failed to initialize GeneticAlgorithm: {}", e.what());
//             throw;
//         }
//     }

//     bool validate() override {
//         spdlog::info("[EvolutionarySelector] Validating configuration...");
//         if (!aggregator_) {
//             spdlog::error("[EvolutionarySelector] Aggregator is null");
//             return false;
//         }
//         if (!runtimeControls_) {
//             spdlog::error("[EvolutionarySelector] RuntimeControls is null");
//             return false;
//         }
//         spdlog::info("[EvolutionarySelector] Validation successful");
//         return true;
//         // return aggregator_ && runtimeControls_; // Alfer testing all code just used thise line
//     }

//     void start() override {
//         spdlog::info("[EvolutionarySelector] Starting evolutionary control loop...");
//         scheduler_.initializePlatformEnvelope();
//         // // Initialize telemetry CSV if configured
//         // std::string csv_path = cfg_.value("action_csv", "output/erl_telemetry.csv");
//         // csv_stream_.open(csv_path, std::ios::out);
//         // if (csv_stream_.is_open()) {
//         //     csv_stream_ << "timestamp_ms," << hrl::ThermalGovernor::getCsvHeader() << "\n";
//         // }
//         //running_.store(true);
//         running_.store(true, std::memory_order_release);
//         controlThread_ = std::thread(&EvolutionarySelector::controlLoop, this);
//     }

//     void stop() override {
//         spdlog::info("[EvolutionarySelector] Stopping...");
//         //running_.store(false);
//         running_.store(false, std::memory_order_release);
//         if (controlThread_.joinable()) controlThread_.join();
//         scheduler_.reset();
//         spdlog::info("[EvolutionarySelector] Stopped");
//         spdlog::info("[EvolutionarySelector] System settings restored to default (BALANCED, 60 FPS, GPU on)");
//     }

//     // Helper function to extract a JSON parameter with fallback support
//     template <typename T>
//     T get_config_param(const nlohmann::json& root_cfg, 
//                         const std::string& key, 
//                         T default_val) 
//     {
//         // 1. Check primary location: root_cfg["Scheduler"][key]
//         if (root_cfg.contains("Scheduler") && root_cfg["Scheduler"].contains(key)) {
//             return root_cfg["Scheduler"][key].get<T>();
//         }
//         // 2. Fallback location: root_cfg["EvolutionarySelector"][key]
//         if (root_cfg.contains("EvolutionarySelector") && root_cfg["EvolutionarySelector"].contains(key)) {
//             return root_cfg["EvolutionarySelector"][key].get<T>();
//         }
//         // 3. Return fallback default if key is missing in both blocks
//         return default_val;
//     }

//     void configure(const nlohmann::json& root_cfg) {
//         // If passed only the "Scheduler" sub-object, fall back gracefully
//         const auto& jSched = root_cfg.contains("Scheduler") ? root_cfg["Scheduler"] : root_cfg;

//         // GA parameters
//         this->pop_size = jSched.value("ga_population_size", 8);
//         this->crossover_rate = jSched.value("ga_crossover_rate", 0.70);
//         this->mutation_rate = jSched.value("ga_mutation_rate", 0.15);

//         // // Adaptive Weight Manager parameters
//         // this->awm_battery_crit_w = jSched.value("awm_battery_critical_watts", 3.5);
//         // this->awm_thermal_warn_cpu = jSched.value("awm_thermal_warn_cpu_c", 57.0);
//         // this->awm_thermal_crit_cpu = jSched.value("awm_thermal_crit_cpu_c", 63.0);


//         // New configure() snippet (use 7.5 W and 45/55°C defaults):
//         this->awm_battery_crit_w = jSched.value("awm_battery_critical_watts", 7.5);
//         this->awm_thermal_warn_cpu = jSched.value("awm_thermal_warn_cpu_c", 45.0);
//         this->awm_thermal_crit_cpu = jSched.value("awm_thermal_crit_cpu_c", 55.0);
//     }

//     // Optional helper for external modules (e.g. logging or RL)
//     hrl::RLAction getBestAction() const {
//         return ga_ ? ga_->getBestAction() : hrl::RLAction{};
//     }

// private:
//     json cfg_;
//     Context& ctx_;
//     // Add missing member declarations:
//     size_t pop_size = 8;
//     double crossover_rate = 0.70;
//     double mutation_rate = 0.15;

//     // double awm_battery_crit_w = 3.5;
//     // double awm_thermal_warn_cpu = 57.0;
//     // double awm_thermal_crit_cpu = 63.0;
//     // New configure() snippet (use 7.5 W and 45/55°C defaults):

//     double awm_battery_crit_w = 7.5;
//     double awm_thermal_warn_cpu = 45.0;
//     double awm_thermal_crit_cpu = 55.0;


//     std::shared_ptr<SystemMetricsAggregatorConcreteV3_2> aggregator_;
//     std::shared_ptr<hrl::RuntimeControls> runtimeControls_;
//     hrl::Scheduler scheduler_;
//     std::unique_ptr<hrl::GeneticAlgorithm> ga_;

//     std::atomic<bool> running_{false};
//     std::thread controlThread_;
//     int loopCount_ = 0;  // Control loop iteration counter (for periodic regime logging)
//     int controlIntervalMs_ = 500;
//     int warmupMs_ = 5000;
//     double minFpsUseful_ = 1.0;
//     double minPowerUseful_ = 0.0;
//     bool requirePowerForEvolution_ = false;

//     // Snapshot admissibility gate kept inside the class to avoid scope drift during patching.
//     bool hasUsefulData(const hrl::MetricsSnapshot& s) const {
//         if (s.fps < minFpsUseful_) return false;
//         if (requirePowerForEvolution_ && s.avg_power_w_alg < minPowerUseful_) return false;
//         return true;
//     }

//     // Temporal credit assignment gate: one applied action receives exactly one
//     // evidence window before the next generation is permitted.
//     bool evidenceGateEnabled_ = true;
//     int evidenceFreshFramesRequired_ = 3;
//     int evidenceTimeoutMs_ = 2000;
//     bool awaitingEvidence_ = false;
//     uint64_t evidenceActionEpoch_ = 0;
//     uint64_t evidenceLastFrameId_ = 0;
//     uint64_t evidenceBaselineFrameId_ = 0;
//     int evidenceFreshCount_ = 0;
//     std::chrono::steady_clock::time_point evidenceStart_{};

//     hrl::MetricsSnapshot evidenceSum_{};

//     // [FIX P5] Consecutive evidence-timeout counter; the old recovery only
//     // counted SKIP_INVALID_SNAPSHOT and was unreachable from the trap.
//     int consecutiveTimeouts_ = 0;
//     static constexpr int kMaxConsecutiveTimeouts_ = 8;

//     // [FIX P8] Last known frame-rate estimate used to scale the evidence window.
//     double lastFpsEstimate_ = 30.0;

//     // ADD THESE TWO LINES:
//     double lastValidPower_ = 0.0;          
//     std::deque<double> powerHistory_;
//     uint64_t powerRejectCount_ = 0;   // [P0-F15] PowerSanity rejects (throttled logging)

//     // [PhD timing instrumentation]
//     std::string erlTimingCsvPath_{"output/erl_control_timing.csv"};
//     std::mutex erlTimingCsvMutex_;
//     uint64_t timingIteration_{0};
//     uint64_t lastTimingFrameId_{0};

//     static uint64_t steadyNowNs_() {
//         return static_cast<uint64_t>(
//             std::chrono::duration_cast<std::chrono::nanoseconds>(
//                 std::chrono::steady_clock::now().time_since_epoch()).count());
//     }

//     static uint64_t threadCpuNowNs_() {
//         struct timespec ts{};
//         if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
//         return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
//                static_cast<uint64_t>(ts.tv_nsec);
//     }

//     void resetEvidenceAccumulator_() {
//         evidenceFreshCount_ = 0;
//         evidenceLastFrameId_ = evidenceBaselineFrameId_;
//         evidenceSum_ = hrl::MetricsSnapshot{};
//     }

//     void beginEvidenceWindow_(uint64_t baselineFrameId) {
//         if (!evidenceGateEnabled_ || !runtimeControls_) {
//             awaitingEvidence_ = false;
//             return;
//         }
//         evidenceActionEpoch_ = runtimeControls_->action_generation.load(std::memory_order_acquire);
//         awaitingEvidence_ = evidenceActionEpoch_ > 0;
//         evidenceBaselineFrameId_ = baselineFrameId;
//         evidenceStart_ = std::chrono::steady_clock::now();
//         resetEvidenceAccumulator_();
//     }

//     bool addFreshEvidenceSample_(const hrl::MetricsSnapshot& s) {
//         if (!runtimeControls_ || s.frameId == 0) return false;
//         const uint64_t observed = runtimeControls_->algorithm_observed_generation.load(std::memory_order_acquire);
//         if (observed < evidenceActionEpoch_) return false;
//         if (s.frameId <= evidenceBaselineFrameId_) return false;
//         if (s.frameId == evidenceLastFrameId_) return false;
//         evidenceLastFrameId_ = s.frameId;
//         ++evidenceFreshCount_;
//         evidenceSum_.fps += s.fps;
//         evidenceSum_.avg_power_w_alg += s.avg_power_w_alg;
//         evidenceSum_.joulesPerFrame += s.joulesPerFrame;
//         evidenceSum_.joules_per_frame += s.joules_per_frame;
//         evidenceSum_.avg_inference_ms += s.avg_inference_ms;
//         evidenceSum_.processing_latency_ms += s.processing_latency_ms;
//         evidenceSum_.end_to_end_latency_ms += s.end_to_end_latency_ms;
//         evidenceSum_.avg_latency_ms += s.avg_latency_ms;
//         evidenceSum_.latency_ms += s.latency_ms;
//         evidenceSum_.cpu_util_avg += s.cpu_util_avg;
//         evidenceSum_.gpu_util_avg += s.gpu_util_avg;
//         evidenceSum_.cpu_temp_c += s.cpu_temp_c;
//         evidenceSum_.gpu_temp_c += s.gpu_temp_c;
//         return true;
//     }

//     hrl::MetricsSnapshot averagedEvidence_(const hrl::MetricsSnapshot& latest) const {
//         if (evidenceFreshCount_ <= 0) return latest;
//         const double n = static_cast<double>(evidenceFreshCount_);
//         hrl::MetricsSnapshot out = latest;
//         out.fps = evidenceSum_.fps / n;
//         out.avg_power_w_alg = evidenceSum_.avg_power_w_alg / n;
//         out.joulesPerFrame = evidenceSum_.joulesPerFrame / n;
//         out.joules_per_frame = evidenceSum_.joules_per_frame / n;
//         out.avg_inference_ms = evidenceSum_.avg_inference_ms / n;
//         out.processing_latency_ms = evidenceSum_.processing_latency_ms / n;
//         out.end_to_end_latency_ms = evidenceSum_.end_to_end_latency_ms / n;
//         out.avg_latency_ms = evidenceSum_.avg_latency_ms / n;
//         out.latency_ms = evidenceSum_.latency_ms / n;
//         out.cpu_util_avg = evidenceSum_.cpu_util_avg / n;
//         out.gpu_util_avg = evidenceSum_.gpu_util_avg / n;
//         out.cpu_temp_c = evidenceSum_.cpu_temp_c / n;
//         out.gpu_temp_c = evidenceSum_.gpu_temp_c / n;
//         out.valid = true;
//         return out;
//     }

//     void appendErlTimingRow_(uint64_t generation,
//                              const hrl::MetricsSnapshot& snapshot,
//                              const hrl::GATimingMetrics& gaTiming,
//                              const char* status,
//                              double snapshotFetchMs,
//                              double erlActiveWallMs,
//                              double erlThreadCpuMs,
//                              uint64_t framesSincePrevious) {
//         if (erlTimingCsvPath_.empty()) return;

//         std::lock_guard<std::mutex> guard(erlTimingCsvMutex_);
//         const bool writeHeader = !std::ifstream(erlTimingCsvPath_).good();
//         std::ofstream out(erlTimingCsvPath_, std::ios::app);
//         if (!out.is_open()) {
//             spdlog::warn("[EvolutionarySelector] Could not open ERL timing CSV: {}", erlTimingCsvPath_);
//             return;
//         }

//         if (writeHeader) {
//             out << "control_iteration,generation,frame_id,status,"
//                 << "algorithm_processing_ms,pipeline_e2e_ms,snapshot_fetch_ms,"
//                 << "ga_compute_total_ms,ga_weight_update_ms,ga_evaluation_ms,ga_nsga_sort_ms,"
//                 << "ga_selection_ms,scheduler_apply_ms,ga_action_dispatch_overhead_ms,ga_pareto_export_ms,ga_diagnostics_ms,"
//                 << "ga_mutation_ms,ga_offspring_ms,ga_postprocess_ms,"
//                 << "erl_active_wall_ms,erl_thread_cpu_ms,control_interval_ms,erl_duty_cycle_pct,"
//                 << "frames_since_previous,erl_amortised_ms_per_frame,"
//                 << "action_epoch,algorithm_observed_epoch,action_response_epoch,action_response_ms,"
//                 << "fps,power_w,cpu_temp_c,gpu_temp_c\n";
//         }

//         const double duty = controlIntervalMs_ > 0
//             ? 100.0 * erlActiveWallMs / static_cast<double>(controlIntervalMs_)
//             : 0.0;
//         const double amortised = framesSincePrevious > 0
//             ? erlActiveWallMs / static_cast<double>(framesSincePrevious)
//             : -1.0;

//         const uint64_t actionEpoch = runtimeControls_
//             ? runtimeControls_->action_generation.load(std::memory_order_acquire) : 0ULL;
//         const uint64_t observedEpoch = runtimeControls_
//             ? runtimeControls_->algorithm_observed_generation.load(std::memory_order_acquire) : 0ULL;
//         const uint64_t responseEpoch = runtimeControls_
//             ? runtimeControls_->action_response_generation.load(std::memory_order_acquire) : 0ULL;
//         const uint64_t responseNs = runtimeControls_
//             ? runtimeControls_->action_response_latency_ns.load(std::memory_order_acquire) : 0ULL;
//         const double responseMs = responseEpoch > 0
//             ? static_cast<double>(responseNs) / 1.0e6 : -1.0;

//         out << timingIteration_++ << ","
//             << generation << ","
//             << snapshot.frameId << ","
//             << status << ","
//             << snapshot.avg_inference_ms << ","
//             << snapshot.end_to_end_latency_ms << ","
//             << snapshotFetchMs << ","
//             << gaTiming.total_compute_ms << ","
//             << gaTiming.weight_update_ms << ","
//             << gaTiming.evaluation_ms << ","
//             << gaTiming.nsga_sort_ms << ","
//             << gaTiming.selection_ms << ","
//             << gaTiming.scheduler_apply_ms << ","
//             << gaTiming.action_dispatch_overhead_ms << ","
//             << gaTiming.pareto_export_ms << ","
//             << gaTiming.diagnostics_ms << ","
//             << gaTiming.mutation_ms << ","
//             << gaTiming.offspring_ms << ","
//             << gaTiming.postprocess_ms << ","
//             << erlActiveWallMs << ","
//             << erlThreadCpuMs << ","
//             << controlIntervalMs_ << ","
//             << duty << ","
//             << framesSincePrevious << ","
//             << amortised << ","
//             << actionEpoch << ","
//             << observedEpoch << ","
//             << responseEpoch << ","
//             << responseMs << ","
//             << snapshot.fps << ","
//             << snapshot.avg_power_w_alg << ","
//             << snapshot.cpu_temp_c << ","
//             << snapshot.gpu_temp_c
//             << "\n";
//     }

// //=========================================================================
// void controlLoop() {
//         spdlog::debug("[EvolutionarySelector] Control loop started");

//         spdlog::info("[EvolutionarySelector] Waiting for pipeline to become READY...");
//         while (!ctx_.pipelineReady.load(std::memory_order_acquire)) {
//             if (ctx_.shutdown_flag.load()) {
//                 spdlog::info("[EvolutionarySelector] Shutdown requested during wait");
//                 std::this_thread::sleep_for(50ms);
//                 return;
//             }
//             std::this_thread::sleep_for(50ms);
//         }

//         spdlog::info("[EvolutionarySelector] Pipeline READY. Warming up...");
//         if (warmupMs_ > 0) {
//             std::this_thread::sleep_for(std::chrono::milliseconds(warmupMs_));
//         }

//         spdlog::info("[EvolutionarySelector] Engaging control loop with split timing instrumentation.");

//         running_.store(true);
//         int consecutiveSkips = 0;
//         const int MAX_SKIPS = 20;
//         uint64_t loopCount = 0;

//         while (running_.load() && !ctx_.shutdown_flag.load()) {
//             const auto loopStart = std::chrono::steady_clock::now();
//             const uint64_t cpuStartNs = threadCpuNowNs_();

//             hrl::MetricsSnapshot snapshot{};
//             hrl::GATimingMetrics gaTiming{};
//             const uint64_t generationBefore = ga_ ? static_cast<uint64_t>(ga_->getCurrentGeneration()) : 0ULL;
//             double snapshotFetchMs = 0.0;
//             const char* status = "EVOLVE";
//             bool skipped = false;
//             bool evidenceWaitingThisLoop = false;

//             try {
//                 const auto snapshotStart = std::chrono::steady_clock::now();
//                 snapshot = getLatestSnapshot();
//                 const auto snapshotEnd = std::chrono::steady_clock::now();
//                 snapshotFetchMs = std::chrono::duration<double, std::milli>(
//                     snapshotEnd - snapshotStart).count();

//                 if (!snapshot.valid || !hasUsefulData(snapshot)) {
//                     status = "SKIP_INVALID_SNAPSHOT";
//                     skipped = true;
//                     consecutiveSkips++;
//                     if (consecutiveSkips >= MAX_SKIPS) {
//                         spdlog::warn("[EvolutionarySelector] EMERGENCY RECOVERY: resetting to MAX PERFORMANCE");
//                         scheduler_.recoverFast(*runtimeControls_);
//                         consecutiveSkips = 0;
//                         status = "SKIP_RECOVERY_RESET";
//                     }
//                     spdlog::warn("[EvolutionarySelector] Snapshot not ready, skipping evolution");
//                 } else {
//                     consecutiveSkips = 0;
//                     bool evolveNow = true;
//                     bool creditPrevious = true;
//                     bool previousStalled = false;

//                     if (evidenceGateEnabled_ && awaitingEvidence_) {
//                         (void)addFreshEvidenceSample_(snapshot);
//                         const auto evidenceElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
//                             std::chrono::steady_clock::now() - evidenceStart_).count();

//                         // if (evidenceFreshCount_ >= evidenceFreshFramesRequired_) {
//                         //     snapshot = averagedEvidence_(snapshot);
//                         //     status = "EVOLVE_EVIDENCE_READY";
//                         //     awaitingEvidence_ = false;
//                         // } else if (evidenceElapsedMs >= evidenceTimeoutMs_) {
//                         //     // Do not credit stale physical measurements. The GA records a
//                         //     // feasibility/stall event for the previous configuration instead.
//                         //     status = "EVIDENCE_TIMEOUT";
//                         //     awaitingEvidence_ = false;
//                         //     creditPrevious = false;
//                         //     previousStalled = true;
//                         //     spdlog::warn("[EvolutionarySelector] Evidence timeout: epoch={} fresh={}/{} after {} ms",
//                         //                  evidenceActionEpoch_, evidenceFreshCount_,
//                         //                  evidenceFreshFramesRequired_, evidenceElapsedMs);
//                         // } else {

//                             // [FIX P8] A fixed 2 s window guarantees timeout below
//                         // ~1.5-6 fps (needs `required` fresh frames). Scale the
//                         // window with the last known frame rate, bounded at 15 s.
//                         const long long effectiveTimeoutMs = std::min<long long>(15000,
//                             std::max<long long>(evidenceTimeoutMs_,
//                                 static_cast<long long>(std::ceil(
//                                     1000.0 * evidenceFreshFramesRequired_ /
//                                     std::max(lastFpsEstimate_, 0.5)))));

//                         if (evidenceFreshCount_ >= evidenceFreshFramesRequired_) {
//                             snapshot = averagedEvidence_(snapshot);
//                             status = "EVOLVE_EVIDENCE_READY";
//                             awaitingEvidence_ = false;
//                             consecutiveTimeouts_ = 0;                       // [FIX P5]
//                             lastFpsEstimate_ = std::max(snapshot.fps, 0.5); // [FIX P8]
//                         } else if (evidenceElapsedMs >= effectiveTimeoutMs) {
//                             // [FIX P1] A bounded timeout IS a measurement: the applied
//                             // configuration produced `evidenceFreshCount_` frames in
//                             // `evidenceElapsedMs`. Synthesise that observation into the
//                             // snapshot so the GA's stall path can teach the surrogate
//                             // the truth instead of leaving its optimistic prior intact.
//                             const double starvedFps =
//                                 1000.0 * static_cast<double>(evidenceFreshCount_) /
//                                 static_cast<double>(std::max<long long>(evidenceElapsedMs, 1));
//                             snapshot.fps = starvedFps;
//                             if (snapshot.avg_latency_ms <= 0.0 && starvedFps > 0.0)
//                                 snapshot.avg_latency_ms = 1000.0 / starvedFps;
//                             snapshot.valid = true;
//                             lastFpsEstimate_ = std::max(starvedFps, 0.5);   // [FIX P8]

//                             status = "EVIDENCE_TIMEOUT";
//                             awaitingEvidence_ = false;
//                             creditPrevious = false;   // GA credits via the stall path
//                             previousStalled = true;
//                             ++consecutiveTimeouts_;                          // [FIX P5]
//                             spdlog::warn("[EvolutionarySelector] Evidence timeout: epoch={} "
//                                          "fresh={}/{} after {} ms (starved_fps={:.2f}, streak={})",
//                                          evidenceActionEpoch_, evidenceFreshCount_,
//                                          evidenceFreshFramesRequired_, evidenceElapsedMs,
//                                          starvedFps, consecutiveTimeouts_);

//                             // [FIX P5] Persistent starvation => force a known-good state.
//                             // Bounds worst-case blindness to ~kMax * window (~13-16 s).
//                             if (consecutiveTimeouts_ >= kMaxConsecutiveTimeouts_) {
//                                 spdlog::warn("[EvolutionarySelector] STARVATION RECOVERY: "
//                                              "forcing MAX_PERFORMANCE after {} consecutive timeouts",
//                                              consecutiveTimeouts_);
//                                 scheduler_.recoverFast(*runtimeControls_);
//                                 if (ga_) ga_->notifyExternalOverride();   // see Patch 2g
//                                 consecutiveTimeouts_ = 0;
//                                 status = "TIMEOUT_RECOVERY_RESET";
//                                 evolveNow = false;   // give the recovered state one clean window
//                                 skipped = true;
//                             }
//                         } else {

//                             status = "WAIT_ACTION_EVIDENCE";
//                             evolveNow = false;
//                             skipped = true;
//                             evidenceWaitingThisLoop = true;
//                         }
//                     }

//                     if (evolveNow) {
//                         const uint64_t genBeforeEvolve = static_cast<uint64_t>(ga_->getCurrentGeneration());
//                         ga_->evolve(snapshot, creditPrevious, previousStalled);
//                         gaTiming = ga_->getLastTimingMetrics();
//                         if (static_cast<uint64_t>(ga_->getCurrentGeneration()) > genBeforeEvolve) {
//                             beginEvidenceWindow_(snapshot.frameId);
//                         }

//                         if (++loopCount % 10 == 0) {
//                             spdlog::info(
//                                 "[EvolutionarySelector] Regime={} | fps={:.1f} | power={:.2f}W | "
//                                 "alg={:.3f}ms | e2e={:.3f}ms | GA={:.3f}ms",
//                                 ga_->getCurrentRegimeName(),
//                                 snapshot.fps, snapshot.avg_power_w_alg,
//                                 snapshot.avg_inference_ms,
//                                 snapshot.end_to_end_latency_ms,
//                                 gaTiming.total_compute_ms);
//                         }
//                     }
//                 }

//             } catch (const std::exception& e) {
//                 status = "EXCEPTION";
//                 spdlog::error("[EvolutionarySelector] Error in control loop: {}", e.what());
//             } catch (...) {
//                 status = "UNKNOWN_EXCEPTION";
//                 spdlog::error("[EvolutionarySelector] Unknown exception in control loop");
//             }

//             const auto activeEnd = std::chrono::steady_clock::now();
//             const uint64_t cpuEndNs = threadCpuNowNs_();
//             const double erlActiveWallMs = std::chrono::duration<double, std::milli>(
//                 activeEnd - loopStart).count();
//             const double erlThreadCpuMs = (cpuEndNs >= cpuStartNs && cpuStartNs != 0)
//                 ? static_cast<double>(cpuEndNs - cpuStartNs) / 1.0e6
//                 : 0.0;

//             uint64_t framesSincePrevious = 0;
//             if (snapshot.valid && snapshot.frameId > 0) {
//                 if (lastTimingFrameId_ > 0 && snapshot.frameId >= lastTimingFrameId_) {
//                     framesSincePrevious = snapshot.frameId - lastTimingFrameId_;
//                 }
//                 lastTimingFrameId_ = snapshot.frameId;
//             }

//             appendErlTimingRow_(generationBefore, snapshot, gaTiming, status,
//                                 snapshotFetchMs, erlActiveWallMs, erlThreadCpuMs,
//                                 framesSincePrevious);

//             // Preserve the original 500 ms starvation backoff. Successful
//             // iterations retain the configured control cadence.
//             if (skipped) {
//                 // Evidence polling is intentionally faster than starvation backoff:
//                 // the gate needs three distinct post-action frames without adding
//                 // an artificial ~1 s sampling delay of its own.
//                 std::this_thread::sleep_for(evidenceWaitingThisLoop ? 50ms : 500ms);
//                 continue;
//             }

//             const auto elapsed = std::chrono::steady_clock::now() - loopStart;
//             const auto sleepTime = std::chrono::milliseconds(controlIntervalMs_) - elapsed;
//             if (sleepTime > 0ms) {
//                 std::this_thread::sleep_for(sleepTime);
//             }
//         }

//         spdlog::info("[EvolutionarySelector] Control loop ended cleanly");
//     }

//     //==================================================================
//     // void controlLoop() {
//     //     spdlog::debug("[EvolutionarySelector] Control loop started");

//     //     // Pipeline readiness wait
//     //     spdlog::info("[EvolutionarySelector] Waiting for pipeline to become READY...");
//     //     while (!ctx_.pipelineReady.load(std::memory_order_acquire)) {
//     //         if (ctx_.shutdown_flag.load()) {
//     //             spdlog::info("[EvolutionarySelector] Shutdown requested during wait");
//     //             std::this_thread::sleep_for(50ms);
//     //             return;
//     //         }
//     //         std::this_thread::sleep_for(50ms);
//     //     }

//     //     // Extended warmup
//     //     spdlog::info("[EvolutionarySelector] Pipeline READY. Warming up for {} ms...", warmupMs_);
//     //     if (warmupMs_ > 0) {
//     //         std::this_thread::sleep_for(std::chrono::milliseconds(warmupMs_));
//     //     }

//     //     spdlog::info("[EvolutionarySelector] Engaging control loop.");

//     //     // Inside the main evolutionary control loop:
//     //     void run() {
//     //         running_ = true;
//     //         int consecutiveSkips = 0;
//     //         const int MAX_SKIPS = 10; // Example threshold

//     //         // while (running_ && !ctx_.shutdown_flag.load()) {
//     //         while (running_.load() && !ctx_.shutdown_flag.load()) {
//     //             auto loopStart = std::chrono::steady_clock::now();

//     //             try {
//     //                 auto snapshot = getLatestSnapshot();

//     //                 if (!snapshot.valid || !hasUsefulData(snapshot)) {
//     //                     consecutiveSkips++;        
//     //                     // Fix 3 (EvolutionarySelector_02.h): Increase starvation sleep 
//     //                     // interval from 80ms to 500ms to reduce CPU contention.
//     //                     if (consecutiveSkips >= MAX_SKIPS) {
//     //                         spdlog::warn("[EvolutionarySelector] EMERGENCY RECOVERY: resetting to MAX PERFORMANCE");
//     //                         scheduler_.reset(); // Force high-performance profile
//     //                         consecutiveSkips = 0;
//     //                     }
//     //                     spdlog::warn("[EvolutionarySelector] Snapshot not ready, skipping iteration");
//     //                     std::this_thread::sleep_for(500ms);
//     //                     continue;
//     //                 }


//     //                 // === Pareto + Scalar GA Evolution (Single Source of Truth) ===
//     //                 // === Pareto + Scalar GA Evolution (Single Source of Truth) ===
//     //                 consecutiveSkips = 0;   

//     //                 ga_->evolve(snapshot);

//     //                 // Log current weight regime every 10 iterations for thesis data
//     //                 if (++loopCount_ % 10 == 0) {
//     //                     spdlog::info("[EvolutionarySelector] Regime={} | fps={:.1f} | power={:.2f}W | "
//     //                                 "cpu_temp={:.1f}°""C | gpu_temp={:.1f}°""C | latency={:.1f}ms",
//     //                                 ga_->getCurrentRegimeName(),
//     //                                 snapshot.fps, snapshot.avg_power_w_alg,
//     //                                 snapshot.cpu_temp_c, snapshot.gpu_temp_c,
//     //                                 snapshot.avg_latency_ms);
//     //                 }

//     //             } catch (const std::exception& e) {
//     //                 spdlog::error("[EvolutionarySelector] Error in control loop: {}", e.what());
//     //             } catch (...) {
//     //                 spdlog::error("[EvolutionarySelector] Unknown exception in control loop");
//     //             }

//     //             // Precise timing
//     //             auto elapsed = std::chrono::steady_clock::now() - loopStart;
//     //             auto sleepTime = std::chrono::milliseconds(controlIntervalMs_) - elapsed;
//     //             if (sleepTime > 0ms) {
//     //                 std::this_thread::sleep_for(sleepTime);
//     //             }
//     //         }
//     //     }

//     //     spdlog::info("[EvolutionarySelector] Control loop ended cleanly");
//     // }

// //======================================================================================
// hrl::MetricsSnapshot getLatestSnapshot() {
//         hrl::MetricsSnapshot snap{};
//         snap.valid = false;

//         if (!aggregator_) {
//             spdlog::warn("[EvolutionarySelector] Aggregator is NULL");
//             return snap;
//         }

//         try {
//             auto raw = aggregator_->getLatestSnapshot();
//             if (!raw.isValid()) {
//                 spdlog::debug("[EvolutionarySelector] Raw snapshot not valid yet");
//                 return snap;
//             }

//             // FIX 1: Map frame_id (Closes the zeroed frame_id column in trace)
//             snap.frameId = raw.frameId;

//             snap.fps = raw.algorithmStats.fps;

//             // [P0-F15] Power mapping rewritten.
//             //  - The measured, window-integrated aggregator power is used
//             //    directly. The old re-derivation from joulesPerFrame was
//             //    circular (the aggregator computes J/F FROM power) and used
//             //    a different time base.
//             //  - The old "mW to W" auto-fix multiplied any reading in
//             //    (0, 0.5) W by 1000, turning a real 0.4 W idle sample into
//             //    400 W - exactly the class of poisoning documented in the
//             //    2026-07-11 PowerSanity postmortem. Deleted.
//             //  - The fabricated 5.0 W default is deleted: absence of a
//             //    measurement is reported honestly as 0.0 (and can pause
//             //    evolution via require_power_for_evolution), never invented.
//             //  - Every sample must pass PowerSanity before it can reach the
//             //    GA fitness, the AdaptiveWeightManager, or the surrogate.
//             double instantPower = (raw.avg_power_w_alg > 0.0)
//                 ? raw.avg_power_w_alg
//                 : raw.powerStats.totalPower();

//             if (hrl::PowerSanity::valid(instantPower)) {
//                 lastValidPower_ = instantPower;
//                 // 5-sample moving average of ACCEPTED samples only.
//                 powerHistory_.push_back(instantPower);
//                 if (powerHistory_.size() > 5) {
//                     powerHistory_.pop_front();
//                 }
//             } else if (instantPower != 0.0) {
//                 // 0.0 means "no power module / no data yet" - not worth a log.
//                 if (++powerRejectCount_ % 10 == 1) {
//                     spdlog::warn("[EvolutionarySelector] PowerSanity rejected "
//                                  "{:.3f}W ({}) - {} rejected so far",
//                                  instantPower,
//                                  hrl::PowerSanity::reject_reason(instantPower),
//                                  powerRejectCount_);
//                 }
//             }

//             double avgPower = 0.0;
//             if (!powerHistory_.empty()) {
//                 for (double p : powerHistory_) {
//                     avgPower += p;
//                 }
//                 avgPower /= static_cast<double>(powerHistory_.size());
//             }

//             snap.avg_power_w_alg = avgPower;

//             // Resource mapping
//             snap.cpu_util_avg = (raw.socInfo.CPU1_Utilization_Percent +
//                                  raw.socInfo.CPU2_Utilization_Percent +
//                                  raw.socInfo.CPU3_Utilization_Percent +
//                                  raw.socInfo.CPU4_Utilization_Percent) / 4.0;
//             snap.gpu_util_avg   = raw.socInfo.GR3D_Frequency_Percent;

//             // FIX 3: Thermal mapping (Closes the obj_temp = 0 bug)
//             // Tegrastats parses can output millidegrees. Normalize to Celsius.
//             double c_temp = raw.socInfo.CPU_Temperature_C;
//             double g_temp = raw.socInfo.GPU_Temperature_C;
            
//             if (c_temp > 1000.0) c_temp /= 1000.0;
//             if (g_temp > 1000.0) g_temp /= 1000.0;

//             snap.cpu_temp_c = c_temp;
//             snap.gpu_temp_c = g_temp;

//             // [PhD timing split] Keep algorithm processing and pipeline E2E
//             // latency as independent measurements. avg_latency_ms remains the
//             // E2E value for backward-compatible GA objective behaviour; thesis
//             // comparisons should use avg_inference_ms for algorithm-only timing.
//             snap.avg_inference_ms = raw.algorithmStats.inferenceTimeMs;
//             snap.processing_latency_ms = raw.algorithmStats.inferenceTimeMs;
//             snap.end_to_end_latency_ms = raw.endToEndLatencyMs;
//             snap.avg_latency_ms = raw.endToEndLatencyMs;
//             snap.latency_ms = raw.endToEndLatencyMs;
            
//             // [P0-F15] Single definition of energy-per-frame: the aggregator's
//             // measured value (power integrated over the algorithm window).
//             // The old re-derivation avg_power * end_to_end_latency used a
//             // different time base and disagreed with the exported metrics.
//             snap.joulesPerFrame   = raw.joulesPerFrame;
//             snap.joules_per_frame = raw.joulesPerFrame;

//             snap.valid = true;
//             spdlog::debug("[EvolutionarySelector] Valid snapshot (fps={:.1f}, pwr={:.2f}W, temp={:.1f}C)", 
//                           snap.fps, snap.avg_power_w_alg, snap.cpu_temp_c);
//             return snap;

//         } catch (const std::exception& e) {
//             spdlog::warn("[EvolutionarySelector] Exception in getLatestSnapshot: {}", e.what());
//         } catch (...) {
//             spdlog::warn("[EvolutionarySelector] Unknown exception in getLatestSnapshot");
//         }

//         return snap;
//     }
// //======================================================================================

// // hrl::MetricsSnapshot getLatestSnapshot() {
// //         hrl::MetricsSnapshot snap{};
// //         snap.valid = false;

// //         if (!aggregator_) {
// //             spdlog::warn("[EvolutionarySelector] Aggregator is NULL");
// //             return snap;
// //         }

// //         try {
// //             auto raw = aggregator_->getLatestSnapshot();
// //             if (!raw.isValid()) {
// //                 spdlog::debug("[EvolutionarySelector] Raw snapshot not valid yet");
// //                 return snap;
// //             }

// //             // FIX 1: Map frame_id (Closes the zeroed frame_id column in trace)
// //             snap.frameId = raw.frameId;

// //             snap.fps = raw.algorithmStats.fps;

// //             // FIX 2: Power mapping (Closes the 0.09W blind energy optimizer)
// //             // If pulling from raw.powerStats.averagePower() yielded 0.09, we have a unit/rail bug.
// //             // We scale up milliWatt-range errors or fallback to a safe 5.0W baseline to ensure 
// //             // the GA has a valid gradient to optimize against.
// //             double power_val = raw.powerStats.averagePower();
// //             if (raw.joulesPerFrame > 0 && raw.algorithmStats.totalProcTimeMs > 0) {
// //                 power_val = raw.joulesPerFrame / (raw.algorithmStats.totalProcTimeMs / 1000.0);
// //             }
            
// //             if (power_val > 0.0 && power_val < 0.5) {
// //                 power_val *= 1000.0; // Correct mW to W scale error
// //             } else if (power_val <= 0.0) {
// //                 power_val = 5.0; // Sane Jetson Nano default
// //             }
// //             snap.avg_power_w_alg = power_val;

// //             snap.cpu_util_avg = (raw.socInfo.CPU1_Utilization_Percent +
// //                                  raw.socInfo.CPU2_Utilization_Percent +
// //                                  raw.socInfo.CPU3_Utilization_Percent +
// //                                  raw.socInfo.CPU4_Utilization_Percent) / 4.0;
// //             snap.gpu_util_avg   = raw.socInfo.GR3D_Frequency_Percent;

// //             // FIX 3: Thermal mapping (Closes the obj_temp = 0 bug)
// //             // Tegrastats parses can output millidegrees. Normalize to Celsius.
// //             double c_temp = raw.socInfo.CPU_Temperature_C;
// //             double g_temp = raw.socInfo.GPU_Temperature_C;
            
// //             if (c_temp > 1000.0) c_temp /= 1000.0;
// //             if (g_temp > 1000.0) g_temp /= 1000.0;

// //             snap.cpu_temp_c = c_temp;
// //             snap.gpu_temp_c = g_temp;

// //             snap.end_to_end_latency_ms = raw.endToEndLatencyMs;
// //             snap.avg_latency_ms = raw.endToEndLatencyMs;
            
// //             // Re-sync Joules per frame with the corrected power value
// //             snap.joulesPerFrame = snap.avg_power_w_alg * (snap.avg_latency_ms / 1000.0);
// //             snap.joules_per_frame = snap.joulesPerFrame;

// //             snap.valid = true;
// //             spdlog::debug("[EvolutionarySelector] Valid snapshot (fps={:.1f}, pwr={:.2f}W, temp={:.1f}C)", 
// //                           snap.fps, snap.avg_power_w_alg, snap.cpu_temp_c);
// //             return snap;

// //         } catch (const std::exception& e) {
// //             spdlog::warn("[EvolutionarySelector] Exception in getLatestSnapshot: {}", e.what());
// //         } catch (...) {
// //             spdlog::warn("[EvolutionarySelector] Unknown exception in getLatestSnapshot");
// //         }

// //         return snap;
// //     }
// //================================================================================
// // //================================================================================

// //     hrl::MetricsSnapshot getLatestSnapshot() {
// //         hrl::MetricsSnapshot snap{};
// //         snap.valid = false;

// //         if (!aggregator_) {
// //             spdlog::warn("[EvolutionarySelector] Aggregator is NULL");
// //             return snap;
// //         }

// //         try {
// //             auto raw = aggregator_->getLatestSnapshot();
// //             if (!raw.isValid()) {
// //                 spdlog::debug("[EvolutionarySelector] Raw snapshot not valid yet");
// //                 return snap;
// //             }

// //             // FIX 1: Map frame_id (Closes the zeroed frame_id column in trace)
// //             snap.frameId = raw.frameId;

// //             // Safe conversion with joulesPerFrame priority
// //             snap.fps = raw.algorithmStats.fps;


// //             snap.avg_power_w_alg = (raw.joulesPerFrame > 0 && raw.algorithmStats.totalProcTimeMs > 0)
// //                 ? raw.joulesPerFrame / (raw.algorithmStats.totalProcTimeMs / 1000.0)
// //                 : raw.powerStats.averagePower();

// //             snap.cpu_util_avg = (raw.socInfo.CPU1_Utilization_Percent +
// //                                  raw.socInfo.CPU2_Utilization_Percent +
// //                                  raw.socInfo.CPU3_Utilization_Percent +
// //                                  raw.socInfo.CPU4_Utilization_Percent) / 4.0;

// //             snap.gpu_util_avg   = raw.socInfo.GR3D_Frequency_Percent;
// //             snap.cpu_temp_c     = raw.socInfo.CPU_Temperature_C;
// //             snap.gpu_temp_c     = raw.socInfo.GPU_Temperature_C;
// //             snap.avg_latency_ms = raw.endToEndLatencyMs;
// //             snap.joulesPerFrame = raw.joulesPerFrame;   // Important for ERL reward

// //             snap.valid = true;
// //             spdlog::debug("[EvolutionarySelector] Valid snapshot (fps={:.1f})", snap.fps);
// //             return snap;

// //         } catch (const std::exception& e) {
// //             spdlog::warn("[EvolutionarySelector] Exception in getLatestSnapshot: {}", e.what());
// //         } catch (...) {
// //             spdlog::warn("[EvolutionarySelector] Unknown exception in getLatestSnapshot");
// //         }

// //         return snap;  // invalid
// //     }

// //================================================================================

// };
