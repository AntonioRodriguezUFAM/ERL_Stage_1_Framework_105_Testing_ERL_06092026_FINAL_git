//========================================================================================================
//SampleTestIntegrationCode_v33.cpp

//========================================================================================================



//================================================================================
//================================================================================
// SampleTestIntegrationCode_v31.cpp
//
// PhD Benchmark Matrix Harness
// Image Processing Algorithms × Control Modes
//
// Benchmarks:
//   1. SobelEdge
//   2. HistogramEqualization
//   3. HeterogeneousGaussianBlu
//   4. MedianFilter
//
// Control modes:
//   1. CPU_STATIC
//   2. GPU_STATIC
//   3. BALANCED
//   4. ERL
//
// Total: 4 algorithms × 4 modes = 16 benchmark cases
//
// Notes:
//   - ERL / GA parameters are placed inside the "Scheduler" block using ga_*
//     keys because ConfigManager passes jSched into EvolutionarySelector.
//   - GaussianBlur is CPU-only in AlgorithmConcrete.
//   - HistogramEqualization CPU fallback is grayscale, not true CPU HistEq.
//================================================================================
//================================================================================

#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO

#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

#include <mutex>
#include <cctype>

#include <experimental/filesystem>

namespace fs = std::experimental::filesystem;


#include "../nlohmann/json.hpp"
using json = nlohmann::json;

#include "Stage_01/SharedStructures/AlgorithmConfig.h"
#include "Stage_01/Concretes/CudaUtiles.h"
#include "Module/RuntimeControls.h"  // foundational ERL types first
#include "Module/ConfigManager.h"
#include "Module/Modules.h"
#include "Module/WorkloadMission.h"


//
// Forward declaration
//static void set_thermal_config(json& cfg, const BenchmarkCase& bc, double temp_offset);

//================================================================================
// Global shutdown
//================================================================================

static std::atomic<bool> g_shutdown_flag{false};
static std::mutex g_shutdown_mutex;
static std::condition_variable g_shutdown_cv;

static void signal_handler(int sig)
{
    g_shutdown_flag.store(true, std::memory_order_relaxed);
    g_shutdown_cv.notify_all();
    spdlog::warn("[SIGNAL] Shutdown signal received: {}", sig);
}

//================================================================================
// Utility helpers
//================================================================================

static json load_json_config(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open()) {
        spdlog::warn("[CONFIG] Cannot open '{}'. Using empty base config.", path);
        return json::object();
    }

    try {
        return json::parse(f);
    }
    catch (const std::exception& e) {
        spdlog::error("[CONFIG] Failed to parse '{}': {}", path, e.what());
        return json::object();
    }
}

static std::string sanitize_file_token(const std::string& s)
{
    std::string out = s;
    for (char& c : out) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) {
            c = '_';
        }
    }
    return out;
}

static std::string timestamp_token()
{
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);

    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y%m%d_%H%M%S");
    return oss.str();
}

//================================================================================
// Control modes
//================================================================================

enum class ControlMode {
    CPU_STATIC,
    GPU_STATIC,
    BALANCED,
    ERL
};

static const char* control_mode_to_string(ControlMode mode)
{
    switch (mode) {
        case ControlMode::CPU_STATIC: return "CPU_STATIC";
        case ControlMode::GPU_STATIC: return "GPU_STATIC";
        case ControlMode::BALANCED:   return "BALANCED";
        case ControlMode::ERL:        return "ERL";
    }
    return "UNKNOWN";
}

static bool parse_control_mode(const std::string& text, ControlMode& mode)
{
    if (text == "CPU_STATIC") {
        mode = ControlMode::CPU_STATIC;
        return true;
    }

    if (text == "GPU_STATIC") {
        mode = ControlMode::GPU_STATIC;
        return true;
    }

    if (text == "BALANCED") {
        mode = ControlMode::BALANCED;
        return true;
    }

    if (text == "ERL") {
        mode = ControlMode::ERL;
        return true;
    }

    return false;
}


//================================================================================
// Thermal-ablation profiles
//================================================================================
// Runtime-only experimental factor. These profiles change ONLY temperature
// contracts in ThermalGovernor + AdaptiveWeightManager. GA parameters, power
// threshold, latency SLA, scheduler policy, objective weights and workload
// mission remain unchanged.
//
// ThermalGovernor contracts:
//   ORIGINAL : 75 / 85 / 95 / 105 / 108 C
//   REVISED1 : 55 / 62 / 70 /  80 /  90 C
//   REVISED2 : 45 / 50 / 55 /  60 /  75 C
//
// AWM thermal contracts use the calibrated values already documented in
// AdaptiveWeightManager.h:
//   ORIGINAL : warn CPU/GPU 75/78 C, critical CPU/GPU 80/83 C
//   REVISED1 : warn CPU/GPU 57/57 C, critical CPU/GPU 63/63 C
//   REVISED2 : warn CPU/GPU 45/45 C, critical CPU/GPU 55/55 C
//
// IMPORTANT: ORIGINAL is an "application-governor inactive" control profile.
// It does NOT disable Jetson/Linux/native thermal protection.

enum class ThermalProfile {
    ORIGINAL,
    REVISED1,
    REVISED2
};

static const char* thermal_profile_to_string(ThermalProfile profile)
{
    switch (profile) {
        case ThermalProfile::ORIGINAL: return "ORIGINAL";
        case ThermalProfile::REVISED1: return "REVISED1";
        case ThermalProfile::REVISED2: return "REVISED2";
    }
    return "UNKNOWN";
}

static bool parse_thermal_profile(std::string text, ThermalProfile& profile)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    if (text == "ORIGINAL") {
        profile = ThermalProfile::ORIGINAL;
        return true;
    }
    if (text == "REVISED1" || text == "REVISED_1") {
        profile = ThermalProfile::REVISED1;
        return true;
    }
    if (text == "REVISED2" || text == "REVISED_2") {
        profile = ThermalProfile::REVISED2;
        return true;
    }
    return false;
}

struct ThermalContract {
    // ThermalGovernor
    double caution_c;
    double warning_c;
    double critical_c;
    double emergency_c;
    double shutdown_c;

    // AdaptiveWeightManager: temperature fields only
    double awm_warn_cpu_c;
    double awm_warn_gpu_c;
    double awm_crit_cpu_c;
    double awm_crit_gpu_c;
};

static ThermalContract thermal_contract(ThermalProfile profile)
{
    switch (profile) {
        case ThermalProfile::ORIGINAL:
            return ThermalContract{
                75.0, 85.0, 95.0, 105.0, 108.0,
                75.0, 78.0, 80.0, 83.0
            };

        case ThermalProfile::REVISED1:
            return ThermalContract{
                55.0, 62.0, 70.0, 80.0, 90.0,
                57.0, 57.0, 63.0, 63.0
            };

        case ThermalProfile::REVISED2:
        default:
            return ThermalContract{
                45.0, 50.0, 55.0, 60.0, 75.0,
                45.0, 45.0, 55.0, 55.0
            };
    }
}

static hrl::ThermalGovernor::Config
make_thermal_governor_config(ThermalProfile profile, double temperature_offset_c = 0.0)
{
    const ThermalContract c = thermal_contract(profile);

    hrl::ThermalGovernor::Config cfg;
    cfg.caution_c = c.caution_c;
    cfg.warning_c = c.warning_c;
    cfg.critical_c = c.critical_c;
    cfg.emergency_c = c.emergency_c;
    cfg.shutdown_c = c.shutdown_c;
    cfg.temperature_offset_c = temperature_offset_c;
    return cfg;
}

static void apply_thermal_profile(json& cfg,
                                  ThermalProfile profile,
                                  double temperature_offset_c = 0.0)
{
    if (!cfg.contains("ThermalGovernor") || !cfg["ThermalGovernor"].is_object()) {
        cfg["ThermalGovernor"] = json::object();
    }
    if (!cfg.contains("Scheduler") || !cfg["Scheduler"].is_object()) {
        cfg["Scheduler"] = json::object();
    }

    const ThermalContract c = thermal_contract(profile);
    auto& tg = cfg["ThermalGovernor"];
    auto& s  = cfg["Scheduler"];

    // ThermalGovernor: change temperature contracts only.
    tg["caution_c"] = c.caution_c;
    tg["warning_c"] = c.warning_c;
    tg["critical_c"] = c.critical_c;
    tg["emergency_c"] = c.emergency_c;
    tg["shutdown_c"] = c.shutdown_c;
    tg["temperature_offset_c"] = temperature_offset_c;

    // AWM: change temperature contracts only. Deliberately preserve
    // battery_critical_watts, latency_sla_ms, FPS threshold, smoothing, etc.
    s["awm_thermal_warn_cpu_c"] = c.awm_warn_cpu_c;
    s["awm_thermal_warn_gpu_c"] = c.awm_warn_gpu_c;
    s["awm_thermal_crit_cpu_c"] = c.awm_crit_cpu_c;
    s["awm_thermal_crit_gpu_c"] = c.awm_crit_gpu_c;

    // Metadata only; no controller reads this key.
    cfg["thermal_profile"] = thermal_profile_to_string(profile);

    spdlog::info(
        "[THERMAL-PROFILE] {} | Governor C/W/Crit/E/S={:.0f}/{:.0f}/{:.0f}/{:.0f}/{:.0f}C "
        "| AWM warn CPU/GPU={:.0f}/{:.0f}C crit CPU/GPU={:.0f}/{:.0f}C | offset={:.1f}C",
        thermal_profile_to_string(profile),
        c.caution_c, c.warning_c, c.critical_c, c.emergency_c, c.shutdown_c,
        c.awm_warn_cpu_c, c.awm_warn_gpu_c, c.awm_crit_cpu_c, c.awm_crit_gpu_c,
        temperature_offset_c);
}

//================================================================================
// Benchmark definitions
//================================================================================

struct AlgorithmBenchmark {
    hrl::AlgorithmType algoType;
    std::string label;
    bool gpuCapable;
    std::string fallbackNote;
};

struct BenchmarkCase {
    AlgorithmBenchmark algo;
    ControlMode mode;
    std::string suite;
    std::string testLabel;
    // Existing experimental offset support.
    double temperature_offset = 0.0;

    // Thermal-threshold ablation profile. REVISED2 preserves the current
    // config.json behaviour when --thermal-profile is not supplied.
    ThermalProfile thermal_profile = ThermalProfile::REVISED2;
};

//================================================================================
// Algorithm roster
//================================================================================

static const std::vector<AlgorithmBenchmark> kAlgorithms = {
    {
        hrl::AlgorithmType::SobelEdge,
        "SobelEdge",
        true,
        "CPU fallback = processEdgeDetectionZeroCopy"
    },
    {
        hrl::AlgorithmType::HistogramEqualization,
        "HistogramEqualization",
        true,
        "CPU fallback = processGrayscaleZeroCopy, not true CPU histogram equalisation"
    },
    {
        hrl::AlgorithmType::HeterogeneousGaussianBlur,
        "HeterogeneousGaussianBlur",
        true,
        "CPU fallback = processGaussianBlurZeroCopy"
    },
    {
        hrl::AlgorithmType::GaussianBlur,
        "GaussianBlur",
        false,
        "Always CPU: AlgorithmConcrete forces useGPU=false for GaussianBlur"
    },
     {
        hrl::AlgorithmType::MedianFilter,
        "MedianFilter",
        true,
        "CPU fallback = pass-through (memcpy) for Graceful Feature Degradation"
    }
};

static const std::vector<ControlMode> kModes = {
    ControlMode::CPU_STATIC,
    ControlMode::GPU_STATIC,
    ControlMode::BALANCED,
    ControlMode::ERL
};

static std::vector<BenchmarkCase> build_benchmark_matrix()
{
    std::vector<BenchmarkCase> matrix;
    matrix.reserve(kAlgorithms.size() * kModes.size());

    for (const auto& algo : kAlgorithms) {
        for (const auto mode : kModes) {
            BenchmarkCase bc;
            bc.algo = algo;
            bc.mode = mode;
            bc.suite = "ImgProc_" + algo.label;
            bc.testLabel = algo.label + "_" + control_mode_to_string(mode);
            matrix.push_back(std::move(bc));
        }
    }

    return matrix;
}


//==========================================================================================================
static void append_manifest_row(const BenchmarkCase& bc,
                                const std::string& metricsCsv,
                                const std::string& metricsJson,
                                double runSeconds,
                                double warmupSeconds)
{
    const std::string path = "output/benchmark_manifest.csv";
    const bool writeHeader = !std::ifstream(path).good();

    std::ofstream out(path, std::ios::app);
    if (!out.is_open()) {
        return;
    }

    if (writeHeader) {
        out << "timestamp,algorithm,mode,test_label,metrics_csv,metrics_json,"
            << "run_seconds,warmup_seconds,gpu_capable,fallback_note\n";
    }

    out << timestamp_token() << ","
        << bc.algo.label << ","
        << control_mode_to_string(bc.mode) << ","
        << bc.testLabel << ","
        << metricsCsv << ","
        << metricsJson << ","
        << runSeconds << ","
        << warmupSeconds << ","
        << (bc.algo.gpuCapable ? 1 : 0) << ","
        << "\"" << bc.algo.fallbackNote << "\""
        << "\n";
}


//==========================================================================================================

//================================================================================
// JSON config helpers
//================================================================================

static void ensure_section(json& cfg, const std::string& key)
{
    if (!cfg.contains(key) || !cfg[key].is_object()) {
        cfg[key] = json::object();
    }
}

// Helper 
static int initial_threads_for_mode(ControlMode mode)
{
    switch (mode) {
        case ControlMode::CPU_STATIC: return 4;
        case ControlMode::GPU_STATIC: return 1;
        case ControlMode::BALANCED:   return 2;
        case ControlMode::ERL:        return 2;
    }
    return 2;
}

