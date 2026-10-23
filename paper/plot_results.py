"""Generate preliminary reference figures, not final CPU/SNIA measurements."""
from pathlib import Path
import csv
import math
import statistics
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "paper" / "figures"
plt.rcParams.update({"font.size": 9, "pdf.fonttype": 42,
                     "axes.spines.top": False, "axes.spines.right": False})

def save(fig, name):
    fig.savefig(OUT / (name + ".pdf"), bbox_inches="tight")
    fig.savefig(OUT / (name + ".png"), dpi=180, bbox_inches="tight")
    plt.close(fig)

with (ROOT / "results/tpch_sf1_auto_global_search.csv").open() as f:
    rows = list(csv.DictReader(f))
host = [float(r["host_guest_ms"]) for r in rows]
auto = [float(r["auto_guest_ms"]) for r in rows]
speedups = [h / a for h, a in zip(host, auto)]
colors = {"CSD": "#2878a5", "MIXED": "#d48428", "HOST": "#777777"}
fig, ax = plt.subplots(figsize=(7.1, 2.45))
for placement in colors:
    subset = [i for i, r in enumerate(rows) if r["placement"] == placement]
    ax.bar([i + 1 for i in subset], [speedups[i] for i in subset],
           color=colors[placement], label=placement, width=.72)
ax.axhline(1, color="black", lw=.8, ls="--")
ax.set(yscale="log", ylabel="Reference HOST / AUTO time", xlabel="TPC-H query (SF=1)",
       xticks=list(range(1, 23)), ylim=(.65, 65))
ax.set_yticks([1, 2, 5, 10, 20, 50], labels=["1", "2", "5", "10", "20", "50"])
ax.legend(ncol=3, frameon=False, loc="upper right")
ax.grid(axis="y", alpha=.15)
save(fig, "sf1_auto_speedup")

with (ROOT / "results/tpch_sf1_aer_cache_validation.csv").open() as f:
    cache = list(csv.DictReader(f))
off = [float(r["host_execution_ms"]) / 1000 for r in cache if r["cache_fill"] == "off"]
on = [float(r["host_execution_ms"]) / 1000 for r in cache if r["cache_fill"] == "on"]
fig, ax = plt.subplots(figsize=(3.35, 2.25))
ax.bar([0, 1], [statistics.mean(off), statistics.mean(on)], color=["#777777", "#2878a5"], width=.55)
for x, samples in enumerate([off, on]):
    ax.scatter([x - .07, x, x + .07], samples, color="black", s=15, zorder=3)
    ax.text(x, statistics.mean(samples) + 1, f"{statistics.mean(samples):.2f} s", ha="center")
ax.set(xticks=[0, 1], xticklabels=["Fill off", "After full AER fill"],
       ylabel="Subsequent HOST scan (s)", ylim=(0, 27))
save(fig, "sf1_cache_benefit")

# Measured bytes in the named validation reports, not synthesized runtimes.
sort_cases = [("Final merge\n30,209 rows", 14646232, 9420282),
              ("Top-N\nLIMIT 100", 9420282, 69190),
              ("Adaptive merge\n229 runs", 3680832, 1183632)]
fig, ax = plt.subplots(figsize=(3.35, 2.45))
for i, (name, before, after) in enumerate(sort_cases):
    ax.bar(i - .16, before / 2**20, width=.3, color="#777777", label="Before" if i == 0 else None)
    ax.bar(i + .16, after / 2**20, width=.3, color="#2878a5", label="After" if i == 0 else None)
    ax.text(i, before / 2**20 + .65, f"{100 * (1 - after / before):.2f}%", ha="center", fontsize=8)
ax.set(xticks=range(3), xticklabels=[c[0] for c in sort_cases], ylabel="Spill read + write (MiB)", ylim=(0, 17))
ax.legend(frameon=False, ncol=2, loc="upper right")
save(fig, "sf1_sort_io")

print(f"AUTO total={sum(auto):.3f} ms; HOST total={sum(host):.3f} ms")
print(f"Aggregate ratio={sum(host)/sum(auto):.4f}; geometric mean={math.exp(statistics.mean(map(math.log, speedups))):.4f}")
print(f"Faster queries={sum(s > 1 for s in speedups)}/22; generated three figure pairs")
