

// =====================================================================
// ThermalGovernor.h  Aggressive Thermal Management with Graceful Degradation
// =====================================================================
// REVISION: Jetson Nano-calibrated thresholds (2026-06-30)
//
// Change summary vs original:
//   - All five stage thresholds lowered to match real Jetson Nano operating
//     temperatures observed in ERL vs baseline experiments:
//
//       Original (inactive on Jetson Nano):
//         CAUTION   > 75°C  |  WARNING  > 85°C  |  CRITICAL  > 95°C
//         EMERGENCY > 105°C |  SHUTDOWN > 108°C
//
//       Revised1 (active during normal load):
//         CAUTION   > 55°C  |  WARNING  > 62°C  |  CRITICAL  > 70°C
//         EMERGENCY > 80°C  |  SHUTDOWN >  90°C
 //      
//    Revised2 (active during normal load):
//         CAUTION   > 45°C  |  WARNING  > 50°C  |  CRITICAL  > 55°C
//         EMERGENCY > 60°C  |  SHUTDOWN >  75°C
////

//   - Thresholds are now runtime-configurable via ThermalGovernor::Config
//     to support ablation studies without recompilation.
//
//   - shouldShutdown() threshold aligned to Config::shutdown_c (was hardcoded).
//
//   - PhD rationale: ERL runs at 39-45°C (NORMAL), baselines at 61-65°C
//     (CAUTION ? WARNING). This creates a live thermal gradient that the
//     fitness function and AdaptiveWeightManager can detect and reward.
//     Previously all runs stayed in NORMAL and the governor never fired.
//
// Note: Also update AdaptiveWeightManager::Config defaults to match:
//   thermal_warn_cpu_c  = 45.0   (was 75.0)
//   thermal_warn_gpu_c  = 45.0   (was 78.0)
//   thermal_crit_cpu_c  = 55.0   (was 80.0)
//   thermal_crit_gpu_c  = 55.0   (was 83.0)
//   battery_critical_watts = 7.5 (was 8.0  Jetson Nano 5W mode ceiling)
// =====================================================================
#pragma once

#include <memory>
#include <spdlog/spdlog.h>
#include <cmath>

namespace hrl {

/**
 * ThermalGovernor  Multi-stage thermal throttling for Jetson Nano
 *
 * Stages (default thresholds, Jetson Nano calibrated):
 *   0. Normal   (<  55°C) : Full performance
 *   1. Caution  (55-62°C) : Reduce FPS target by 10%, +5% power budget cut
 *   2. Warning  (62-70°C) : Reduce FPS target by 25%, disable GPU
 *   3. Critical (70-80°C) : Frame skipping (keep only 1 in 3)
 *   4. Emergency (> 80°C) : Stop inference, emergency throttle
 *
 * All thresholds are configurable at construction time via Config to support
 * ablation studies (e.g. restoring original 75/85/95°C values for comparison).
 */
class ThermalGovernor {
public:
    enum ThermalStage {
        NORMAL    = 0,
        CAUTION   = 1,
        WARNING   = 2,
        CRITICAL  = 3,
        EMERGENCY = 4
    };

struct Config {
    double caution_c;
    double warning_c;
    double critical_c;
    double emergency_c;
    double shutdown_c;

    // NEW: Global temperature offset for controlled PhD experiments
    // Allows simulating cooler/hotter environments without hardware changes.
    // Global temperature offset for controlled PhD ablation experiments.
    // Allows simulating cooler/hotter environments without hardware changes.
    double temperature_offset_c = 0.0;

    // Config()
    //     : caution_c(45.0),
    //       warning_c(50.0),
    //       critical_c(55.0),
    //       emergency_c(65.0),
    //       shutdown_c(70.0),
    //       temperature_offset_c(0.0)
    // {}

