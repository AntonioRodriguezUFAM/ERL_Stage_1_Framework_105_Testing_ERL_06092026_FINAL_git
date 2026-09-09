# Pareto Fromt Plotting

import pandas as pd
import matplotlib.pyplot as plt

pareto = pd.read_csv("../build/output/realtime_metrics_Suite_D_ERL_Sobel_ERL_t1780384338.csv")

last_gen = pareto["generation"].max()
df = pareto[pareto["generation"] == last_gen]

rank0 = df[df["rank"] == 0]
selected = df[df["selected"] == 1]

plt.figure(figsize=(8, 6))
plt.scatter(df["obj_energy"], df["obj_latency"], label="All candidates", alpha=0.35)
plt.scatter(rank0["obj_energy"], rank0["obj_latency"], label="Rank-0 Pareto front")
plt.scatter(selected["obj_energy"], selected["obj_latency"], marker="*", s=250, label="Selected ERL action")

plt.xlabel("Normalized energy objective")
plt.ylabel("Normalized latency objective")
plt.title(f"Pareto Front at Generation {last_gen}")
plt.legend()
plt.tight_layout()
plt.savefig("pareto_front_last_generation.png", dpi=300)
plt.show()