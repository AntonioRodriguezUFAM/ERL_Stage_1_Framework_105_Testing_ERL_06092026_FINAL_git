# GeneticAlgorithm_05.h  production candidate (2026-08-20)

This is a complete replacement header, based on the synchronized telemetry-v3 / P0 constraint-aware GA and the latest requested fixes.

## Mandatory fixes included

1. `selectBestEvaluatedPareto()` never falls back to arbitrary `population_[0]`.
   - feasible Rank-0 + best fitness is the normal path;
   - if ranking regresses, any feasible evaluated candidate still beats infeasible candidates;
   - if all evaluated candidates are infeasible, the globally lowest `constraintViolation` is selected, then rank/fitness break ties;
   - if nothing has been evaluated, selection returns `nullptr`.
2. `lastRank0RunnerUpFitness_` and `lastSelectionMargin_` are reset to NaN every evaluated generation.
3. `nonDominatedSortAndCrowding()` uses `ParetoIndividual::dominates()`, so Deb-style feasibility is actually used by NSGA-II.

## Configuration/wiring hardening

- The four-argument constructor now delegates with `cfg.awm_config`; it cannot silently replace the parsed AWM thresholds with a new default config.
- Startup logging prints effective AWM warning/critical thresholds and objective temperature onsets. A mismatch is explicitly warned.
- `gpuSplitApplicable` fails closed to `false`. HistogramEqualization, MedianFilter and Sobel therefore stay binary CPU/GPU unless the selector explicitly opts in.
- For `HeterogeneousGaussianBlur`, EvolutionarySelector/ConfigManager must explicitly set `gaCfg.gpuSplitApplicable = true` because that workload physically consumes a continuous split.

## Other hardening included

- Independent latency model retained; no `latency >= 1000/FPS` clamp.
- Exact-bucket timeout feasibility, headroom recovery, thermal-critical MAX blocking, and mode-diversity floor retained.
- Adaptive mutation uses the canonical feasibility-aware selector as its progress reference.
- Replay-buffer seeding sees the evaluated population, not newly created unevaluated children.
- `crossoverRate` is honored.
- `power_budget_watts` is preserved through crossover and can mutate.
- Surrogate-mode GPU-utilization bonus is disabled because current measured GPU utilization is not a counterfactual candidate prediction.
- Surrogate-mode temperature objective uses only the modeled temperature dimension until GPU temperature is independently predicted.

## Schema note

Telemetry remains schema v3 for analysis compatibility. The historical `obj_energy` CSV column is actually normalized predicted/measured **power** in the surrogate-enabled path. Treat it as `obj_power` in v3 analysis. Renaming the CSV column should be done only with a deliberate schema-version bump.

## Required external wiring check before HGB

For HistogramEqualization / MedianFilter / Sobel:

```cpp
gaCfg.gpuSplitApplicable = false;
```

For HeterogeneousGaussianBlur only:

```cpp
gaCfg.gpuSplitApplicable = true;
```

The current GA default is false, so binary workloads fail safely even if the selector omits the field.