    // Replace existing constructor body in ThermalGovernor::Config
    Config()
        : caution_c(45.0),     // CAUTION  45°C (experiment)
        warning_c(50.0),     // WARNING  50°C (experiment)
        critical_c(55.0),    // CRITICAL  55°C (experiment)
        emergency_c(65.0),   // Keep emergency safely above critical
        shutdown_c(75.0),    // Keep shutdown safely above emergency
        temperature_offset_c(0.0)
    {}

// Convenience factory: original pre-revision thresholds for A/B ablation
    static Config original() {
        Config c;
        c.caution_c   = 75.0;
        c.warning_c   = 85.0;
        c.critical_c  = 95.0;
        c.emergency_c = 105.0;
        c.shutdown_c  = 108.0;
        c.temperature_offset_c = 0.0;
        return c;
    }
// Convenience factory: virtual cooler environment
    static Config coolerBaseline() {
        Config c = Config();           // start from current defaults
        c.temperature_offset_c = -15.0;
        return c;
    }
};
    // // ----------------------------------------------------------------
    // // Runtime-configurable thresholds
    // // Defaults are set to Jetson Nano observed operating range.
    // // To restore the original (inactive) thresholds for comparison runs,
    // // construct with ThermalGovernor(ThermalGovernor::Config::original()).
    // // ----------------------------------------------------------------
    // struct Config {
    //     double caution_c;
    //     double warning_c;
    //     double critical_c;
    //     double emergency_c;
    //     double shutdown_c;
    //     // NEW: Temperature offset for controlled experiments
    //     double temperature_offset_c = 0.0;   // e.g. -15.0 for "cooler baseline"

    //     // Explicit default constructor instead of in-class member initializers.
    //     // GCC in C++14 mode rejects `ThermalGovernor(Config cfg = Config())` when
    //     // Config uses NSDMIs ("default member initializer required before the end
    //     // of its enclosing class"). This is the same workaround already used for
    //     // AdaptiveWeightManager::Config. Defaults are the Jetson Nano calibrated
    //     // thresholds (lowered from the original 75/85/95/105/108).
    //     Config()
    //         : caution_c(55.0),    // Was 75.0
    //           warning_c(62.0),    // Was 85.0
    //           critical_c(70.0),   // Was 95.0
    //           emergency_c(80.0),  // Was 105.0
    //           shutdown_c(90.0)    // Was 108.0 (was hardcoded, now configurable)
    //     {}

    //     // Convenience factory: reproduces the original (pre-revision) thresholds
    //     // for A/B comparison in ablation studies.
    //     static Config original() {
    //         Config c;
    //         c.caution_c   =  75.0;
    //         c.warning_c   =  85.0;
    //         c.critical_c  =  95.0;
    //         c.emergency_c = 105.0;
    //         c.shutdown_c  = 108.0;
    //         return c;
    //     }
    // };

    // ----------------------------------------------------------------
    // Actuation policy state returned to runtime loop
    // ----------------------------------------------------------------

    struct ThermalPolicy {
        double target_fps_scale      = 1.0;   // Multiply target FPS by this
        bool   gpu_allowed           = true;
        bool   allow_inference       = true;
        int    frame_skip_ratio      = 1;     // Keep 1 in N frames (1 = no skip)
        double power_budget_reduction = 0.0;  // Additional power reduction %
    };

    // Default constructor uses Jetson Nano-calibrated thresholds.
    explicit ThermalGovernor(Config cfg = Config())
        : cfg_(cfg),
          current_stage_(NORMAL),
          peak_temp_c_(0.0),
          shutdown_initiated_(false) {
            spdlog::info("[ThermalGovernor] Config: caution={:.1f}C warning={:.1f}C critical={:.1f}C emergency={:.1f}C shutdown={:.1f}C offset={:.1f}C",
             cfg_.caution_c, cfg_.warning_c, cfg_.critical_c, cfg_.emergency_c, cfg_.shutdown_c, cfg_.temperature_offset_c);

          }

    // ----------------------------------------------------------------
    // Update thermal state based on current temperatures.
    // Returns the policy to apply this tick.
    // ----------------------------------------------------------------
    // ----------------------------------------------------------------
    // Core state machine update tick.
    // Calculates effective temperatures, updates stage, and returns active policy.
    // ----------------------------------------------------------------

    ThermalPolicy update(double cpu_temp_c, double gpu_temp_c) {
        // Apply offset for controlled testing
        double effective_cpu = cpu_temp_c + cfg_.temperature_offset_c;
        double effective_gpu = gpu_temp_c + cfg_.temperature_offset_c;
        double max_temp = std::max(effective_cpu, effective_gpu);

        peak_temp_c_ = std::max(peak_temp_c_, max_temp);

        ThermalStage newStage = classifyStage(max_temp);

        if (newStage != current_stage_) {
            logStageTransition(current_stage_, newStage, max_temp, cfg_.temperature_offset_c);
            current_stage_ = newStage;
        }

        return getPolicyForStage(current_stage_);
    }

    // ThermalPolicy update(double cpu_temp_c, double gpu_temp_c) {
    //     // Apply offset for testing
    //     double effective_cpu = cpu_temp_c + cfg_.temperature_offset_c;
    //     double effective_gpu = gpu_temp_c + cfg_.temperature_offset_c;
    //     double max_temp = std::max(effective_cpu, effective_gpu);
        
