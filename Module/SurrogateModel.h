// =====================================================================
// SurrogateModel.h
// Online power/performance surrogate for the ERL fitness function.
// Header-only | C++14 | Jetson Nano friendly (std + spdlog only)
// =====================================================================
//
// PhD MOTIVATION (closes evaluation gap: single-sample + surrogate fitness)
// ------------------------------------------------------------------------
// In an online evolutionary controller, only ONE candidate configuration can
// be applied to the hardware per control interval ? you cannot run the CPU at
// 102 MHz and 1479 MHz simultaneously. The remaining population members were
// previously scored against the CURRENTLY measured snapshot via a crude,
// frequency-blind simulation, so their objectives were largely fictional and
// inconsistent with the one measured member. NSGA-II ranking over inconsistent
// objectives is exactly what produced the observed premature collapse.
//
// This surrogate replaces "1 measured + N fabricated" with a single consistent
// predictor that is:
//   1. EXACT where a configuration has been measured (bucket mean, EMA-smoothed);
//   2. INTERPOLATED for nearby unmeasured configs (distance-weighted k-NN in a
//      normalised feature space);
//   3. PHYSICS-PRIOR extrapolated during cold start (P ~ P_static + k*f,
//      fps ~ fps_max * freq_ratio), so early generations are not garbage.
//
// It also exposes an OPTIMISM-UNDER-UNCERTAINTY (UCB-style) exploration bonus:
// rarely/never-sampled configurations receive a bonus that makes them more
// likely to be selected and therefore actually measured on hardware, which in
// turn improves the surrogate. This couples exploration to model improvement in
// one principled term and directly counters the single-mode collapse.
//
// Contribution framing for the thesis:
//   "An online, physically-grounded surrogate with optimism-driven sampling for
//    consistent counterfactual fitness in on-device evolutionary DVFS control."
// =====================================================================
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <spdlog/spdlog.h>

namespace hrl {

// Prediction provenance. The confidence value is a support heuristic, not a
// calibrated probability of correctness.
enum class SurrogateSource {
    DISABLED = 0,
    PHYSICS_PRIOR = 1,
    KNN_INTERPOLATED = 2,
    EXACT_BUCKET = 3
};

// Prediction returned for an arbitrary candidate configuration.
struct SurrogatePrediction {
    double fps        = 0.0;
    double power_w    = 0.0;
    double latency_ms = 0.0;
    double temp_c     = 0.0;
    double confidence = 0.0;   // support/confidence heuristic, not probability
    bool   from_data  = false;

    SurrogateSource source = SurrogateSource::DISABLED;
    uint32_t support_count = 0;   // measured samples contributing to this prediction
    uint32_t neighbor_count = 0;  // 1 exact bucket, k for interpolation, 0 prior
    double nearest_distance = -1.0; // normalized feature-space distance; 0 exact
    uint32_t stall_count = 0;      // bounded evidence-timeout observations
    double stall_penalty = 0.0;    // feasibility penalty; not a physical measurement
};

// Exact-bucket feasibility state. Keep this separate from KNN interpolation:
// a neighbouring timeout should not make an untried candidate infeasible.
struct SurrogateFeasibilityStats {
    uint32_t success_count = 0;
    uint32_t stall_count = 0;
    uint32_t attempts = 0;
    double timeout_ratio = 0.0;
};

class SurrogateModel {
public:
    struct Config {
        double ema_alpha;
        int    knn_k;
        double exploration_weight;
        double cpu_min_khz;
        double cpu_max_khz;
        double p_static_w;
        double p_dynamic_w;
        double p_gpu_w;
        double fps_max;
        double latency_prior_ms;
        bool   gpu_split_applicable;
        double ambient_c;
        double temp_per_watt;

        Config()
            : ema_alpha(0.30),
              knn_k(3),
              exploration_weight(0.15),
              cpu_min_khz(102000.0),
              cpu_max_khz(1479000.0),
              p_static_w(1.5),
              p_dynamic_w(2.0),
              p_gpu_w(1.0),
              fps_max(60.0),
              latency_prior_ms(10.0),
              gpu_split_applicable(false),
              ambient_c(38.0),
              temp_per_watt(6.0) {}
    };

    explicit SurrogateModel(Config cfg = Config())
        : cfg_(cfg), active_workload_id_(0), active_workload_label_("UNSPECIFIED")
    {
        // workload id 0 is a temporary construction context only. The GA binds
        // a real non-zero workloadId before the first measured generation.
        ContextStore& initial = contexts_[active_workload_id_];
        initial.label = active_workload_label_;
    }

    static const char* sourceName(SurrogateSource source) {
        switch (source) {
            case SurrogateSource::PHYSICS_PRIOR:     return "PHYSICS_PRIOR";
            case SurrogateSource::KNN_INTERPOLATED:  return "KNN_INTERPOLATED";
            case SurrogateSource::EXACT_BUCKET:      return "EXACT_BUCKET";
            case SurrogateSource::DISABLED:
            default:                                 return "DISABLED";
        }
    }

    // ------------------------------------------------------------------
    // Dynamic-adaptation context switch.
    // ------------------------------------------------------------------
    // NO learned bucket is erased here.  We only move the active pointer to a
    // workload-specific bucket bank and update that workload's physical priors.
    // Therefore:
    //   HistEq -> Sobel cannot reuse HistEq measurements incorrectly;
    //   Sobel -> HeterogeneousGaussian cannot contaminate split semantics;
    //   returning to HistEq reactivates the original HistEq knowledge.
    void setWorkloadContext(uint32_t workloadId,
                            const std::string& workload,
                            bool gpuSplitApplicable,
                            double fpsMax,
                            double latencyPriorMs)
    {
        std::lock_guard<std::mutex> lk(mutex_);

        // workloadId is the namespace key. A recurrence of the same workload
        // therefore reactivates the same learned bank instead of cold-starting.
        // Id 0 is reserved for the constructor's unset context.
        if (workloadId == 0) {
            spdlog::warn("[Surrogate] workloadId=0 for '{}'; using unset bank", workload);
        }

        active_workload_id_ = workloadId;
        active_workload_label_ = workload.empty() ? std::string("UNSPECIFIED") : workload;
        cfg_.gpu_split_applicable = gpuSplitApplicable;
        if (std::isfinite(fpsMax) && fpsMax > 0.0) cfg_.fps_max = fpsMax;
        if (std::isfinite(latencyPriorMs) && latencyPriorMs >= 0.0) {
            cfg_.latency_prior_ms = latencyPriorMs;
        }

        ContextStore& store = contexts_[active_workload_id_];
        if (store.label.empty()) {
            store.label = active_workload_label_;
        } else if (store.label != active_workload_label_) {
            // This should never happen with WorkloadMission::workloadIdFor().
            // Do not merge evidence silently if a caller violates the id contract.
            spdlog::error(
                "[Surrogate] workloadId collision: id={} existing='{}' requested='{}'",
                active_workload_id_, store.label, active_workload_label_);
            active_workload_label_ = store.label;
        }
    }

    std::string currentWorkload() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return active_workload_label_;
    }

