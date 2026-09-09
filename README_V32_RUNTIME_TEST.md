# ERL Dynamic Adaptation v32  provenance-safe hot swap

## Purpose

This bundle is the runtime-test implementation of the fixes identified after the failed dynamic-adaptation run. It keeps one persistent ConfigManager/Scheduler/GA/AWM/controller instance while replacing only the AlgorithmConcrete worker at phase boundaries.

The critical runtime invariants are now:

- `AlgorithmConcrete::stopAlgorithm()` never stops or restarts shared pipeline queues.
- The algorithm worker uses `SharedQueue::pop_for(..., 50 ms)` so it can be joined without destroying a queue.
- The worker re-checks `running_` immediately after a successful pop, so an old workload cannot process a newly-arrived frame after a stop request.
- Both `cam2Alg` and `alg2Disp` are drained only after the old worker is fully joined.
- A stopped shared queue during a workload transition is an invariant violation; the code fails closed and never auto-restarts it.
- Workload provenance is published as `processed_frame_id` first and `processed_workload_epoch` last with release semantics. The epoch is the publication/commit marker.
- The ERL selector accepts at most one generation per distinct finalized frame and requires the processed epoch to equal the GA workload epoch.
- The transition harness requires a real replacement-workload frame within 5 seconds, using exact epoch equality.
- A 5-second algorithm starvation condition invalidates a thesis replicate instead of silently allowing ERL to train on camera-fallback metrics.

## Files to install

Copy the bundle files over the corresponding project files:

```text
Module/ConfigManager.h
Module/EvolutionarySelector_02.h
Module/GeneticAlgorithm_05.h
Module/MetricsSnapshot.h
Module/Modules.h
Module/RuntimeControls.h
Module/SurrogateModel.h
Module/WorkloadMission.h
Stage_01/Concretes/AlgorithmConcrete_new.h
Stage_01/SharedStructures/SharedQueue.h   # see SHAREDQUEUE_MERGE_INSTRUCTIONS.md
SampleTestIntegrationCode_v32.cpp
validate_dynamic_adaptation.py
```

If your build system expects `SampleTestIntegrationCode_v31.cpp`, either update the source name in the build or copy `SampleTestIntegrationCode_v32.cpp` over that path after backing up the old file.

## Important SharedQueue note

The original local `SharedQueue.h` was not supplied. The included file is a conservative complete reference implementation. Before replacing a customized local queue, read `SHAREDQUEUE_MERGE_INSTRUCTIONS.md`; preserve any local full-queue/drop policy and merge the v32 methods if necessary.

## Host-side checks

On any Linux C++14 host:

```bash
./tests/run_static_tests.sh
```

These tests cover queue survival across consumer replacement, frame?epoch provenance publication, WorkloadMission v32 evidence columns, validator behavior, and Python syntax.

## Jetson compile

From the real project root, after installing the files:

```bash
make clean
make -j2
```

The current analysis environment does not contain the full Jetson/CUDA project (ThreadManager, camera/display stack, CUDA kernel translation units, system aggregator dependencies, etc.), so only the protocol components can be compiled here. The full application must be compiled on the Jetson source tree.

## Required smoke run first

```bash
mkdir -p benchmark_logs

./MySystem \
  --dynamic-adaptation \
  --phase-seconds 30 \
  --dynamic-warmup 5 \
  config.json \
  2>&1 | tee "benchmark_logs/DynamicAdaptation_ERL_V32_$(date +%Y%m%d_%H%M%S).log"
```

At startup, verify the executable reports approximately:

```text
warmup=5.0s
mission=120.0s
0  HistogramEqualization
30 SobelEdge
60 HeterogeneousGaussianBlur
90 HistogramEqualization
```

If it reports `warmup=30`, `mission=1200`, or 300-second boundaries, stop it: that is a stale binary/source selection.

## Expected healthy transition signature

For each transition the log should show this ordering:

```text
[ERL-WORKLOAD] BEGIN ... generation=N ... requested epoch=E+1
[WORKLOAD] Preparing hot swap ...
[AlgorithmConcrete] Worker stopped (hot-swap) - shared queues preserved
[WORKLOAD] COMMIT ... drained input=X output=Y ...
[ERL-WORKLOAD] COMMIT ... generation=N PRESERVED ...
[DYNAMIC] First replacement-workload frame proven: epoch=E+1 frame=F ...
[ERL-WORKLOAD] First provenance-safe post-transition snapshot accepted ...
```

There must be no repeated `Push to stopped queue` flood and no 40 kHz queue-pop spin.

## Post-run validation

```bash
TRANS="$(ls -t output/workload_transitions_*.csv | head -1)"
ACT="$(ls -t output/actions_DynamicAdaptation_ERL_*.csv | head -1)"

python3 validate_dynamic_adaptation.py \
  "$TRANS" "$ACT" \
  --phase-seconds 30
```

Also check the runtime log:

```bash
LOG="$(ls -t benchmark_logs/DynamicAdaptation_ERL_V32_*.log | head -1)"

grep -c "Push to stopped queue" "$LOG"
grep -c "ALGORITHM STARVED" "$LOG"
grep -E "Worker stopped \(hot-swap\)|First replacement-workload frame proven|Workload switch committed" "$LOG"
```

The first two counts should be `0` in a healthy smoke run.

## Transition evidence v32

`workload_transitions_*.csv` now contains, in addition to the previous barrier proof:

- `algorithm_processed_epoch_after`
- `algorithm_processed_frame_after`
- `first_new_frame_latency_ms`
- `input_frames_drained`
- `output_frames_drained`

A transition is valid only when:

```text
generation_before == generation_after
workload_epoch_after == workload_epoch_before + 1
algorithm_processed_epoch_after == workload_epoch_after
algorithm_processed_frame_after > frame_id_before
frame_id_after >= algorithm_processed_frame_after
```

The selected-action CSV remains schema version 5 and contains explicit active/processed workload epoch and processed frame provenance.

## Full thesis mission only after smoke PASS

```bash
./MySystem \
  --dynamic-adaptation \
  --phase-seconds 300 \
  --dynamic-warmup 30 \
  config.json \
  2>&1 | tee "benchmark_logs/DynamicAdaptation_ERL_V32_FULL_$(date +%Y%m%d_%H%M%S).log"
```

Do not use a failed/starved run as a thesis replicate. The harness intentionally fails closed when the algorithm data plane cannot prove the active workload.

