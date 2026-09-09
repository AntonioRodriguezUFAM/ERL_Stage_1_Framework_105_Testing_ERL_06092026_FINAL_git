// LynsynMonitorConcrete_new.h
// PRODUCTION VERSION - FIFO-draining + Wall-clock CSV + Robust Recovery + Clock Sync Fixes

#pragma once

#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO

#include "../Interfaces/ILynsynMonitor.h"
#include "../Interfaces/ISystemMetricsAggregator.h"
#include "../SharedStructures/ThreadManager.h"
#include "../SharedStructures/allModulesStatcs.h"
#include "../SharedStructures/LynsynMonitorConfig.h"
#include "../../Module/PowerSanity.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <iomanip>
#include <cmath>

#if __cplusplus >= 201703L
    #include <filesystem>
    namespace fs = std::filesystem;
#else
    #include <experimental/filesystem>
    namespace fs = std::experimental::filesystem;
#endif

#include <spdlog/spdlog.h>
#include <libusb.h>

extern "C" {
    #include "../Stage_01/Includes/lynsyn.h"
    int lynsyn_reset_device(void);
}

class LynsynMonitorConcrete final : public ILynsynMonitor {
public:
    explicit LynsynMonitorConcrete(std::shared_ptr<ISystemMetricsAggregator> aggregator,
                                   ThreadManager& threadManager)
        : initialized_(false),
          running_(false),
          threadManager_(threadManager),
          threadShouldExit_(false),
          metricAggregator_(std::move(aggregator)),
          enabled_(true),
          lynsynBaseTimeS_(0.0),
          lastLynsynTimeS_(0.0) {
        spdlog::debug("[LynsynMonitorConcrete] ctor");
    }

    ~LynsynMonitorConcrete() override { stop(); }

    bool isRunning() const noexcept { return running_.load(); }

    bool configure(const LynsynMonitorConfig& config) override {
        config_ = config;
        return config_.validate();
    }

    bool initialize() override {
        std::lock_guard<std::mutex> g(lifecycleMutex_);
        if (initialized_.load(std::memory_order_acquire)) return true;

        enabled_ = true;
        threadShouldExit_.store(false, std::memory_order_relaxed);

        spdlog::info("[LYNSYN] Starting robust initialization");

        constexpr int MAX_ATTEMPTS = 6;
        constexpr auto RETRY_DELAY = std::chrono::milliseconds(900);

        for (int attempt = 1; attempt <= MAX_ATTEMPTS; ++attempt) {
            spdlog::info("[LYNSYN INIT] Attempt {}/{}", attempt, MAX_ATTEMPTS);
            fullResetUnsafe();

            if (lynsyn_init()) {
                initialized_.store(true, std::memory_order_release);
                spdlog::info("[LYNSYN] SUCCESS on attempt {}", attempt);

                LynsynSample sample{};
                if (lynsyn_getAvgSample(&sample, 0.2, config_.coreMask)) {
                    spdlog::info("[LYNSYN] Live sample verified. Sensor0={:.3f}A", sample.current[0]);
                    return true;
                }
            }

            spdlog::warn("[LYNSYN INIT] Attempt {} failed", attempt);
            std::this_thread::sleep_for(RETRY_DELAY);
        }

        enabled_ = false;
        initialized_.store(false, std::memory_order_release);
        spdlog::critical("[LYNSYN] Failed after {} attempts.", MAX_ATTEMPTS);
        return false;
    }