    uint32_t currentWorkloadId() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return active_workload_id_;
    }

    uint64_t currentWorkloadObservations() const {
        std::lock_guard<std::mutex> lk(mutex_);
        const auto it = contexts_.find(active_workload_id_);
        return it == contexts_.end() ? 0ULL : it->second.observations;
    }

    size_t currentWorkloadDistinctConfigsSeen() const {
        std::lock_guard<std::mutex> lk(mutex_);
        const auto it = contexts_.find(active_workload_id_);
        return it == contexts_.end() ? 0u : it->second.buckets.size();
    }

    // Feed one REAL measured outcome for the currently active workload.
    void observe(double freq_khz, bool gpu, double gpu_split, int concurrency,
                 double fps, double power_w, double latency_ms, double temp_c)
    {
        std::lock_guard<std::mutex> lk(mutex_);

        const bool fpsValid = std::isfinite(fps) && fps >= 0.0;
        const bool powerValid = std::isfinite(power_w) && power_w > 0.0;
        const bool latencyValid = std::isfinite(latency_ms) && latency_ms > 0.0;
        const bool tempValid = std::isfinite(temp_c) && temp_c > 0.0;
        if (!fpsValid && !powerValid && !latencyValid && !tempValid) return;

        if (fpsValid) fps = std::min(fps, 1.10 * cfg_.fps_max);

        ContextStore& store = activeStoreUnsafe_();
        const uint32_t key = encodeUnsafe_(freq_khz, gpu, gpu_split, concurrency);
        Bucket& b = store.buckets[key];

        const bool first = (b.count == 0);
        if (first) {
            const SurrogatePrediction prior =
                physicsPriorUnsafe_(freq_khz, gpu, gpu_split, concurrency);
            b.fps = prior.fps;
            b.power_w = prior.power_w;
            b.latency_ms = prior.latency_ms;
            b.temp_c = prior.temp_c;
        }

        const double a = first ? 0.60 : cfg_.ema_alpha;
        if (fpsValid)     b.fps        = (1.0 - a) * b.fps        + a * fps;
        if (powerValid)   b.power_w    = (1.0 - a) * b.power_w    + a * power_w;
        if (latencyValid) b.latency_ms = (1.0 - a) * b.latency_ms + a * latency_ms;
        if (tempValid)    b.temp_c     = (1.0 - a) * b.temp_c     + a * temp_c;

        ++b.count;
        ++store.observations;
        ++total_observations_;
    }

    SurrogatePrediction predict(double freq_khz, bool gpu, double gpu_split,
                                int concurrency) const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        const ContextStore& store = activeStoreUnsafeConst_();
        const uint32_t key = encodeUnsafe_(freq_khz, gpu, gpu_split, concurrency);

        // 1) Exact bucket in THIS workload only.
        auto it = store.buckets.find(key);
        if (it != store.buckets.end()) {
            const Bucket& b = it->second;
            if (b.count > 0) {
                SurrogatePrediction p;
                p.fps = b.fps;
                p.power_w = b.power_w;
                p.latency_ms = b.latency_ms;
                p.temp_c = b.temp_c;
                p.from_data = true;
                p.source = SurrogateSource::EXACT_BUCKET;
                p.support_count = b.count;
                p.neighbor_count = 1;
                p.nearest_distance = 0.0;
                p.stall_count = b.stall_count;
                p.stall_penalty = stallPenaltyForBucket_(b);
                const double supportConf =
                    1.0 - 1.0 / (1.0 + static_cast<double>(b.count));
                const double successRatio = static_cast<double>(b.count) /
                    static_cast<double>(b.count + b.stall_count);
                p.confidence = supportConf * successRatio;
                return p;
            }
            if (b.stall_count > 0) {
                SurrogatePrediction p =
                    physicsPriorUnsafe_(freq_khz, gpu, gpu_split, concurrency);
                p.stall_count = b.stall_count;
                p.stall_penalty = stallPenaltyForBucket_(b);
                p.nearest_distance = 0.0;
                return p;
            }
        }

        // 2) k-NN interpolation is also strictly workload-local.
        if (!store.buckets.empty()) {
            const std::array<double, 4> q =
                featuresUnsafe_(freq_khz, gpu, gpu_split, concurrency);
            std::vector<std::pair<double, const Bucket*> > nn;
            nn.reserve(store.buckets.size());

            for (const auto& kv : store.buckets) {
                if (kv.second.count == 0) continue;
                const std::array<double, 4> f = decodeFeaturesUnsafe_(kv.first);
                double d2 = 0.0;
                for (int i = 0; i < 4; ++i) {
                    const double e = q[i] - f[i];
                    d2 += e * e;
                }
                nn.push_back(std::make_pair(std::sqrt(d2), &kv.second));
            }

            if (!nn.empty()) {
                const size_t k = std::min<size_t>(
                    static_cast<size_t>(std::max(1, cfg_.knn_k)), nn.size());
                std::partial_sort(nn.begin(), nn.begin() + k, nn.end(),
                    [](const std::pair<double, const Bucket*>& x,
                       const std::pair<double, const Bucket*>& y) {
                        return x.first < y.first;
                    });

                double wsum = 0.0, fpsOut = 0.0, pw = 0.0, lat = 0.0, tmp = 0.0;
                const double nearest = nn[0].first;
                uint32_t support = 0;
                uint32_t stalls = 0;
                for (size_t i = 0; i < k; ++i) {
                    const double w = 1.0 / (1e-3 + nn[i].first);
                    wsum += w;
                    fpsOut += w * nn[i].second->fps;
                    pw     += w * nn[i].second->power_w;
                    lat    += w * nn[i].second->latency_ms;
                    tmp    += w * nn[i].second->temp_c;
                    support += nn[i].second->count;
                    stalls += nn[i].second->stall_count;
                }

                SurrogatePrediction p;
                p.fps = fpsOut / wsum;
                p.power_w = pw / wsum;
                p.latency_ms = lat / wsum;
                p.temp_c = tmp / wsum;
                p.from_data = true;
                p.source = SurrogateSource::KNN_INTERPOLATED;
                p.support_count = support;
                p.neighbor_count = static_cast<uint32_t>(k);
                p.nearest_distance = nearest;
                p.stall_count = stalls;
                p.stall_penalty = (support + stalls) > 0
                    ? 0.50 * static_cast<double>(stalls) /
                        static_cast<double>(support + stalls)
                    : 0.0;
                p.confidence = 0.6 / (1.0 + 4.0 * nearest);
                return p;
            }
        }

        // 3) No evidence for this workload/configuration yet.
        return physicsPriorUnsafe_(freq_khz, gpu, gpu_split, concurrency);
    }

    double explorationBonus(double freq_khz, bool gpu, double gpu_split,
                            int concurrency) const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        const ContextStore& store = activeStoreUnsafeConst_();
        const uint32_t key = encodeUnsafe_(freq_khz, gpu, gpu_split, concurrency);
        const auto it = store.buckets.find(key);
        const double attempts = (it == store.buckets.end())
            ? 0.0
            : static_cast<double>(it->second.count + it->second.stall_count);
        return cfg_.exploration_weight / std::sqrt(1.0 + attempts);
    }

    void observeStarvation(double freq_khz, bool gpu, double gpu_split, int concurrency)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        ContextStore& store = activeStoreUnsafe_();
        Bucket& b = store.buckets[encodeUnsafe_(freq_khz, gpu, gpu_split, concurrency)];
        ++b.stall_count;
        ++store.stalls;
        ++total_stalls_;
    }

    void seed(double freq_khz, bool gpu, double gpu_split, int concurrency,
              double fps, double power_w, double latency_ms, double temp_c,
              uint32_t count = 3)
    {
        if (count == 0 || !std::isfinite(fps) || !std::isfinite(power_w) ||
            !std::isfinite(latency_ms) || !std::isfinite(temp_c)) return;

        std::lock_guard<std::mutex> lk(mutex_);
        fps = std::min(std::max(0.0, fps), 1.10 * cfg_.fps_max);
        ContextStore& store = activeStoreUnsafe_();
        Bucket& b = store.buckets[encodeUnsafe_(freq_khz, gpu, gpu_split, concurrency)];
        b.fps = fps;
        b.power_w = power_w;
        b.latency_ms = latency_ms;
        b.temp_c = temp_c;

        const uint32_t oldCount = b.count;
        b.count = std::max(b.count, count);
        const uint32_t added = b.count - oldCount;
        store.observations += added;
        total_observations_ += added;
    }

    double stallPenalty(double freq_khz, bool gpu, double gpu_split, int concurrency) const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        const ContextStore& store = activeStoreUnsafeConst_();
        const auto it = store.buckets.find(encodeUnsafe_(freq_khz, gpu, gpu_split, concurrency));
        return it == store.buckets.end() ? 0.0 : stallPenaltyForBucket_(it->second);
    }

    SurrogateFeasibilityStats feasibilityStats(double freq_khz, bool gpu,
                                                double gpu_split, int concurrency) const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        SurrogateFeasibilityStats stats;
        const ContextStore& store = activeStoreUnsafeConst_();
        const auto it = store.buckets.find(encodeUnsafe_(freq_khz, gpu, gpu_split, concurrency));
        if (it == store.buckets.end()) return stats;

        stats.success_count = it->second.count;
        stats.stall_count = it->second.stall_count;
        stats.attempts = stats.success_count + stats.stall_count;
        if (stats.attempts > 0) {
            stats.timeout_ratio = static_cast<double>(stats.stall_count) /
                                  static_cast<double>(stats.attempts);
        }
        return stats;
    }

    uint64_t totalStalls() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return total_stalls_;
    }

    uint64_t totalObservations() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return total_observations_;
    }

    size_t distinctConfigsSeen() const {
        std::lock_guard<std::mutex> lk(mutex_);
        size_t total = 0;
        for (const auto& ctx : contexts_) total += ctx.second.buckets.size();
        return total;
    }

    // Export every workload bank. This is direct thesis evidence that model state
    // survives transitions while observations remain correctly partitioned.
    std::string exportCSV() const {
        std::lock_guard<std::mutex> lk(mutex_);
        std::string csv =
            "workload_id,workload,freq_bucket,gpu,split_bucket,concurrency,count,stall_count,"
            "fps,power_w,latency_ms,temp_c\n";

        for (const auto& ctx : contexts_) {
            for (const auto& kv : ctx.second.buckets) {
                int fb, g, sb, cc;
                decode_(kv.first, fb, g, sb, cc);
                const Bucket& b = kv.second;
                csv += std::to_string(ctx.first) + "," + ctx.second.label + "," +
                       std::to_string(fb) + "," + std::to_string(g) + "," +
                       std::to_string(sb) + "," + std::to_string(cc) + "," +
                       std::to_string(b.count) + "," + std::to_string(b.stall_count) + "," +
                       std::to_string(b.fps) + "," + std::to_string(b.power_w) + "," +
                       std::to_string(b.latency_ms) + "," + std::to_string(b.temp_c) + "\n";
            }
        }
        return csv;
    }

