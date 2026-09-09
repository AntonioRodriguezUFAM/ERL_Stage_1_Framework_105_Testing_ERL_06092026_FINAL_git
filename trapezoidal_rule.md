trapezoidal rule

more sophisticated approach.

My previous solution (simple averaging at the end) was a coarse approximation. To truly isolate the hardware cost of the algorithm, we must integrate the high-frequency SoC and power data during the algorithm's specific execution window.

Using the trapezoidal rule is the correct, scientifically-backed method to do this. It calculates the energy (area under the power curve) by treating the time between samples as trapezoids, which is far more accurate than simple averaging, especially with spiky workloads.

This creates the per-frame energy profile you're looking for, linking specific energy consumption to each frame's processing.

Here are the precise code modifications to implement this trapezoidal integration in your aggregator.


This is an excellent and highly accurate requirement. You're moving from simple correlation (what was the power level when the frame finished?) to integration (how much energy did this specific frame's algorithm consume?).

Using the trapezoidal rule is the correct, scientifically-backed method to accurately measure the energy consumed during that specific time window, as it properly accounts for fluctuations in power draw between samples.

Here are the precise code modifications required across your files to implement this per-frame energy profiling.