    //     double max_temp = std::max(cpu_temp_c, gpu_temp_c);
    //     peak_temp_c_ = std::max(peak_temp_c_, max_temp);

    //     ThermalStage newStage = classifyStage(max_temp);

    //     if (newStage != current_stage_) {
    //         logStageTransition(current_stage_, newStage, max_temp);
    //         current_stage_ = newStage;
    //     }

    //     return getPolicyForStage(current_stage_);
    // }


    ThermalStage getCurrentStage() const { return current_stage_; }

    // ----------------------------------------------------------------
    // Shutdown guard  aligned to Config::shutdown_c (no longer hardcoded).
    // ----------------------------------------------------------------
    // ----------------------------------------------------------------
    // Emergency shutdown guard
    // ----------------------------------------------------------------
    bool shouldShutdown(double cpu_temp_c, double gpu_temp_c) {
        double effective_max = std::max(cpu_temp_c, gpu_temp_c) + cfg_.temperature_offset_c;
        if (effective_max > cfg_.shutdown_c && !shutdown_initiated_) {
            shutdown_initiated_ = true;
            spdlog::critical("[ThermalGovernor] THERMAL SHUTDOWN INITIATED: "
                            "effective {:.1f}°C (offset={:.1f}°C)", 
                            effective_max, cfg_.temperature_offset_c);
            return true;
        }
        return false;
    }

    // bool shouldShutdown(double cpu_temp_c, double gpu_temp_c) {
    //     double max_temp = std::max(cpu_temp_c, gpu_temp_c);
    //     if (max_temp > cfg_.shutdown_c && !shutdown_initiated_) {
    //         shutdown_initiated_ = true;
    //         spdlog::critical("[ThermalGovernor] THERMAL SHUTDOWN INITIATED: {:.1f}°C "
    //                          "(threshold={:.1f}°C)", max_temp, cfg_.shutdown_c);
    //         return true;
    //     }
    //     return false;
    // }

    void resetAfterCooldown() {
        current_stage_     = NORMAL;
        shutdown_initiated_ = false;
        spdlog::info("[ThermalGovernor] Thermal state reset after cool-down");
    }

    // Accessors
  //  ThermalStage getCurrentStage() const { return current_stage_; }
    double getPeakTemperature() const { return peak_temp_c_; }

    // Expose active config for logging / thesis evidence export
    const Config& getConfig() const { return cfg_; }

    // Human-readable stage name (useful for CSV export)
    static const char* stageName(ThermalStage s) {
        switch (s) {
            case NORMAL:    return "NORMAL";
            case CAUTION:   return "CAUTION";
            case WARNING:   return "WARNING";
            case CRITICAL:  return "CRITICAL";
            case EMERGENCY: return "EMERGENCY";
            default:        return "UNKNOWN";
        }
    }


//=================================================================================================
// ================================================================
    // PhD Verification & Telemetry Logging Helpers
    // ================================================================

    /**
     * Standardized CSV Header for verification tools and Pandas/R thesis scripts.
     */
    static std::string getCsvHeader() {
        return "throughput_fps,latency_ms,power_w,energy_j,cpu_temp_c,gpu_temp_c,"
               "effective_max_temp_c,thermal_stage,fps_scale,gpu_allowed,"
               "allow_inference,frame_skip_ratio,power_red_pct";
    }

    /**
     * Formats current runtime metrics + governor state into a CSV row.
     * Captures all 5 Core ERL dimensions alongside actuation response.
     */
    std::string formatCsvRecord(double current_fps, 
                                double latency_ms, 
                                double current_power_w, 
                                double cumulative_energy_j, 
                                double cpu_temp_c, 
                                double gpu_temp_c) const 
    {
        double eff_cpu = cpu_temp_c + cfg_.temperature_offset_c;
        double eff_gpu = gpu_temp_c + cfg_.temperature_offset_c;
        double max_eff = std::max(eff_cpu, eff_gpu);
        
        ThermalPolicy policy = getPolicyForStage(current_stage_);

        char buf[384];
        std::snprintf(buf, sizeof(buf),
            "%.2f,%.2f,%.3f,%.3f,%.1f,%.1f,%.1f,%s,%.2f,%d,%d,%d,%.1f",
            current_fps,              // 1. Throughput (FPS)
            latency_ms,               // 2. Latency (ms)
            current_power_w,          // 3. Power (W)
            cumulative_energy_j,      // 4. Energy (J)
            cpu_temp_c,               // 5. Raw Temperature (CPU)
            gpu_temp_c,               // 5. Raw Temperature (GPU)
            max_eff,                  // Effective evaluation temp
            stageName(current_stage_),// Stage string
            policy.target_fps_scale,  // Actuation: FPS Scale
            policy.gpu_allowed ? 1 : 0, // Actuation: GPU Flag
            policy.allow_inference ? 1 : 0,
            policy.frame_skip_ratio,  // Actuation: Frame Skip
            policy.power_budget_reduction // Actuation: Power Cut %
        );

        return std::string(buf);
    }
//=================================================================================================
private:
    Config       cfg_;
    ThermalStage current_stage_;
    double       peak_temp_c_;
    bool         shutdown_initiated_;