private:
    struct Bucket {
        double fps = 0.0;
        double power_w = 0.0;
        double latency_ms = 0.0;
        double temp_c = 0.0;
        uint32_t count = 0;
        uint32_t stall_count = 0;
    };

    struct ContextStore {
        std::string label;
        std::unordered_map<uint32_t, Bucket> buckets;
        uint64_t observations = 0;
        uint64_t stalls = 0;
    };

    Config cfg_;
    mutable std::mutex mutex_;
    uint32_t active_workload_id_;
    std::string active_workload_label_;
    std::unordered_map<uint32_t, ContextStore> contexts_;
    uint64_t total_observations_ = 0;
    uint64_t total_stalls_ = 0;

    ContextStore& activeStoreUnsafe_() {
        return contexts_[active_workload_id_];
    }

    const ContextStore& activeStoreUnsafeConst_() const {
        const auto it = contexts_.find(active_workload_id_);
        if (it != contexts_.end()) return it->second;
        static const ContextStore empty;
        return empty;
    }

    static const std::array<double, 5>& freqSteps_() {
        static const std::array<double, 5> s{{102000, 460800, 921600, 1190400, 1479000}};
        return s;
    }

    static int freqBucket_(double khz) {
        const auto& s = freqSteps_();
        int best = 0;
        double bestDistance = std::abs(s[0] - khz);
        for (int i = 1; i < 5; ++i) {
            const double d = std::abs(s[i] - khz);
            if (d < bestDistance) {
                bestDistance = d;
                best = i;
            }
        }
        return best;
    }

    static int splitBucket_(double split) {
        split = clamp01_(split);
        return static_cast<int>(std::lround(split * 4.0));
    }

    static int concBucket_(int c) {
        return (c < 1) ? 1 : (c > 4 ? 4 : c);
    }

    double canonicalSplitUnsafe_(bool gpu, double split) const {
        if (!cfg_.gpu_split_applicable) return gpu ? 1.0 : 0.0;
        return clamp01_(split);
    }

    uint32_t encodeUnsafe_(double freq_khz, bool gpu, double split, int conc) const {
        const double effectiveSplit = canonicalSplitUnsafe_(gpu, split);
        return static_cast<uint32_t>(freqBucket_(freq_khz)) * 1000u +
               static_cast<uint32_t>(gpu ? 1 : 0) * 100u +
               static_cast<uint32_t>(splitBucket_(effectiveSplit)) * 10u +
               static_cast<uint32_t>(concBucket_(conc));
    }

    static void decode_(uint32_t key, int& fb, int& g, int& sb, int& cc) {
        fb = (key / 1000) % 10;
        g  = (key / 100) % 10;
        sb = (key / 10) % 10;
        cc = key % 10;
    }

    std::array<double, 4> featuresUnsafe_(double freq_khz, bool gpu,
                                          double split, int conc) const {
        const double fr = (freq_khz - cfg_.cpu_min_khz) /
                          std::max(1.0, cfg_.cpu_max_khz - cfg_.cpu_min_khz);
        const double effectiveSplit = canonicalSplitUnsafe_(gpu, split);
        return {{clamp01_(fr), gpu ? 1.0 : 0.0, effectiveSplit,
                 (concBucket_(conc) - 1) / 3.0}};
    }

    std::array<double, 4> decodeFeaturesUnsafe_(uint32_t key) const {
        int fb, g, sb, cc;
        decode_(key, fb, g, sb, cc);
        return featuresUnsafe_(freqSteps_()[fb], g != 0, sb / 4.0, cc);
    }

    static double stallPenaltyForBucket_(const Bucket& b) {
        const double denom = static_cast<double>(b.count + b.stall_count);
        return denom > 0.0
            ? 0.50 * static_cast<double>(b.stall_count) / denom
            : 0.0;
    }

    SurrogatePrediction physicsPriorUnsafe_(double freq_khz, bool gpu,
                                             double /*split*/, int conc) const {
        const double fr = clamp01_((freq_khz - cfg_.cpu_min_khz) /
                                   std::max(1.0, cfg_.cpu_max_khz - cfg_.cpu_min_khz));
        SurrogatePrediction p;
        const double concScale = 1.0 + 0.15 * (concBucket_(conc) - 2);
        p.power_w = cfg_.p_static_w + cfg_.p_dynamic_w * fr * concScale +
                    (gpu ? cfg_.p_gpu_w : 0.0);
        p.fps = std::max(1.0, cfg_.fps_max * fr);
        p.latency_ms = std::max(0.0, cfg_.latency_prior_ms);
        p.temp_c = cfg_.ambient_c + cfg_.temp_per_watt *
                   (p.power_w - cfg_.p_static_w);
        p.confidence = 0.1;
        p.from_data = false;
        p.source = SurrogateSource::PHYSICS_PRIOR;
        p.support_count = 0;
        p.neighbor_count = 0;
        p.nearest_distance = -1.0;
        p.stall_count = 0;
        p.stall_penalty = 0.0;
        return p;
    }

    static double clamp01_(double v) {
        return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
    }
};

} // namespace hrl

// // =====================================================================
// // SurrogateModel.h
// // Online power/performance surrogate for the ERL fitness function.
// // Header-only | C++14 | Jetson Nano friendly (std + spdlog only)
// // =====================================================================
// //
// // PhD MOTIVATION (closes evaluation gap: single-sample + surrogate fitness)
// // ------------------------------------------------------------------------
// // In an online evolutionary controller, only ONE candidate configuration can
// // be applied to the hardware per control interval ? you cannot run the CPU at
// // 102 MHz and 1479 MHz simultaneously. The remaining population members were
// // previously scored against the CURRENTLY measured snapshot via a crude,
// // frequency-blind simulation, so their objectives were largely fictional and
// // inconsistent with the one measured member. NSGA-II ranking over inconsistent
// // objectives is exactly what produced the observed premature collapse.
// //
// // This surrogate replaces "1 measured + N fabricated" with a single consistent
// // predictor that is:
// //   1. EXACT where a configuration has been measured (bucket mean, EMA-smoothed);
// //   2. INTERPOLATED for nearby unmeasured configs (distance-weighted k-NN in a
// //      normalised feature space);
// //   3. PHYSICS-PRIOR extrapolated during cold start (P ~ P_static + k*f,
// //      fps ~ fps_max * freq_ratio), so early generations are not garbage.
// //
// // It also exposes an OPTIMISM-UNDER-UNCERTAINTY (UCB-style) exploration bonus:
// // rarely/never-sampled configurations receive a bonus that makes them more
// // likely to be selected and therefore actually measured on hardware, which in
// // turn improves the surrogate. This couples exploration to model improvement in
// // one principled term and directly counters the single-mode collapse.
// //
// // Contribution framing for the thesis:
// //   "An online, physically-grounded surrogate with optimism-driven sampling for
// //    consistent counterfactual fitness in on-device evolutionary DVFS control."
// // =====================================================================
// #pragma once

// #include <algorithm>
// #include <array>
// #include <cmath>
// #include <cstdint>
// #include <string>
// #include <unordered_map>
// #include <vector>
// #include <mutex>
// #include <spdlog/spdlog.h>

// namespace hrl {

// // Prediction provenance. The confidence value is a support heuristic, not a
// // calibrated probability of correctness.
// enum class SurrogateSource {
//     DISABLED = 0,
//     PHYSICS_PRIOR = 1,
//     KNN_INTERPOLATED = 2,
//     EXACT_BUCKET = 3
// };

// // Prediction returned for an arbitrary candidate configuration.
// struct SurrogatePrediction {
//     double fps        = 0.0;
//     double power_w    = 0.0;
//     double latency_ms = 0.0;
//     double temp_c     = 0.0;
//     double confidence = 0.0;   // support/confidence heuristic, not probability
//     bool   from_data  = false;

//     SurrogateSource source = SurrogateSource::DISABLED;
//     uint32_t support_count = 0;   // measured samples contributing to this prediction
//     uint32_t neighbor_count = 0;  // 1 exact bucket, k for interpolation, 0 prior
//     double nearest_distance = -1.0; // normalized feature-space distance; 0 exact
//     uint32_t stall_count = 0;      // bounded evidence-timeout observations
//     double stall_penalty = 0.0;    // feasibility penalty; not a physical measurement
// };

// // Exact-bucket feasibility state. Keep this separate from KNN interpolation:
// // a neighbouring timeout should not make an untried candidate infeasible.
// struct SurrogateFeasibilityStats {
//     uint32_t success_count = 0;
//     uint32_t stall_count = 0;
//     uint32_t attempts = 0;
//     double timeout_ratio = 0.0;
// };

// class SurrogateModel {
// public:
//     struct Config {
//         double ema_alpha;            // smoothing for repeated observations of a bucket
//         int    knn_k;                // neighbours used for interpolation
//         double exploration_weight;   // UCB bonus scale (fitness units)
//         // Hardware envelope priors (Jetson Nano; override per platform).
//         double cpu_min_khz;
//         double cpu_max_khz;
//         double p_static_w;           // idle/static power floor
//         double p_dynamic_w;          // extra power at max clock, CPU only
//         double p_gpu_w;              // extra power when GPU active
//         double fps_max;              // achievable fps at max clock (workload-dependent)
//         double latency_prior_ms;     // independent cold-start processing-latency prior
//         bool   gpu_split_applicable; // true only for genuinely partitioned workloads
//         double ambient_c;            // baseline temperature
//         double temp_per_watt;        // steady-state temperature rise per watt

//         Config()
//             : ema_alpha(0.30),
//               knn_k(3),
//               exploration_weight(0.15),
//               cpu_min_khz(102000.0),
//               cpu_max_khz(1479000.0),
//               p_static_w(1.5),
//               p_dynamic_w(2.0),
//               p_gpu_w(1.0),
//               fps_max(60.0),
//               latency_prior_ms(10.0),
//               gpu_split_applicable(true),
//               ambient_c(38.0),
//               temp_per_watt(6.0)
//         {}
//     };

//     explicit SurrogateModel(Config cfg = Config()) : cfg_(cfg) {}

//     static const char* sourceName(SurrogateSource source) {
//         switch (source) {
//             case SurrogateSource::PHYSICS_PRIOR:     return "PHYSICS_PRIOR";
//             case SurrogateSource::KNN_INTERPOLATED:  return "KNN_INTERPOLATED";
//             case SurrogateSource::EXACT_BUCKET:      return "EXACT_BUCKET";
//             case SurrogateSource::DISABLED:
//             default:                                 return "DISABLED";
//         }
//     }

//     // ------------------------------------------------------------------
//     // observe(): feed a REAL measured outcome for the configuration that
//     // produced it. Call once per control interval with the config that was
//     // actually applied and the snapshot that resulted from it.
//     // ------------------------------------------------------------------
//     void observe(double freq_khz, bool gpu, double gpu_split, int concurrency,
//                  double fps, double power_w, double latency_ms, double temp_c) {
//         // Each metric is learned independently. In particular, do NOT replace a
//         // missing processing-latency sample with 1000/FPS: that is an inter-frame
//         // period, not per-frame processing latency.
//         const bool fpsValid = std::isfinite(fps) && fps >= 0.0;
//         const bool powerValid = std::isfinite(power_w) && power_w > 0.0;
//         const bool latencyValid = std::isfinite(latency_ms) && latency_ms > 0.0;
//         const bool tempValid = std::isfinite(temp_c) && temp_c > 0.0;

//         if (!fpsValid && !powerValid && !latencyValid && !tempValid) return;

//         if (fpsValid) {
//             const double fps_ceiling = 1.10 * cfg_.fps_max;
//             fps = std::min(fps, fps_ceiling);
//         }

//         std::lock_guard<std::mutex> lk(mutex_);
//         const uint32_t key = encode(freq_khz, gpu, gpu_split, concurrency);
//         Bucket& b = buckets_[key];

//         // Seed a new bucket from the low-confidence physics prior, then blend the
//         // first real observation 60/40. One transient can no longer become the
//         // exact-bucket truth in a single update.
//         const bool first = (b.count == 0);
//         if (first) {
//             const SurrogatePrediction prior =
//                 physicsPrior(freq_khz, gpu, gpu_split, concurrency);
//             b.fps = prior.fps;
//             b.power_w = prior.power_w;
//             b.latency_ms = prior.latency_ms;
//             b.temp_c = prior.temp_c;
//         }

