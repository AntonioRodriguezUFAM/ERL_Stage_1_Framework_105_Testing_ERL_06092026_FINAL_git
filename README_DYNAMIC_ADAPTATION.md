# Continual ERL Dynamic-Adaptation Production Patch

## Experimental claim implemented

This patch implements **one continuous ERL instance** with runtime algorithm-only workload changes:

- mission warmup: 30 s (excluded from measured mission time; an explicit GA hold keeps generation at 0 while the hardware/telemetry pipeline warms)
- 0300 s: `HistogramEqualization`
- 300600 s: `SobelEdge`
- 600900 s: `HeterogeneousGaussianBlur`
- 9001200 s: `HistogramEqualization`

`ConfigManager`, camera, display, Lynsyn, SoC telemetry, aggregator, `RuntimeControls`, Scheduler, ThermalGovernor, `EvolutionarySelector`, GeneticAlgorithm, AdaptiveWeightManager and SurrogateModel are created once and destroyed once. Only the internal `AlgorithmConcrete` worker is hot-swapped.

## Files

- `SampleTestIntegrationCode_v32.cpp`
  - adds `--dynamic-adaptation` mission mode;
  - starts/stops the pipeline once;
  - explicitly holds GA evolution at generation 0 during excluded warmup, then starts mission time and releases ERL at the same t=0 boundary;
  - removes periodic RuntimeControls re-pinning for normal ERL cases;
  - writes a 2 Hz cross-log trace;
  - writes transition evidence and exports persistent AWM/surrogate memory at the end.
- `Module/WorkloadMission.h`
  - runtime mission parser and explicit phase clock;
  - `WorkloadProfile` with stable `workloadId`;
  - durable transition evidence CSV.
- `Module/Modules.h`
  - makes `AlgorithmModule` a persistent slot with transactional `switchWorkload()`;
  - configures replacement before stopping the old worker;
  - keeps all shared queues/controls/telemetry objects alive;
  - records actual algorithm-only downtime.
- `Module/ConfigManager.h`
  - exposes `switchAlgorithmWorkload(profile)` without rebuilding the pipeline;
  - coordinates the GA transition barrier;
  - captures generation before/after while the barrier mutex is still held, eliminating post-commit sampling races;
  - exposes the mission-warmup evolution hold without stopping any pipeline module;
  - exposes continuity/evidence accessors.
- `Module/EvolutionarySelector_02.h`
  - serializes workload commits against `ga_->evolve()`;
  - pauses only evolution during warmup/hot-swap synchronization;
  - rejects pre-transition finalized snapshots before resuming;
  - keeps one authoritative GA-owned AWM (shadow AWM removed).
- `Module/GeneticAlgorithm_05.h`
  - changes workload context in place without resetting generation, population, RNG, adaptive mutation or AWM;
  - canonicalizes only physically invalid genes on transition;
  - conditions max-measured-FPS evidence by workload;
  - adds `workload_id`, `workload`, and `workload_epoch` to Pareto/action evidence.
- `Module/SurrogateModel.h`
  - retains a separate model bank per `workloadId`;
  - prevents Histogram/Sobel/Gaussian evidence contamination;
  - reactivates the original Histogram bank at phase 4.
- `validate_dynamic_adaptation.py`
  - checks the no-reset barrier, epoch increments, phase order, phase-4 surrogate recall, and optional action-generation monotonicity.

## Build integration

Place the files in the same locations as the originals:

```text
<project>/SampleTestIntegrationCode_v31.cpp
<project>/Module/ConfigManager.h
<project>/Module/EvolutionarySelector_02.h
<project>/Module/GeneticAlgorithm_05.h
<project>/Module/Modules.h
<project>/Module/SurrogateModel.h
<project>/Module/WorkloadMission.h
```

No change is required to camera, Lynsyn, Scheduler, AdaptiveWeightManager, ThermalGovernor, RuntimeControls or the aggregator for this experiment.

## Canonical thesis run

```bash
./MySystem --dynamic-adaptation config.json
```

This means 30 s excluded warmup followed by exactly 1200 s measured mission time.

For a short hardware smoke test using the exact same code path:

```bash
./MySystem --dynamic-adaptation --phase-seconds 30 --dynamic-warmup 5 config.json
```

For a custom schedule:

```bash
./MySystem --dynamic-adaptation \
  --mission "0:HistogramEqualization,10:SobelEdge,20:HeterogeneousGaussianBlur,30:HistogramEqualization" \
  --dynamic-duration 40 --dynamic-warmup 5 config.json
```

## Optional per-workload surrogate FPS ceilings

The default is 60 FPS. They can be overridden without recompilation:

```json
{
  "DynamicAdaptation": {
    "fps_max": {
      "HistogramEqualization": 60.0,
      "SobelEdge": 60.0,
      "HeterogeneousGaussianBlur": 60.0
    }
  }
}
```

## Evidence generated

The existing continuous files remain active: aggregator metrics/NDJSON, telemetry, Lynsyn, Pareto, action trace and ERL timing. The mission adds:

```text
output/dynamic_adaptation_trace_<timestamp>.csv
output/workload_transitions_<timestamp>.csv
output/surrogate_by_workload_<timestamp>.csv
output/awm_history_dynamic_<timestamp>.csv
```

### Strong no-reset invariants

At mission t=0 the log should report GA generation 0. For each successful transition, `workload_transitions_*.csv` should show:

1. `generation_before == generation_after`  no GA reconstruction or hidden generation during the atomic boundary.
2. `workload_epoch_after == workload_epoch_before + 1`  exactly one context change.
3. A non-zero, increasing generation at later boundaries  ERL learning survived the prior phases.
4. AWM regime/history persists; no AWM object is recreated.
5. On the third transition back to Histogram, `surrogate_obs_after > 0`  direct evidence that the earlier Histogram model bank was retained and recalled.
6. `frame_id_after > frame_id_before`  frames continue through the same live camera/aggregator pipeline after every swap.
7. Transition times occur at the requested ~300/600/900 s mission boundaries.
8. `algorithm_downtime_ms` is only the algorithm-worker stop/start gap; `transition_total_ms` includes the synchronization/context commit around it.

The action/Pareto CSV generation column should remain monotonic across workload epochs 0, 1, 2 and 3 and must never return to zero at 300/600/900 s.

## Automated validation

```bash
python3 validate_dynamic_adaptation.py \
  output/workload_transitions_<timestamp>.csv \
  output/actions_DynamicAdaptation_ERL_<timestamp>.csv
```

A successful run reports both transition-barrier and action-trace continuity checks as `PASS`.

## Validation performed on this bundle

`WorkloadMission.h` and the workload-conditioned `SurrogateModel.h` were compiled with `g++ -std=c++14 -Wall -Wextra -pedantic` using a minimal `spdlog` stub. The test also verified that a Histogram observation disappears when switching to Sobel and reappears when returning to the same Histogram `workloadId`.

The complete Jetson application cannot be fully linked in this environment because the rest of the project (`Stage_01`, CUDA implementation, SharedQueue, aggregator implementation, etc.) was not supplied in this conversation. The integration changes therefore retain the existing public interfaces of those components and are intended to be built in the full project tree on the Jetson.