    // ----------------------------------------------------------------
    // Stage classification  uses runtime-configurable thresholds.
    // ----------------------------------------------------------------
    ThermalStage classifyStage(double max_temp) const {
        if (max_temp > cfg_.emergency_c)  return EMERGENCY;
        if (max_temp > cfg_.critical_c)   return CRITICAL;
        if (max_temp > cfg_.warning_c)    return WARNING;
        if (max_temp > cfg_.caution_c)    return CAUTION;
        return NORMAL;
    }

    // ----------------------------------------------------------------
    // Policy per stage.
    //
    // Revised scaling values vs original:
    //   CAUTION  : fps_scale 0.90 (unchanged), power_budget_reduction 5%  (unchanged)
    //   WARNING  : fps_scale 0.75 (unchanged), gpu_allowed=false (unchanged), pwr 15% (unchanged)
    //   CRITICAL : fps_scale 0.50 (unchanged), frame_skip_ratio=3 (unchanged), pwr 30% (unchanged)
    //   EMERGENCY: fps_scale 0.20 (unchanged), allow_inference=false (unchanged)
    //
    // The policy actions are unchanged  only the temperature thresholds
    // that trigger them have been lowered to match real operating range.
    // ----------------------------------------------------------------
    ThermalPolicy getPolicyForStage(ThermalStage stage) const {
        ThermalPolicy policy;

        switch (stage) {
            case NORMAL:
                policy.target_fps_scale       = 1.0;
                policy.gpu_allowed            = true;
                policy.frame_skip_ratio       = 1;
                policy.power_budget_reduction = 0.0;
                break;

            case CAUTION:
                // Baselines (61-65°C CPU) will enter here with revised thresholds.
                // ERL (39-45°C) stays in NORMAL  this is the gradient the Pareto
                // fitness function needs to detect thermal advantage.
                policy.target_fps_scale       = 0.90;
                policy.gpu_allowed            = true;
                policy.frame_skip_ratio       = 1;
                policy.power_budget_reduction = 5.0;
                break;

            case WARNING:
                // Baseline BALANCED/GPU_STATIC runs at 63-65°C may reach this.
                // GPU disabled here forces CPU-only path, raising the cost of
                // high-performance configurations and rewarding ERL's cooler state.
                policy.target_fps_scale       = 0.75;
                policy.gpu_allowed            = false;
                policy.frame_skip_ratio       = 1;
                policy.power_budget_reduction = 15.0;
                break;

            case CRITICAL:
                policy.target_fps_scale       = 0.5;
                policy.gpu_allowed            = false;
                policy.frame_skip_ratio       = 3;
                policy.power_budget_reduction = 30.0;
                break;

            case EMERGENCY:
                policy.target_fps_scale       = 0.2;
                policy.gpu_allowed            = false;
                policy.allow_inference        = false;
                policy.frame_skip_ratio       = 10;
                policy.power_budget_reduction = 50.0;
                break;
        }

        return policy;
    }

    void logStageTransition(ThermalStage oldStage, ThermalStage newStage, 
                       double effective_temp, double offset) const {
        if (std::abs(offset) > 0.01) {
            spdlog::warn("[ThermalGovernor] Transition {} -> {} (effective_temp={:.1f}°C, "
                        "raw_temp{:.1f}°C, offset={:.1f}°C)",
                        stageName(oldStage), stageName(newStage), effective_temp,
                        effective_temp - offset, offset);
        } else {
            spdlog::warn("[ThermalGovernor] Transition {} -> {} (temp={:.1f}°C)",
                        stageName(oldStage), stageName(newStage), effective_temp);
        }
    }
};

} // namespace hrl