//         const double a = first ? 0.60 : cfg_.ema_alpha;
//         if (fpsValid) {
//             b.fps = (1.0 - a) * b.fps + a * fps;
//         }
//         if (powerValid) {
//             b.power_w = (1.0 - a) * b.power_w + a * power_w;
//         }
//         if (latencyValid) {
//             b.latency_ms = (1.0 - a) * b.latency_ms + a * latency_ms;
//         }
//         if (tempValid) {
//             b.temp_c = (1.0 - a) * b.temp_c + a * temp_c;
//         }

//         ++b.count;
//         ++total_observations_;
//     }

//     // ------------------------------------------------------------------
//     // predict(): consistent outcome estimate for ANY candidate config.
//     // ------------------------------------------------------------------
//     SurrogatePrediction predict(double freq_khz, bool gpu, double gpu_split,
//                                 int concurrency) const {
//         std::lock_guard<std::mutex> lk(mutex_);
//         const uint32_t key = encode(freq_khz, gpu, gpu_split, concurrency);

//         // 1) Exact bucket hit.
//         auto it = buckets_.find(key);
//         if (it != buckets_.end()) {
//             const Bucket& b = it->second;

//             if (b.count > 0) {
//                 SurrogatePrediction p;
//                 p.fps = b.fps; p.power_w = b.power_w;
//                 p.latency_ms = b.latency_ms; p.temp_c = b.temp_c;
//                 p.from_data = true;
//                 p.source = SurrogateSource::EXACT_BUCKET;
//                 p.support_count = b.count;
//                 p.neighbor_count = 1;
//                 p.nearest_distance = 0.0;
//                 p.stall_count = b.stall_count;
//                 p.stall_penalty = stallPenaltyForBucket(b);

//                 // Confidence reflects both successful support and repeated stalls.
//                 const double supportConf =
//                     1.0 - 1.0 / (1.0 + static_cast<double>(b.count));
//                 const double successRatio =
//                     static_cast<double>(b.count) /
//                     static_cast<double>(b.count + b.stall_count);
//                 p.confidence = supportConf * successRatio;
//                 return p;
//             }

//             // A stall-only bucket is not "unseen". Keep the prior physical values
//             // but propagate its exact feasibility history and penalty.
//             if (b.stall_count > 0) {
//                 SurrogatePrediction p =
//                     physicsPrior(freq_khz, gpu, gpu_split, concurrency);
//                 p.stall_count = b.stall_count;
//                 p.stall_penalty = stallPenaltyForBucket(b);
//                 p.nearest_distance = 0.0;
//                 return p;
//             }
//         }

//         // 2) Distance-weighted k-NN interpolation from observed buckets.
//         if (!buckets_.empty()) {
//             const std::array<double, 4> q = features(freq_khz, gpu, gpu_split, concurrency);
//             std::vector<std::pair<double, const Bucket*>> nn;  // (distance, bucket)
//             nn.reserve(buckets_.size());
//             for (const auto& kv : buckets_) {
//                 if (kv.second.count == 0) continue;
//                 const std::array<double, 4> f = decodeFeatures(kv.first);
//                 double d2 = 0.0;
//                 for (int i = 0; i < 4; ++i) { const double e = q[i] - f[i]; d2 += e * e; }
//                 nn.emplace_back(std::sqrt(d2), &kv.second);
//             }
//             if (!nn.empty()) {
//                 std::partial_sort(
//                     nn.begin(),
//                     nn.begin() + std::min<size_t>(cfg_.knn_k, nn.size()),
//                     nn.end(),
//                     [](const std::pair<double, const Bucket*>& x,
//                        const std::pair<double, const Bucket*>& y) { return x.first < y.first; });
//                 const size_t k = std::min<size_t>(cfg_.knn_k, nn.size());
//                 double wsum = 0, fps = 0, pw = 0, lat = 0, tmp = 0, nearest = nn[0].first;
//                 uint32_t support = 0;
//                 uint32_t stalls = 0;
//                 for (size_t i = 0; i < k; ++i) {
//                     const double w = 1.0 / (1e-3 + nn[i].first);   // inverse-distance
//                     wsum += w;
//                     fps += w * nn[i].second->fps;
//                     pw  += w * nn[i].second->power_w;
//                     lat += w * nn[i].second->latency_ms;
//                     tmp += w * nn[i].second->temp_c;
//                     support += nn[i].second->count;
//                     stalls += nn[i].second->stall_count;
//                 }
//                 SurrogatePrediction p;
//                 p.fps = fps / wsum; p.power_w = pw / wsum;
//                 p.latency_ms = lat / wsum; p.temp_c = tmp / wsum;
//                 p.from_data = true;
//                 p.source = SurrogateSource::KNN_INTERPOLATED;
//                 p.support_count = support;
//                 p.neighbor_count = static_cast<uint32_t>(k);
//                 p.nearest_distance = nearest;
//                 p.stall_count = stalls;
//                 p.stall_penalty = (support + stalls) > 0
//                     ? 0.50 * static_cast<double>(stalls) / static_cast<double>(support + stalls)
//                     : 0.0;
//                 // Confidence falls off with distance to the nearest observation.
//                 p.confidence = 0.6 / (1.0 + 4.0 * nearest);
//                 return p;
//             }
//         }

//         // 3) Cold-start physics prior.
//         return physicsPrior(freq_khz, gpu, gpu_split, concurrency);
//     }

//     // ------------------------------------------------------------------
//     // explorationBonus(): optimism under uncertainty. Large for configs that
//     // have been sampled rarely or never; ~0 for well-measured configs.
//     // Add this to a MAXIMISED scalar fitness so under-explored configs are more
//     // likely to be selected and thus actually measured on hardware.
//     // ------------------------------------------------------------------
//     double explorationBonus(double freq_khz, bool gpu, double gpu_split,
//                             int concurrency) const {
//         std::lock_guard<std::mutex> lk(mutex_);
//         const uint32_t key = encode(freq_khz, gpu, gpu_split, concurrency);
//         auto it = buckets_.find(key);
//         const double attempts = (it == buckets_.end())
//             ? 0.0
//             : static_cast<double>(it->second.count + it->second.stall_count);
//         // A timeout is still an attempted hardware sample. Without including stalls,
//         // a repeatedly failing bucket can retain the full "unseen" exploration bonus.
//         return cfg_.exploration_weight / std::sqrt(1.0 + attempts);
//     }

//     // Record a bounded evidence timeout without fabricating physical measurements.
//     // The stall count is a feasibility signal used as a deterministic fitness penalty.
//     void observeStarvation(double freq_khz, bool gpu, double gpu_split, int concurrency) {
//         std::lock_guard<std::mutex> lk(mutex_);
//         Bucket& b = buckets_[encode(freq_khz, gpu, gpu_split, concurrency)];
//         ++b.stall_count;
//         ++total_stalls_;
//     }

//     // [FIX P7] Warm-start: inject an offline-measured operating point (e.g. a
//     // row from a previous run's actions CSV) as `count` pre-baked observations.
//     // Call once per known point right after construction, before the first evolve.
//     void seed(double freq_khz, bool gpu, double gpu_split, int concurrency,
//               double fps, double power_w, double latency_ms, double temp_c,
//               uint32_t count = 3) {
//         if (count == 0) return;
//         if (!std::isfinite(fps) || !std::isfinite(power_w) ||
//             !std::isfinite(latency_ms) || !std::isfinite(temp_c)) {
//             return;
//         }

//         fps = std::min(std::max(0.0, fps), 1.10 * cfg_.fps_max);

//         std::lock_guard<std::mutex> lk(mutex_);
//         Bucket& b = buckets_[encode(freq_khz, gpu, gpu_split, concurrency)];
//         b.fps = fps;
//         b.power_w = power_w;
//         b.latency_ms = latency_ms;
//         b.temp_c = temp_c;

//         const uint32_t oldCount = b.count;
//         b.count = std::max(b.count, count);
//         total_observations_ += static_cast<uint64_t>(b.count - oldCount);
//     }

//     double stallPenalty(double freq_khz, bool gpu, double gpu_split, int concurrency) const {
//         std::lock_guard<std::mutex> lk(mutex_);
//         const auto it = buckets_.find(encode(freq_khz, gpu, gpu_split, concurrency));
//         return it == buckets_.end() ? 0.0 : stallPenaltyForBucket(it->second);
//     }

//     SurrogateFeasibilityStats feasibilityStats(double freq_khz, bool gpu,
//                                                double gpu_split, int concurrency) const {
//         std::lock_guard<std::mutex> lk(mutex_);
//         SurrogateFeasibilityStats stats;
//         const auto it = buckets_.find(encode(freq_khz, gpu, gpu_split, concurrency));
//         if (it == buckets_.end()) return stats;

//         stats.success_count = it->second.count;
//         stats.stall_count = it->second.stall_count;
//         stats.attempts = stats.success_count + stats.stall_count;
//         if (stats.attempts > 0) {
//             stats.timeout_ratio =
//                 static_cast<double>(stats.stall_count) /
//                 static_cast<double>(stats.attempts);
//         }
//         return stats;
//     }

//     uint64_t totalStalls() const {
//         std::lock_guard<std::mutex> lk(mutex_);
//         return total_stalls_;
//     }

//     size_t distinctConfigsSeen() const {
//         std::lock_guard<std::mutex> lk(mutex_);
//         return buckets_.size();
//     }
//     uint64_t totalObservations() const {
//         std::lock_guard<std::mutex> lk(mutex_);
//         return total_observations_;
//     }

//     // CSV export of the learned model, for a thesis figure ("surrogate coverage
//     // of the configuration space" / "predicted vs measured").
//     std::string exportCSV() const {
//         std::lock_guard<std::mutex> lk(mutex_);
//         std::string csv = "freq_bucket,gpu,split_bucket,concurrency,count,stall_count,"
//                           "fps,power_w,latency_ms,temp_c\n";
//         for (const auto& kv : buckets_) {
//             int fb, g, sb, cc; decode(kv.first, fb, g, sb, cc);
//             const Bucket& b = kv.second;
//             csv += std::to_string(fb) + "," + std::to_string(g) + ","
//                  + std::to_string(sb) + "," + std::to_string(cc) + ","
//                  + std::to_string(b.count) + ","
//                  + std::to_string(b.stall_count) + ","
//                  + std::to_string(b.fps) + "," + std::to_string(b.power_w) + ","
//                  + std::to_string(b.latency_ms) + "," + std::to_string(b.temp_c) + "\n";
//         }
//         return csv;
//     }

