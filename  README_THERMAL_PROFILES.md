# ERL Thermal-Threshold Ablation  Runtime Profiles

## Scope
Only `SampleTestIntegrationCode_v33.cpp` is changed.

No changes are required to:
- `ThermalGovernor.h`
- `AdaptiveWeightManager.h`
- `ConfigManager.h`
- Scheduler / GA / Surrogate / Algorithm modules
- `config.json`

`ConfigManager` already reads the runtime JSON values for both
`ThermalGovernor::Config` and `AdaptiveWeightManager::Config`.

## New CLI
```bash
--thermal-profile ORIGINAL
--thermal-profile REVISED1
--thermal-profile REVISED2
```

Default when omitted: `REVISED2` (preserves the current config.json behavior).

## Temperature contracts

### ThermalGovernor
| Profile | CAUTION | WARNING | CRITICAL | EMERGENCY | SHUTDOWN |
|---|---:|---:|---:|---:|---:|
| ORIGINAL | 75 | 85 | 95 | 105 | 108 |
| REVISED1 | 55 | 62 | 70 | 80 | 90 |
| REVISED2 | 45 | 50 | 55 | 60 | 75 |

### AdaptiveWeightManager
Only thermal warning/critical fields are changed.

| Profile | warn CPU | warn GPU | crit CPU | crit GPU |
|---|---:|---:|---:|---:|
| ORIGINAL | 75 | 78 | 80 | 83 |
| REVISED1 | 57 | 57 | 63 | 63 |
| REVISED2 | 45 | 45 | 55 | 55 |

The following remain identical across all three cases:
- `awm_battery_critical_watts`
- `awm_latency_sla_ms`
- `awm_fps_underperform_ratio`
- `awm_transition_smoothing`
- GA population/mutation/crossover
- objective weights
- workload mission
- scheduler timing
- camera / SoC / Lynsyn configuration

For this threshold-only ablation, do **not** add a non-zero `--temp-offset`.
Leave it at its default `0.0`.


ORIGINAL smoke
./MySystem \
  --dynamic-adaptation \
  --thermal-profile ORIGINAL \
  --mission "0:HistogramEqualization,30:SobelEdge,60:MedianFilter,90:HeterogeneousGaussianBlur,120:HistogramEqualization" \
  --dynamic-duration 150 \
  --dynamic-warmup 5 \
  config.json \
  2>&1 | tee "benchmark_logs/DynamicAdaptation_ERL_ORIGINAL_V33_SMOKE_$(date +%Y%m%d_%H%M%S).log"
REVISED1 smoke
./MySystem \
  --dynamic-adaptation \
  --thermal-profile REVISED1 \
  --mission "0:HistogramEqualization,30:SobelEdge,60:MedianFilter,90:HeterogeneousGaussianBlur,120:HistogramEqualization" \
  --dynamic-duration 150 \
  --dynamic-warmup 5 \
  config.json \
  2>&1 | tee "benchmark_logs/DynamicAdaptation_ERL_REVISED1_V33_SMOKE_$(date +%Y%m%d_%H%M%S).log"
REVISED2 smoke
./MySystem \
  --dynamic-adaptation \
  --thermal-profile REVISED2 \
  --mission "0:HistogramEqualization,30:SobelEdge,60:MedianFilter,90:HeterogeneousGaussianBlur,120:HistogramEqualization" \
  --dynamic-duration 150 \
  --dynamic-warmup 5 \
  config.json \
  2>&1 | tee "benchmark_logs/DynamicAdaptation_ERL_REVISED2_V33_SMOKE_$(date +%Y%m%d_%H%M%S).log"



## Canonical 5-phase / 30-minute-per-phase mission

Use the same explicit mission for all three thermal profiles:

```text
0       HistogramEqualization
1800    SobelEdge
3600    MedianFilter
5400    HeterogeneousGaussianBlur
7200    HistogramEqualization
stop    9000
```

### ORIGINAL
```bash
./MySystem \
  --dynamic-adaptation \
  --thermal-profile ORIGINAL \
  --mission "0:HistogramEqualization,1800:SobelEdge,3600:MedianFilter,5400:HeterogeneousGaussianBlur,7200:HistogramEqualization" \
  --dynamic-duration 9000 \
  --dynamic-warmup 30 \
  config.json \
  2>&1 | tee "benchmark_logs/DynamicAdaptation_ERL_ORIGINAL__V33$(date +%Y%m%d_%H%M%S).log"
```

### REVISED1
```bash
./MySystem \
  --dynamic-adaptation \
  --thermal-profile REVISED1 \
  --mission "0:HistogramEqualization,1800:SobelEdge,3600:MedianFilter,5400:HeterogeneousGaussianBlur,7200:HistogramEqualization" \
  --dynamic-duration 9000 \
  --dynamic-warmup 30 \
  config.json \
  2>&1 | tee "benchmark_logs/DynamicAdaptation_ERL_REVISED1_V33_$(date +%Y%m%d_%H%M%S).log"
```

### REVISED2
```bash
./MySystem \
  --dynamic-adaptation \
  --thermal-profile REVISED2 \
  --mission "0:HistogramEqualization,1800:SobelEdge,3600:MedianFilter,5400:HeterogeneousGaussianBlur,7200:HistogramEqualization" \
  --dynamic-duration 9000 \
  --dynamic-warmup 30 \
  config.json \
  2>&1 | tee "benchmark_logs/DynamicAdaptation_ERL_REVISED2_V33_$(date +%Y%m%d_%H%M%S).log"
```

## Output naming
The profile is embedded into the ERL output names, for example:

```text
actions_DynamicAdaptation_ERL_THERMAL_ORIGINAL_<timestamp>.csv
pareto_DynamicAdaptation_ERL_THERMAL_ORIGINAL_<timestamp>.csv
dynamic_adaptation_trace_THERMAL_ORIGINAL_<timestamp>.csv
workload_transitions_THERMAL_ORIGINAL_<timestamp>.csv
surrogate_by_workload_THERMAL_ORIGINAL_<timestamp>.csv
awm_history_dynamic_THERMAL_ORIGINAL_<timestamp>.csv
```

This prevents profile ambiguity during thesis analysis.

## Run-control recommendation
Do not run the three 2.5-hour cases immediately back-to-back without thermal
normalization. Start each case from the same practical temperature band and
record the initial CPU/GPU temperatures. Keep native Jetson/Linux thermal
protection enabled. `ORIGINAL` is an application-governor control profile; it
does not and must not disable platform thermal protection.

For replicated thesis data, counterbalance/randomize profile order rather than
always running ORIGINAL -> REVISED1 -> REVISED2.