    void startMonitoring() override {
        std::lock_guard<std::mutex> g(lifecycleMutex_);
        if (!initialized_ || running_) return;

        // Reset state anchors for new monitoring session
        firstSampleReceived_.store(false, std::memory_order_release);
        sanityRejects_ = 0;

        // CSV Setup
        if (!config_.outputCSV.empty()) {
            fs::path p(config_.outputCSV);
            if (p.has_parent_path()) {
                fs::create_directories(p.parent_path());
            }

            outputFile_.open(config_.outputCSV, std::ios::out | std::ios::trunc);
            if (outputFile_.is_open()) {
                outputFile_ << "device_time_s,system_time_us,pc0,pc1,pc2,pc3,"
                            << "i0,v0,i1,v1,i2,v2,i3,v3,i4,v4,i5,v5,i6,v6,n_avg,energy_j,avg_power_w_trap\n";
                outputFile_.flush();
                spdlog::info("[Lynsyn] CSV logging enabled: {}", config_.outputCSV);
            }
        }

        armSamplingModeWithPeriodMs_(config_.sampleRateMs);

        threadShouldExit_ = false;
        running_ = true;

        threadManager_.addThread(Component::Lynsyn,
            std::thread(&LynsynMonitorConcrete::monitoringThreadLoop_, this));

        spdlog::info("[Lynsyn] Monitoring started (target {} ms)", config_.sampleRateMs);
    }

    void stop() override {
        threadShouldExit_ = true;

        {
            std::lock_guard<std::mutex> g(lifecycleMutex_);
            if (running_) {
                threadManager_.joinThreadsFor(Component::Lynsyn);
                running_ = false;
            }
        }

        if (outputFile_.is_open()) {
            outputFile_.flush();
            outputFile_.close();
        }

        {
            std::lock_guard<std::mutex> g(lifecycleMutex_);
            if (initialized_) {
                lynsyn_reset_device();
                lynsyn_release();
                initialized_ = false;
            }
        }

        spdlog::info("[Lynsyn] Monitoring stopped");
    }

    void setSampleCallback(std::function<void(const LynsynSample&)> cb) override {
        sampleCallback_ = std::move(cb);
    }

    void setErrorCallback(std::function<void(const std::string&)> cb) override {
        errorCallback_ = std::move(cb);
    }

private:
    void fullResetUnsafe() {
        spdlog::debug("[Lynsyn] Performing surgical device reset");
        lynsyn_release();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        lynsyn_reset_device();
        spdlog::debug("[Lynsyn] Full reset completed");
    }

    void fullResetThreadSafe() {
        std::lock_guard<std::mutex> g(lifecycleMutex_);
        fullResetUnsafe();
    }