// private:
//     struct Bucket {
//         double fps = 0, power_w = 0, latency_ms = 0, temp_c = 0;
//         uint32_t count = 0;
//         uint32_t stall_count = 0;
//     };

//     Config cfg_;
//     mutable std::mutex mutex_;
//     std::unordered_map<uint32_t, Bucket> buckets_;
//     uint64_t total_observations_ = 0;
//     uint64_t total_stalls_ = 0;

//     // ---- Discretisation --------------------------------------------------
//     // 5 CPU DVFS steps, GPU on/off, 5 GPU-split levels, concurrency 1..4.
//     static const std::array<double, 5>& freqSteps() {
//         static const std::array<double, 5> s{{102000, 460800, 921600, 1190400, 1479000}};
//         return s;
//     }
//     static int freqBucket(double khz) {
//         const auto& s = freqSteps();
//         int best = 0; double bd = std::abs(s[0] - khz);
//         for (int i = 1; i < 5; ++i) { double d = std::abs(s[i] - khz); if (d < bd) { bd = d; best = i; } }
//         return best;
//     }
//     static int splitBucket(double split) {
//         if (split < 0) split = 0;
//         if (split > 1) split = 1;
//         return static_cast<int>(std::lround(split * 4.0));  // 0,1,2,3,4 -> 0,.25,.5,.75,1
//     }
//     static int concBucket(int c) { return (c < 1) ? 1 : (c > 4) ? 4 : c; }

//     double canonicalSplit(bool gpu, double split) const {
//         if (!cfg_.gpu_split_applicable) {
//             return gpu ? 1.0 : 0.0;
//         }
//         return clamp01(split);
//     }

//     uint32_t encode(double freq_khz, bool gpu, double split, int conc) const {
//         const double effectiveSplit = canonicalSplit(gpu, split);
//         return static_cast<uint32_t>(freqBucket(freq_khz)) * 1000u
//              + static_cast<uint32_t>(gpu ? 1 : 0) * 100u
//              + static_cast<uint32_t>(splitBucket(effectiveSplit)) * 10u
//              + static_cast<uint32_t>(concBucket(conc));
//     }
//     static void decode(uint32_t key, int& fb, int& g, int& sb, int& cc) {
//         fb = (key / 1000) % 10; g = (key / 100) % 10; sb = (key / 10) % 10; cc = key % 10;
//     }

//     // Normalised feature vector for distance computation (all ~0..1).
//     std::array<double, 4> features(double freq_khz, bool gpu, double split, int conc) const {
//         const double fr = (freq_khz - cfg_.cpu_min_khz) /
//                           std::max(1.0, (cfg_.cpu_max_khz - cfg_.cpu_min_khz));
//         const double effectiveSplit = canonicalSplit(gpu, split);
//         return {{ clamp01(fr), gpu ? 1.0 : 0.0, effectiveSplit,
//                   (concBucket(conc) - 1) / 3.0 }};
//     }
//     std::array<double, 4> decodeFeatures(uint32_t key) const {
//         int fb, g, sb, cc; decode(key, fb, g, sb, cc);
//         const double khz = freqSteps()[fb];
//         return features(khz, g != 0, sb / 4.0, cc);
//     }

//     // Cold-start physical prior.
//     static double stallPenaltyForBucket(const Bucket& b) {
//         const double denom = static_cast<double>(b.count + b.stall_count);
//         return denom > 0.0 ? 0.50 * static_cast<double>(b.stall_count) / denom : 0.0;
//     }

//     SurrogatePrediction physicsPrior(double freq_khz, bool gpu, double /*split*/, int conc) const {
//         const double fr = clamp01((freq_khz - cfg_.cpu_min_khz) /
//                                   std::max(1.0, (cfg_.cpu_max_khz - cfg_.cpu_min_khz)));
//         SurrogatePrediction p;
//         // Power: static floor + clock-proportional dynamic + optional GPU term,
//         // scaled mildly by concurrency (more active cores => more power).
//         const double conc_scale = 1.0 + 0.15 * (concBucket(conc) - 2);
//         p.power_w = cfg_.p_static_w + cfg_.p_dynamic_w * fr * conc_scale
//                   + (gpu ? cfg_.p_gpu_w : 0.0);
//         // FPS: compute-bound, roughly clock-proportional up to the workload cap.
//         p.fps = std::max(1.0, cfg_.fps_max * fr);
//         // Processing latency is independent of the frame-arrival period 1000/FPS.
//         // Keep a low-confidence, workload-configurable prior until real latency
//         // observations are available.
//         p.latency_ms = std::max(0.0, cfg_.latency_prior_ms);
//         p.temp_c = cfg_.ambient_c + cfg_.temp_per_watt * (p.power_w - cfg_.p_static_w);
//         p.confidence = 0.1;   // low: this is a prior, not a measurement
//         p.from_data = false;
//         p.source = SurrogateSource::PHYSICS_PRIOR;
//         p.support_count = 0;
//         p.neighbor_count = 0;
//         p.nearest_distance = -1.0;
//         p.stall_count = 0;
//         p.stall_penalty = 0.0;
//         return p;
//     }

//     static double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
// };

// } // namespace hrl

// // =====================================================================
// // SurrogateModel.h
// // Online power/performance surrogate for the ERL fitness function.
// // Header-only | C++14 | Jetson Nano friendly (std + spdlog only)
// // =====================================================================
// //
// // PhD MOTIVATION (closes evaluation gap: single-sample + surrogate fitness)
// // ------------------------------------------------------------------------
// // In an online evolutionary controller, only ONE candidate configuration can
// // be applied to the hardware per control interval ? you cannot run the CPU at
// // 102 MHz and 1479 MHz simultaneously. The remaining population members were
// // previously scored against the CURRENTLY measured snapshot via a crude,
// // frequency-blind simulation, so their objectives were largely fictional and
// // inconsistent with the one measured member. NSGA-II ranking over inconsistent
// // objectives is exactly what produced the observed premature collapse.
// //
// // This surrogate replaces "1 measured + N fabricated" with a single consistent
// // predictor that is:
// //   1. EXACT where a configuration has been measured (bucket mean, EMA-smoothed);
// //   2. INTERPOLATED for nearby unmeasured configs (distance-weighted k-NN in a
// //      normalised feature space);
// //   3. PHYSICS-PRIOR extrapolated during cold start (P ~ P_static + k*f,
// //      fps ~ fps_max * freq_ratio), so early generations are not garbage.
// //
// // It also exposes an OPTIMISM-UNDER-UNCERTAINTY (UCB-style) exploration bonus:
// // rarely/never-sampled configurations receive a bonus that makes them more
// // likely to be selected and therefore actually measured on hardware, which in
// // turn improves the surrogate. This couples exploration to model improvement in
// // one principled term and directly counters the single-mode collapse.
// //
// // Contribution framing for the thesis:
// //   "An online, physically-grounded surrogate with optimism-driven sampling for
// //    consistent counterfactual fitness in on-device evolutionary DVFS control."
// // =====================================================================
// #pragma once

// #include <algorithm>
// #include <array>
// #include <cmath>
// #include <cstdint>
// #include <string>
// #include <unordered_map>
// #include <vector>
// #include <mutex>
// #include <spdlog/spdlog.h>

// namespace hrl {

// // Prediction provenance. The confidence value is a support heuristic, not a
// // calibrated probability of correctness.
// enum class SurrogateSource {
//     DISABLED = 0,
//     PHYSICS_PRIOR = 1,
//     KNN_INTERPOLATED = 2,
//     EXACT_BUCKET = 3
// };

// // Prediction returned for an arbitrary candidate configuration.
// struct SurrogatePrediction {
//     double fps        = 0.0;
//     double power_w    = 0.0;
//     double latency_ms = 0.0;
//     double temp_c     = 0.0;
//     double confidence = 0.0;   // support/confidence heuristic, not probability
//     bool   from_data  = false;

//     SurrogateSource source = SurrogateSource::DISABLED;
//     uint32_t support_count = 0;   // measured samples contributing to this prediction
//     uint32_t neighbor_count = 0;  // 1 exact bucket, k for interpolation, 0 prior
//     double nearest_distance = -1.0; // normalized feature-space distance; 0 exact
//     uint32_t stall_count = 0;      // bounded evidence-timeout observations
//     double stall_penalty = 0.0;    // feasibility penalty; not a physical measurement
// };

// class SurrogateModel {
// public:
//     struct Config {
//         double ema_alpha;            // smoothing for repeated observations of a bucket
//         int    knn_k;                // neighbours used for interpolation
//         double exploration_weight;   // UCB bonus scale (fitness units)
//         // Hardware envelope priors (Jetson Nano; override per platform).
//         double cpu_min_khz;
//         double cpu_max_khz;
//         double p_static_w;           // idle/static power floor
//         double p_dynamic_w;          // extra power at max clock, CPU only
//         double p_gpu_w;              // extra power when GPU active
//         double fps_max;              // achievable fps at max clock (workload-dependent)
//         double ambient_c;            // baseline temperature
//         double temp_per_watt;        // steady-state temperature rise per watt

//         Config()
//             : ema_alpha(0.30),
//               knn_k(3),
//               exploration_weight(0.15),
//               cpu_min_khz(102000.0),
//               cpu_max_khz(1479000.0),
//               p_static_w(1.5),
//               p_dynamic_w(2.0),
//               p_gpu_w(1.0),
//               fps_max(60.0),
//               ambient_c(38.0),
//               temp_per_watt(6.0)
//         {}
//     };

//     explicit SurrogateModel(Config cfg = Config()) : cfg_(cfg) {}

//     static const char* sourceName(SurrogateSource source) {
//         switch (source) {
//             case SurrogateSource::PHYSICS_PRIOR:     return "PHYSICS_PRIOR";
//             case SurrogateSource::KNN_INTERPOLATED:  return "KNN_INTERPOLATED";
//             case SurrogateSource::EXACT_BUCKET:      return "EXACT_BUCKET";
//             case SurrogateSource::DISABLED:
//             default:                                 return "DISABLED";
//         }
//     }

