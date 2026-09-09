##ERL Experiment Matrix Thesis.md

The clean way is to make thermal thresholds a runtime experimental factor and run the same ERL mission three times, changing only the temperature contracts.

Revised experiment matrix for the thesis
ThermalGovernor:
60 / 70 / 80 / 85 / 90°C

AWM:
warning 65°C
critical 75°C

I would now formally define the experimental program as:

EXPERIMENT A
Static Oracle Baselines
--------------------------------------------
Histogram       CPU / GPU / BALANCED × 30 min
Sobel           CPU / GPU / BALANCED × 30 min
Median          CPU / GPU / BALANCED × 30 min
HetGaussian     CPU / GPU / BALANCED × 30 min

Purpose:
identify workload-specific static oracle
calculate Normalized Oracle Gap


EXPERIMENT B
Canonical ERL Dynamic Adaptation
--------------------------------------------
030 min       Histogram
3060 min      Sobel
6090 min      Median
90120 min     HeterogeneousGaussian
120150 min    Histogram recurrence

ONE CONTINUOUS ERL EXECUTION
NO GA RESET
NO AWM RESET
NO SURROGATE RESET

Purpose:
dynamic adaptation
transition response
resource migration
cross-workload memory
normalized Oracle Gap


EXPERIMENT C
Thermal-Stress / Safety Adaptation
--------------------------------------------
Use aggressive thermal thresholds
or controlled thermal-offset experiment

Purpose:
prove AWM/governor thermal response separately


EXPERIMENT D
2-hour endurance run already completed
--------------------------------------------
Hist ? Sobel ? HetGaussian ? Hist
4 × 30 min

Purpose:
long-duration stability
thermal behavior
controller continuity
surrogate recurrence