static void ensure_required_sections(json& cfg)
{
    ensure_section(cfg, "Scheduler");
    ensure_section(cfg, "AlgorithmConfig");
    ensure_section(cfg, "Aggregator");
    ensure_section(cfg, "CameraConfig");
    ensure_section(cfg, "DisplayConfig");
    ensure_section(cfg, "SoCConfig");
    ensure_section(cfg, "LynsynMonitorConfig");
    ensure_section(cfg, "Profiling");
    ensure_section(cfg, "SystemBehavior");
}

static void set_output_paths(json& cfg, const BenchmarkCase& bc)
{
    const std::string stamp = timestamp_token();
    const std::string token = sanitize_file_token(bc.testLabel + "_" + stamp);


    const std::string metricsCsv  = "output/metrics_" + token + ".csv";
    const std::string metricsJson = "output/metrics_" + token + ".ndjson";

    // --- FIX 1: Create a distinct file path for the manual telemetry loop ---
    const std::string telemetryCsv = "output/telemetry_" + token + ".csv";
    cfg["telemetry_csv"] = telemetryCsv;

     // Preferred location
    cfg["Aggregator"]["metrics_csv"]  = metricsCsv;
    cfg["Aggregator"]["metrics_json"] = metricsJson;

    // Backward-compatible fallback
    cfg["metrics_csv"]  = metricsCsv;
    cfg["metrics_json"] = metricsJson;

    cfg["LynsynMonitorConfig"]["outputCSV"] =
        "output/lynsyn_" + token + ".csv";

    cfg["Profiling"]["metricsOutputFile"] =
        "output/perf_" + token + ".csv";

    const bool isERL = (bc.mode == ControlMode::ERL);

    cfg["Scheduler"]["export_pareto_evidence"] = isERL;

    if (isERL) {
        cfg["Scheduler"]["pareto_csv"] =
            "output/pareto_" + token + ".csv";

        cfg["Scheduler"]["action_csv"] =
            "output/actions_" + token + ".csv";

        // PhD timing evidence: controller/GA overhead is logged separately
        // from per-frame algorithm and end-to-end pipeline latency.
        cfg["Scheduler"]["erl_timing_csv"] =
            "output/erl_timing_" + token + ".csv";
    }
}

static void set_algorithm_config(json& cfg, const BenchmarkCase& bc)
{
    auto& a = cfg["AlgorithmConfig"];

    a["algorithmType"] = bc.algo.label;
    //a["concurrencyLevel"] = 4;
    a["concurrencyLevel"] = initial_threads_for_mode(bc.mode);
    a["blurRadius"] = 5;
    a["medianWindowSize"] = 5;

    switch (bc.mode) {
        case ControlMode::CPU_STATIC:
            a["useGPU"] = false;
            break;

        case ControlMode::GPU_STATIC:
            a["useGPU"] = bc.algo.gpuCapable;
            break;

        case ControlMode::BALANCED:
        case ControlMode::ERL:
            a["useGPU"] = bc.algo.gpuCapable;
            break;
    }
}

static void set_scheduler_config(json& cfg, const BenchmarkCase& bc)
{
    auto& s = cfg["Scheduler"];

    s["policy"] = "BALANCED";
    s["targetFPS"] = 30.0;
    s["powerBudgetW"] = 7.5;
    s["preferGPU"] = bc.algo.gpuCapable;
    s["control_interval_ms"] = 800;

    switch (bc.mode) {
        case ControlMode::CPU_STATIC:
            s["enabled"] = false;
            s["use_erl"] = false;
            break;

        case ControlMode::GPU_STATIC:
            s["enabled"] = false;
            s["use_erl"] = false;
            break;

        case ControlMode::BALANCED:
            // Keep BALANCED deterministic for thesis baseline.
            // RuntimeControls are pinned later in pin_static_controls().
            s["enabled"] = false;
            s["use_erl"] = false;
            break;

        case ControlMode::ERL:
            s["enabled"] = true;
            s["use_erl"] = true;

            // ERL / GA configuration ? must be in Scheduler block.
            s["ga_population_size"] = 8;
            s["ga_min_pop_size"] = 4;
            s["ga_elite_count"] = 2;
            s["ga_crossover_rate"] = 0.70;
            s["ga_mutation_rate"] = 0.15;
            s["ga_exploration_decay"] = 0.96;
            s["ga_reduction_start_gen"] = 10;
            s["ga_reduction_ratio"] = 0.60;

            // Adaptive weight manager.
            s["adaptive_weights_enabled"] = true;
            s["awm_update_interval_gens"] = 5;
            // [P0-F14] Threshold overrides removed. Single source of truth is
            // AdaptiveWeightManager::Config's Jetson-Nano-calibrated defaults
            // (warn 57C, crit 63C, battery 3.5W, latency SLA 30ms). The old
            // explicit 75/78/80/83C + 8.0W + 45ms values silently reverted
            // that calibration and made the THERMAL_* / BATTERY_CRITICAL
            // regimes unreachable on hardware that tops out at ~61-65C and
            // ~6.5W. Re-add awm_* keys here ONLY for explicit ablation runs,
            // and record them in the run manifest when you do.
            s["awm_fps_underperform_ratio"] = 0.70;
            s["awm_transition_smoothing"] = 0.30;
            break;
    }
}

//================================================================================
// Logging helpers
//================================================================================

static void log_case_config(const BenchmarkCase& bc, const json& cfg)
{
    const auto& s = cfg["Scheduler"];
    const auto& a = cfg["AlgorithmConfig"];

    spdlog::info("============================================================");
    spdlog::info("BENCHMARK CASE: {}", bc.testLabel);
    spdlog::info("============================================================");
    spdlog::info("Algorithm       : {}", bc.algo.label);
    spdlog::info("Mode            : {}", control_mode_to_string(bc.mode));
    spdlog::info("GPU Capable     : {}", bc.algo.gpuCapable ? "YES" : "NO");
    spdlog::info("Config useGPU   : {}", a.value("useGPU", false) ? "YES" : "NO");
    spdlog::info("Concurrency     : {}", a.value("concurrencyLevel", 0));
    spdlog::info("Scheduler       : {}", s.value("enabled", false) ? "ON" : "OFF");
    spdlog::info("ERL             : {}", s.value("use_erl", false) ? "ON" : "OFF");
    spdlog::info("Fallback note   : {}", bc.algo.fallbackNote);

    if (s.value("use_erl", false)) {
        spdlog::info("---- Effective ERL / GA configuration ----");
        spdlog::info("ga_population_size     = {}", s.value("ga_population_size", 12));
        spdlog::info("ga_min_pop_size        = {}", s.value("ga_min_pop_size", 6));
        spdlog::info("ga_elite_count         = {}", s.value("ga_elite_count", 3));
        spdlog::info("ga_crossover_rate      = {:.2f}", s.value("ga_crossover_rate", 0.75));
        spdlog::info("ga_mutation_rate       = {:.2f}", s.value("ga_mutation_rate", 0.18));
        spdlog::info("ga_exploration_decay   = {:.2f}", s.value("ga_exploration_decay", 0.96));
        spdlog::info("ga_reduction_start_gen = {}", s.value("ga_reduction_start_gen", 10));
        spdlog::info("ga_reduction_ratio     = {:.2f}", s.value("ga_reduction_ratio", 0.60));
        spdlog::info("pareto_csv             = {}", s.value("pareto_csv", std::string("N/A")));
        spdlog::info("action_csv             = {}", s.value("action_csv", std::string("N/A")));
        spdlog::info("erl_timing_csv         = {}", s.value("erl_timing_csv", std::string("N/A")));
    }

    spdlog::info("============================================================");
}

static void log_runtime_state(
    const BenchmarkCase& bc,
    const hrl::RuntimeControls& rt,
    const hrl::MetricsSnapshot& snap,
    double elapsedSec)
{
    const bool effectiveGpu =
        hrl::isCudaAvailable() &&
        rt.enable_gpu.load(std::memory_order_relaxed);

    const int threads =
        rt.concurrency_level.load(std::memory_order_relaxed);

    spdlog::info(
        "[RUNTIME] t={:.0f}s | {} | {} | GPU={} | Threads={} | "
        "FPS={:.1f} | Inf={:.2f}ms | Proc={:.2f}ms | E2E={:.2f}ms | "
        "Power={:.2f}W | J/F={:.4f} | CPU={:.0f}% | GPU={:.0f}% | "
        "Tcpu={:.1f}C | Tgpu={:.1f}C",
        elapsedSec,
        bc.algo.label,
        control_mode_to_string(bc.mode),
        effectiveGpu ? "ON" : "OFF",
        threads,
        snap.fps,
        snap.avg_inference_ms,
        snap.processing_latency_ms,
        snap.end_to_end_latency_ms,
        snap.avg_power_w_alg,
        snap.joules_per_frame,
        snap.cpu_util_avg,
        snap.gpu_util_avg,
        snap.cpu_temp_c,
        snap.gpu_temp_c
    );
}

static void log_case_summary(
    const BenchmarkCase& bc,
    const hrl::MetricsSnapshot& snap,
    double totalSec)
{
    spdlog::info("------------------------------------------------------------");
    spdlog::info("SUMMARY: {}", bc.testLabel);
    spdlog::info("------------------------------------------------------------");
    spdlog::info("Duration              : {:.1f} s", totalSec);
    spdlog::info("Final FPS             : {:.2f}", snap.fps);
    spdlog::info("Average inference     : {:.2f} ms", snap.avg_inference_ms);
    spdlog::info("Processing latency    : {:.2f} ms", snap.processing_latency_ms);
    spdlog::info("Display latency       : {:.2f} ms", snap.display_latency_ms);
    spdlog::info("End-to-end latency    : {:.2f} ms", snap.end_to_end_latency_ms);
    spdlog::info("Average power         : {:.2f} W", snap.avg_power_w_alg);
    spdlog::info("Joules per frame      : {:.4f}", snap.joules_per_frame);
    spdlog::info("CPU utilisation       : {:.1f} %", snap.cpu_util_avg);
    spdlog::info("GPU utilisation       : {:.1f} %", snap.gpu_util_avg);
    spdlog::info("CPU temperature       : {:.1f} C", snap.cpu_temp_c);
    spdlog::info("GPU temperature       : {:.1f} C", snap.gpu_temp_c);
    spdlog::info("Fallback note         : {}", bc.algo.fallbackNote);
    spdlog::info("------------------------------------------------------------");
}

//================================================================================
// Runtime control pinning for static modes
//================================================================================
static void pin_static_controls(const BenchmarkCase& bc,
                                const std::shared_ptr<hrl::RuntimeControls>& rt)
{
    if (!rt) {
        return;
    }

    switch (bc.mode) {
        case ControlMode::CPU_STATIC:
            rt->enable_gpu.store(false, std::memory_order_relaxed);
            rt->concurrency_level.store(4, std::memory_order_relaxed);
            rt->affinity.store(hrl::Affinity::Spread, std::memory_order_relaxed);
            break;

        case ControlMode::GPU_STATIC:
            rt->enable_gpu.store(
                bc.algo.gpuCapable && hrl::isCudaAvailable(),
                std::memory_order_relaxed
            );
            rt->concurrency_level.store(1, std::memory_order_relaxed);
            rt->affinity.store(hrl::Affinity::Pack, std::memory_order_relaxed);
            break;

        case ControlMode::BALANCED:
            rt->enable_gpu.store(
                bc.algo.gpuCapable && hrl::isCudaAvailable(),
                std::memory_order_relaxed
            );
            rt->concurrency_level.store(2, std::memory_order_relaxed);
            rt->affinity.store(hrl::Affinity::Spread, std::memory_order_relaxed);
            break;

        case ControlMode::ERL:
            // ERL starts from a neutral state. Scheduler/GA may adapt it.
            rt->enable_gpu.store(
                bc.algo.gpuCapable && hrl::isCudaAvailable(),
                std::memory_order_relaxed
            );
            rt->concurrency_level.store(2, std::memory_order_relaxed);
            rt->affinity.store(hrl::Affinity::Spread, std::memory_order_relaxed);

             //set_scheduler_config(cfg, bc);
             //set_thermal_config(cfg, bc, temp_offset);   // NEW
            
            break;
    }
}


// Adjust ERL objective weights to include thermal/power penalties.
// [P0-F13] The weights MUST be written into the "Scheduler" block:
// ConfigManager hands the selector only (Scheduler + EvolutionarySelector)
// sections, so the old root-level obj_weight_* keys were never read and
// --temp-offset was a silent no-op (every "thermal-weighted" run actually
// used the defaults 1.0/0.5/0.5/0.3). The selector logs its effective
// config at startup - use that line to identify affected historical runs.
static void set_thermal_config(json& cfg, const BenchmarkCase& bc, double temp_offset)
    {
        if (bc.mode != ControlMode::ERL) return;

        ensure_section(cfg, "Scheduler");
        auto& s = cfg["Scheduler"];

        // Base weights (defaults match EvolutionarySelector's fallbacks)
        double base_power = s.value("obj_weight_power", 0.5);
        double base_temp  = s.value("obj_weight_temp", 0.5);

        // Offset scaling: e.g., offset=0.0 -> no change, offset=1.0 -> double penalty
        double temp_factor   = 1.0 + temp_offset;
        double power_factor  = 1.0 + 0.5 * temp_offset;   // less aggressive

        s["obj_weight_temp"]  = base_temp * temp_factor;
        s["obj_weight_power"] = base_power * power_factor;
        // FPS and latency remain unchanged (they are performance objectives)

        spdlog::info("[THERMAL] temp_weight={:.2f}  power_weight={:.2f}  (offset={:.2f})",
                    s["obj_weight_temp"].get<double>(),
                    s["obj_weight_power"].get<double>(),
                    temp_offset);
    }


//================================================================================
// Single benchmark runner
//================================================================================