//     // ------------------------------------------------------------------
//     // observe(): feed a REAL measured outcome for the configuration that
//     // produced it. Call once per control interval with the config that was
//     // actually applied and the snapshot that resulted from it.
//     // ------------------------------------------------------------------
//     void observe(double freq_khz, bool gpu, double gpu_split, int concurrency,
//                  double fps, double power_w, double latency_ms, double temp_c) {
//         // Reject obviously invalid samples (e.g. warmup zeros) so the model is
//         // not poisoned by frames that never ran.
//         if (fps <= 0.0 && power_w <= 0.0) return;

//         // std::lock_guard<std::mutex> lk(mutex_);
//         // const uint32_t key = encode(freq_khz, gpu, gpu_split, concurrency);
//         // Bucket& b = buckets_[key];
//         // const double a = (b.count == 0) ? 1.0 : cfg_.ema_alpha;  // seed on first sample

//         std::lock_guard<std::mutex> lk(mutex_);
//         // [FIX P6] Sample hygiene: a lagging pipeline average credited across an
//         // action boundary can exceed anything this workload can do (the immortal
//         // 71.8-fps MedianFilter bucket). Clamp to the workload envelope.
//         const double fps_ceiling = 1.10 * cfg_.fps_max;
//         if (fps > fps_ceiling) fps = fps_ceiling;

//         const uint32_t key = encode(freq_khz, gpu, gpu_split, concurrency);
//         Bucket& b = buckets_[key];
//         // [FIX P6] The first sample blends 60/40 with the physics prior instead of
//         // overwriting the bucket outright (old a=1.0 made one transient the truth).
//         const bool first = (b.count == 0);
//         if (first) {
//             const SurrogatePrediction prior =
//                 physicsPrior(freq_khz, gpu, gpu_split, concurrency);
//             b.fps = prior.fps; b.power_w = prior.power_w;
//             b.latency_ms = prior.latency_ms; b.temp_c = prior.temp_c;
//         }
//         const double a = first ? 0.6 : cfg_.ema_alpha;
//         b.fps        = (1 - a) * b.fps        + a * fps;
//         b.power_w    = (1 - a) * b.power_w    + a * power_w;
//         b.latency_ms = (1 - a) * b.latency_ms + a * (latency_ms > 0 ? latency_ms
//                                                                      : (fps > 0 ? 1000.0 / fps : 0.0));
//         b.temp_c     = (1 - a) * b.temp_c     + a * temp_c;
//         b.count++;
//         total_observations_++;
//     }

//     // ------------------------------------------------------------------
//     // predict(): consistent outcome estimate for ANY candidate config.
//     // ------------------------------------------------------------------
//     SurrogatePrediction predict(double freq_khz, bool gpu, double gpu_split,
//                                 int concurrency) const {
//         std::lock_guard<std::mutex> lk(mutex_);
//         const uint32_t key = encode(freq_khz, gpu, gpu_split, concurrency);

//         // 1) Exact bucket hit.
//         auto it = buckets_.find(key);
//         if (it != buckets_.end() && it->second.count > 0) {
//             const Bucket& b = it->second;
//             SurrogatePrediction p;
//             p.fps = b.fps; p.power_w = b.power_w;
//             p.latency_ms = b.latency_ms; p.temp_c = b.temp_c;
//             p.from_data = true;
//             p.source = SurrogateSource::EXACT_BUCKET;
//             p.support_count = b.count;
//             p.neighbor_count = 1;
//             p.nearest_distance = 0.0;
//             p.stall_count = b.stall_count;
//             p.stall_penalty = stallPenaltyForBucket(b);
//             // Confidence saturates with repeated observation of this bucket.
//             p.confidence = 1.0 - 1.0 / (1.0 + static_cast<double>(b.count));
//             return p;
//         }

//         // 2) Distance-weighted k-NN interpolation from observed buckets.
//         if (!buckets_.empty()) {
//             const std::array<double, 4> q = features(freq_khz, gpu, gpu_split, concurrency);
//             std::vector<std::pair<double, const Bucket*>> nn;  // (distance, bucket)
//             nn.reserve(buckets_.size());
//             for (const auto& kv : buckets_) {
//                 if (kv.second.count == 0) continue;
//                 const std::array<double, 4> f = decodeFeatures(kv.first);
//                 double d2 = 0.0;
//                 for (int i = 0; i < 4; ++i) { const double e = q[i] - f[i]; d2 += e * e; }
//                 nn.emplace_back(std::sqrt(d2), &kv.second);
//             }
//             if (!nn.empty()) {
//                 std::partial_sort(
//                     nn.begin(),
//                     nn.begin() + std::min<size_t>(cfg_.knn_k, nn.size()),
//                     nn.end(),
//                     [](const std::pair<double, const Bucket*>& x,
//                        const std::pair<double, const Bucket*>& y) { return x.first < y.first; });
//                 const size_t k = std::min<size_t>(cfg_.knn_k, nn.size());
//                 double wsum = 0, fps = 0, pw = 0, lat = 0, tmp = 0, nearest = nn[0].first;
//                 uint32_t support = 0;
//                 uint32_t stalls = 0;
//                 for (size_t i = 0; i < k; ++i) {
//                     const double w = 1.0 / (1e-3 + nn[i].first);   // inverse-distance
//                     wsum += w;
//                     fps += w * nn[i].second->fps;
//                     pw  += w * nn[i].second->power_w;
//                     lat += w * nn[i].second->latency_ms;
//                     tmp += w * nn[i].second->temp_c;
//                     support += nn[i].second->count;
//                     stalls += nn[i].second->stall_count;
//                 }
//                 SurrogatePrediction p;
//                 p.fps = fps / wsum; p.power_w = pw / wsum;
//                 p.latency_ms = lat / wsum; p.temp_c = tmp / wsum;
//                 p.from_data = true;
//                 p.source = SurrogateSource::KNN_INTERPOLATED;
//                 p.support_count = support;
//                 p.neighbor_count = static_cast<uint32_t>(k);
//                 p.nearest_distance = nearest;
//                 p.stall_count = stalls;
//                 p.stall_penalty = (support + stalls) > 0
//                     ? 0.50 * static_cast<double>(stalls) / static_cast<double>(support + stalls)
//                     : 0.0;
//                 // Confidence falls off with distance to the nearest observation.
//                 p.confidence = 0.6 / (1.0 + 4.0 * nearest);
//                 return p;
//             }
//         }

//         // 3) Cold-start physics prior.
//         return physicsPrior(freq_khz, gpu, gpu_split, concurrency);
//     }

//     // ------------------------------------------------------------------
//     // explorationBonus(): optimism under uncertainty. Large for configs that
//     // have been sampled rarely or never; ~0 for well-measured configs.
//     // Add this to a MAXIMISED scalar fitness so under-explored configs are more
//     // likely to be selected and thus actually measured on hardware.
//     // ------------------------------------------------------------------
//     double explorationBonus(double freq_khz, bool gpu, double gpu_split,
//                             int concurrency) const {
//         std::lock_guard<std::mutex> lk(mutex_);
//         const uint32_t key = encode(freq_khz, gpu, gpu_split, concurrency);
//         auto it = buckets_.find(key);
//         const double n = (it == buckets_.end()) ? 0.0 : static_cast<double>(it->second.count);
//         // UCB-style: bonus ~ w / sqrt(1+n); unseen configs get the full weight.
//         return cfg_.exploration_weight / std::sqrt(1.0 + n);
//     }

//     // Record a bounded evidence timeout without fabricating physical measurements.
//     // The stall count is a feasibility signal used as a deterministic fitness penalty.
//     void observeStarvation(double freq_khz, bool gpu, double gpu_split, int concurrency) {
//         std::lock_guard<std::mutex> lk(mutex_);
//         Bucket& b = buckets_[encode(freq_khz, gpu, gpu_split, concurrency)];
//         ++b.stall_count;
//         ++total_stalls_;
//     }

//     // [FIX P7] Warm-start: inject an offline-measured operating point (e.g. a
//     // row from a previous run's actions CSV) as `count` pre-baked observations.
//     // Call once per known point right after construction, before the first evolve.
//     void seed(double freq_khz, bool gpu, double gpu_split, int concurrency,
//               double fps, double power_w, double latency_ms, double temp_c,
//               uint32_t count = 3) {
//         std::lock_guard<std::mutex> lk(mutex_);
//         Bucket& b = buckets_[encode(freq_khz, gpu, gpu_split, concurrency)];
//         b.fps = fps; b.power_w = power_w;
//         b.latency_ms = latency_ms; b.temp_c = temp_c;
//         b.count = std::max(b.count, count);
//         total_observations_ += count;
//     }

//     double stallPenalty(double freq_khz, bool gpu, double gpu_split, int concurrency) const {
//         std::lock_guard<std::mutex> lk(mutex_);
//         const auto it = buckets_.find(encode(freq_khz, gpu, gpu_split, concurrency));
//         return it == buckets_.end() ? 0.0 : stallPenaltyForBucket(it->second);
//     }

//     uint64_t totalStalls() const {
//         std::lock_guard<std::mutex> lk(mutex_);
//         return total_stalls_;
//     }

//     size_t distinctConfigsSeen() const {
//         std::lock_guard<std::mutex> lk(mutex_);
//         return buckets_.size();
//     }
//     uint64_t totalObservations() const {
//         std::lock_guard<std::mutex> lk(mutex_);
//         return total_observations_;
//     }

//     // CSV export of the learned model, for a thesis figure ("surrogate coverage
//     // of the configuration space" / "predicted vs measured").
//     std::string exportCSV() const {
//         std::lock_guard<std::mutex> lk(mutex_);
//         std::string csv = "freq_bucket,gpu,split_bucket,concurrency,count,stall_count,"
//                           "fps,power_w,latency_ms,temp_c\n";
//         for (const auto& kv : buckets_) {
//             int fb, g, sb, cc; decode(kv.first, fb, g, sb, cc);
//             const Bucket& b = kv.second;
//             csv += std::to_string(fb) + "," + std::to_string(g) + ","
//                  + std::to_string(sb) + "," + std::to_string(cc) + ","
//                  + std::to_string(b.count) + ","
//                  + std::to_string(b.stall_count) + ","
//                  + std::to_string(b.fps) + "," + std::to_string(b.power_w) + ","
//                  + std::to_string(b.latency_ms) + "," + std::to_string(b.temp_c) + "\n";
//         }
//         return csv;
//     }

