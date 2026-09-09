# ERL/Pareto Evidence Export for PhD Validation

This patch adds traceable evidence that the ERL controller is not only running an ERL-labelled workload, but is selecting runtime actions from an evaluated Rank-0 Pareto front.

## What is exported

### `erl_pareto_front.csv`
One row per GA individual per generation.

Key columns:

- `generation`, `individual_id`
- `rank`, `fitness`, `crowding_distance`
- `obj_fps`, `obj_energy`, `obj_temp`, `obj_latency`
- `target_fps`, `power_budget_watts`, `prefer_gpu`, `policy_mode`
- `selected`: `1` when this individual was deployed to runtime
- measured metrics: FPS, power, Joules/frame, latency, temperatures
- runtime controls: GPU enabled, concurrency, affinity
- adaptive weights: `w_fps`, `w_power`, `w_temp`, `w_latency`
- `regime`

### `erl_action_trace.csv`
One row per deployed ERL action.

Key columns:

- selected action genome
- Rank-0 count
- best/average fitness
- runtime controls after scheduling
- measured metrics at the decision point
- adaptive weights/regime

## How this proves ERL optimization

The evidence chain is:

```text
Frame metrics -> GA population -> objective evaluation -> non-dominated sorting
-> Rank-0 Pareto front -> selected scalarized action -> Scheduler/RuntimeControls
-> measured energy/latency response
```

The most important proof is:

```text
selected == 1 and rank == 0
```

This shows that the deployed runtime action came from the non-dominated Pareto front.

## Required config keys

Inside your test harness, when creating each ERL workload config, add:

```cpp
cfg["Scheduler"]["export_pareto_evidence"] = true;
cfg["Scheduler"]["pareto_csv"] = "output/pareto_" + outputSuffix + ".csv";
cfg["Scheduler"]["action_csv"] = "output/actions_" + outputSuffix + ".csv";
```

The patched `EvolutionarySelector_02.h` reads these from the `Scheduler` JSON object.

## How to run

Example for Sobel ERL:

```bash
sudo ./MySystem --algo SobelEdge 1800 config.json
```

or your suite mode:

```bash
sudo ./MySystem --phd-suite 1800 --cooldown 30 config.json
```

After the run, generate plots:

```bash
python3 plot_pareto_evidence.py \
  --pareto output/pareto_Sobel_ERL.csv \
  --actions output/actions_Sobel_ERL.csv \
  --outdir output/pareto_plots_Sobel_ERL
```

## Generated figures

The script creates:

- `pareto_front_last_gen.png/pdf`
- `hypervolume_over_generations.png/pdf`
- `selected_action_trajectory.png/pdf`
- `adaptive_weights_timeline.png/pdf`
- `pareto_evidence_summary.csv`

## Thesis statement

> The ERL controller maintains a population of runtime policies, evaluates them against the most recent frame-level metrics, and extracts the non-dominated Rank-0 Pareto front. The deployed hardware action is selected from this front using adaptive scalarized fitness. The exported Pareto front and action trace show that observed latency/energy improvements originate from the multi-objective optimization process rather than a fixed heuristic.
