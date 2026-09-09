// Integration Guide

// =====================================================================
// Integration Guide  Wiring into GeneticAlgorithm
// =====================================================================

/*
  USAGE EXAMPLE: Integrate into GeneticAlgorithm::evolve()

  1. Add member variables to GeneticAlgorithm class:

  private:
    MemoryOptimizedMetricsBuffer<100> metrics_buffer_;
    ThermalGovernor thermal_governor_;
    FrameSkippingScheduler frame_skipper_;
    AdaptivePopulationManager<ParetoIndividual> pop_manager_;
    LatencyHistogram latency_histogram_;

  2. In GeneticAlgorithm::evolve():

    void evolve(const hrl::MetricsSnapshot& snapshot) {
        if (population_.empty() || !snapshot.valid) return;

        // === STEP 1: Update thermal throttling ===
        auto thermal_policy = thermal_governor_.update(
            snapshot.cpu_temp_c, snapshot.gpu_temp_c);
        
        if (thermal_governor_.shouldShutdown(snapshot.cpu_temp_c, snapshot.gpu_temp_c)) {
            spdlog::critical("[GA] Emergency thermal shutdown triggered!");
            runtimeControls_->shutdown = true;
            return;
        }

        // === STEP 2: Record latency for decision-making ===
        latency_histogram_.recordSample(snapshot.alg_total_proc_ms);
        
        // === STEP 3: Decide frame processing ===
        auto frame_decision = frame_skipper_.decide(
            snapshot.alg_avg_proc_ms,
            calculateQueueDepth(snapshot));  // TODO: implement queue depth tracking
        
        if (!frame_decision.should_process) {
            spdlog::debug("[GA] Frame skipped (latency={:.1f}ms, backlog={})",
                          snapshot.alg_avg_proc_ms, frame_decision.skip_budget_remaining);
            return;
        }

        // === STEP 4: Evaluate and evolve ===
        evaluateParetoPopulation(snapshot, thermal_policy, frame_decision.quality_scale);
        nonDominatedSortAndCrowding();
        
        // === STEP 5: Adaptive culling ===
        pop_manager_.cullPopulation(population_, [](const ParetoIndividual& ind) {
            return -ind.objectives[0];  // Fitness = FPS score
        });

        // Remove near-duplicates
        pop_manager_.removeDuplicates(population_, [](const auto& a, const auto& b) {
            // Simple genotype distance
            double dist = 0.0;
            if (a.genome.mode != b.genome.mode) dist += 0.25;
            if (a.genome.target_fps.has != b.genome.target_fps.has) dist += 0.25;
            if (a.genome.prefer_gpu.has != b.genome.prefer_gpu.has) dist += 0.25;
            return dist;
        });

        // === STEP 6: Adaptive mutation ===
        double adaptive_mutation = pop_manager_.getAdaptiveMutationRate(
            -population_[0].objectives[0], cfg_.mutationRate);
        cfg_.mutationRate = adaptive_mutation;

        // === STEP 7: Selection & breeding ===
        auto newPop = selectEliteAndOffspring();
        population_ = std::move(newPop);

        ++generation_;
        applyBestParetoToRuntime(snapshot);
        seedRLReplayBuffer();
        
        // === STEP 8: Buffer metrics ===
        MetricsFrame frame;
        frame.timestamp_ms = snapshot.timestamp_ms;
        frame.fps = snapshot.fps;
        frame.inference_ms = snapshot.alg_avg_proc_ms;
        frame.power_w = snapshot.avg_power_w_alg;
        frame.cpu_temp_c = snapshot.cpu_temp_c;
        frame.gpu_temp_c = snapshot.gpu_temp_c;
        frame.dropped_frames = snapshot.alg_dropped_frames;
        frame.valid = true;
        metrics_buffer_.push(frame);

        // === STEP 9: Log periodically ===
        if (generation_ % 10 == 0) {
            auto buf_stats = metrics_buffer_.getStats();
            auto hist_p95 = latency_histogram_.getPercentile(95.0);
            auto pop_metrics = pop_manager_.getMetrics(population_);
            
            spdlog::info("[GA] Gen {}| PopSize {}| Thermal {} | P95-latency {:.1f}ms | Mem {:.1f}KB",
                         generation_, population_.size(),
                         static_cast<int>(thermal_governor_.getCurrentStage()),
                         hist_p95, buf_stats.memory_kb);
        }
    }

  3. Add helper method:
  
    int calculateQueueDepth(const hrl::MetricsSnapshot& snap) {
        // Estimate queue depth from frame gaps
        static uint64_t last_ts = 0;
        uint64_t gap_ms = snap.timestamp_ms - last_ts;
        last_ts = snap.timestamp_ms;
        
        // If FPS target is 30, expect ~33ms per frame
        // If actual gap is 2x that, we have backlog
        int depth = (gap_ms > 66) ? (gap_ms / 33) : 0;
        return depth;
    }
*/