// private:
//     struct Bucket {
//         double fps = 0, power_w = 0, latency_ms = 0, temp_c = 0;
//         uint32_t count = 0;
//         uint32_t stall_count = 0;
//     };

//     Config cfg_;
//     mutable std::mutex mutex_;
//     std::unordered_map<uint32_t, Bucket> buckets_;
//     uint64_t total_observations_ = 0;
//     uint64_t total_stalls_ = 0;

//     // ---- Discretisation --------------------------------------------------
//     // 5 CPU DVFS steps, GPU on/off, 5 GPU-split levels, concurrency 1..4.
//     static const std::array<double, 5>& freqSteps() {
//         static const std::array<double, 5> s{{102000, 460800, 921600, 1190400, 1479000}};
//         return s;
//     }
//     static int freqBucket(double khz) {
//         const auto& s = freqSteps();
//         int best = 0; double bd = std::abs(s[0] - khz);
//         for (int i = 1; i < 5; ++i) { double d = std::abs(s[i] - khz); if (d < bd) { bd = d; best = i; } }
//         return best;
//     }
//     static int splitBucket(double split) {
//         if (split < 0) split = 0;
//         if (split > 1) split = 1;
//         return static_cast<int>(std::lround(split * 4.0));  // 0,1,2,3,4 -> 0,.25,.5,.75,1
//     }
//     static int concBucket(int c) { return (c < 1) ? 1 : (c > 4) ? 4 : c; }

//     static uint32_t encode(double freq_khz, bool gpu, double split, int conc) {
//         return static_cast<uint32_t>(freqBucket(freq_khz)) * 1000u
//              + static_cast<uint32_t>(gpu ? 1 : 0) * 100u
//              + static_cast<uint32_t>(splitBucket(split)) * 10u
//              + static_cast<uint32_t>(concBucket(conc));
//     }
//     static void decode(uint32_t key, int& fb, int& g, int& sb, int& cc) {
//         fb = (key / 1000) % 10; g = (key / 100) % 10; sb = (key / 10) % 10; cc = key % 10;
//     }

//     // Normalised feature vector for distance computation (all ~0..1).
//     std::array<double, 4> features(double freq_khz, bool gpu, double split, int conc) const {
//         const double fr = (freq_khz - cfg_.cpu_min_khz) /
//                           std::max(1.0, (cfg_.cpu_max_khz - cfg_.cpu_min_khz));
//         return {{ clamp01(fr), gpu ? 1.0 : 0.0, clamp01(split), (concBucket(conc) - 1) / 3.0 }};
//     }
//     std::array<double, 4> decodeFeatures(uint32_t key) const {
//         int fb, g, sb, cc; decode(key, fb, g, sb, cc);
//         const double khz = freqSteps()[fb];
//         return features(khz, g != 0, sb / 4.0, cc);
//     }

//     // Cold-start physical prior.
//     static double stallPenaltyForBucket(const Bucket& b) {
//         const double denom = static_cast<double>(b.count + b.stall_count);
//         return denom > 0.0 ? 0.50 * static_cast<double>(b.stall_count) / denom : 0.0;
//     }

//     SurrogatePrediction physicsPrior(double freq_khz, bool gpu, double /*split*/, int conc) const {
//         const double fr = clamp01((freq_khz - cfg_.cpu_min_khz) /
//                                   std::max(1.0, (cfg_.cpu_max_khz - cfg_.cpu_min_khz)));
//         SurrogatePrediction p;
//         // Power: static floor + clock-proportional dynamic + optional GPU term,
//         // scaled mildly by concurrency (more active cores => more power).
//         const double conc_scale = 1.0 + 0.15 * (concBucket(conc) - 2);
//         p.power_w = cfg_.p_static_w + cfg_.p_dynamic_w * fr * conc_scale
//                   + (gpu ? cfg_.p_gpu_w : 0.0);
//         // FPS: compute-bound, roughly clock-proportional up to the workload cap.
//         p.fps = std::max(1.0, cfg_.fps_max * fr);
//         p.latency_ms = (p.fps > 0) ? 1000.0 / p.fps : 0.0;
//         p.temp_c = cfg_.ambient_c + cfg_.temp_per_watt * (p.power_w - cfg_.p_static_w);
//         p.confidence = 0.1;   // low: this is a prior, not a measurement
//         p.from_data = false;
//         p.source = SurrogateSource::PHYSICS_PRIOR;
//         p.support_count = 0;
//         p.neighbor_count = 0;
//         p.nearest_distance = -1.0;
//         p.stall_count = 0;
//         p.stall_penalty = 0.0;
//         return p;
//     }

//     static double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
// };

// } // namespace hrl


// // =====================================================================
// // SurrogateModel.h
// // Online power/performance surrogate for the ERL fitness function.
// // Header-only | C++14 | Jetson Nano friendly (std + spdlog only)
// // =====================================================================
// //
// // PhD MOTIVATION (closes evaluation gap: single-sample + surrogate fitness)
// // ------------------------------------------------------------------------
// // In an online evolutionary controller, only ONE candidate configuration can
// // be applied to the hardware per control interval ? you cannot run the CPU at
// // 102 MHz and 1479 MHz simultaneously. The remaining population members were
// // previously scored against the CURRENTLY measured snapshot via a crude,
// // frequency-blind simulation, so their objectives were largely fictional and
// // inconsistent with the one measured member. NSGA-II ranking over inconsistent
// // objectives is exactly what produced the observed premature collapse.
// //
// // This surrogate replaces "1 measured + N fabricated" with a single consistent
// // predictor that is:
// //   1. EXACT where a configuration has been measured (bucket mean, EMA-smoothed);
// //   2. INTERPOLATED for nearby unmeasured configs (distance-weighted k-NN in a
// //      normalised feature space);
// //   3. PHYSICS-PRIOR extrapolated during cold start (P ~ P_static + k*f,
// //      fps ~ fps_max * freq_ratio), so early generations are not garbage.
// //
// // It also exposes an OPTIMISM-UNDER-UNCERTAINTY (UCB-style) exploration bonus:
// // rarely/never-sampled configurations receive a bonus that makes them more
// // likely to be selected and therefore actually measured on hardware, which in
// // turn improves the surrogate. This couples exploration to model improvement in
// // one principled term and directly counters the single-mode collapse.
// //
// // Contribution framing for the thesis:
// //   "An online, physically-grounded surrogate with optimism-driven sampling for
// //    consistent counterfactual fitness in on-device evolutionary DVFS control."
// // =====================================================================
// #pragma once

// #include <algorithm>
// #include <array>
// #include <cmath>
// #include <cstdint>
// #include <string>
// #include <unordered_map>
// #include <vector>
// #include <mutex>
// #include <spdlog/spdlog.h>

// namespace hrl {

// // Prediction returned for an arbitrary candidate configuration.
// struct SurrogatePrediction {
//     double fps        = 0.0;
//     double power_w    = 0.0;
//     double latency_ms = 0.0;
//     double temp_c     = 0.0;
//     double confidence = 0.0;   // 1.0 = directly observed; decays with distance
//     bool   from_data  = false; // false => pure physics prior (cold start)
// };

// class SurrogateModel {
// public:
//     struct Config {
//         double ema_alpha;            // smoothing for repeated observations of a bucket
//         int    knn_k;                // neighbours used for interpolation
//         double exploration_weight;   // UCB bonus scale (fitness units)
//         // Hardware envelope priors (Jetson Nano; override per platform).
//         double cpu_min_khz;
//         double cpu_max_khz;
//         double p_static_w;           // idle/static power floor
//         double p_dynamic_w;          // extra power at max clock, CPU only
//         double p_gpu_w;              // extra power when GPU active
//         double fps_max;              // achievable fps at max clock (workload-dependent)
//         double ambient_c;            // baseline temperature
//         double temp_per_watt;        // steady-state temperature rise per watt

//         Config()
//             : ema_alpha(0.30),
//               knn_k(3),
//               exploration_weight(0.15),
//               cpu_min_khz(102000.0),
//               cpu_max_khz(1479000.0),
//               p_static_w(1.5),
//               p_dynamic_w(2.0),
//               p_gpu_w(1.0),
//               fps_max(60.0),
//               ambient_c(38.0),
//               temp_per_watt(6.0)
//         {}
//     };

//     explicit SurrogateModel(Config cfg = Config()) : cfg_(cfg) {}

//     // ------------------------------------------------------------------
//     // observe(): feed a REAL measured outcome for the configuration that
//     // produced it. Call once per control interval with the config that was
//     // actually applied and the snapshot that resulted from it.
//     // ------------------------------------------------------------------

//     // Observe real measured outcome
//     void observe(double freq_khz, bool gpu, double gpu_split, int concurrency,
//                  double fps, double power_w, double latency_ms, double temp_c) {
//         // Reject obviously invalid samples (e.g. warmup zeros) so the model is
//         // not poisoned by frames that never ran.

//         // Allow starvation/penalty observations (fps == 0) to be stored.
//        // if (fps <= 0.0 && power_w <= 0.0) return;   // Drops starvation data!

//         if (fps < 0.0 || power_w < 0.0) return;

//         std::lock_guard<std::mutex> lk(mutex_);
//         const uint32_t key = encode(freq_khz, gpu, gpu_split, concurrency);
//         Bucket& b = buckets_[key];
//         const double a = (b.count == 0) ? 1.0 : cfg_.ema_alpha;  // seed on first sample
//         b.fps        = (1 - a) * b.fps        + a * fps;
//         b.power_w    = (1 - a) * b.power_w    + a * power_w;
//         b.latency_ms = (1 - a) * b.latency_ms + a * (latency_ms > 0 ? latency_ms
//                                                                      : (fps > 0 ? 1000.0 / fps : 0.0));
//         b.temp_c     = (1 - a) * b.temp_c     + a * temp_c;
//         b.count++;
//         total_observations_++;
//     }

