
//==================================================================================================
// Scheduler.hpp ? FINAL PRODUCTION VERSION ? Jetson Nano 2GB
// Fixed: Removed extra semicolon after namespace Sysfs
// Fixed: Namespace scoping for Affinity
//==================================================================================================
#pragma once
#include <atomic>  // Good: For thread-safe controls
#include <algorithm>  // For clamp/min/max
#include <vector>  // For CPU lists
#include <string>  // Path construction
#include <sched.h>  // sched_setaffinity
#include <unistd.h>  // access()
#include <fstream>  // Not used?remove? (Dead code)
#include <cmath>  // abs/clamp

#include <iterator>
#include <cstdio>  // fopen/fprintf (efficient for sysfs)
#include <chrono>  // steady_clock for transitions
#include "spdlog/spdlog.h"  // Logging
#include "RuntimeControls.h"  // External dep (Affinity enum) // Uses shared definition
#include "IScheduler.h"          // Pure-virtual interface implemented by Scheduler

#include "ThermalGovernor.h"


namespace hrl {

//==================================================================================================
// 1. Core Enums & Data Structures
// NOTE: PolicyMode, MaybeDouble, MaybeBool, RLAction, MetricsSnapshot are defined in RuntimeControls.h
//==================================================================================================

//==================================================================================================
// 2. Helpers
//==================================================================================================
namespace {
    template<typename T>
    constexpr T local_clamp(T v, T lo, T hi) {
        return (v < lo) ? lo : (hi < v) ? hi : v;
    }
    
    inline const char* policyName(PolicyMode p) {
        switch (p) {
            case PolicyMode::MAX_PERFORMANCE: return "MAX_PERFORMANCE";
            case PolicyMode::LOW_POWER:       return "LOW_POWER";
            case PolicyMode::BALANCED:        return "BALANCED";
            case PolicyMode::UNKNOWN:         return "UNKNOWN";
            default:                          return "UNKNOWN";
        }
    }
}

//==================================================================================================
// 3. Sysfs & Hardware Abstraction
//==================================================================================================
namespace Sysfs {
    inline bool write(const std::string& path, const std::string& value) {
        FILE* f = fopen(path.c_str(), "w");
        if (!f) return false;
        int ret = fprintf(f, "%s", value.c_str());
        fclose(f);
        return ret > 0;
    }

    inline bool writeInt(const std::string& path, long v) {
        return write(path, std::to_string(v));
    }

    inline void cpuSetGovernorAll(const std::string& gov) {
        for (int i = 0; i < 4; ++i) { 
            std::string p = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/scaling_governor";
            if (access(p.c_str(), W_OK) == 0) write(p, gov);
        }
    }

