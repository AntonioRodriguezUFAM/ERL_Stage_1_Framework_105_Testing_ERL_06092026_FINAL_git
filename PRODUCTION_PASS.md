# Production Pass  Final Module Map & Run-2 Acceptance Gates
Updated 2026-07-25 (final). All findings F1F34 dispositioned.

## 1. Canonical module set (this folder)

| Module | File | State |
|---|---|---|
| **Capture** | `DataConcrete_new.h` (your tree) | ? SEC-FIX native-ioctl injection fix live; zero `std::system`. **Housekeeping:** `DataConcrete_new_old.h` is an inert archive (move to `reference/`); `DataConcrete_new_USB_CSI_Modular.h` **re-introduces a live `std::system()` with no SEC-FIX**  port the native-ioctl fix into it *before* any switch-over or the injection fix silently regresses. `Modules.h` includes `DataConcrete_new.h`, so today's build is safe. |
| **Algorithm** | `AlgorithmConcrete_new.h` (here) + kernels (your tree, unchanged) | ? **[F21-FIX]** serial guard now `(end-start) < numThreads*16`  the concurrency gene finally actuates row-parallel workloads (EdgeDetection, GaussianBlur, hetero CPU shares) at 320×240. **[PERF]** `std::move(processedBuffer_)` restored (the copy added ~150 KB memcpy/frame for zero allocation savings). **[F8]** persistent-pool spec embedded above `parallelFor`  implement post-run-2 with a compile loop (must honor the gene; nested-call guard; exit-safe lifetime). F3 double-push confirmed absent (the two push sites are exclusive branches: main vs OpticalFlow). Kernels: clean bill  hetero blur uses correct clamped-halo/owned-rows seam architecture; Mandelbrot output-only. |
| **Display** | `SdlDisplayConcrete_new.h` (here / your merge) | ? FIX-1 null-window guard; FIX-2 both-sides conversion guards; P1-LAT steady_clock stamp + processed-frame id; **[PERF]** render-tick dedup  also makes `DisplayStats.fps` measure frame arrivals (~13) instead of render ticks (~60); STARVING warns demoted (were 14,869 of run 1's 40,321 warnings). |
| **SoC** | `SoCConcrete_new.h` (here / your merge) | ? **F26-FIX**: `pushSoCStats` forwards to the aggregator (the stub + commented-out mis-cased call that kept every temperature at 0.0). +local-cache restore (your catch  repairs `getPerformance()`), tegrastats `--interval` coupled to `pollIntervalMs` (pipe-lag prevention), throttled temperature-parse WARN, explicit `<array>`. Residual one-word edit: make the temp-parse warn unconditional (`else {` instead of `else if (partialParseAllowed)`). |
| **Lynsyn** | `LynsynMonitorConcrete_new.h` (here  converged on YOUR lineage) | ? Software decimation (one averaged emission per `sampleRateMs`; ends the measured ~2 kHz push storm), raw-rate **[TRAP]** trapezoidal window energy (`n_avg,energy_j,avg_power_w_trap` CSV columns  canonical energy figures), 20k drain cap, per-raw spike filter, CSV failbit one-shot (answers run 1's 562 KB mystery), 30 s ingest stats. Preserved: your pairwise sync, `fullResetThreadSafe`, session resets. |
| **Aggregator** | `SystemMetricsAggregatorConcrete_v3_2.h` (here) | ? Canonical v3_2: P0-F15 gate, [RUN1] history caps, `attachWindowIntegratedAsync` on the display-less finalize path, **[TW-INT]** zero-order-hold time-weighted `integratePowerInWindow`. The `metrics::` rewrite is **rejected** (C++17 on your C++14 toolchain; API mismatch vs your own callers incl. the F26 casing; display re-coupling; `pendingFrames_` leak; CSV export loss)  keep it only in `reference/`. |

**Also required from earlier folders (deploy together):** `P0_fixes/`  harness (`--temp-offset` ? Scheduler block, `expectsDisplay=false`), `GeneticAlgorithm_05.h` (NSGA-II sort + `modeDist(1,3)`), `AdaptiveWeightManager.h` (battery 6.0 W), `EvolutionarySelector_02.h`; `p1_fixes/`  `ConfigManager.h` (`expects*` plumbing).

**Pre-build verification (30 s):**
`grep -c "SAMPLING-FIX\|TRAP]" Lynsyn` = 7 · `grep -c "TW-INT\|RUN1\|P0-F15" Aggregator` = 3 · `grep -c "F26-FIX" SoC` = 1 · `grep -c "F21-FIX" Algorithm` = 1 · `grep -c "namespace metrics" Aggregator` = 0.

## 2. Run-2 acceptance gates

1. **Temperature (F26/F17):** `cpu_temp_c`/`gpu_temp_c` nonzero in the action trace and within ±2 °C of the run-window tegrastats CSV; THERMAL_* regimes reachable under a hot baseline; `obj_temp > 0` for candidates above the 57 °C onset.
2. **Power/energy (F24/F25/TRAP):** ingest line `~2000 Hz raw  emitted @ 100 ms cadence`; Lynsyn CSV  run_seconds×10 rows with `n_avg  200`; `energy_j  avg_power_w_trap × 0.1` per row; S`energy_j` within a few % of mean-power × duration (~6.9 kJ / 30 min); zero steady-state re-anchor warnings; no `CSV stream FAILED` (and if it fires, that solves the run-1 mystery).
3. **Pipeline (P1/F29):** `disp=false` frame loss  0; `DisplayLatencyMs` nonzero and sane (040 ms), zero clamping warnings; `DisplayStats.fps`  supply rate (~13), not ~60; total warnings collapse from 40,321 toward ~0 (now itself a health metric).
4. **ERL (F27/F28/F21):** zero `Unknown policy mode` fallbacks; `BATTERY_CRITICAL`  0 on nominal SobelEdge; **`runtime_concurrency` shows throughput coupling on row-parallel workloads** (the F21 gate  first run where the gene can pull its lever); fitness trend preserved or better.
5. **Hygiene:** run-1 data quarantined to methodology; tegrastats filtered by run window; framework revision + full effective config in every manifest; fps/latency/energy figures never mixed across fix boundaries.

## 3. Grades: what code bought, what remains for A/A+
Functionality is A-territory for a research measurement platform: all five
dimensions (throughput, latency, power, energy, temperature) have live,
validated measurement **and** actuation paths  pending run-2 proof. True
production A/A+ still requires what code edits cannot confer: a test target
+ CI (PowerSanity, ZOH integrator vs synthetic integrals, NSGA-II
invariants, F31 SoCConfig fix, one scripted hardware smoke run asserting
§2 automatically), build hygiene (`sudo jetson_clocks` out of CMake,
toolchain file, artifact cleanup, one filesystem idiom  F32), splitting
the single-TU monolith + deleting dead code (three marker-collision
incidents in this review alone), capability-based privilege drop, and the
F31F34 config one-liners. Standing P1s: Scheduler F19/F20 (pinning /
DVFS restore between cases), F8 pool per the embedded spec.