//     // [FIX: Bounded Timeout Penalty] Dedicated handler to teach surrogate that config caused pipeline stall
//     void observeStarvation(double freq_khz, bool gpu, double gpu_split, int concurrency) {
//         spdlog::warn("[SurrogateModel] Registering STARVATION penalty for freq={:.0f}kHz gpu={} split={:.2f} conc={}",
//                      freq_khz, gpu, gpu_split, concurrency);
//         // Force 0 FPS and maximum latency penalty (10000ms)
//         observe(freq_khz, gpu, gpu_split, concurrency, 0.0, cfg_.p_static_w, 10000.0, cfg_.ambient_c);
//     }
    

//     // ------------------------------------------------------------------
//     // predict(): consistent outcome estimate for ANY candidate config.
//     // ------------------------------------------------------------------
//     SurrogatePrediction predict(double freq_khz, bool gpu, double gpu_split,
//                                 int concurrency) const {
//         std::lock_guard<std::mutex> lk(mutex_);
//         const uint32_t key = encode(freq_khz, gpu, gpu_split, concurrency);

//         // 1) Exact bucket hit.
//         auto it = buckets_.find(key);
//         if (it != buckets_.end() && it->second.count > 0) {
//             const Bucket& b = it->second;
//             SurrogatePrediction p;
//             p.fps = b.fps; p.power_w = b.power_w;
//             p.latency_ms = b.latency_ms; p.temp_c = b.temp_c;
//             p.from_data = true;
//             // Confidence saturates with repeated observation of this bucket.
//             p.confidence = 1.0 - 1.0 / (1.0 + static_cast<double>(b.count));
//             return p;
//         }

//         // 2) Distance-weighted k-NN interpolation from observed buckets.
//         if (!buckets_.empty()) {
//             const std::array<double, 4> q = features(freq_khz, gpu, gpu_split, concurrency);
//             std::vector<std::pair<double, const Bucket*>> nn;  // (distance, bucket)
//             nn.reserve(buckets_.size());
//             for (const auto& kv : buckets_) {
//                 if (kv.second.count == 0) continue;
//                 const std::array<double, 4> f = decodeFeatures(kv.first);
//                 double d2 = 0.0;
//                 for (int i = 0; i < 4; ++i) { const double e = q[i] - f[i]; d2 += e * e; }
//                 nn.emplace_back(std::sqrt(d2), &kv.second);
//             }
//             if (!nn.empty()) {
//                 std::partial_sort(
//                     nn.begin(),
//                     nn.begin() + std::min<size_t>(cfg_.knn_k, nn.size()),
//                     nn.end(),
//                     [](const std::pair<double, const Bucket*>& x,
//                        const std::pair<double, const Bucket*>& y) { return x.first < y.first; });
//                 const size_t k = std::min<size_t>(cfg_.knn_k, nn.size());
//                 double wsum = 0, fps = 0, pw = 0, lat = 0, tmp = 0, nearest = nn[0].first;
//                 for (size_t i = 0; i < k; ++i) {
//                     const double w = 1.0 / (1e-3 + nn[i].first);   // inverse-distance
//                     wsum += w;
//                     fps += w * nn[i].second->fps;
//                     pw  += w * nn[i].second->power_w;
//                     lat += w * nn[i].second->latency_ms;
//                     tmp += w * nn[i].second->temp_c;
//                 }
//                 SurrogatePrediction p;
//                 p.fps = fps / wsum; p.power_w = pw / wsum;
//                 p.latency_ms = lat / wsum; p.temp_c = tmp / wsum;
//                 p.from_data = true;
//                 // Confidence falls off with distance to the nearest observation.
//                 p.confidence = 0.6 / (1.0 + 4.0 * nearest);
//                 return p;
//             }
//         }

//         // 3) Cold-start physics prior.
//         return physicsPrior(freq_khz, gpu, gpu_split, concurrency);
//     }

//     // ------------------------------------------------------------------
//     // explorationBonus(): optimism under uncertainty. Large for configs that
//     // have been sampled rarely or never; ~0 for well-measured configs.
//     // Add this to a MAXIMISED scalar fitness so under-explored configs are more
//     // likely to be selected and thus actually measured on hardware.
//     // ------------------------------------------------------------------
//     double explorationBonus(double freq_khz, bool gpu, double gpu_split,
//                             int concurrency) const {
//         std::lock_guard<std::mutex> lk(mutex_);
//         const uint32_t key = encode(freq_khz, gpu, gpu_split, concurrency);
//         auto it = buckets_.find(key);
//         const double n = (it == buckets_.end()) ? 0.0 : static_cast<double>(it->second.count);
//         // UCB-style: bonus ~ w / sqrt(1+n); unseen configs get the full weight.
//         return cfg_.exploration_weight / std::sqrt(1.0 + n);
//     }

//     size_t distinctConfigsSeen() const {
//         std::lock_guard<std::mutex> lk(mutex_);
//         return buckets_.size();
//     }
//     uint64_t totalObservations() const {
//         std::lock_guard<std::mutex> lk(mutex_);
//         return total_observations_;
//     }

//     // CSV export of the learned model, for a thesis figure ("surrogate coverage
//     // of the configuration space" / "predicted vs measured").
//     std::string exportCSV() const {
//         std::lock_guard<std::mutex> lk(mutex_);
//         std::string csv = "freq_bucket,gpu,split_bucket,concurrency,count,"
//                           "fps,power_w,latency_ms,temp_c\n";
//         for (const auto& kv : buckets_) {
//             int fb, g, sb, cc; decode(kv.first, fb, g, sb, cc);
//             const Bucket& b = kv.second;
//             csv += std::to_string(fb) + "," + std::to_string(g) + ","
//                  + std::to_string(sb) + "," + std::to_string(cc) + ","
//                  + std::to_string(b.count) + ","
//                  + std::to_string(b.fps) + "," + std::to_string(b.power_w) + ","
//                  + std::to_string(b.latency_ms) + "," + std::to_string(b.temp_c) + "\n";
//         }
//         return csv;
//     }

// private:
//     struct Bucket {
//         double fps = 0, power_w = 0, latency_ms = 0, temp_c = 0;
//         uint32_t count = 0;
//     };

//     Config cfg_;
//     mutable std::mutex mutex_;
//     std::unordered_map<uint32_t, Bucket> buckets_;
//     uint64_t total_observations_ = 0;

//     // ---- Discretisation --------------------------------------------------
//     // 5 CPU DVFS steps, GPU on/off, 5 GPU-split levels, concurrency 1..4.
//     static const std::array<double, 5>& freqSteps() {
//         static const std::array<double, 5> s{{102000, 460800, 921600, 1190400, 1479000}};
//         return s;
//     }
//     static int freqBucket(double khz) {
//         const auto& s = freqSteps();
//         int best = 0; double bd = std::abs(s[0] - khz);
//         for (int i = 1; i < 5; ++i) { double d = std::abs(s[i] - khz); if (d < bd) { bd = d; best = i; } }
//         return best;
//     }
//     static int splitBucket(double split) {
//         if (split < 0) split = 0; if (split > 1) split = 1;
//         return static_cast<int>(std::lround(split * 4.0));  // 0,1,2,3,4 -> 0,.25,.5,.75,1
//     }
//     static int concBucket(int c) { return (c < 1) ? 1 : (c > 4) ? 4 : c; }

//     static uint32_t encode(double freq_khz, bool gpu, double split, int conc) {
//         return static_cast<uint32_t>(freqBucket(freq_khz)) * 1000u
//              + static_cast<uint32_t>(gpu ? 1 : 0) * 100u
//              + static_cast<uint32_t>(splitBucket(split)) * 10u
//              + static_cast<uint32_t>(concBucket(conc));
//     }
//     static void decode(uint32_t key, int& fb, int& g, int& sb, int& cc) {
//         fb = (key / 1000) % 10; g = (key / 100) % 10; sb = (key / 10) % 10; cc = key % 10;
//     }

//     // Normalised feature vector for distance computation (all ~0..1).
//     std::array<double, 4> features(double freq_khz, bool gpu, double split, int conc) const {
//         const double fr = (freq_khz - cfg_.cpu_min_khz) /
//                           std::max(1.0, (cfg_.cpu_max_khz - cfg_.cpu_min_khz));
//         return {{ clamp01(fr), gpu ? 1.0 : 0.0, clamp01(split), (concBucket(conc) - 1) / 3.0 }};
//     }
//     std::array<double, 4> decodeFeatures(uint32_t key) const {
//         int fb, g, sb, cc; decode(key, fb, g, sb, cc);
//         const double khz = freqSteps()[fb];
//         return features(khz, g != 0, sb / 4.0, cc);
//     }

//     // Cold-start physical prior.
//     SurrogatePrediction physicsPrior(double freq_khz, bool gpu, double /*split*/, int conc) const {
//         const double fr = clamp01((freq_khz - cfg_.cpu_min_khz) /
//                                   std::max(1.0, (cfg_.cpu_max_khz - cfg_.cpu_min_khz)));
//         SurrogatePrediction p;
//         // Power: static floor + clock-proportional dynamic + optional GPU term,
//         // scaled mildly by concurrency (more active cores => more power).
//         const double conc_scale = 1.0 + 0.15 * (concBucket(conc) - 2);
//         p.power_w = cfg_.p_static_w + cfg_.p_dynamic_w * fr * conc_scale
//                   + (gpu ? cfg_.p_gpu_w : 0.0);
//         // FPS: compute-bound, roughly clock-proportional up to the workload cap.
//         p.fps = std::max(1.0, cfg_.fps_max * fr);
//         p.latency_ms = (p.fps > 0) ? 1000.0 / p.fps : 0.0;
//         p.temp_c = cfg_.ambient_c + cfg_.temp_per_watt * (p.power_w - cfg_.p_static_w);
//         p.confidence = 0.1;   // low: this is a prior, not a measurement
//         p.from_data = false;
//         return p;
//     }

//     static double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
// };

// } // namespace hrl