    void monitoringThreadLoop_() {
        spdlog::info("[Lynsyn] Monitoring thread started (target {} ms)", config_.sampleRateMs);
        if (!enabled_) {
            running_.store(false);
            return;
        }

        const int64_t sr_ms = std::max<int64_t>(1, config_.sampleRateMs);
        const auto sampleInterval = std::chrono::milliseconds(sr_ms);

        armSamplingModeWithPeriodMs_(static_cast<int>(sr_ms));

        constexpr int MAX_CONSECUTIVE_FAILS = 10;
        constexpr std::chrono::milliseconds BASE_BACKOFF{200};
        int consecutiveFails = 0;
        size_t sampleCount = 0;

        using clock = std::chrono::steady_clock;
        auto nextWake = clock::now() + sampleInterval;

        // [SAMPLING-FIX] Software decimation state. The hardware ignores
        // sampleRateMs (no period API): the 2026-07-20 run measured
        // 3,543,025 samples / 1,813 s = ~1,954 Hz free-run, i.e. ~2,000
        // aggregator-mutex ops/s and ~6,000 heap allocs/s. Raw samples now
        // accumulate per wake and ONE averaged sample per sampleRateMs is
        // emitted to the aggregator / callback / CSV (~200x less traffic;
        // window-integration accuracy preserved, it consumes means anyway).
        double   accV[4] = {0,0,0,0};
        double   accC[4] = {0,0,0,0};
        uint32_t accN = 0;
        LynsynSample lastRaw{};
        // [TRAP] Incremental trapezoidal energy over RAW samples (device
        // clock, pre-decimation): E = sum 0.5*(P_k+P_{k-1})*dt. prev* carry
        // ACROSS windows (no inter-window undercount); reset only on HALT.
        double trapEnergyJ  = 0.0;
        double trapPrevP    = -1.0;
        double trapPrevDevS = 0.0;
        double winStartDevS = -1.0;
        uint64_t emitted = 0, rawRejects = 0, lastStatSamples = 0;
        auto lastStatTime = clock::now();
        bool csvFailedLogged = false;
        constexpr size_t kMaxDrainPerWake = 20000;

        while (!threadShouldExit_) {
            std::this_thread::sleep_until(nextWake);
            nextWake += sampleInterval;

            bool anySuccess = false;
            LynsynSample latest{};
            size_t drainedThisWake = 0;

            // FIFO DRAIN - [SAMPLING-FIX] accumulate raw; emit once per wake.
            while (lynsyn_getNextSample(&latest)) {
                anySuccess = true;
                consecutiveFails = 0;
                ++sampleCount;

                if (latest.flags & SAMPLE_FLAG_HALTED) {
                    spdlog::info("[Lynsyn] HALT detected - re-arming");
                    accN = 0;              // discard partial window
                    trapEnergyJ = 0.0;     // [TRAP] device clock restarts
                    trapPrevP = -1.0;
                    winStartDevS = -1.0;
                    firstSampleReceived_.store(false, std::memory_order_release);  // re-anchor sync
                    if (config_.periodSampling) {
                        armSamplingModeWithPeriodMs_(static_cast<int>(sr_ms));
                        nextWake = clock::now() + sampleInterval;
                    }
                    break;
                }

                if (++drainedThisWake > kMaxDrainPerWake) {
                    spdlog::warn("[Lynsyn] Drain cap hit ({} samples in one wake) - deferring rest",
                                 drainedThisWake);
                    break;
                }

                // Crude per-raw spike filter (2x PowerSanity headroom): one
                // absurd sample must not shift the window mean (~+2 W per
                // 400 W glitch in a 200-sample window) or poison the energy
                // integral before the real gate below sees it.
                double rawW = 0.0;
                for (int i = 0; i < 4; ++i)
                    rawW += static_cast<double>(latest.voltage[i]) *
                            static_cast<double>(latest.current[i]);
                if (!std::isfinite(rawW) || rawW < 0.0 || rawW > 2.0 * hrl::PowerSanity::kMaxW) {
                    ++rawRejects;
                    continue;
                }

                // [TRAP] Trapezoidal step on the raw stream (reuses rawW).
                {
                    const double devSRaw = lynsyn_cyclesToSeconds(latest.time);
                    if (trapPrevP >= 0.0) {
                        const double dtS = devSRaw - trapPrevDevS;
                        if (dtS > 0.0 && dtS < 0.5) {   // discontinuity guard
                            trapEnergyJ += 0.5 * (rawW + trapPrevP) * dtS;
                        }
                    }
                    trapPrevP    = rawW;
                    trapPrevDevS = devSRaw;
                    if (winStartDevS < 0.0) winStartDevS = devSRaw;
                }

                for (int i = 0; i < 4; ++i) {
                    accV[i] += static_cast<double>(latest.voltage[i]);
                    accC[i] += static_cast<double>(latest.current[i]);
                }
                lastRaw = latest;
                ++accN;
            }

            // [SAMPLING-FIX] One averaged emission per wake (= per sampleRateMs).
            if (accN > 0) {
                LynsynSample avg = lastRaw;   // time / pc / flags from newest raw
                for (int i = 0; i < 4; ++i) {
                    avg.voltage[i] = accV[i] / static_cast<double>(accN);
                    avg.current[i] = accC[i] / static_cast<double>(accN);
                }

                PowerStats stats = convertLynsynSampleToPowerStats_(avg);

                // [P0-F15] Physical-plausibility gate on the averaged sample.
                const double totalW = stats.totalPower();
                if (!hrl::PowerSanity::valid(totalW)) {
                    if (++sanityRejects_ % 10 == 1) {
                        spdlog::warn("[Lynsyn] PowerSanity rejected sample "
                                     "{:.3f}W ({}) - {} rejected so far",
                                     totalW,
                                     hrl::PowerSanity::reject_reason(totalW),
                                     sanityRejects_);
                    }
                } else {
                    if (metricAggregator_) metricAggregator_->pushPowerStats(stats);
                    if (sampleCallback_) sampleCallback_(avg);

                    if (outputFile_.is_open() && !csvFailedLogged) {
                        const double devSec = lynsyn_cyclesToSeconds(avg.time);
                        const auto sys_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                                std::chrono::system_clock::now().time_since_epoch()).count();
                        const double winDurS =
                            (winStartDevS >= 0.0 && trapPrevDevS > winStartDevS)
                                ? (trapPrevDevS - winStartDevS) : 0.0;
                        const double trapAvgW = (winDurS > 0.0) ? (trapEnergyJ / winDurS) : totalW;

                        outputFile_ << std::fixed << std::setprecision(6) << devSec << ',' << sys_us;
                        for (int i = 0; i < 4; ++i) outputFile_ << ',' << avg.pc[i];
                        // Pairs 0..3 averaged; 4..6 pass through (schema kept).
                        for (int i = 0; i < 7; ++i)
                            outputFile_ << ',' << avg.current[i] << ',' << avg.voltage[i];
                        // [TRAP] energy_j / avg_power_w_trap are CANONICAL for
                        // energy (true raw-rate integral); the averaged stats'
                        // totalPower understates by the V-I covariance.
                        outputFile_ << ',' << accN << ',' << trapEnergyJ << ',' << trapAvgW << '\n';

                        ++emitted;
                        if (emitted % 10 == 0) outputFile_.flush();
                        if (!outputFile_) {
                            csvFailedLogged = true;
                            spdlog::error("[Lynsyn] CSV stream FAILED (failbit) after {} rows - "
                                          "path '{}'. Run-1's 562KB-vs-3.5M-samples anomaly is "
                                          "consistent with exactly this silent failure.",
                                          emitted, config_.outputCSV);
                        }
                    }
                }
                accN = 0;
                for (int i = 0; i < 4; ++i) { accV[i] = 0.0; accC[i] = 0.0; }
                trapEnergyJ  = 0.0;
                winStartDevS = -1.0;
            }

            // Ingest statistics every ~30 s.
            if (clock::now() - lastStatTime >= std::chrono::seconds(30)) {
                const double secs = std::chrono::duration<double>(clock::now() - lastStatTime).count();
                spdlog::info("[Lynsyn] Ingest: {:.0f} Hz raw ({} total), {} emitted @ {} ms cadence, "
                             "{} raw-rejected",
                             (sampleCount - lastStatSamples) / (secs > 0 ? secs : 1.0),
                             sampleCount, emitted, sr_ms, rawRejects);
                lastStatSamples = sampleCount;
                lastStatTime = clock::now();
            }

            if (!anySuccess) {
                ++consecutiveFails;
                auto backoff = BASE_BACKOFF * (1 << std::min(consecutiveFails - 1, 5));

                if (consecutiveFails >= 3 && consecutiveFails % 3 == 0) {
                    spdlog::warn("[Lynsyn] No sample - attempting thread-safe surgical reset");
                    fullResetThreadSafe();
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    
                    std::lock_guard<std::mutex> g(lifecycleMutex_);
                    if (lynsyn_init()) armSamplingModeWithPeriodMs_(static_cast<int>(sr_ms));
                }

                if (consecutiveFails >= MAX_CONSECUTIVE_FAILS) {
                    spdlog::error("[Lynsyn] Too many failures - stopping monitor thread");
                    reportError("Lynsyn: Too many consecutive failures");
                    break;
                }

                std::this_thread::sleep_for(backoff);
            }

            // (raw-count log removed: superseded by the 30 s Ingest line;
            //  at ~2 kHz it fired every 0.25 s)
        }