static int run_benchmark_case(
    const BenchmarkCase& bc,
    double runSeconds,
    const std::string& configPath)
{
    g_shutdown_flag.store(false, std::memory_order_relaxed);

    constexpr double warmupSeconds = 30.0;
    constexpr double loopIntervalSec = 0.8;
    constexpr double pinIntervalSec = 5.0;
    constexpr int logEveryN = 10;

    try {
        Context ctx{};
        ctx.shutdown_flag.store(false);
        ctx.pipelineReady.store(false);
        ctx.tm = std::make_shared<ThreadManager>();

        json cfg = load_json_config(configPath);
        ensure_required_sections(cfg);
        set_algorithm_config(cfg, bc);
        set_scheduler_config(cfg, bc);
        set_output_paths(cfg, bc);

        // Apply ERL thermal experimental controls.
        if (bc.mode == ControlMode::ERL) {
            // Existing optional objective-weight offset support.
            set_thermal_config(cfg, bc, bc.temperature_offset);

            // Thermal-ablation factor: temperature contracts only.
            apply_thermal_profile(cfg, bc.thermal_profile, 0.0);
        }

        log_case_config(bc, cfg);
        // ... then build ConfigManager, etc.
        
        // json cfg = load_json_config(configPath);
        // ensure_required_sections(cfg);
        // set_algorithm_config(cfg, bc);
        // set_scheduler_config(cfg, bc);
        // set_output_paths(cfg, bc);

        // log_case_config(bc, cfg);

        {
            ConfigManager manager(cfg, ctx);
            manager.buildPipeline();

            auto rt = manager.getRuntimeControls();

            if (rt) {
                pin_static_controls(bc, rt);

                if (bc.mode == ControlMode::BALANCED ||
                    bc.mode == ControlMode::ERL) {
                    rt->concurrency_level.store(2, std::memory_order_relaxed);
                    rt->enable_gpu.store(
                        bc.algo.gpuCapable && hrl::isCudaAvailable(),
                        std::memory_order_relaxed
                    );
                }
            }

            if (!manager.validateAll()) {
                spdlog::error("[BENCH] Validation failed for {}", bc.testLabel);
                return EXIT_FAILURE;
            }

            // manager.startAll();
            // spdlog::info("[BENCH] Started {}", bc.testLabel);

            // // --- FIX 2: Open the dedicated telemetry file, NOT the Aggregator's file ---
            // std::ofstream csv_file(cfg["telemetry_csv"].get<std::string>());
            // // 1. Create an instance of ThermalGovernor (State preservation fix applied)
            // hrl::ThermalGovernor thermalGovernor;

            // if (csv_file.is_open()) {
            //     csv_file << "timestamp_ms," << hrl::ThermalGovernor::getCsvHeader() << "\n";
            // }

            // auto runStart = std::chrono::steady_clock::now();

            // // CSV logging
            // std::ofstream csv_file(cfg["Aggregator"]["metrics_csv"].get<std::string>());

            // // --- FIX: Instantiate ThermalGovernor OUTSIDE the loop to preserve its state ---
            // //hrl::ThermalGovernor thermalGovernor;
            // // 1. Create an instance of ThermalGovernor
            // hrl::ThermalGovernor thermalGovernor;

            // if (csv_file.is_open()) {
            //     csv_file << "timestamp_ms," << hrl::ThermalGovernor::getCsvHeader() << "\n";
            // }

            // auto runStart = std::chrono::steady_clock::now();
            // auto lastPin = runStart;
            // bool warmupDone = false;
            // int loopCount = 0;

            // // Fail-fast camera guard: a healthy run must show frames flowing shortly
            // // after warmup. If the camera/algorithm delivers nothing (the all-zero
            // // GaussianBlur signature), abort within seconds instead of recording an
            // // empty 30-minute run. Common causes: CSI sensor_mode/60FPS config failed
            // // (DataConcrete strict-mode returned false) or Lynsyn USB left in a bad
            // // state by a prior run.
            // bool cameraConfirmed = false;
            // const double firstFrameDeadlineSec = warmupSeconds + 10.0;

            manager.startAll();
            spdlog::info("[BENCH] Started {}", bc.testLabel);

            // --- FIX 2: Open the dedicated telemetry file ---
            std::ofstream csv_file(cfg["telemetry_csv"].get<std::string>());
            
            // Instantiate the telemetry-only governor with the SAME runtime
            // thermal contract as the scheduler/ERL controller.
            hrl::ThermalGovernor thermalGovernor(
                make_thermal_governor_config(bc.thermal_profile, 0.0));

            if (csv_file.is_open()) {
                csv_file << "timestamp_ms," << hrl::ThermalGovernor::getCsvHeader() << "\n";
            }

            auto runStart = std::chrono::steady_clock::now();
            auto lastPin = runStart;
            bool warmupDone = false;
            int loopCount = 0;

            // Fail-fast camera guard...
            bool cameraConfirmed = false;
            const double firstFrameDeadlineSec = warmupSeconds + 10.0;
            // ... rest of your loop remains the same
            
            while (true) {
                auto now = std::chrono::steady_clock::now();
                double elapsed =
                    std::chrono::duration<double>(now - runStart).count();

                if (elapsed >= runSeconds) break;

                if (g_shutdown_flag.load(std::memory_order_relaxed)) {
                    spdlog::warn("[BENCH] Shutdown requested during {}", bc.testLabel);
                    break;
                }

                if (!warmupDone && elapsed >= warmupSeconds) {
                    warmupDone = true;
                    spdlog::info("============================================================");
                    spdlog::info("WARMUP COMPLETE ? measurement window begins now");
                    spdlog::info("Case: {}", bc.testLabel);
                    spdlog::info("Elapsed: {:.1f}s", elapsed);
                    spdlog::info("Exclude all data before this marker from thesis analysis.");
                    spdlog::info("============================================================");
                }

                // === Fail-fast camera guard ===
                // Confirm frames are flowing; abort the case if nothing arrives by the
                // deadline rather than burning the full run on an all-zero capture
                // (the GaussianBlur all-zero signature). Causes: CSI sensor_mode/60FPS
                // config failed (DataConcrete strict-mode returned false) or Lynsyn USB
                // left in a bad state by the previous run.
                if (!cameraConfirmed) {
                    auto guardSnap = manager.getLatestMetricsSnapshot();
                    // hrl::MetricsSnapshot is the flat snapshot: snap.fps already
                    // resolves to algorithm fps, falling back to camera fps, and
                    // snap.frameId tracks finalized frames. Either being non-zero
                    // means frames are flowing.
                    const bool framesFlowing =
                        guardSnap.frameId > 0 ||
                        guardSnap.fps > 0.5;

                    if (framesFlowing) {
                        cameraConfirmed = true;
                        spdlog::info("[BENCH] Camera confirmed: frames flowing "
                                     "(fps={:.1f}, frameId={})",
                                     guardSnap.fps, guardSnap.frameId);
                    } else if (elapsed >= firstFrameDeadlineSec) {
                        spdlog::critical(
                            "[BENCH] ABORT: no frames after {:.0f}s for {} "
                            "(fps={:.1f}, frameId={}). "
                            "Refusing to record an all-zero run. Check CSI sensor_mode=4 / "
                            "60FPS programming (DataConcrete strict-mode) and Lynsyn USB "
                            "reset state from the previous run.",
                            elapsed, bc.testLabel,
                            guardSnap.fps, guardSnap.frameId);

                        ctx.shutdown_flag.store(true, std::memory_order_relaxed);
                        manager.stopAll();
                        manager.flushMetrics();
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                        ctx.tm.reset();
                        return EXIT_FAILURE;
                    }
                }

                double sinceLastPin =
                    std::chrono::duration<double>(now - lastPin).count();

                // Static baselines are intentionally re-pinned. ERL is NOT:
                // after startup, RuntimeControls belong exclusively to the GA/Scheduler.
                if (bc.mode != ControlMode::ERL && sinceLastPin >= pinIntervalSec) {
                    pin_static_controls(bc, rt);
                    lastPin = now;
                }

                if (++loopCount % logEveryN == 0) {
                    auto snap = manager.getLatestMetricsSnapshot();
                    if (rt) {
                        log_runtime_state(bc, *rt, snap, elapsed);
                    }
                    // --- ADD TELEMETRY LOGGING HERE ---
                    if (csv_file.is_open()) {
                        auto current_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()
                        ).count();

                        // // 1. Create an instance of ThermalGovernor
                        // hrl::ThermalGovernor thermalGovernor;
                        auto policy = thermalGovernor.update(snap.cpu_temp_c, snap.gpu_temp_c);
                        (void)policy;   // or inspect policy.gpu_allowed / target_fps_scale

                        // 2. Call the method using the object
                        csv_file << current_time_ms << "," 
                                << thermalGovernor.formatCsvRecord(
                                        snap.fps, 
                                        snap.end_to_end_latency_ms, 
                                        snap.avg_power_w_alg, 
                                        snap.joules_per_frame, 
                                        snap.cpu_temp_c, 
                                        snap.gpu_temp_c) 
                                    << "\n";
                    }
                }

                std::this_thread::sleep_for(
                    std::chrono::milliseconds(
                        static_cast<int>(loopIntervalSec * 1000)
                    )
                );
            }

            auto finalSnap = manager.getLatestMetricsSnapshot();
            double totalElapsed =
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - runStart
                ).count();

            log_case_summary(bc, finalSnap, totalElapsed);

            spdlog::info("[BENCH] Stopping {}", bc.testLabel);
            ctx.shutdown_flag.store(true, std::memory_order_relaxed);
            manager.stopAll();
            manager.flushMetrics();
        }

        std::this_thread::sleep_for(std::chrono::seconds(2));
        ctx.tm.reset();

        spdlog::info("[BENCH] Completed {}", bc.testLabel);
        return EXIT_SUCCESS;
    }
    catch (const std::exception& e) {
        spdlog::critical("[BENCH] Critical failure in {}: {}", bc.testLabel, e.what());
        return EXIT_FAILURE;
    }
}

// // add thermal controls flags
// static void set_thermal_config(json& cfg, const BenchmarkCase& bc, double temp_offset = 0.0)
// {
//     ensure_section(cfg, "ThermalGovernor");   // or put under Scheduler / ERL

//     cfg["ThermalGovernor"]["temperature_offset_c"] = temp_offset;

//     if (std::abs(temp_offset) > 0.1) {
//         spdlog::warn("[TEST] Using temperature offset = {:.1f}°C for this run", temp_offset);
//     }
// }


//================================================================================
// Continuous ERL dynamic-adaptation mission
//================================================================================
// This is intentionally NOT four benchmark cases. The pipeline is constructed
// once, started once, and stopped once. At 300/600/900 s only AlgorithmConcrete
// is hot-swapped; Scheduler/GA/AWM/Surrogate/RuntimeControls keep object identity
// and accumulated state.

static const AlgorithmBenchmark* find_algorithm_benchmark(const std::string& label)
{
    for (const auto& a : kAlgorithms) {
        if (a.label == label) return &a;
    }
    return nullptr;
}

static double dynamic_workload_fps_max(const json& cfg, const std::string& label)
{
    // Optional per-workload override:
    // "DynamicAdaptation": { "fps_max": { "SobelEdge": 55.0, ... } }
    try {
        if (cfg.contains("DynamicAdaptation") && cfg["DynamicAdaptation"].is_object()) {
            const auto& d = cfg["DynamicAdaptation"];
            if (d.contains("fps_max") && d["fps_max"].is_object()) {
                const auto it = d["fps_max"].find(label);
                if (it != d["fps_max"].end() && it->is_number()) {
                    const double v = it->get<double>();
                    if (std::isfinite(v) && v > 0.0) return v;
                }
            }
        }
    } catch (...) {}

    // The camera is configured around a 60 FPS experimental ceiling, so 60 is
    // the safe default physics prior when no workload-specific value is supplied.
    return 60.0;
}

static hrl::WorkloadProfile profile_for_mission_label(
    const json& cfg,
    const std::string& label)
{
    const AlgorithmBenchmark* a = find_algorithm_benchmark(label);
    if (!a) {
        throw std::runtime_error(
            "[DYNAMIC] Mission workload '" + label + "' is not present in kAlgorithms");
    }
    return hrl::makeWorkloadProfile(
        label, a->gpuCapable, dynamic_workload_fps_max(cfg, label));
}