    // [FIX P3] Prefer a utilisation-driven governor that respects the written
    // max-frequency cap; "powersave" pins all cores to the minimum frequency
    // regardless of the cap, which made every LOW_POWER deployment run at
    // 102 MHz while the surrogate was told >= 460.8 MHz.
    inline std::string pickBoundedGovernor() {
        std::ifstream f("/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors");
        std::string avail((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
        if (avail.find("schedutil") != std::string::npos) return "schedutil";
        if (avail.find("ondemand")  != std::string::npos) return "ondemand";
        return "performance";   // still bounded by the cap written via scaling_max_freq
    }

    inline void cpuSetFreqKHzAll(long min_khz, long max_khz) {
        for (int i = 0; i < 4; ++i) {
            std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq";
            if (access(base.c_str(), F_OK) != 0) continue;
            if (min_khz > 0) writeInt(base + "/scaling_min_freq", min_khz);
            if (max_khz > 0) writeInt(base + "/scaling_max_freq", max_khz);
        }
    }

    inline void cpuOnline(int cores) {
        // Core 0 is generally always online on Tegra. Control 1-3.
        for (int i = 1; i < 4; ++i) {
            std::string p = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/online";
            if (access(p.c_str(), W_OK) == 0) writeInt(p, (i < cores ? 1 : 0));
        }
    }

    inline void gpuSetFreqHz(long min_hz, long max_hz) {
        const std::string base = "/sys/devices/57000000.gpu/devfreq/57000000.gpu";
        if (access(base.c_str(), F_OK) == 0) {
            if (min_hz > 0) writeInt(base + "/min_freq", min_hz);
            if (max_hz > 0) writeInt(base + "/max_freq", max_hz);
        }
    }

    inline void emcSetFreqHz(long min_hz, long max_hz) {
        const std::string base = "/sys/kernel/nvpmodel_emc";
        if (access(base.c_str(), F_OK) == 0) {
             if (min_hz > 0) writeInt(base + "/min_freq", min_hz);
             if (max_hz > 0) writeInt(base + "/max_freq", max_hz);
        }
    }
    
    inline void jetsonClocks(bool enable) {
        if (enable) {
            const int rc = ::system("jetson_clocks > /dev/null 2>&1");
            (void)rc;
        } else {
            const int rc = ::system("jetson_clocks --restore > /dev/null 2>&1");
            (void)rc;
        }
        //if (enable) system("jetson_clocks > /dev/null 2>&1");
        //else system("jetson_clocks --restore > /dev/null 2>&1");
    }

    inline void nvpmodel(int mode) {
        std::string cmd = "nvpmodel -m " + std::to_string(mode) + " > /dev/null 2>&1";
        //system(cmd.c_str());
        const int rc = ::system(cmd.c_str());
        (void)rc;
    }
} // namespace Sysfs  <-- FIXED: Removed extra semicolon

//==================================================================================================
// 4. Thread Affinity Helpers
//==================================================================================================
inline bool setThreadAffinity(const std::vector<int>& cpus) {
    if (cpus.empty()) return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int c : cpus) if (c >= 0) CPU_SET(c, &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0;
}

inline std::vector<int> chooseCpus(int n, Affinity mode) {
    std::vector<int> v;
    v.reserve(n);
    if (mode == Affinity::Pack) {
        for (int i = 0; i < n; ++i) v.push_back(i % 4);
    } else { 
        for (int i = 0; i < n; ++i) {
            if (i == 0) v.push_back(0);
            else if (i == 1) v.push_back(2);
            else if (i == 2) v.push_back(1);
            else v.push_back(3);
        } 
    }
    return v;
}

//==================================================================================================
// 5. Scheduler Class
//==================================================================================================
class Scheduler : public IScheduler {
public:
    struct Limits {
        static constexpr int    max_cores      = 4;
        static constexpr long   cpu_min_khz    = 102000;
        static constexpr long   cpu_max_khz    = 1479000;
        static constexpr long   gpu_min_hz     = 76800000;    
        static constexpr long   gpu_max_hz     = 921600000;
        static constexpr long   emc_min_hz     = 204000000;
        static constexpr long   emc_max_hz     = 1600000000;
    };

    //explicit Scheduler(const Limits& lim = Limits()) : lim_(lim) {}

   

    // Updated constructor: accepts thermal config as second argument
    explicit Scheduler(const Limits& lim = Limits(),
                       const ThermalGovernor::Config& thermalCfg = ThermalGovernor::Config())
        : lim_(lim),
          thermalGovernor_(thermalCfg)   // Forward to governor
    {}

    // One-time platform envelope initialization. Expensive shell commands are
    // deliberately kept OUT of the fast policy-transition path. MAXN preserves
    // EMC/GPU bandwidth; jetson_clocks is restored once so per-resource DVFS
    // commands below remain effective.
    void initializePlatformEnvelope() {
        if (platform_envelope_initialized_) return;
        Sysfs::nvpmodel(0);
        Sysfs::jetsonClocks(false);
        nvpmodel_mode_ = 0;
        jetson_clocks_enabled_ = false;
        platform_envelope_initialized_ = true;
        spdlog::info("[Scheduler] Platform envelope pinned once: nvpmodel=0, jetson_clocks=OFF");
    }



    // Fast in-run recovery: restore a safe high-throughput resource state using
    // direct sysfs only. Never fork nvpmodel/jetson_clocks from the control loop.
    void recoverFast(RuntimeControls& rt) {
        initializePlatformEnvelope();
        Sysfs::cpuSetGovernorAll("performance");
        Sysfs::cpuOnline(Limits::max_cores);
        Sysfs::cpuSetFreqKHzAll(Limits::cpu_min_khz, Limits::cpu_max_khz);
        Sysfs::gpuSetFreqHz(Limits::gpu_min_hz, Limits::gpu_max_hz);

        rt.concurrency_level.store(Limits::max_cores, std::memory_order_relaxed);
        rt.affinity.store(Affinity::Spread, std::memory_order_relaxed);
        rt.enable_gpu.store(true, std::memory_order_relaxed);
        rt.gpu_workload_split.store(1.0, std::memory_order_relaxed);
        rt.commanded_cpu_max_freq_khz.store(Limits::cpu_max_khz, std::memory_order_relaxed);

        last_policy_ = PolicyMode::MAX_PERFORMANCE;
        last_core_count = Limits::max_cores;
        last_cpu_freq = Limits::cpu_max_khz;
        last_gpu_freq_val = Limits::gpu_max_hz;
        last_gpu_enabled_ = true;
        last_aff_mode = Affinity::Spread;
        setThreadAffinity(chooseCpus(Limits::max_cores, Affinity::Spread));
        spdlog::warn("[Scheduler] Fast recovery applied without platform-profile shell transition");
    }

void reset() override {
    // Set governor to "schedutil" (or "ondemand"), max frequency, all cores online,
    // GPU at max, nvpmodel 0, jetson_clocks true (or --restore).
    Sysfs::nvpmodel(0);
    Sysfs::jetsonClocks(true);
    Sysfs::cpuSetGovernorAll("schedutil");
    Sysfs::cpuOnline(4);
    Sysfs::cpuSetFreqKHzAll(Limits::cpu_min_khz, Limits::cpu_max_khz);
    Sysfs::gpuSetFreqHz(Limits::gpu_min_hz, Limits::gpu_max_hz);
    // Reset last cached states
    last_policy_ = PolicyMode::BALANCED;
    jetson_clocks_enabled_ = true;
    nvpmodel_mode_ = 0;
}

void apply(const RLAction& action, const MetricsSnapshot& m, RuntimeControls& rt) override {
    initializePlatformEnvelope();
    if (!m.valid) {
        spdlog::warn("[Scheduler] Metrics invalid ? using safe default (BALANCED, 30 FPS)");
        
        RLAction safeAction;
        safeAction.mode = PolicyMode::BALANCED;
        safeAction.target_fps = MaybeDouble(30.0);
        safeAction.prefer_gpu = MaybeBool(true);
        
        applyBalanced(safeAction, m, rt);
        return;
    }

    // === THERMAL GOVERNOR INTEGRATION ===
    ThermalGovernor::ThermalPolicy thermalPolicy = thermalGovernor_.update(m.cpu_temp_c, m.gpu_temp_c);

    if (thermalGovernor_.shouldShutdown(m.cpu_temp_c, m.gpu_temp_c)) {
        spdlog::critical("[Scheduler] THERMAL EMERGENCY ? Forcing LOW_POWER mode");
        RLAction emergencyAction = action;
        emergencyAction.mode = PolicyMode::LOW_POWER;
        emergencyAction.prefer_gpu = MaybeBool(false);
        emergencyAction.gpu_workload_split = MaybeDouble(0.0);
        rt.gpu_workload_split.store(0.0, std::memory_order_relaxed);
        applyGpuStateOnce(false);
        applyLowPower(emergencyAction, rt);
        return;
    }

    // Create thermally adjusted action
    RLAction adjusted = action;

    // Apply thermal constraints
    if (thermalPolicy.target_fps_scale < 1.0 && adjusted.target_fps.has) {
        adjusted.target_fps.value *= thermalPolicy.target_fps_scale;
        spdlog::debug("[Scheduler] Thermal scaling FPS target by {:.0f}%", 
                      thermalPolicy.target_fps_scale * 100.0);
    }

    if (!thermalPolicy.gpu_allowed) {
        adjusted.prefer_gpu = MaybeBool(false);
        adjusted.gpu_workload_split = MaybeDouble(0.0);
    }

    // Policy change detection (with thermal override awareness)
    // [P0] Pass GPU intent so LOW_POWER + prefer_gpu keeps EMC bandwidth (nvpmodel 0).
    bool gpuWanted = adjusted.prefer_gpu.has ? adjusted.prefer_gpu.value
                                             : (adjusted.mode != PolicyMode::LOW_POWER);

    // [Micro-scheduler] Publish the continuous GPU workload fraction the algorithm
    // reads each frame. Prefer the explicit split gene; fall back to the binary
    // prefer_gpu as its degenerate {0,1} case. Thermal WARNING+ disables the GPU,
    // so force the split to 0 (all CPU) when the governor has revoked GPU access.
    double split = adjusted.gpu_workload_split.has
        ? adjusted.gpu_workload_split.value
        : (gpuWanted ? 1.0 : 0.0);
    if (!thermalPolicy.gpu_allowed) split = 0.0;
    if (split < 0.0) split = 0.0;
    if (split > 1.0) split = 1.0;
    rt.gpu_workload_split.store(split, std::memory_order_relaxed);

    if (adjusted.mode != last_policy_) {
        spdlog::debug("[Scheduler] Policy transition {} -> {} (fast resource controls only)",
                      policyName(last_policy_), policyName(adjusted.mode));
        last_policy_ = adjusted.mode;
    }

    // === Dispatch to mode handlers ===
    switch (adjusted.mode) {
        case PolicyMode::MAX_PERFORMANCE:
            applyGpuStateOnce(gpuWanted);
            applyMaxPerf(adjusted, rt);
            break;

        case PolicyMode::LOW_POWER:
            // [P0] Do NOT force GPU off here. Honour the evolved prefer_gpu gene so
            // a low-power genome can still use the GPU path. Forcing false previously
            // meant GR3D utilisation stayed ~0% even when prefer_gpu=true was selected.
            applyGpuStateOnce(gpuWanted);
            applyLowPower(adjusted, rt);
            break;

        case PolicyMode::BALANCED:
            applyBalanced(adjusted, m, rt);   // Note: still passes original m for PID logic
            break;

        case PolicyMode::UNKNOWN:
            spdlog::warn("[Scheduler] Unknown policy mode ? falling back to BALANCED");
            applyBalanced(adjusted, m, rt);
            break;
    }

    // Optional: Log thermal state periodically
    static int logCounter = 0;
    if (++logCounter % 8 == 0) {
        spdlog::info("[Thermal] Stage={} | Temp CPU/GPU={:.1f}/{:.1f}°C | GPU Allowed={}",
                     static_cast<int>(thermalGovernor_.getCurrentStage()),
                     m.cpu_temp_c, m.gpu_temp_c, thermalPolicy.gpu_allowed);
    }
}

//     //============================================================================
//     // OLD 
//     void apply(const RLAction& action, const MetricsSnapshot& m, RuntimeControls& rt) override {
//         if (!m.valid) {
//             // FIX: Instead of skipping, use a safe default action
//             spdlog::warn("[Scheduler] Metrics invalid ? using safe default (BALANCED, 30 FPS)");
            
//             RLAction safeAction;
//             safeAction.mode = PolicyMode::BALANCED;
//             safeAction.target_fps = MaybeDouble(30.0);
//             safeAction.prefer_gpu = MaybeBool(true);
            
//             // Continue with safe action instead of returning
//             applyBalanced(safeAction, m, rt);   // or call your balanced logic directly
//             return;        
//         }
        
//         if (action.mode != last_policy_) {
//             applyPowerProfileOnce(action.mode);
//             last_policy_ = action.mode;
//         }

//         switch (action.mode) {
//             case PolicyMode::MAX_PERFORMANCE: applyGpuStateOnce(true); applyMaxPerf(action, rt); break;
//             case PolicyMode::LOW_POWER:
//                 applyGpuStateOnce(false);
//                 applyLowPower(action, rt);
//                 break;

//             case PolicyMode::BALANCED:        applyBalanced(action, m, rt); break;
//             case PolicyMode::UNKNOWN: /* log error */ break;
//         }
// }

// //=========================================================================


private:
    Limits lim_;
 //   ThermalGovernor thermalGovernor_;   // Will be constructed with custom config
    
    bool last_gpu_enabled_ = true; // Default assumption  // actual HW state

    std::chrono::steady_clock::time_point last_gpu_transition_;


   // bool last_gpu_state_ = false;
   // bool last_gpu_state = !current_gpu_req; // Force update on first run
    int last_core_count = -1;
    long last_cpu_freq = -1;
    long last_gpu_freq_val = -1;
    Affinity last_aff_mode = Affinity::Pack; 


     // ---- Cached HW State (Fork-Storm Killer) ----
     // Add Cached Power State (Class Members)
    ThermalGovernor thermalGovernor_;   // Thermal management
    PolicyMode last_policy_ = PolicyMode::BALANCED;
    bool jetson_clocks_enabled_ = false;
    int nvpmodel_mode_ = -1;   // invalid sentinel
    bool platform_envelope_initialized_ = false;

    void applyMaxPerf(const RLAction& action, RuntimeControls& rt) {
        rt.concurrency_level.store(Limits::max_cores, std::memory_order_relaxed);
        rt.affinity.store(Affinity::Spread, std::memory_order_relaxed);
        const bool gpuPref = action.prefer_gpu.has ? action.prefer_gpu.value : true;
        rt.enable_gpu.store(gpuPref, std::memory_order_relaxed);

        const long cpu_cap = action.cpu_max_freq_khz.has
            ? local_clamp(static_cast<long>(action.cpu_max_freq_khz.value),
                          Limits::cpu_min_khz, Limits::cpu_max_khz)
            : Limits::cpu_max_khz;

        Sysfs::cpuSetGovernorAll("performance");
        Sysfs::cpuOnline(Limits::max_cores);
        if (cpu_cap != last_cpu_freq) {
            Sysfs::cpuSetFreqKHzAll(cpu_cap, cpu_cap);
            last_cpu_freq = cpu_cap;
        }
        rt.commanded_cpu_max_freq_khz.store(cpu_cap, std::memory_order_relaxed);

        const long gpu_freq = gpuPref ? Limits::gpu_max_hz : Limits::gpu_min_hz;
        if (gpu_freq != last_gpu_freq_val) {
            Sysfs::gpuSetFreqHz(gpu_freq, gpu_freq);
            last_gpu_freq_val = gpu_freq;
        }

        last_core_count = Limits::max_cores;
        last_aff_mode = Affinity::Spread;
        setThreadAffinity(chooseCpus(Limits::max_cores, Affinity::Spread));
    }

void applyLowPower(const RLAction& action, RuntimeControls& rt) {
        const double budget = action.power_budget_watts.has ? action.power_budget_watts.value : 4.0;
        const int cores = (budget < 3.5) ? 1 : 2;
        const bool gpuPref = action.prefer_gpu.has ? action.prefer_gpu.value : false;

        rt.concurrency_level.store(cores, std::memory_order_relaxed);
        rt.affinity.store(Affinity::Pack, std::memory_order_relaxed);
        rt.enable_gpu.store(gpuPref, std::memory_order_relaxed);

        const long cpu_cap = action.cpu_max_freq_khz.has
            ? local_clamp(static_cast<long>(action.cpu_max_freq_khz.value), 460800L, Limits::cpu_max_khz)
            : 1020000L;

        // Sysfs::cpuSetGovernorAll("powersave");
        // if (cores != last_core_count) {
        //     Sysfs::cpuOnline(cores);
        //     last_core_count = cores;
        // }
        // if (cpu_cap != last_cpu_freq) {
        //     Sysfs::cpuSetFreqKHzAll(Limits::cpu_min_khz, cpu_cap);
        //     last_cpu_freq = cpu_cap;
        // }
        // rt.commanded_cpu_max_freq_khz.store(cpu_cap, std::memory_order_relaxed);

        // // GPU-preferred low-power points keep the GPU available but at its minimum
        // // clock; CPU-only points also keep it at minimum. Runtime enable_gpu is the
        // // execution-path authority.
        // if (last_gpu_freq_val != Limits::gpu_min_hz) {
        //     Sysfs::gpuSetFreqHz(Limits::gpu_min_hz, Limits::gpu_min_hz);
        //     last_gpu_freq_val = Limits::gpu_min_hz;
        // }
        // [FIX P3] Bounded utilisation-driven governor instead of "powersave"
        // (which floors every core to 102 MHz and made the executed state
        // unrepresentable in the surrogate's config space).
        Sysfs::cpuSetGovernorAll(Sysfs::pickBoundedGovernor());
        if (cores != last_core_count) {
            Sysfs::cpuOnline(cores);
            last_core_count = cores;
        }
        if (cpu_cap != last_cpu_freq) {
            Sysfs::cpuSetFreqKHzAll(Limits::cpu_min_khz, cpu_cap);
            last_cpu_freq = cpu_cap;
        }
        rt.commanded_cpu_max_freq_khz.store(cpu_cap, std::memory_order_relaxed);

        // [FIX P3] Honour the evolved GPU preference with a usable clock: pinning
        // the GPU to min made prefer_gpu=true low-power genomes fictional (the
        // surrogate's gpu=on feature never corresponded to a working GPU).
        const long gpu_freq = gpuPref ? Limits::gpu_max_hz : Limits::gpu_min_hz;
        if (last_gpu_freq_val != gpu_freq) {
            Sysfs::gpuSetFreqHz(Limits::gpu_min_hz, gpu_freq);
            last_gpu_freq_val = gpu_freq;
        }

        last_aff_mode = Affinity::Pack;
        setThreadAffinity(chooseCpus(cores, Affinity::Pack));
    }


/*
/home/ansorosa/Desktop/CODE_REPOSITORIO/ERL_Stage_1_Framework_88_Testing_ERL_08072026/Module/Scheduler.h:369:29: error: clamp is not a member of std
  369 |                      ? std::clamp(static_cast<long>(action.cpu_max_freq_khz.value), 460800L, Limits::cpu_max_khz)
      |                             ^~~~~
In file included from /home/ansorosa/Desktop/CODE_REPOSITORIO/ERL_Stage_1_Framework_88_Testing_ERL_08072026/Module/Modules.h:62,

*/
    // void applyLowPower(const RLAction& action, RuntimeControls& rt) {
    //     double budget = action.power_budget_watts.has ? action.power_budget_watts.value : 4.0;
    //     int cores = (budget < 3.5) ? 1 : 2; 

    //     rt.concurrency_level.store(cores);
    //     rt.affinity.store(Affinity::Pack); 
    //     rt.enable_gpu.store(action.prefer_gpu.has ? action.prefer_gpu.value : false);

    //     // [P0] Honour the evolved CPU max-frequency gene if present, otherwise fall
    //     // back to the legacy 1020000 kHz low-power cap. Clamped to hardware limits.
    //     long cpu_cap = action.cpu_max_freq_khz.has
    //         ? local_clamp(static_cast<long>(action.cpu_max_freq_khz.value),
    //                       Limits::cpu_min_khz, Limits::cpu_max_khz)
    //         : 1020000L;

    //     // Sysfs::jetsonClocks(false);
    //     // Sysfs::nvpmodel(1);
    //     Sysfs::cpuSetGovernorAll("powersave");
    //     Sysfs::cpuOnline(cores);
    //     Sysfs::cpuSetFreqKHzAll(Limits::cpu_min_khz, cpu_cap); 
    //     Sysfs::gpuSetFreqHz(Limits::gpu_min_hz, Limits::gpu_min_hz); 
        
    //     setThreadAffinity(chooseCpus(cores, rt.affinity.load()));
    // }

    // void applyBalanced(const RLAction& a, const MetricsSnapshot& m, RuntimeControls& rt) {
    //     const double target = a.target_fps.has ? a.target_fps.value : 30.0;
    //     const double err = target - m.fps;
        
    //     int curC = rt.concurrency_level.load();
    //     int nextC = curC;
    //     if (err > 2.0) nextC++;        
    //     if (err < -5.0) nextC--;       
    //     nextC = local_clamp(nextC, 1, Limits::max_cores);
    //     rt.concurrency_level.store(nextC);

    //     if (m.cpu_util_avg > 85.0 && m.fps < target) 
    //         rt.affinity.store(Affinity::Spread);
    //     else 
    //         rt.affinity.store(Affinity::Pack);
        
    //     bool gpu = rt.enable_gpu.load();
    //     if (m.fps < target - 2.0 && m.gpu_util_avg < 60.0) gpu = true;
    //     if (m.fps > target + 5.0 && m.avg_power_w_alg > 5.5) gpu = false;
    //     if (a.prefer_gpu.has) gpu = a.prefer_gpu.value;
    //     rt.enable_gpu.store(gpu);

    //     Sysfs::jetsonClocks(false);
    //     Sysfs::nvpmodel(1); 
    //     Sysfs::cpuSetGovernorAll("schedutil"); 
    //     Sysfs::cpuOnline(nextC);

    //     long cpu_cap = (err > 0.0) ? Limits::cpu_max_khz : static_cast<long>(Limits::cpu_max_khz * 0.7);
    //     Sysfs::cpuSetFreqKHzAll(Limits::cpu_min_khz, cpu_cap);

    //     long gpu_freq = gpu ? Limits::gpu_max_hz : Limits::gpu_min_hz;
    //     Sysfs::gpuSetFreqHz(Limits::gpu_min_hz, gpu_freq);

    //     setThreadAffinity(chooseCpus(nextC, rt.affinity.load()));
    // }

    // void applyBalanced(const RLAction& a, const MetricsSnapshot& m, RuntimeControls& rt) {
    //     // ---------------------------------------------------------
    //     // 1. CALCULATION PHASE (Pure Math - No System Calls)
    //     // ---------------------------------------------------------
    //     const double target = a.target_fps.has ? a.target_fps.value : 30.0;
    //     const double err = target - m.fps;
        
    //     // Calculate needed cores
    //     int curC = rt.concurrency_level.load();
    //     int nextC = curC;
    //     if (err > 2.0) nextC++;         
    //     if (err < -5.0) nextC--;        
    //     nextC = local_clamp(nextC, 1, Limits::max_cores);
    //     rt.concurrency_level.store(nextC);

    //     // Calculate Affinity Strategy
    //     Affinity nextAffinity;
    //     if (m.cpu_util_avg > 85.0 && m.fps < target) 
    //         nextAffinity = Affinity::Spread;
    //     else 
    //         nextAffinity = Affinity::Pack;
    //     rt.affinity.store(nextAffinity);
        
    //     // Calculate GPU State
    //     bool current_gpu_req = rt.enable_gpu.load();
    //     if (m.fps < target - 2.0 && m.gpu_util_avg < 60.0) current_gpu_req = true;
    //     if (m.fps > target + 5.0 && m.avg_power_w_alg > 5.5) current_gpu_req = false;
    //     if (a.prefer_gpu.has) current_gpu_req = a.prefer_gpu.value;
    //     rt.enable_gpu.store(current_gpu_req);

    //     // Calculate Frequencies
    //     long cpu_cap = (err > 0.0) ? Limits::cpu_max_khz : static_cast<long>(Limits::cpu_max_khz * 0.7);
    //     long gpu_freq = current_gpu_req ? Limits::gpu_max_hz : Limits::gpu_min_hz;

    //     // ---------------------------------------------------------
    //     // 2. ACTUATION PHASE (State Caching / Fork Storm Fix)
    //     // [FIX] We use static vars to remember the LAST applied state.
    //     // We only call the expensive Sysfs:: functions if state CHANGED.
    //     // ---------------------------------------------------------
    //     // static bool last_gpu_state = !current_gpu_req; // Force update on first run
    //     //last_gpu_state_= !current_gpu_req; // Force update on first run
    //     // static int last_core_count = -1;
    //     // static long last_cpu_freq = -1;
    //     // static long last_gpu_freq_val = -1;
    //     // static Affinity last_aff_mode = Affinity::Pack; 

    //     // // A. GPU & Power Model (Heavy - Only update on change)
    //     // if (current_gpu_req != last_gpu_state_) {
    //     //     // Sysfs::jetsonClocks(false); // Ensure we aren't locked
    //     //     // Sysfs::nvpmodel(1);         // 5W Mode (Balanced)
    //     //     last_gpu_state_ = current_gpu_req;
    //     // }
    //     applyGpuStateOnce(current_gpu_req);


    //     // B. CPU Governor (Only set once or if we suspect it changed)
    //     // We can do this periodically or just once. For now, doing it on core change is safe.
    //     if (nextC != last_core_count) {
    //          Sysfs::cpuSetGovernorAll("schedutil"); 
    //          Sysfs::cpuOnline(nextC);
    //          last_core_count = nextC;
             
    //          // If core count changes, we must re-apply thread affinity
    //          setThreadAffinity(chooseCpus(nextC, nextAffinity));
    //          last_aff_mode = nextAffinity;
    //     }
    //     else if (nextAffinity != last_aff_mode) {
    //          // If only affinity changed (but not core count)
    //          setThreadAffinity(chooseCpus(nextC, nextAffinity));
    //          last_aff_mode = nextAffinity;
    //     }

    //     // C. CPU Frequencies (Apply Deadband to prevent jitter)
    //     // Only update if requested freq changed by > 100 MHz
    //     if (std::abs(cpu_cap - last_cpu_freq) > 100000) { 
    //         Sysfs::cpuSetFreqKHzAll(Limits::cpu_min_khz, cpu_cap);
    //         last_cpu_freq = cpu_cap;
    //     }

    //     // D. GPU Frequencies
    //     if (gpu_freq != last_gpu_freq_val) {
    //         Sysfs::gpuSetFreqHz(Limits::gpu_min_hz, gpu_freq);
    //         last_gpu_freq_val = gpu_freq;
    //     }
    // }

    void applyBalanced(const RLAction& a, const MetricsSnapshot& m, RuntimeControls& rt) {
    // ---------------------------------------------------------
    // 1. CALCULATION PHASE (Pure Math - No System Calls)
    // ---------------------------------------------------------
    const double target = a.target_fps.has ? a.target_fps.value : 30.0;
    const double err = target - m.fps;
   
    // Calculate needed cores
    int curC = rt.concurrency_level.load();
    int nextC = curC;
    if (err > 2.0) nextC++;
    if (err < -5.0) nextC--;
    nextC = local_clamp(nextC, 1, Limits::max_cores);
    rt.concurrency_level.store(nextC);
    // Calculate Affinity Strategy
    Affinity nextAffinity;
    if (m.cpu_util_avg > 85.0 && m.fps < target)
        nextAffinity = Affinity::Spread;
    else
        nextAffinity = Affinity::Pack;
    rt.affinity.store(nextAffinity);
   
    // Calculate GPU State
    bool current_gpu_req = rt.enable_gpu.load();
    if (m.fps < target - 2.0 && m.gpu_util_avg < 60.0) current_gpu_req = true;
    if (m.fps > target + 5.0 && m.avg_power_w_alg > 5.5) current_gpu_req = false;
    if (a.prefer_gpu.has) current_gpu_req = a.prefer_gpu.value;
    rt.enable_gpu.store(current_gpu_req);
    // Calculate Frequencies
    long cpu_cap = (err > 0.0) ? Limits::cpu_max_khz : static_cast<long>(Limits::cpu_max_khz * 0.7);
    long gpu_freq = current_gpu_req ? Limits::gpu_max_hz : Limits::gpu_min_hz;

    // [P0] If the evolved gene specifies a max frequency, use it as an upper
    // ceiling on the PID-derived cap (the controller can still go lower).
    if (a.cpu_max_freq_khz.has) {
        long gene_cap = local_clamp(static_cast<long>(a.cpu_max_freq_khz.value),
                                    Limits::cpu_min_khz, Limits::cpu_max_khz);
        cpu_cap = std::min(cpu_cap, gene_cap);
    }

    // Temp Throttling: If m.cpu_temp_c >75, reduce max_khz by 20%.
    // [ADD] Thermal Guard - Reduce freq if hot (prevents hardware throttle)
    // Thresholds: Warn at 75°C, cap at 80°C (Nano trips at ~100°C, but we proactive)
    if (m.cpu_temp_c > 75.0) {
        spdlog::warn("[Scheduler] CPU Temp {}°C >75 ? Throttling CPU by 20%", m.cpu_temp_c);
        cpu_cap = static_cast<long>(cpu_cap * 0.8); // 20% reduction
        cpu_cap = local_clamp(cpu_cap, Limits::cpu_min_khz, Limits::cpu_max_khz);
    }
    if (m.cpu_temp_c > 80.0) {
        spdlog::warn("[Scheduler] CPU Temp {}°C >80 ? Aggressive Throttle (50%)", m.cpu_temp_c);
        cpu_cap = static_cast<long>(cpu_cap * 0.5); // Additional 50% if critical
        cpu_cap = local_clamp(cpu_cap, Limits::cpu_min_khz, Limits::cpu_max_khz);
    }
    // GPU Temp Guard (symmetric)
    if (m.gpu_temp_c > 75.0) {
        spdlog::warn("[Scheduler] GPU Temp {}°C >75 ? Throttling GPU by 20%", m.gpu_temp_c);
        gpu_freq = static_cast<long>(gpu_freq * 0.8);
        gpu_freq = local_clamp(gpu_freq, Limits::gpu_min_hz, Limits::gpu_max_hz);
    }
    if (m.gpu_temp_c > 80.0) {
        spdlog::warn("[Scheduler] GPU Temp {}°C >80 ? Aggressive Throttle (50%)", m.gpu_temp_c);
        gpu_freq = static_cast<long>(gpu_freq * 0.5);
        gpu_freq = local_clamp(gpu_freq, Limits::gpu_min_hz, Limits::gpu_max_hz);
    }

    // ---------------------------------------------------------
    // 2. ACTUATION PHASE (State Caching / Fork Storm Fix)
    // [FIX] We use static vars to remember the LAST applied state.
    // We only call the expensive Sysfs:: functions if state CHANGED.
    // ---------------------------------------------------------
    // static bool last_gpu_state = !current_gpu_req; // Force update on first run
    //last_gpu_state_= !current_gpu_req; // Force update on first run
    // static int last_core_count = -1;
    // static long last_cpu_freq = -1;
    // static long last_gpu_freq_val = -1;
    // static Affinity last_aff_mode = Affinity::Pack;
    // // A. GPU & Power Model (Heavy - Only update on change)
    // if (current_gpu_req != last_gpu_state_) {
    // // Sysfs::jetsonClocks(false); // Ensure we aren't locked
    // // Sysfs::nvpmodel(1); // 5W Mode (Balanced)
    // last_gpu_state_ = current_gpu_req;
    // }
    applyGpuStateOnce(current_gpu_req);
    // B. CPU Governor (Only set once or if we suspect it changed)
    // We can do this periodically or just once. For now, doing it on core change is safe.
    if (nextC != last_core_count) {
         Sysfs::cpuSetGovernorAll("schedutil");
         Sysfs::cpuOnline(nextC);
         last_core_count = nextC;
        
         // If core count changes, we must re-apply thread affinity
         setThreadAffinity(chooseCpus(nextC, nextAffinity));
         last_aff_mode = nextAffinity;
    }
    else if (nextAffinity != last_aff_mode) {
         // If only affinity changed (but not core count)
         setThreadAffinity(chooseCpus(nextC, nextAffinity));
         last_aff_mode = nextAffinity;
    }
    // C. CPU Frequencies (Apply Deadband to prevent jitter)
    // Only update if requested freq changed by > 100 MHz
    if (std::abs(cpu_cap - last_cpu_freq) > 100000) {
        Sysfs::cpuSetFreqKHzAll(Limits::cpu_min_khz, cpu_cap);
        last_cpu_freq = cpu_cap;
    }
    const long effectiveCpuCap = (last_cpu_freq > 0) ? last_cpu_freq : cpu_cap;
    rt.commanded_cpu_max_freq_khz.store(effectiveCpuCap, std::memory_order_relaxed);
    // D. GPU Frequencies
    if (gpu_freq != last_gpu_freq_val) {
        Sysfs::gpuSetFreqHz(Limits::gpu_min_hz, gpu_freq);
        last_gpu_freq_val = gpu_freq;
    }
}


    void applyPowerProfileOnce(PolicyMode mode, bool gpuRequested = true) {
        // Retained for source compatibility only. Platform profiles are pinned once
        // by initializePlatformEnvelope(); fast-loop policy transitions must never
        // fork nvpmodel/jetson_clocks.
        (void)mode;
        (void)gpuRequested;
    }

 

void applyGpuStateOnce(bool enable) {
    auto now = std::chrono::steady_clock::now();

    if (enable == last_gpu_enabled_)
        return;

    if (!last_gpu_transition_.time_since_epoch().count() ||
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_gpu_transition_).count() > 200) {

        if (enable)
            Sysfs::gpuSetFreqHz(Limits::gpu_min_hz, Limits::gpu_max_hz);
        else
            Sysfs::gpuSetFreqHz(Limits::gpu_min_hz, Limits::gpu_min_hz);

        last_gpu_enabled_ = enable;
        last_gpu_transition_ = now;
    }
}




};

} // namespace hrl