        if (outputFile_.is_open()) outputFile_.flush();
        running_.store(false);
        spdlog::info("[Lynsyn] Thread exiting. Total samples: {}", sampleCount);
    }

    void armSamplingModeWithPeriodMs_(int period_ms) {
        const int effectiveMs = std::max(1, period_ms);

#ifdef LYN_SUPPORTS_SET_PERIOD_MS
        try {
            lynsyn_set_period_ms(effectiveMs);
            lynsyn_set_core_mask(config_.coreMask);
            lynsyn_arm(config_.durationSec);
            spdlog::info("[Lynsyn] Armed via hardware period API: {} ms", effectiveMs);
        } catch (...) {
            spdlog::warn("[Lynsyn] Hardware period API failed - falling back");
            armSamplingMode_();
        }
#else
        spdlog::info("[Lynsyn] No period_ms API - legacy free-run + FIFO drain");
        armSamplingMode_();
#endif
    }

    void armSamplingMode_() {
        if (config_.periodSampling) {
            lynsyn_startPeriodSampling(config_.durationSec, config_.coreMask);
        } else if (config_.startBreakpoint && config_.endBreakpoint) {
            lynsyn_startBpSampling(config_.startBreakpoint, config_.endBreakpoint, config_.coreMask);
        } else {
            lynsyn_startPeriodSampling(config_.durationSec, config_.coreMask);
        }
    }

    PowerStats convertLynsynSampleToPowerStats_(const LynsynSample& s) {
        PowerStats stats;
        const double currentLynsynSec = lynsyn_cyclesToSeconds(s.time);

        if (!firstSampleReceived_.load(std::memory_order_acquire)) {
            firstSampleReceived_.store(true, std::memory_order_release);
            jetsonBaseTime_ = std::chrono::system_clock::now();
            lynsynBaseTimeS_ = currentLynsynSec;
            lastLynsynTimeS_ = currentLynsynSec;
            stats.timestamp = jetsonBaseTime_;
        } else {
            const double deltaFromLast = currentLynsynSec - lastLynsynTimeS_;

            // Detect backward jumps or large sequence gaps between consecutive samples (>5s gap)
            if (deltaFromLast < 0.0 || deltaFromLast > 5.0) {
                spdlog::warn("[Lynsyn] Irregular step detected ({:.3f}s gap) - resetting clock anchor", deltaFromLast);
                firstSampleReceived_.store(false, std::memory_order_release);
                return convertLynsynSampleToPowerStats_(s);
            }

            lastLynsynTimeS_ = currentLynsynSec;

            // Calculate elapsed time relative to base timestamp
            const double totalElapsedSec = currentLynsynSec - lynsynBaseTimeS_;
            auto deltaDuration = std::chrono::duration<double>(totalElapsedSec);
            stats.timestamp = jetsonBaseTime_ +
                              std::chrono::duration_cast<std::chrono::system_clock::duration>(deltaDuration);
        }

        const size_t maxSensors = 4;
        stats.voltages.assign(maxSensors, 0.0);
        stats.currents.assign(maxSensors, 0.0);
        stats.power.assign(maxSensors, 0.0);

        for (size_t i = 0; i < maxSensors; ++i) {
            double v = static_cast<double>(s.voltage[i]);
            double c = static_cast<double>(s.current[i]);
            stats.setSensorData(i, v, c);
        }

        stats.updateDerivedMetrics();
        return stats;
    }

    void reportError(const std::string& msg) {
        if (errorCallback_) errorCallback_(msg);
        else spdlog::error("{}", msg);
    }

private:
    LynsynMonitorConfig config_;
    std::function<void(const LynsynSample&)> sampleCallback_;
    std::function<void(const std::string&)> errorCallback_;

    std::atomic<bool> initialized_;
    std::atomic<bool> running_;
    ThreadManager& threadManager_;
    std::atomic<bool> threadShouldExit_;
    std::ofstream outputFile_;
    std::shared_ptr<ISystemMetricsAggregator> metricAggregator_;

    std::mutex lifecycleMutex_;

    // Clock sync anchors
    std::atomic<bool> firstSampleReceived_{false};
    double lynsynBaseTimeS_{0.0};
    double lastLynsynTimeS_{0.0};
    std::chrono::system_clock::time_point jetsonBaseTime_;

    bool enabled_ = true;
    uint64_t sanityRejects_ = 0;
};