static int run_dynamic_adaptation_mission(
    const std::string& configPath,
    const std::string& missionSpec,
    double missionDurationSec,
    double warmupSeconds,
    ThermalProfile thermalProfile,
    double temperatureOffset)
{
    g_shutdown_flag.store(false, std::memory_order_relaxed);

    if (!std::isfinite(missionDurationSec) || missionDurationSec <= 0.0) {
        spdlog::critical("[DYNAMIC] mission duration must be > 0");
        return EXIT_FAILURE;
    }
    if (!std::isfinite(warmupSeconds) || warmupSeconds < 0.0) {
        spdlog::critical("[DYNAMIC] warmup must be >= 0");
        return EXIT_FAILURE;
    }

    hrl::WorkloadMission mission;
    try {
        mission = hrl::WorkloadMission::parse(missionSpec);
    } catch (const std::exception& e) {
        spdlog::critical("[DYNAMIC] Invalid mission: {}", e.what());
        return EXIT_FAILURE;
    }

    if (missionDurationSec <= mission.lastBoundarySec()) {
        spdlog::critical(
            "[DYNAMIC] mission duration {:.3f}s must be greater than last boundary {:.3f}s",
            missionDurationSec, mission.lastBoundarySec());
        return EXIT_FAILURE;
    }

    Context ctx{};
    ctx.shutdown_flag.store(false);
    ctx.pipelineReady.store(false);
    ctx.tm = std::make_shared<ThreadManager>();

    std::unique_ptr<ConfigManager> manager;
    bool pipelineStarted = false;

    try {
        json cfg = load_json_config(configPath);
        ensure_required_sections(cfg);

        // Validate every label before creating hardware resources. A typo must
        // fail before the 20-minute data-producing mission starts.
        for (const auto& phase : mission.phases()) {
            (void)profile_for_mission_label(cfg, phase.label);
        }

        const hrl::WorkloadProfile initialProfile =
            profile_for_mission_label(cfg, mission.initialLabel());
        const AlgorithmBenchmark* initialAlgo =
            find_algorithm_benchmark(initialProfile.label);

        BenchmarkCase initialCase;
        initialCase.algo = *initialAlgo;
        initialCase.mode = ControlMode::ERL;
        initialCase.suite = "DynamicAdaptation";
        initialCase.testLabel =
            std::string("DynamicAdaptation_ERL_THERMAL_") +
            thermal_profile_to_string(thermalProfile);
        initialCase.temperature_offset = temperatureOffset;
        initialCase.thermal_profile = thermalProfile;

        set_algorithm_config(cfg, initialCase);
        set_scheduler_config(cfg, initialCase);
        set_output_paths(cfg, initialCase);

        // Preserve the existing optional objective-weight offset mechanism, then
        // apply the selected threshold contract. For the thermal-profile ablation
        // use --temp-offset 0 so thresholds are the ONLY thermal factor changed.
        set_thermal_config(cfg, initialCase, temperatureOffset);
        apply_thermal_profile(cfg, thermalProfile, 0.0);

        // Dynamic mission warmup is governed by an explicit ERL evolution hold
        // below, not by a second timer inside EvolutionarySelector. This removes
        // timer skew: camera/CUDA/Lynsyn/telemetry settle while GA generation
        // remains exactly 0, then the hold is released at measured mission t=0.
        cfg["Scheduler"]["warmup_ms"] = 0;

        // Make initial workload capability/prior explicit in the same config used
        // to construct the single persistent ERL instance.
        cfg["AlgorithmConfig"]["algorithmType"] = initialProfile.label;
        cfg["AlgorithmConfig"]["useGPU"] = initialProfile.gpuCapable;
        cfg["Scheduler"]["gpu_split_applicable"] = initialProfile.gpuSplitApplicable;
        cfg["Scheduler"]["surrogate_fps_max"] = initialProfile.fpsMax;
        cfg["Scheduler"]["workload_id"] = initialProfile.workloadId;

        const std::string stamp = timestamp_token();
        const std::string thermalToken =
            sanitize_file_token(thermal_profile_to_string(thermalProfile));
        const std::string missionCsv =
            "output/dynamic_adaptation_trace_THERMAL_" + thermalToken + "_" + stamp + ".csv";
        const std::string transitionCsv =
            "output/workload_transitions_THERMAL_" + thermalToken + "_" + stamp + ".csv";
        const std::string surrogateCsv =
            "output/surrogate_by_workload_THERMAL_" + thermalToken + "_" + stamp + ".csv";
        const std::string awmHistoryCsv =
            "output/awm_history_dynamic_THERMAL_" + thermalToken + "_" + stamp + ".csv";

        spdlog::info("============================================================");
        spdlog::info("CONTINUOUS ERL DYNAMIC-ADAPTATION MISSION");
        spdlog::info("Warmup outside mission : {:.1f}s", warmupSeconds);
        spdlog::info("Measured mission       : {:.1f}s", missionDurationSec);
        spdlog::info("Mission spec           : {}", missionSpec);
        spdlog::info("Thermal profile        : {}", thermal_profile_to_string(thermalProfile));
        for (size_t i = 0; i < mission.phases().size(); ++i) {
            const auto& ph = mission.phases()[i];
            const auto profile = profile_for_mission_label(cfg, ph.label);
            spdlog::info(
                "  phase {} @ {:.1f}s -> id={} {} | GPU={} | split={} | fpsMax={:.1f}",
                i, ph.startSec, profile.workloadId, profile.label,
                profile.gpuCapable ? "YES" : "NO",
                profile.gpuSplitApplicable ? "continuous" : "binary",
                profile.fpsMax);
        }
        spdlog::info("Mission trace          : {}", missionCsv);
        spdlog::info("Transition proof       : {}", transitionCsv);
        spdlog::info("Surrogate memory       : {}", surrogateCsv);
        spdlog::info("AWM history            : {}", awmHistoryCsv);
        spdlog::info("============================================================");

        manager.reset(new ConfigManager(cfg, ctx));
        manager->buildPipeline();

        // Engage the GA hold BEFORE module threads start. The same camera,
        // algorithm, Lynsyn, telemetry and ERL objects then remain alive during
        // warmup, but no evolutionary generation is allowed before mission t=0.
        manager->pauseERLEvolutionForMissionWarmup();

        auto rt = manager->getRuntimeControls();

        // Neutral ERL bootstrap exactly once. There is NO periodic ERL re-pinning.
        if (rt) pin_static_controls(initialCase, rt);

        if (!manager->validateAll()) {
            spdlog::critical("[DYNAMIC] Pipeline validation failed");
            return EXIT_FAILURE;
        }

        std::ofstream missionOut(missionCsv, std::ios::out | std::ios::trunc);
        if (!missionOut.is_open()) {
            spdlog::critical("[DYNAMIC] Cannot open mission trace '{}'", missionCsv);
            return EXIT_FAILURE;
        }

        hrl::WorkloadEventLog eventLog;
        if (!eventLog.open(transitionCsv)) {
            return EXIT_FAILURE;
        }

        missionOut
            << "wall_time_ms,mission_elapsed_s,phase_index,workload_id,workload,workload_epoch,"
            << "seconds_since_transition,transition_in_progress,ga_generation,awm_regime,"
            << "surrogate_observations_current_workload,policy_mode,target_fps,power_budget_w,"
            << "prefer_gpu,runtime_enable_gpu,runtime_gpu_split,runtime_concurrency,"
            << "runtime_cpu_max_freq_khz,action_generation,algorithm_observed_generation,"
            << "active_algorithm_workload_epoch,algorithm_processed_workload_epoch,algorithm_processed_frame_id,"
            << "algorithm_starved,action_response_generation,action_response_ms,frame_id,fps,avg_inference_ms,"
            << "processing_latency_ms,display_latency_ms,end_to_end_latency_ms,power_w,"
            << "joules_per_frame,cpu_util_pct,gpu_util_pct,cpu_temp_c,gpu_temp_c\n";
        missionOut.flush();

        pipelineStarted = true; // enables exception cleanup even if startAll() fails mid-way
        manager->startAll(); // ONE pipeline start for warmup + all four phases.

        // ------------------------------------------------------------
        // Excluded warmup. Camera/algorithm/telemetry run continuously while
        // the explicit selector hold prevents hidden evolutionary generations.
        // ------------------------------------------------------------
        const auto warmupStart = std::chrono::steady_clock::now();
        while (!g_shutdown_flag.load(std::memory_order_relaxed) &&
               !ctx.shutdown_flag.load(std::memory_order_relaxed)) {
            const double warmElapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - warmupStart).count();
            if (warmElapsed >= warmupSeconds) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (g_shutdown_flag.load(std::memory_order_relaxed) ||
            ctx.shutdown_flag.load(std::memory_order_relaxed)) {
            throw std::runtime_error("[DYNAMIC] Shutdown requested during warmup");
        }

        const hrl::MetricsSnapshot warmSnap = manager->getLatestMetricsSnapshot();
        if (warmSnap.frameId == 0 && warmSnap.fps <= 0.5) {
            throw std::runtime_error(
                "[DYNAMIC] No frames after warmup; refusing an all-zero data-producing mission");
        }

        // Mission time starts only after warmup. Start the clock BEFORE releasing
        // the GA hold so every evolutionary decision belongs to the measured window.
        const auto missionStart = std::chrono::steady_clock::now();
        mission.start(missionStart);
        const size_t generationAtMissionStart =
            manager->resumeERLEvolutionAfterMissionWarmup();

        auto lastTelemetry = missionStart - std::chrono::seconds(1);
        double lastTransitionCommitSec = 0.0;
        bool failed = false;

        spdlog::info("============================================================");
        spdlog::info(
            "[DYNAMIC] WARMUP COMPLETE -> MISSION t=0 | workload id={} {} | GA generation={} | epoch={}",
            manager->getActiveERLWorkloadId(), manager->getActiveERLWorkload(),
            generationAtMissionStart, manager->getWorkloadEpoch());
        if (generationAtMissionStart != 0u) {
            spdlog::critical(
                "[DYNAMIC] INVALID RUN: expected generation 0 at mission start but observed {}. "
                "Refusing a mission with hidden pre-measurement ERL training.",
                generationAtMissionStart);
            throw std::runtime_error(
                "[DYNAMIC] ERL evolved before mission t=0 despite the warmup hold");
        }
        spdlog::info("============================================================");

        while (!g_shutdown_flag.load(std::memory_order_relaxed) &&
               !ctx.shutdown_flag.load(std::memory_order_relaxed)) {
            const auto now = std::chrono::steady_clock::now();
            const double elapsed = mission.elapsedSec(now);
            if (elapsed >= missionDurationSec) break;

            // Process every due boundary. confirmAdvanced() is called only after
            // a successful algorithm+ERL context commit.
            hrl::WorkloadMission::Phase nextPhase;
            while (mission.dueTransition(now, nextPhase)) {
                const hrl::WorkloadProfile nextProfile =
                    profile_for_mission_label(cfg, nextPhase.label);

                hrl::WorkloadTransitionEvidence ev;
                ev.missionElapsedSec = mission.elapsedSec();
                ev.phaseIndex = mission.currentPhaseIndex() + 1U;
                ev.fromWorkload = manager->getActiveERLWorkload();
                ev.toWorkload = nextProfile.label;
                ev.generationBefore = manager->getERLGeneration();
                ev.workloadEpochBefore = manager->getWorkloadEpoch();
                ev.surrogateObsBefore = manager->getCurrentWorkloadSurrogateObservations();
                ev.awmRegimeBefore = manager->getERLRegimeName();
                ev.frameIdBefore = manager->getLatestMetricsSnapshot().frameId;

                const auto switchStart = std::chrono::steady_clock::now();

                spdlog::info("============================================================");
                spdlog::info(
                    "[DYNAMIC] TRANSITION {} @ {:.3f}s | {} -> id={} {} | generation={} epoch={}",
                    ev.phaseIndex, ev.missionElapsedSec, ev.fromWorkload,
                    nextProfile.workloadId, nextProfile.label,
                    ev.generationBefore, ev.workloadEpochBefore);

                const bool ok = manager->switchAlgorithmWorkload(nextProfile);
                const auto switchEnd = std::chrono::steady_clock::now();
                ev.transitionTotalMs = std::chrono::duration<double, std::milli>(
                    switchEnd - switchStart).count();
                ev.swapGapMs = manager->getLastAlgorithmSwitchDowntimeMs();
                ev.inputFramesDrained = static_cast<uint64_t>(
                    manager->getLastAlgorithmInputDrainedFrames());
                ev.outputFramesDrained = static_cast<uint64_t>(
                    manager->getLastAlgorithmOutputDrainedFrames());

                // Overwrite the outer samples with the exact values captured
                // inside ConfigManager's begin/commit GA barrier.
                ev.generationBefore = manager->getLastTransitionGenerationBefore();
                ev.generationAfter  = manager->getLastTransitionGenerationAfter();
                ev.workloadEpochAfter = manager->getWorkloadEpoch();
                ev.surrogateObsAfter = manager->getCurrentWorkloadSurrogateObservations();
                ev.awmRegimeAfter = manager->getERLRegimeName();

                // Provenance-safe transition proof. `frame_id_after > before`
                // is NOT sufficient: the failed run showed an old Histogram frame
                // could finalize after Sobel was installed. Require a frame stamped
                // by the replacement AlgorithmConcrete under the committed workload
                // epoch, plus an aggregator snapshot that has reached that frame.
                hrl::MetricsSnapshot afterSnap = manager->getLatestMetricsSnapshot();
                const uint64_t expectedEpoch = ev.workloadEpochAfter;

                // Latch the FIRST replacement-worker provenance frame. Do not
                // chase algorithm_processed_frame_id while the algorithm keeps
                // advancing faster than display/aggregator finalization.
                uint64_t provenProcessedEpoch = 0ULL;
                uint64_t provenProcessedFrame = 0ULL;

                const auto frameDeadline = std::chrono::steady_clock::now() +
                                           std::chrono::seconds(5);
                while (ok &&
                       std::chrono::steady_clock::now() < frameDeadline &&
                       !g_shutdown_flag.load(std::memory_order_relaxed)) {
                    if (rt && provenProcessedFrame == 0ULL) {
                        // Epoch is the release/acquire publication marker; read it
                        // first, then read the frame payload it publishes.
                        const uint64_t observedEpoch =
                            rt->algorithm_processed_workload_epoch.load(std::memory_order_acquire);
                        const uint64_t observedFrame =
                            rt->algorithm_processed_frame_id.load(std::memory_order_relaxed);
                        if (observedEpoch == expectedEpoch &&
                            observedFrame > ev.frameIdBefore) {
                            provenProcessedEpoch = observedEpoch;
                            provenProcessedFrame = observedFrame;
                            ev.firstNewFrameLatencyMs =
                                std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - switchStart).count();
                            spdlog::info(
                                "[DYNAMIC] First replacement-workload frame proven: "
                                "epoch={} frame={} latency={:.3f}ms",
                                provenProcessedEpoch, provenProcessedFrame,
                                ev.firstNewFrameLatencyMs);
                        }
                    }

                    afterSnap = manager->getLatestMetricsSnapshot();
                    if (provenProcessedFrame > 0ULL &&
                        afterSnap.frameId >= provenProcessedFrame) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }

                ev.frameIdAfter = afterSnap.frameId;
                ev.algorithmProcessedEpochAfter = provenProcessedEpoch;
                ev.algorithmProcessedFrameAfter = provenProcessedFrame;
                eventLog.write(ev);

                if (!ok) {
                    spdlog::critical(
                        "[DYNAMIC] Transition failed. Mission phase is NOT advanced; aborting run.");
                    failed = true;
                    break;
                }

                // Fail closed on the experimental invariants. A run that violates
                // any of these may still be useful for debugging, but it must not
                // be accepted as thesis evidence for continual adaptation.
                if (ev.generationAfter != ev.generationBefore) {
                    spdlog::critical(
                        "[DYNAMIC] INVALID TRANSITION: GA generation changed inside barrier {} -> {}",
                        ev.generationBefore, ev.generationAfter);
                    failed = true;
                    break;
                }
                if (ev.workloadEpochAfter != ev.workloadEpochBefore + 1ULL) {
                    spdlog::critical(
                        "[DYNAMIC] INVALID TRANSITION: workload epoch did not advance exactly once {} -> {}",
                        ev.workloadEpochBefore, ev.workloadEpochAfter);
                    failed = true;
                    break;
                }
                if (ev.algorithmProcessedEpochAfter != ev.workloadEpochAfter ||
                    ev.algorithmProcessedFrameAfter <= ev.frameIdBefore ||
                    ev.frameIdAfter < ev.algorithmProcessedFrameAfter) {
                    spdlog::critical(
                        "[DYNAMIC] INVALID TRANSITION: no provenance-safe new-workload frame within 5 s "
                        "(before={} snapshot_after={} processed_after={} processed_epoch={} expected_epoch={})",
                        ev.frameIdBefore, ev.frameIdAfter, ev.algorithmProcessedFrameAfter,
                        ev.algorithmProcessedEpochAfter, ev.workloadEpochAfter);
                    failed = true;
                    break;
                }
                if (nextProfile.workloadId == initialProfile.workloadId &&
                    ev.phaseIndex > 0U && ev.surrogateObsAfter == 0ULL) {
                    spdlog::critical(
                        "[DYNAMIC] INVALID RECURRENCE: returned to {} but retained surrogate bank is empty",
                        nextProfile.label);
                    failed = true;
                    break;
                }

                mission.confirmAdvanced();
                lastTransitionCommitSec = mission.elapsedSec();

                spdlog::info(
                    "[DYNAMIC] COMMIT id={} {} | gen {} -> {} | epoch {} -> {} | "
                    "AWM {} -> {} | recalled surrogate observations={} | "
                    "transition={:.3f}ms algorithm_downtime={:.3f}ms "
                    "first_new_frame={:.3f}ms drained_in={} drained_out={}",
                    nextProfile.workloadId, nextProfile.label,
                    ev.generationBefore, ev.generationAfter,
                    ev.workloadEpochBefore, ev.workloadEpochAfter,
                    ev.awmRegimeBefore, ev.awmRegimeAfter,
                    ev.surrogateObsAfter, ev.transitionTotalMs, ev.swapGapMs,
                    ev.firstNewFrameLatencyMs, ev.inputFramesDrained,
                    ev.outputFramesDrained);
                spdlog::info("============================================================");
            }

            if (failed) break;

            // A five-second algorithm-input starvation invalidates a controlled
            // thesis replicate even though the worker remains alive and recoverable
            // for normal application operation. Fail closed before ERL can learn
            // from any camera-only/fallback metric stream.
            if (manager->algorithmStarved()) {
                spdlog::critical(
                    "[DYNAMIC] INVALID RUN: active algorithm starved for >=5 s; "
                    "aborting replicate before further ERL learning");
                failed = true;
                break;
            }

            // 2 Hz mission trace: this is the cross-log proof stream. Frame-level
            // metrics, action/Pareto evidence and Lynsyn remain in their own files.
            if (std::chrono::duration<double>(now - lastTelemetry).count() >= 0.5) {
                lastTelemetry = now;
                const hrl::MetricsSnapshot snap = manager->getLatestMetricsSnapshot();
                const hrl::RLAction action = manager->getLastRLAction();
                const uint64_t responseNs = rt
                    ? rt->action_response_latency_ns.load(std::memory_order_relaxed) : 0ULL;
                const int64_t wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();

                missionOut
                    << wallMs << ',' << elapsed << ',' << mission.currentPhaseIndex() << ','
                    << manager->getActiveERLWorkloadId() << ','
                    << manager->getActiveERLWorkload() << ',' << manager->getWorkloadEpoch() << ','
                    << (elapsed - lastTransitionCommitSec) << ','
                    << (manager->isWorkloadTransitionInProgress() ? 1 : 0) << ','
                    << manager->getERLGeneration() << ',' << manager->getERLRegimeName() << ','
                    << manager->getCurrentWorkloadSurrogateObservations() << ','
                    << static_cast<int>(action.mode) << ','
                    << (action.target_fps.has ? action.target_fps.value : -1.0) << ','
                    << (action.power_budget_watts.has ? action.power_budget_watts.value : -1.0) << ','
                    << (action.prefer_gpu.has ? (action.prefer_gpu.value ? 1 : 0) : -1) << ','
                    << (rt ? (rt->enable_gpu.load(std::memory_order_relaxed) ? 1 : 0) : -1) << ','
                    << (rt ? rt->gpu_workload_split.load(std::memory_order_relaxed) : -1.0) << ','
                    << (rt ? rt->concurrency_level.load(std::memory_order_relaxed) : -1) << ','
                    << (rt ? rt->commanded_cpu_max_freq_khz.load(std::memory_order_relaxed) : 0L) << ','
                    << (rt ? rt->action_generation.load(std::memory_order_relaxed) : 0ULL) << ','
                    << (rt ? rt->algorithm_observed_generation.load(std::memory_order_relaxed) : 0ULL) << ','
                    << (rt ? rt->active_workload_epoch.load(std::memory_order_relaxed) : 0ULL) << ','
                    << (rt ? rt->algorithm_processed_workload_epoch.load(std::memory_order_relaxed) : 0ULL) << ','
                    << (rt ? rt->algorithm_processed_frame_id.load(std::memory_order_relaxed) : 0ULL) << ','
                    << (manager->algorithmStarved() ? 1 : 0) << ','
                    << (rt ? rt->action_response_generation.load(std::memory_order_relaxed) : 0ULL) << ','
                    << (responseNs > 0 ? static_cast<double>(responseNs) / 1.0e6 : -1.0) << ','
                    << snap.frameId << ',' << snap.fps << ',' << snap.avg_inference_ms << ','
                    << snap.processing_latency_ms << ',' << snap.display_latency_ms << ','
                    << snap.end_to_end_latency_ms << ',' << snap.avg_power_w_alg << ','
                    << snap.joules_per_frame << ',' << snap.cpu_util_avg << ','
                    << snap.gpu_util_avg << ',' << snap.cpu_temp_c << ',' << snap.gpu_temp_c
                    << '\n';
                missionOut.flush();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        const hrl::MetricsSnapshot finalSnap = manager->getLatestMetricsSnapshot();
        spdlog::info(
            "[DYNAMIC] Mission ending | active id={} {} | epoch={} | generation={} | "
            "AWM={} | FPS={:.2f} | Power={:.2f}W",
            manager->getActiveERLWorkloadId(), manager->getActiveERLWorkload(),
            manager->getWorkloadEpoch(), manager->getERLGeneration(),
            manager->getERLRegimeName(), finalSnap.fps, finalSnap.avg_power_w_alg);

        // Export persistent controller memory BEFORE shutdown. The surrogate CSV
        // contains all workloadId banks; AWM history is one uninterrupted history.
        {
            std::ofstream out(surrogateCsv, std::ios::out | std::ios::trunc);
            if (out.is_open()) out << manager->exportERLSurrogateCSV();
        }
        {
            std::ofstream out(awmHistoryCsv, std::ios::out | std::ios::trunc);
            if (out.is_open()) out << manager->exportERLWeightHistoryCSV();
        }

        // ONE shutdown at the end of the complete mission.
        ctx.shutdown_flag.store(true, std::memory_order_relaxed);
        manager->stopAll();
        manager->flushMetrics();
        pipelineStarted = false;
        ctx.tm.reset();

        return failed ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    catch (const std::exception& e) {
        spdlog::critical("[DYNAMIC] Critical failure: {}", e.what());
    }
    catch (...) {
        spdlog::critical("[DYNAMIC] Unknown critical failure");
    }

    // Exception-safe single shutdown path.
    ctx.shutdown_flag.store(true, std::memory_order_relaxed);
    if (manager && pipelineStarted) {
        try { manager->stopAll(); } catch (...) {}
        try { manager->flushMetrics(); } catch (...) {}
    }
    ctx.tm.reset();
    return EXIT_FAILURE;
}

//================================================================================
// Main
//================================================================================

int main(int argc, char* argv[])
{
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

    std::system("mkdir -p output");

    hrl::detectCudaAvailability();

    spdlog::info("============================================================");
    spdlog::info("PhD Benchmark Harness v32 + Provenance-Safe Hot Swap");
    spdlog::info("5 image-processing algorithms × 4 control modes");
    spdlog::info("CUDA Available: {}", hrl::cudaAvailabilityString());
    spdlog::info("============================================================");

    bool runMatrix = false;
    bool runDynamicAdaptation = false;
    bool dynamicMissionExplicit = false;
    bool dynamicPhaseSecondsExplicit = false;
    double dynamicPhaseSeconds = 300.0;
    double dynamicDurationSeconds = 1200.0;
    double dynamicWarmupSeconds = 30.0;
    std::string dynamicMissionSpec =
        "0:HistogramEqualization,300:SobelEdge,"
        "600:HeterogeneousGaussianBlur,900:HistogramEqualization";
    double runSeconds = 1800.0;
    double cooldownSeconds = 30.0;
    std::string configPath = "config.json";
    std::string singleAlgo;
    std::string singleMode;

    // Thermal threshold ablation. Current config.json corresponds to REVISED2.
    ThermalProfile thermalProfile = ThermalProfile::REVISED2;

    // temp offset
    // 1. Add a temperature_offset field to BenchmarkCase
    // ERL thermal penalty weight offset (fractional scale, e.g. 0.0 to 1.0)
    double temperature_offset = 0.0; // offset the temerature to simulated higher temp values
    //double temp_offset = 20.0;


    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--temp-offset" && i+1 < argc) {
            // [P0-F13] Consume the value and continue: previously this fell
            // through to the option chain below and logged a spurious
            // "[ARGS] Ignored argument '--temp-offset'". Also guard stod so a
            // malformed value degrades to 0.0 instead of terminating.
            try {
                temperature_offset = std::stod(argv[++i]);
            } catch (const std::exception&) {
                spdlog::warn("[ARGS] Invalid --temp-offset value '{}'; using 0.0", argv[i]);
                temperature_offset = 0.0;
            }
            continue;
        }

        if (arg == "--thermal-profile" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (!parse_thermal_profile(value, thermalProfile)) {
                spdlog::error(
                    "[ARGS] Unknown --thermal-profile '{}'. "
                    "Use ORIGINAL, REVISED1, or REVISED2.", value);
                return EXIT_FAILURE;
            }
            continue;
        }

        if (arg == "--dynamic-adaptation" || arg == "--erl-dynamic-mission") {
            runDynamicAdaptation = true;
        }
        else if (arg == "--phase-seconds" && i + 1 < argc) {
            try {
                dynamicPhaseSeconds = std::stod(argv[++i]);
                dynamicPhaseSecondsExplicit = true;
            } catch (...) {
                spdlog::error("[ARGS] Invalid --phase-seconds value");
                return EXIT_FAILURE;
            }
        }
        else if (arg == "--mission" && i + 1 < argc) {
            dynamicMissionSpec = argv[++i];
            dynamicMissionExplicit = true;
        }
        else if (arg == "--dynamic-duration" && i + 1 < argc) {
            try { dynamicDurationSeconds = std::stod(argv[++i]); }
            catch (...) {
                spdlog::error("[ARGS] Invalid --dynamic-duration value");
                return EXIT_FAILURE;
            }
        }
        else if (arg == "--dynamic-warmup" && i + 1 < argc) {
            try { dynamicWarmupSeconds = std::stod(argv[++i]); }
            catch (...) {
                spdlog::error("[ARGS] Invalid --dynamic-warmup value");
                return EXIT_FAILURE;
            }
        }
        else if (arg == "--phd-matrix" || arg == "--benchmark-matrix") {
            runMatrix = true;
        }
        else if (arg == "--algo" && i + 1 < argc) {
            singleAlgo = argv[++i];
        }
        else if (arg == "--mode" && i + 1 < argc) {
            singleMode = argv[++i];
        }
        else if (arg == "--cooldown" && i + 1 < argc) {
            cooldownSeconds = std::stod(argv[++i]);
        }
        else if (arg.size() >= 5 &&
                 arg.substr(arg.size() - 5) == ".json") {
            configPath = arg;
        }
        else {
            try {
                runSeconds = std::stod(arg);
            }
            catch (...) {
                spdlog::warn("[ARGS] Ignored argument '{}'", arg);
            }
        }
    }

    if (runDynamicAdaptation) {
        // --phase-seconds is a convenient smoke-test shorthand for the canonical
        // four-phase mission. An explicit --mission always wins.
        if (dynamicPhaseSecondsExplicit && !dynamicMissionExplicit) {
            if (!std::isfinite(dynamicPhaseSeconds) || dynamicPhaseSeconds <= 0.0) {
                spdlog::error("[DYNAMIC] --phase-seconds must be > 0");
                return EXIT_FAILURE;
            }
            std::ostringstream spec;
            spec << "0:HistogramEqualization,"
                 << dynamicPhaseSeconds << ":SobelEdge,"
                 << 2.0 * dynamicPhaseSeconds << ":HeterogeneousGaussianBlur,"
                 << 3.0 * dynamicPhaseSeconds << ":HistogramEqualization";
            dynamicMissionSpec = spec.str();
            dynamicDurationSeconds = 4.0 * dynamicPhaseSeconds;
        }

        spdlog::info(
            "[DYNAMIC] Launching ONE continuous ERL instance | thermal={} | warmup={:.1f}s | "
            "mission={:.1f}s | schedule={}",
            thermal_profile_to_string(thermalProfile),
            dynamicWarmupSeconds, dynamicDurationSeconds, dynamicMissionSpec);

        return run_dynamic_adaptation_mission(
            configPath, dynamicMissionSpec, dynamicDurationSeconds,
            dynamicWarmupSeconds, thermalProfile, temperature_offset);
    }

    if (runMatrix) {
        const auto matrix = build_benchmark_matrix();

        spdlog::info("============================================================");
        spdlog::info("Running full PhD matrix");
        spdlog::info("Cases: {}", matrix.size());
        spdlog::info("Duration per case: {:.1f}s", runSeconds);
        spdlog::info("Cooldown between cases: {:.1f}s", cooldownSeconds);
        spdlog::info("============================================================");

        for (size_t i = 0; i < matrix.size(); ++i) {
            spdlog::info("[{:02d}/{}] {}",
                         static_cast<int>(i + 1),
                         matrix.size(),
                         matrix[i].testLabel);
        }

        int passed = 0;
        int failed = 0;

        for (size_t i = 0; i < matrix.size(); ++i) {
            if (g_shutdown_flag.load(std::memory_order_relaxed)) {
                spdlog::warn("[MATRIX] Shutdown requested before next case.");
                break;
            }

            const auto& bc = matrix[i];

            // --- Copy and set the temperature offset ---
            BenchmarkCase bc_with_offset = bc;
            bc_with_offset.temperature_offset = temperature_offset;
            bc_with_offset.thermal_profile = thermalProfile;

            spdlog::info("============================================================");
            spdlog::info("MATRIX PROGRESS [{}/{}]: {}",
                         i + 1,
                         matrix.size(),
                         bc.testLabel);
            spdlog::info("============================================================");

            //int rc = run_benchmark_case(bc, runSeconds, configPath);
            int rc = run_benchmark_case(bc_with_offset, runSeconds, configPath);

            if (rc == EXIT_SUCCESS) {
                ++passed;
            }
            else {
                ++failed;
            }

            if (i + 1 < matrix.size()) {
                spdlog::info("[MATRIX] Cooldown {:.1f}s before next case...",
                             cooldownSeconds);

                auto coolStart = std::chrono::steady_clock::now();

                while (std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - coolStart
                       ).count() < cooldownSeconds) {
                    if (g_shutdown_flag.load(std::memory_order_relaxed)) break;
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }
        }

        spdlog::info("============================================================");
        spdlog::info("MATRIX COMPLETE");
        spdlog::info("Passed: {}", passed);
        spdlog::info("Failed: {}", failed);
        spdlog::info("Total : {}", passed + failed);
        spdlog::info("============================================================");

        return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (!singleAlgo.empty() && !singleMode.empty()) {
        const AlgorithmBenchmark* algo = nullptr;

        for (const auto& a : kAlgorithms) {
            if (a.label == singleAlgo) {
                algo = &a;
                break;
            }
        }

        if (!algo) {
            spdlog::error("Unknown algorithm '{}'", singleAlgo);
            spdlog::error("Available algorithms:");
            for (const auto& a : kAlgorithms) {
                spdlog::error("  - {}", a.label);
            }
            return EXIT_FAILURE;
        }

        ControlMode mode;
        if (!parse_control_mode(singleMode, mode)) {
            spdlog::error("Unknown mode '{}'", singleMode);
            spdlog::error("Available modes: CPU_STATIC, GPU_STATIC, BALANCED, ERL");
            return EXIT_FAILURE;
        }

        BenchmarkCase bc;
        bc.algo = *algo;
        bc.mode = mode;
        bc.suite = "Single_" + algo->label;
        bc.testLabel = algo->label + "_" + control_mode_to_string(mode);
        if (mode == ControlMode::ERL) {
            bc.testLabel += std::string("_THERMAL_") +
                            thermal_profile_to_string(thermalProfile);
        }
        bc.temperature_offset = temperature_offset;
        bc.thermal_profile = thermalProfile;
        return run_benchmark_case(bc, runSeconds, configPath);
    }

    spdlog::info("Usage:");
    spdlog::info("  Continuous ERL dynamic-adaptation mission (thesis experiment):");
    spdlog::info("    --thermal-profile ORIGINAL|REVISED1|REVISED2");
    spdlog::info("    ./MySystem --dynamic-adaptation --thermal-profile REVISED2 config.json");
    spdlog::info("      # 30s warmup, then 0/300/600/900 boundaries, stop at mission t=1200s");
    spdlog::info("    ./MySystem --dynamic-adaptation --phase-seconds 30 --dynamic-warmup 5 config.json");
    spdlog::info("      # smoke test: 4 x 30s phases after a 5s excluded warmup");
    spdlog::info("    ./MySystem --dynamic-adaptation --mission \"0:HistogramEqualization,10:SobelEdge,20:HeterogeneousGaussianBlur,30:HistogramEqualization\" --dynamic-duration 40 --dynamic-warmup 5 config.json");
    spdlog::info("");
    spdlog::info("  Full matrix:");
    spdlog::info("    ./MySystem --phd-matrix 1800 config.json");
    spdlog::info("    ./MySystem --phd-matrix 1800 --cooldown 60 config.json");
    spdlog::info("");
    spdlog::info("  Single case:");
    spdlog::info("    ./MySystem --algo SobelEdge --mode ERL 300 config.json");
    spdlog::info("    ./MySystem --algo GaussianBlur --mode CPU_STATIC 60 config.json");
    spdlog::info("");
    spdlog::info("Algorithms:");
    for (const auto& a : kAlgorithms) {
        spdlog::info("  - {} | GPU={} | {}",
                     a.label,
                     a.gpuCapable ? "YES" : "NO",
                     a.fallbackNote);
    }
    spdlog::info("");
    spdlog::info("Modes:");
    spdlog::info("  - CPU_STATIC");
    spdlog::info("  - GPU_STATIC");
    spdlog::info("  - BALANCED");
    spdlog::info("  - ERL");

    return EXIT_SUCCESS;
}

// //SampleTestIntegrationCode_v30.cpp

// //================================================================================
// // SampleTestIntegrationCode_v30.cpp
// //
// // PhD Benchmark Matrix Harness
// // Image Processing Algorithms × Control Modes
// //
// // Benchmarks:
// //   1. SobelEdge
// //   2. HistogramEqualization
// //   3. HeterogeneousGaussianBlur
// //   4. GaussianBlur
// //   5. MedianFilter
// //
// // Control modes:
// //   1. CPU_STATIC
// //   2. GPU_STATIC
// //   3. BALANCED
// //   4. ERL
// //
// // Total: 5 algorithms × 4 modes = 20 benchmark cases
// //
// // Notes:
// //   - ERL / GA parameters are placed inside the "Scheduler" block using ga_*
// //     keys because ConfigManager passes jSched into EvolutionarySelector.
// //   - GaussianBlur is CPU-only in AlgorithmConcrete.
// //   - HistogramEqualization CPU fallback is grayscale, not true CPU HistEq.
// //================================================================================

// #define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO

// #include <spdlog/spdlog.h>
// #include <spdlog/fmt/ostr.h>

// #include <atomic>
// #include <chrono>
#include <cmath>
// #include <condition_variable>
// #include <csignal>
// #include <cstdlib>
// #include <ctime>
// #include <exception>
// #include <fstream>
// #include <iomanip>
// #include <memory>
// #include <sstream>
// #include <string>
// #include <thread>
// #include <vector>
// #include <algorithm>

// #include <mutex>
// #include <cctype>

// #include "../nlohmann/json.hpp"
// using json = nlohmann::json;

// #include "Stage_01/SharedStructures/AlgorithmConfig.h"
// #include "Stage_01/Concretes/CudaUtiles.h"
// #include "Module/ConfigManager.h"
// #include "Module/Modules.h"
// #include "Module/RuntimeControls.h"


// //
// // Forward declaration
// //static void set_thermal_config(json& cfg, const BenchmarkCase& bc, double temp_offset);

// //================================================================================
// // Global shutdown
// //================================================================================

// static std::atomic<bool> g_shutdown_flag{false};
// static std::mutex g_shutdown_mutex;
// static std::condition_variable g_shutdown_cv;

// static void signal_handler(int sig)
// {
//     g_shutdown_flag.store(true, std::memory_order_relaxed);
//     g_shutdown_cv.notify_all();
//     spdlog::warn("[SIGNAL] Shutdown signal received: {}", sig);
// }

// //================================================================================
// // Utility helpers
// //================================================================================

// static json load_json_config(const std::string& path)
// {
//     std::ifstream f(path);
//     if (!f.is_open()) {
//         spdlog::warn("[CONFIG] Cannot open '{}'. Using empty base config.", path);
//         return json::object();
//     }

//     try {
//         return json::parse(f);
//     }
//     catch (const std::exception& e) {
//         spdlog::error("[CONFIG] Failed to parse '{}': {}", path, e.what());
//         return json::object();
//     }
// }

// static std::string sanitize_file_token(const std::string& s)
// {
//     std::string out = s;
//     for (char& c : out) {
//         if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) {
//             c = '_';
//         }
//     }
//     return out;
// }

// static std::string timestamp_token()
// {
//     auto now = std::chrono::system_clock::now();
//     auto t = std::chrono::system_clock::to_time_t(now);

//     std::tm tm_buf{};
//     localtime_r(&t, &tm_buf);

//     std::ostringstream oss;
//     oss << std::put_time(&tm_buf, "%Y%m%d_%H%M%S");
//     return oss.str();
// }

// //================================================================================
// // Control modes
// //================================================================================

// enum class ControlMode {
//     CPU_STATIC,
//     GPU_STATIC,
//     BALANCED,
//     ERL
// };

// static const char* control_mode_to_string(ControlMode mode)
// {
//     switch (mode) {
//         case ControlMode::CPU_STATIC: return "CPU_STATIC";
//         case ControlMode::GPU_STATIC: return "GPU_STATIC";
//         case ControlMode::BALANCED:   return "BALANCED";
//         case ControlMode::ERL:        return "ERL";
//     }
//     return "UNKNOWN";
// }

// static bool parse_control_mode(const std::string& text, ControlMode& mode)
// {
//     if (text == "CPU_STATIC") {
//         mode = ControlMode::CPU_STATIC;
//         return true;
//     }

//     if (text == "GPU_STATIC") {
//         mode = ControlMode::GPU_STATIC;
//         return true;
//     }

//     if (text == "BALANCED") {
//         mode = ControlMode::BALANCED;
//         return true;
//     }

//     if (text == "ERL") {
//         mode = ControlMode::ERL;
//         return true;
//     }

//     return false;
// }

// //================================================================================
// // Benchmark definitions
// //================================================================================

// struct AlgorithmBenchmark {
//     hrl::AlgorithmType algoType;
//     std::string label;
//     bool gpuCapable;
//     std::string fallbackNote;
// };

// struct BenchmarkCase {
//     AlgorithmBenchmark algo;
//     ControlMode mode;
//     std::string suite;
//     std::string testLabel;
//     // 1. Add a temperature_offset field to BenchmarkCase
//     double temperature_offset = 0.0;   // NEW: thermal penalty offset
// };

// //================================================================================
// // Algorithm roster
// //================================================================================

// static const std::vector<AlgorithmBenchmark> kAlgorithms = {
//     {
//         hrl::AlgorithmType::SobelEdge,
//         "SobelEdge",
//         true,
//         "CPU fallback = processEdgeDetectionZeroCopy"
//     },
//     {
//         hrl::AlgorithmType::HistogramEqualization,
//         "HistogramEqualization",
//         true,
//         "CPU fallback = processGrayscaleZeroCopy, not true CPU histogram equalisation"
//     },
//     {
//         hrl::AlgorithmType::HeterogeneousGaussianBlur,
//         "HeterogeneousGaussianBlur",
//         true,
//         "CPU fallback = processGaussianBlurZeroCopy"
//     },
//     {
//         hrl::AlgorithmType::GaussianBlur,
//         "GaussianBlur",
//         false,
//         "Always CPU: AlgorithmConcrete forces useGPU=false for GaussianBlur"
//     },
//      {
//         hrl::AlgorithmType::MedianFilter,
//         "MedianFilter",
//         true,
//         "CPU fallback = pass-through (memcpy) for Graceful Feature Degradation"
//     }
// };

// static const std::vector<ControlMode> kModes = {
//     ControlMode::CPU_STATIC,
//     ControlMode::GPU_STATIC,
//     ControlMode::BALANCED,
//     ControlMode::ERL
// };

// static std::vector<BenchmarkCase> build_benchmark_matrix()
// {
//     std::vector<BenchmarkCase> matrix;
//     matrix.reserve(kAlgorithms.size() * kModes.size());

//     for (const auto& algo : kAlgorithms) {
//         for (const auto mode : kModes) {
//             BenchmarkCase bc;
//             bc.algo = algo;
//             bc.mode = mode;
//             bc.suite = "ImgProc_" + algo.label;
//             bc.testLabel = algo.label + "_" + control_mode_to_string(mode);
//             matrix.push_back(std::move(bc));
//         }
//     }

//     return matrix;
// }


// //==========================================================================================================
// static void append_manifest_row(const BenchmarkCase& bc,
//                                 const std::string& metricsCsv,
//                                 const std::string& metricsJson,
//                                 double runSeconds,
//                                 double warmupSeconds)
// {
//     const std::string path = "output/benchmark_manifest.csv";
//     const bool writeHeader = !std::ifstream(path).good();

//     std::ofstream out(path, std::ios::app);
//     if (!out.is_open()) {
//         return;
//     }

//     if (writeHeader) {
//         out << "timestamp,algorithm,mode,test_label,metrics_csv,metrics_json,"
//             << "run_seconds,warmup_seconds,gpu_capable,fallback_note\n";
//     }

//     out << timestamp_token() << ","
//         << bc.algo.label << ","
//         << control_mode_to_string(bc.mode) << ","
//         << bc.testLabel << ","
//         << metricsCsv << ","
//         << metricsJson << ","
//         << runSeconds << ","
//         << warmupSeconds << ","
//         << (bc.algo.gpuCapable ? 1 : 0) << ","
//         << "\"" << bc.algo.fallbackNote << "\""
//         << "\n";
// }


// //==========================================================================================================

// //================================================================================
// // JSON config helpers
// //================================================================================

// static void ensure_section(json& cfg, const std::string& key)
// {
//     if (!cfg.contains(key) || !cfg[key].is_object()) {
//         cfg[key] = json::object();
//     }
// }

// // Helper 
// static int initial_threads_for_mode(ControlMode mode)
// {
//     switch (mode) {
//         case ControlMode::CPU_STATIC: return 4;
//         case ControlMode::GPU_STATIC: return 1;
//         case ControlMode::BALANCED:   return 2;
//         case ControlMode::ERL:        return 2;
//     }
//     return 2;
// }

// static void ensure_required_sections(json& cfg)
// {
//     ensure_section(cfg, "Scheduler");
//     ensure_section(cfg, "AlgorithmConfig");
//     ensure_section(cfg, "Aggregator");
//     ensure_section(cfg, "CameraConfig");
//     ensure_section(cfg, "DisplayConfig");
//     ensure_section(cfg, "SoCConfig");
//     ensure_section(cfg, "LynsynMonitorConfig");
//     ensure_section(cfg, "Profiling");
//     ensure_section(cfg, "SystemBehavior");
// }

// static void set_output_paths(json& cfg, const BenchmarkCase& bc)
// {
//     const std::string stamp = timestamp_token();
//     const std::string token = sanitize_file_token(bc.testLabel + "_" + stamp);


//     const std::string metricsCsv  = "output/metrics_" + token + ".csv";
//     const std::string metricsJson = "output/metrics_" + token + ".ndjson";

//      // Preferred location
//     cfg["Aggregator"]["metrics_csv"]  = metricsCsv;
//     cfg["Aggregator"]["metrics_json"] = metricsJson;

//     // Backward-compatible fallback
//     cfg["metrics_csv"]  = metricsCsv;
//     cfg["metrics_json"] = metricsJson;

//     cfg["LynsynMonitorConfig"]["outputCSV"] =
//         "output/lynsyn_" + token + ".csv";

//     cfg["Profiling"]["metricsOutputFile"] =
//         "output/perf_" + token + ".csv";

//     const bool isERL = (bc.mode == ControlMode::ERL);

//     cfg["Scheduler"]["export_pareto_evidence"] = isERL;

//     if (isERL) {
//         cfg["Scheduler"]["pareto_csv"] =
//             "output/pareto_" + token + ".csv";

//         cfg["Scheduler"]["action_csv"] =
//             "output/actions_" + token + ".csv";
//     }
// }

// static void set_algorithm_config(json& cfg, const BenchmarkCase& bc)
// {
//     auto& a = cfg["AlgorithmConfig"];

//     a["algorithmType"] = bc.algo.label;
//     //a["concurrencyLevel"] = 4;
//     a["concurrencyLevel"] = initial_threads_for_mode(bc.mode);
//     a["blurRadius"] = 5;
//     a["medianWindowSize"] = 5;

//     switch (bc.mode) {
//         case ControlMode::CPU_STATIC:
//             a["useGPU"] = false;
//             break;

//         case ControlMode::GPU_STATIC:
//             a["useGPU"] = bc.algo.gpuCapable;
//             break;

//         case ControlMode::BALANCED:
//         case ControlMode::ERL:
//             a["useGPU"] = bc.algo.gpuCapable;
//             break;
//     }
// }

// static void set_scheduler_config(json& cfg, const BenchmarkCase& bc)
// {
//     auto& s = cfg["Scheduler"];

//     s["policy"] = "BALANCED";
//     s["targetFPS"] = 30.0;
//     s["powerBudgetW"] = 5.0;
//     s["preferGPU"] = bc.algo.gpuCapable;
//     s["control_interval_ms"] = 800;

//     switch (bc.mode) {
//         case ControlMode::CPU_STATIC:
//             s["enabled"] = false;
//             s["use_erl"] = false;
//             break;

//         case ControlMode::GPU_STATIC:
//             s["enabled"] = false;
//             s["use_erl"] = false;
//             break;

//         case ControlMode::BALANCED:
//             // Keep BALANCED deterministic for thesis baseline.
//             // RuntimeControls are pinned later in pin_static_controls().
//             s["enabled"] = false;
//             s["use_erl"] = false;
//             break;

//         case ControlMode::ERL:
//             s["enabled"] = true;
//             s["use_erl"] = true;

//             // ERL / GA configuration ? must be in Scheduler block.
//             s["ga_population_size"] = 8;
//             s["ga_min_pop_size"] = 4;
//             s["ga_elite_count"] = 2;
//             s["ga_crossover_rate"] = 0.70;
//             s["ga_mutation_rate"] = 0.15;
//             s["ga_exploration_decay"] = 0.96;
//             s["ga_reduction_start_gen"] = 10;
//             s["ga_reduction_ratio"] = 0.60;

//             // Adaptive weight manager.
//             s["adaptive_weights_enabled"] = true;
//             s["awm_update_interval_gens"] = 5;
//             s["awm_battery_critical_watts"] = 8.0;
//             s["awm_thermal_warn_cpu_c"] = 75.0;
//             s["awm_thermal_warn_gpu_c"] = 78.0;
//             s["awm_thermal_crit_cpu_c"] = 80.0;
//             s["awm_thermal_crit_gpu_c"] = 83.0;
//             s["awm_latency_sla_ms"] = 45.0;
//             s["awm_fps_underperform_ratio"] = 0.70;
//             s["awm_transition_smoothing"] = 0.30;
//             break;
//     }
// }

// //================================================================================
// // Logging helpers
// //================================================================================

// static void log_case_config(const BenchmarkCase& bc, const json& cfg)
// {
//     const auto& s = cfg["Scheduler"];
//     const auto& a = cfg["AlgorithmConfig"];

//     spdlog::info("============================================================");
//     spdlog::info("BENCHMARK CASE: {}", bc.testLabel);
//     spdlog::info("============================================================");
//     spdlog::info("Algorithm       : {}", bc.algo.label);
//     spdlog::info("Mode            : {}", control_mode_to_string(bc.mode));
//     spdlog::info("GPU Capable     : {}", bc.algo.gpuCapable ? "YES" : "NO");
//     spdlog::info("Config useGPU   : {}", a.value("useGPU", false) ? "YES" : "NO");
//     spdlog::info("Concurrency     : {}", a.value("concurrencyLevel", 0));
//     spdlog::info("Scheduler       : {}", s.value("enabled", false) ? "ON" : "OFF");
//     spdlog::info("ERL             : {}", s.value("use_erl", false) ? "ON" : "OFF");
//     spdlog::info("Fallback note   : {}", bc.algo.fallbackNote);

//     if (s.value("use_erl", false)) {
//         spdlog::info("---- Effective ERL / GA configuration ----");
//         spdlog::info("ga_population_size     = {}", s.value("ga_population_size", 12));
//         spdlog::info("ga_min_pop_size        = {}", s.value("ga_min_pop_size", 6));
//         spdlog::info("ga_elite_count         = {}", s.value("ga_elite_count", 3));
//         spdlog::info("ga_crossover_rate      = {:.2f}", s.value("ga_crossover_rate", 0.75));
//         spdlog::info("ga_mutation_rate       = {:.2f}", s.value("ga_mutation_rate", 0.18));
//         spdlog::info("ga_exploration_decay   = {:.2f}", s.value("ga_exploration_decay", 0.96));
//         spdlog::info("ga_reduction_start_gen = {}", s.value("ga_reduction_start_gen", 10));
//         spdlog::info("ga_reduction_ratio     = {:.2f}", s.value("ga_reduction_ratio", 0.60));
//         spdlog::info("pareto_csv             = {}", s.value("pareto_csv", std::string("N/A")));
//         spdlog::info("action_csv             = {}", s.value("action_csv", std::string("N/A")));
//     }

//     spdlog::info("============================================================");
// }

// static void log_runtime_state(
//     const BenchmarkCase& bc,
//     const hrl::RuntimeControls& rt,
//     const hrl::MetricsSnapshot& snap,
//     double elapsedSec)
// {
//     const bool effectiveGpu =
//         hrl::isCudaAvailable() &&
//         rt.enable_gpu.load(std::memory_order_relaxed);

//     const int threads =
//         rt.concurrency_level.load(std::memory_order_relaxed);

//     spdlog::info(
//         "[RUNTIME] t={:.0f}s | {} | {} | GPU={} | Threads={} | "
//         "FPS={:.1f} | Inf={:.2f}ms | Proc={:.2f}ms | E2E={:.2f}ms | "
//         "Power={:.2f}W | J/F={:.4f} | CPU={:.0f}% | GPU={:.0f}% | "
//         "Tcpu={:.1f}C | Tgpu={:.1f}C",
//         elapsedSec,
//         bc.algo.label,
//         control_mode_to_string(bc.mode),
//         effectiveGpu ? "ON" : "OFF",
//         threads,
//         snap.fps,
//         snap.avg_inference_ms,
//         snap.processing_latency_ms,
//         snap.end_to_end_latency_ms,
//         snap.avg_power_w_alg,
//         snap.joules_per_frame,
//         snap.cpu_util_avg,
//         snap.gpu_util_avg,
//         snap.cpu_temp_c,
//         snap.gpu_temp_c
//     );
// }

// static void log_case_summary(
//     const BenchmarkCase& bc,
//     const hrl::MetricsSnapshot& snap,
//     double totalSec)
// {
//     spdlog::info("------------------------------------------------------------");
//     spdlog::info("SUMMARY: {}", bc.testLabel);
//     spdlog::info("------------------------------------------------------------");
//     spdlog::info("Duration              : {:.1f} s", totalSec);
//     spdlog::info("Final FPS             : {:.2f}", snap.fps);
//     spdlog::info("Average inference     : {:.2f} ms", snap.avg_inference_ms);
//     spdlog::info("Processing latency    : {:.2f} ms", snap.processing_latency_ms);
//     spdlog::info("Display latency       : {:.2f} ms", snap.display_latency_ms);
//     spdlog::info("End-to-end latency    : {:.2f} ms", snap.end_to_end_latency_ms);
//     spdlog::info("Average power         : {:.2f} W", snap.avg_power_w_alg);
//     spdlog::info("Joules per frame      : {:.4f}", snap.joules_per_frame);
//     spdlog::info("CPU utilisation       : {:.1f} %", snap.cpu_util_avg);
//     spdlog::info("GPU utilisation       : {:.1f} %", snap.gpu_util_avg);
//     spdlog::info("CPU temperature       : {:.1f} C", snap.cpu_temp_c);
//     spdlog::info("GPU temperature       : {:.1f} C", snap.gpu_temp_c);
//     spdlog::info("Fallback note         : {}", bc.algo.fallbackNote);
//     spdlog::info("------------------------------------------------------------");
// }

// //================================================================================
// // Runtime control pinning for static modes
// //================================================================================
// static void pin_static_controls(const BenchmarkCase& bc,
//                                 const std::shared_ptr<hrl::RuntimeControls>& rt)
// {
//     if (!rt) {
//         return;
//     }

//     switch (bc.mode) {
//         case ControlMode::CPU_STATIC:
//             rt->enable_gpu.store(false, std::memory_order_relaxed);
//             rt->concurrency_level.store(4, std::memory_order_relaxed);
//             rt->affinity.store(hrl::Affinity::Spread, std::memory_order_relaxed);
//             break;

//         case ControlMode::GPU_STATIC:
//             rt->enable_gpu.store(
//                 bc.algo.gpuCapable && hrl::isCudaAvailable(),
//                 std::memory_order_relaxed
//             );
//             rt->concurrency_level.store(1, std::memory_order_relaxed);
//             rt->affinity.store(hrl::Affinity::Pack, std::memory_order_relaxed);
//             break;

//         case ControlMode::BALANCED:
//             rt->enable_gpu.store(
//                 bc.algo.gpuCapable && hrl::isCudaAvailable(),
//                 std::memory_order_relaxed
//             );
//             rt->concurrency_level.store(2, std::memory_order_relaxed);
//             rt->affinity.store(hrl::Affinity::Spread, std::memory_order_relaxed);
//             break;

//         case ControlMode::ERL:
//             // ERL starts from a neutral state. Scheduler/GA may adapt it.
//             rt->enable_gpu.store(
//                 bc.algo.gpuCapable && hrl::isCudaAvailable(),
//                 std::memory_order_relaxed
//             );
//             rt->concurrency_level.store(2, std::memory_order_relaxed);
//             rt->affinity.store(hrl::Affinity::Spread, std::memory_order_relaxed);

//             // set_scheduler_config(cfg, bc);
//             // set_thermal_config(cfg, bc, temp_offset);   // NEW
            
//             break;
//     }
// }


// // Adjust ERL objective weights to include thermal/power penalties.
// static void set_thermal_config(json& cfg, const BenchmarkCase& bc, double temp_offset)
//     {
//         if (bc.mode != ControlMode::ERL) return;

//         // Base weights (defaults match EvolutionarySelector's fallbacks)
//         double base_fps   = cfg.value("obj_weight_fps", 1.0);
//         double base_power = cfg.value("obj_weight_power", 0.5);
//         double base_temp  = cfg.value("obj_weight_temp", 0.5);
//         double base_lat   = cfg.value("obj_weight_latency", 0.3);

//         // Offset scaling: e.g., offset=0.0 ? no change, offset=1.0 ? double penalty
//         double temp_factor   = 1.0 + temp_offset;
//         double power_factor  = 1.0 + 0.5 * temp_offset;   // less aggressive

//         cfg["obj_weight_temp"]  = base_temp * temp_factor;
//         cfg["obj_weight_power"] = base_power * power_factor;
//         // FPS and latency remain unchanged (they are performance objectives)

//         spdlog::info("[THERMAL] temp_weight={:.2f}  power_weight={:.2f}  (offset={:.2f})",
//                     cfg["obj_weight_temp"].get<double>(),
//                     cfg["obj_weight_power"].get<double>(),
//                     temp_offset);
//     }


// //================================================================================
// // Single benchmark runner
// //================================================================================

// static int run_benchmark_case(
//     const BenchmarkCase& bc,
//     double runSeconds,
//     const std::string& configPath)
// {
//     g_shutdown_flag.store(false, std::memory_order_relaxed);

//     constexpr double warmupSeconds = 30.0;
//     constexpr double loopIntervalSec = 0.8;
//     constexpr double pinIntervalSec = 5.0;
//     constexpr int logEveryN = 10;

//     try {
//         Context ctx{};
//         ctx.shutdown_flag.store(false);
//         ctx.pipelineReady.store(false);
//         ctx.tm = std::make_shared<ThreadManager>();

//         json cfg = load_json_config(configPath);
//         ensure_required_sections(cfg);
//         set_algorithm_config(cfg, bc);
//         set_scheduler_config(cfg, bc);
//         set_output_paths(cfg, bc);

//         // --- NEW: Apply thermal config for ERL ---
//         if (bc.mode == ControlMode::ERL) {
//             set_thermal_config(cfg, bc, bc.temperature_offset);
//         }

//         log_case_config(bc, cfg);
//         // ... then build ConfigManager, etc.
        
//         // json cfg = load_json_config(configPath);
//         // ensure_required_sections(cfg);
//         // set_algorithm_config(cfg, bc);
//         // set_scheduler_config(cfg, bc);
//         // set_output_paths(cfg, bc);

//         // log_case_config(bc, cfg);

//         {
//             ConfigManager manager(cfg, ctx);
//             manager.buildPipeline();

//             auto rt = manager.getRuntimeControls();

//             if (rt) {
//                 pin_static_controls(bc, rt);

//                 if (bc.mode == ControlMode::BALANCED ||
//                     bc.mode == ControlMode::ERL) {
//                     rt->concurrency_level.store(2, std::memory_order_relaxed);
//                     rt->enable_gpu.store(
//                         bc.algo.gpuCapable && hrl::isCudaAvailable(),
//                         std::memory_order_relaxed
//                     );
//                 }
//             }

//             if (!manager.validateAll()) {
//                 spdlog::error("[BENCH] Validation failed for {}", bc.testLabel);
//                 return EXIT_FAILURE;
//             }

//             manager.startAll();
//             spdlog::info("[BENCH] Started {}", bc.testLabel);

//             auto runStart = std::chrono::steady_clock::now();
//             auto lastPin = runStart;
//             bool warmupDone = false;
//             int loopCount = 0;

//             // Fail-fast camera guard: a healthy run must show frames flowing shortly
//             // after warmup. If the camera/algorithm delivers nothing (the all-zero
//             // GaussianBlur signature), abort within seconds instead of recording an
//             // empty 30-minute run. Common causes: CSI sensor_mode/60FPS config failed
//             // (DataConcrete strict-mode returned false) or Lynsyn USB left in a bad
//             // state by a prior run.
//             bool cameraConfirmed = false;
//             const double firstFrameDeadlineSec = warmupSeconds + 10.0;

//             while (true) {
//                 auto now = std::chrono::steady_clock::now();
//                 double elapsed =
//                     std::chrono::duration<double>(now - runStart).count();

//                 if (elapsed >= runSeconds) break;

//                 if (g_shutdown_flag.load(std::memory_order_relaxed)) {
//                     spdlog::warn("[BENCH] Shutdown requested during {}", bc.testLabel);
//                     break;
//                 }

//                 if (!warmupDone && elapsed >= warmupSeconds) {
//                     warmupDone = true;
//                     spdlog::info("============================================================");
//                     spdlog::info("WARMUP COMPLETE ? measurement window begins now");
//                     spdlog::info("Case: {}", bc.testLabel);
//                     spdlog::info("Elapsed: {:.1f}s", elapsed);
//                     spdlog::info("Exclude all data before this marker from thesis analysis.");
//                     spdlog::info("============================================================");
//                 }

//                 // === Fail-fast camera guard ===
//                 // Confirm frames are flowing; abort the case if nothing arrives by the
//                 // deadline rather than burning the full run on an all-zero capture
//                 // (the GaussianBlur all-zero signature). Causes: CSI sensor_mode/60FPS
//                 // config failed (DataConcrete strict-mode returned false) or Lynsyn USB
//                 // left in a bad state by the previous run.
//                 if (!cameraConfirmed) {
//                     auto guardSnap = manager.getLatestMetricsSnapshot();
//                     // hrl::MetricsSnapshot is the flat snapshot: snap.fps already
//                     // resolves to algorithm fps, falling back to camera fps, and
//                     // snap.frameId tracks finalized frames. Either being non-zero
//                     // means frames are flowing.
//                     const bool framesFlowing =
//                         guardSnap.frameId > 0 ||
//                         guardSnap.fps > 0.5;

//                     if (framesFlowing) {
//                         cameraConfirmed = true;
//                         spdlog::info("[BENCH] Camera confirmed: frames flowing "
//                                      "(fps={:.1f}, frameId={})",
//                                      guardSnap.fps, guardSnap.frameId);
//                     } else if (elapsed >= firstFrameDeadlineSec) {
//                         spdlog::critical(
//                             "[BENCH] ABORT: no frames after {:.0f}s for {} "
//                             "(fps={:.1f}, frameId={}). "
//                             "Refusing to record an all-zero run. Check CSI sensor_mode=4 / "
//                             "60FPS programming (DataConcrete strict-mode) and Lynsyn USB "
//                             "reset state from the previous run.",
//                             elapsed, bc.testLabel,
//                             guardSnap.fps, guardSnap.frameId);

//                         ctx.shutdown_flag.store(true, std::memory_order_relaxed);
//                         manager.stopAll();
//                         manager.flushMetrics();
//                         std::this_thread::sleep_for(std::chrono::seconds(2));
//                         ctx.tm.reset();
//                         return EXIT_FAILURE;
//                     }
//                 }

//                 double sinceLastPin =
//                     std::chrono::duration<double>(now - lastPin).count();

//                 if (sinceLastPin >= pinIntervalSec) {
//                     pin_static_controls(bc, rt);
//                     lastPin = now;
//                 }

//                 if (++loopCount % logEveryN == 0) {
//                     auto snap = manager.getLatestMetricsSnapshot();
//                     if (rt) {
//                         log_runtime_state(bc, *rt, snap, elapsed);
//                     }
//                 }

//                 std::this_thread::sleep_for(
//                     std::chrono::milliseconds(
//                         static_cast<int>(loopIntervalSec * 1000)
//                     )
//                 );
//             }

//             auto finalSnap = manager.getLatestMetricsSnapshot();
//             double totalElapsed =
//                 std::chrono::duration<double>(
//                     std::chrono::steady_clock::now() - runStart
//                 ).count();

//             log_case_summary(bc, finalSnap, totalElapsed);

//             spdlog::info("[BENCH] Stopping {}", bc.testLabel);
//             ctx.shutdown_flag.store(true, std::memory_order_relaxed);
//             manager.stopAll();
//             manager.flushMetrics();
//         }

//         std::this_thread::sleep_for(std::chrono::seconds(2));
//         ctx.tm.reset();

//         spdlog::info("[BENCH] Completed {}", bc.testLabel);
//         return EXIT_SUCCESS;
//     }
//     catch (const std::exception& e) {
//         spdlog::critical("[BENCH] Critical failure in {}: {}", bc.testLabel, e.what());
//         return EXIT_FAILURE;
//     }
// }

// // // add thermal controls flags
// // static void set_thermal_config(json& cfg, const BenchmarkCase& bc, double temp_offset = 0.0)
// // {
// //     ensure_section(cfg, "ThermalGovernor");   // or put under Scheduler / ERL

// //     cfg["ThermalGovernor"]["temperature_offset_c"] = temp_offset;

// //     if (std::abs(temp_offset) > 0.1) {
// //         spdlog::warn("[TEST] Using temperature offset = {:.1f}°C for this run", temp_offset);
// //     }
// // }


// //================================================================================
// // Main
// //================================================================================

// int main(int argc, char* argv[])
// {
//     std::signal(SIGINT, signal_handler);
//     std::signal(SIGTERM, signal_handler);

//     spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

//     std::system("mkdir -p output");

//     hrl::detectCudaAvailability();

//     spdlog::info("============================================================");
//     spdlog::info("PhD Benchmark Harness v30");
//     spdlog::info("5 image-processing algorithms × 4 control modes");
//     spdlog::info("CUDA Available: {}", hrl::cudaAvailabilityString());
//     spdlog::info("============================================================");

//     bool runMatrix = false;
//     double runSeconds = 1800.0;
//     double cooldownSeconds = 30.0;
//     std::string configPath = "config.json";
//     std::string singleAlgo;
//     std::string singleMode;

//     // temp offset
//     double temperature_offset = 0.0;

//     for (int i = 1; i < argc; ++i) {
//         std::string arg = argv[i];
//         if (arg == "--temp-offset" && i+1 < argc) {
//             temperature_offset = std::stod(argv[++i]);
//         }

//         if (arg == "--phd-matrix" || arg == "--benchmark-matrix") {
//             runMatrix = true;
//         }
//         else if (arg == "--algo" && i + 1 < argc) {
//             singleAlgo = argv[++i];
//         }
//         else if (arg == "--mode" && i + 1 < argc) {
//             singleMode = argv[++i];
//         }
//         else if (arg == "--cooldown" && i + 1 < argc) {
//             cooldownSeconds = std::stod(argv[++i]);
//         }
//         else if (arg.size() >= 5 &&
//                  arg.substr(arg.size() - 5) == ".json") {
//             configPath = arg;
//         }
//         else {
//             try {
//                 runSeconds = std::stod(arg);
//             }
//             catch (...) {
//                 spdlog::warn("[ARGS] Ignored argument '{}'", arg);
//             }
//         }
//     }

//     if (runMatrix) {
//         const auto matrix = build_benchmark_matrix();

//         spdlog::info("============================================================");
//         spdlog::info("Running full PhD matrix");
//         spdlog::info("Cases: {}", matrix.size());
//         spdlog::info("Duration per case: {:.1f}s", runSeconds);
//         spdlog::info("Cooldown between cases: {:.1f}s", cooldownSeconds);
//         spdlog::info("============================================================");

//         for (size_t i = 0; i < matrix.size(); ++i) {
//             spdlog::info("[{:02d}/{}] {}",
//                          static_cast<int>(i + 1),
//                          matrix.size(),
//                          matrix[i].testLabel);
//         }

//         int passed = 0;
//         int failed = 0;

//         for (size_t i = 0; i < matrix.size(); ++i) {
//             if (g_shutdown_flag.load(std::memory_order_relaxed)) {
//                 spdlog::warn("[MATRIX] Shutdown requested before next case.");
//                 break;
//             }

//             const auto& bc = matrix[i];

//             // --- Copy and set the temperature offset ---
//             BenchmarkCase bc_with_offset = bc;
//             bc_with_offset.temperature_offset = temperature_offset;

//             spdlog::info("============================================================");
//             spdlog::info("MATRIX PROGRESS [{}/{}]: {}",
//                          i + 1,
//                          matrix.size(),
//                          bc.testLabel);
//             spdlog::info("============================================================");

//             //int rc = run_benchmark_case(bc, runSeconds, configPath);
//             int rc = run_benchmark_case(bc_with_offset, runSeconds, configPath);

//             if (rc == EXIT_SUCCESS) {
//                 ++passed;
//             }
//             else {
//                 ++failed;
//             }

//             if (i + 1 < matrix.size()) {
//                 spdlog::info("[MATRIX] Cooldown {:.1f}s before next case...",
//                              cooldownSeconds);

//                 auto coolStart = std::chrono::steady_clock::now();

//                 while (std::chrono::duration<double>(
//                            std::chrono::steady_clock::now() - coolStart
//                        ).count() < cooldownSeconds) {
//                     if (g_shutdown_flag.load(std::memory_order_relaxed)) break;
//                     std::this_thread::sleep_for(std::chrono::seconds(1));
//                 }
//             }
//         }

//         spdlog::info("============================================================");
//         spdlog::info("MATRIX COMPLETE");
//         spdlog::info("Passed: {}", passed);
//         spdlog::info("Failed: {}", failed);
//         spdlog::info("Total : {}", passed + failed);
//         spdlog::info("============================================================");

//         return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
//     }

//     if (!singleAlgo.empty() && !singleMode.empty()) {
//         const AlgorithmBenchmark* algo = nullptr;

//         for (const auto& a : kAlgorithms) {
//             if (a.label == singleAlgo) {
//                 algo = &a;
//                 break;
//             }
//         }

//         if (!algo) {
//             spdlog::error("Unknown algorithm '{}'", singleAlgo);
//             spdlog::error("Available algorithms:");
//             for (const auto& a : kAlgorithms) {
//                 spdlog::error("  - {}", a.label);
//             }
//             return EXIT_FAILURE;
//         }

//         ControlMode mode;
//         if (!parse_control_mode(singleMode, mode)) {
//             spdlog::error("Unknown mode '{}'", singleMode);
//             spdlog::error("Available modes: CPU_STATIC, GPU_STATIC, BALANCED, ERL");
//             return EXIT_FAILURE;
//         }

//         BenchmarkCase bc;
//         bc.algo = *algo;
//         bc.mode = mode;
//         bc.suite = "Single_" + algo->label;
//         bc.testLabel = algo->label + "_" + control_mode_to_string(mode);
//         bc.temperature_offset = temperature_offset;   // <-- add this
//         return run_benchmark_case(bc, runSeconds, configPath);
//     }

//     spdlog::info("Usage:");
//     spdlog::info("  Full matrix:");
//     spdlog::info("    ./MySystem --phd-matrix 1800 config.json");
//     spdlog::info("    ./MySystem --phd-matrix 1800 --cooldown 60 config.json");
//     spdlog::info("");
//     spdlog::info("  Single case:");
//     spdlog::info("    ./MySystem --algo SobelEdge --mode ERL 300 config.json");
//     spdlog::info("    ./MySystem --algo GaussianBlur --mode CPU_STATIC 60 config.json");
//     spdlog::info("");
//     spdlog::info("Algorithms:");
//     for (const auto& a : kAlgorithms) {
//         spdlog::info("  - {} | GPU={} | {}",
//                      a.label,
//                      a.gpuCapable ? "YES" : "NO",
//                      a.fallbackNote);
//     }
//     spdlog::info("");
//     spdlog::info("Modes:");
//     spdlog::info("  - CPU_STATIC");
//     spdlog::info("  - GPU_STATIC");
//     spdlog::info("  - BALANCED");
//     spdlog::info("  - ERL");

//     return EXIT_SUCCESS;
// }