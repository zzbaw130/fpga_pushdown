"""Reproducible analytical scenarios, NOT experimental results or CPU predictions.

Uses one archived timing dataset only, with explicitly assumed cost decompositions.
Never modifies the measured results/ files or previously generated figures.
"""
from pathlib import Path
import csv
import json
import math
import statistics

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "paper" / "model"
FIG = ROOT / "paper" / "figures"
OUT.mkdir(exist_ok=True)
plt.rcParams.update({"font.size": 8, "pdf.fonttype": 42,
                     "axes.spines.top": False, "axes.spines.right": False})
NAMES = ["Host-only", "Filter-only", "CSD-only", "CSD + Cache Refill", "Full system"]
COLORS = ["#737373", "#7aa6c2", "#24749c", "#e3a13b", "#27856d"]
SHORT = ["Host", "Filter", "CSD", "CSD+refill", "Full"]

def save(fig, stem):
    fig.text(.5, 1.01, "THEORETICAL ESTIMATE — NOT MEASURED", ha="center", fontsize=9,
             color="#a13c30", weight="bold")
    fig.savefig(FIG / (stem + ".pdf"), bbox_inches="tight",
                metadata={"CreationDate": None, "ModDate": None})
    fig.savefig(FIG / (stem + ".png"), dpi=180, bbox_inches="tight")
    plt.close(fig)

with (ROOT / "results/tpch_sf1_auto_global_search.csv").open() as f:
    raw = list(csv.DictReader(f))
with (ROOT / "results/tpch_sf1_aer_cache_validation.csv").open() as f:
    cache = list(csv.DictReader(f))
cold_probe = statistics.mean(float(r["host_execution_ms"]) for r in cache if r["cache_fill"] == "off") / 1000
warm_probe = statistics.mean(float(r["host_execution_ms"]) for r in cache if r["cache_fill"] == "on") / 1000
lam = 1 - warm_probe / cold_probe
host = np.array([float(r["host_guest_ms"]) / 1000 for r in raw])
auto = np.array([float(r["auto_guest_ms"]) / 1000 for r in raw])
# Assumed returned fractions: not measured selectivities or page fractions.
sigma = np.array([.98,.15,.25,.25,.35,.02,.35,.45,.60,.30,.50,.30,.75,.50,.65,.60,.45,.80,.15,.45,.70,.30])
alpha = .55              # Assumed avoidable fraction of the HOST cost.
overhead = .02           # Assumed request cost in reference-scale seconds.
delta = .03              # Assumed non-overlapped refill contention fraction.
placement_penalty = 1.20 # Assumed forced-device penalty for HOST-selected queries.
mixed_penalty = 1.25     # Assumed forced-device / mixed-fragment ratio.
device = np.array([auto[i] if r["placement"] == "CSD" else
                   placement_penalty * host[i] if r["placement"] == "HOST" else
                   mixed_penalty * auto[i] for i,r in enumerate(raw)])
mixed = np.array([auto[i] if r["placement"] == "MIXED" else np.inf for i,r in enumerate(raw)])

def times(h):
    H = host * (1 - lam * h)
    F = H * ((1 - alpha) + alpha * sigma) + overhead
    R = device * (1 + delta)
    # Optimistic selection among HOST, whole-device, and retained mixed candidates.
    # This is a model candidate minimum, not an execution of the proposed DP.
    M = mixed * (1 - .30 * lam * h) * (1 + delta)
    return np.vstack([H,F,device,R,np.minimum.reduce([H,R,M])])

cold, warm = times(0), times(1)
assumptions = {
    "status": "analytical scenario, not measured; no physical CPU timing prediction",
    "timing_source": "results/tpch_sf1_auto_global_search.csv",
    "cache_source": "results/tpch_sf1_aer_cache_validation.csv",
    "reference_host_total_s": float(host.sum()),
    "reference_auto_total_s": float(auto.sum()),
    "cache_latency_reduction_lambda": float(lam),
    "assumed_host_avoidable_fraction_alpha": alpha,
    "assumed_returned_fractions_sigma_by_query": sigma.tolist(),
    "assumed_request_seconds": overhead,
    "assumed_refill_contention_delta": delta,
    "assumed_forced_device_host_selected_multiplier": placement_penalty,
    "assumed_forced_device_mixed_selected_multiplier": mixed_penalty,
    "assumed_mixed_host_cost_fraction": .30,
    "traffic": {"normalized_base_input": 1, "assumed_result_fraction": .001,
                "assumed_filter_return_fraction": .25, "assumed_filter_cached_page_fraction": .20},
    "sort": {"assumed_input_MiB": 256, "assumed_AFDM_MiB": 16,
             "work_budgets_MiB": [8,16,32,64,128,256],
             "assumed_internal_bandwidth_MiB_s": 1000,
             "model": "one write/read pass for input beyond work budget; no extra merge passes"},
    "sequence": {"kinds": ["reduce","probe","probe","reduce","probe","probe"],
                 "probe_H_cold_s": cold_probe, "probe_H_warm_s": warm_probe,
                 "assumed_probe_device_fraction_of_cold_host": .65,
                 "assumed_probe_filter_fraction_of_host": .80,
                 "assumed_filter_warming_fraction": .20,
                 "assumption": "full relevant working set resident by next request; adequate asynchronous window"},
    "selection": "optimistic candidate minimum; not measured planner output"
}
(OUT / "assumptions.json").write_text(json.dumps(assumptions,indent=2)+"\n")
with (OUT / "query_projection.csv").open("w") as f:
    w=csv.writer(f,lineterminator="\n"); w.writerow(["query","cache_fraction","configuration","estimated_reference_s","status"])
    for h,arr in [(0,cold),(1,warm)]:
        for i in range(22):
            for j,n in enumerate(NAMES):w.writerow([i+1,h,n,f"{arr[j,i]:.6f}","THEORETICAL"])
summary = {"status":"THEORETICAL", "cold_total_s":dict(zip(NAMES,cold.sum(axis=1).tolist())),
           "warm_total_s":dict(zip(NAMES,warm.sum(axis=1).tolist()))}

fig,axes=plt.subplots(2,1,figsize=(7.1,4.2),sharex=True)
for ax,arr,title in zip(axes,[cold,warm],["Cold input (h=0)","Resident input (h=1)"]):
    for j,n in enumerate(NAMES):
        ax.plot(np.arange(1,23),arr[0]/arr[j],marker="o",ms=2.5,lw=1,color=COLORS[j],label=n)
    ax.set(yscale="log",ylabel="Host / configuration",title=title)
    ax.set_yticks([1,2,5,10,20,50],labels=["1","2","5","10","20","50"])
    ax.grid(axis="y",alpha=.2)
axes[0].legend(ncol=3,fontsize=7,frameon=False)
axes[-1].set(xticks=range(1,23),xlabel="TPC-H query (SF=1 reference cost scale)")
fig.tight_layout();save(fig,"theory_query_comparison")

fig,axes=plt.subplots(1,2,figsize=(7.1,2.5))
for ax,arr,title in zip(axes,[cold,warm],["Cold suite","Warm suite"]):
    vals=arr.sum(axis=1)
    ax.bar(np.arange(5),vals,color=COLORS)
    for i,v in enumerate(vals):ax.text(i,v+max(vals)*.025,f"{v:.1f}",ha="center",fontsize=7)
    ax.set(xticks=range(5),xticklabels=SHORT,title=title,ylabel="Estimated reference-scale time (s)",ylim=(0,max(vals)*1.18))
    ax.tick_params(axis="x",labelrotation=20)
fig.tight_layout();save(fig,"theory_suite_totals")

# Sequence traces include first-query cost; no free cache warming before Q1.
def sequence(kinds):
    values=[]; decisions=[]
    for j in range(5):
        h=0.; elapsed=[]; chosen=[]
        for kind in kinds:
            H0=host[5] if kind=="reduce" else cold_probe
            H=H0*(1-lam*h)
            D=auto[5] if kind=="reduce" else .65*cold_probe
            F=H*((1-alpha)+alpha*sigma[5])+.02 if kind=="reduce" else .8*H+.02
            if j==0:t,loc=H,"HOST"
            elif j==1:t,loc=F,"FILTER"
            elif j==2:t,loc=D,"CSD"
            elif j==3:t,loc=D*(1+delta),"CSD"
            else:t,loc=min((H,"HOST"),(D*(1+delta),"CSD"))
            elapsed.append(t);chosen.append(loc)
            if j==0 or j in (3,4):h=1.
            elif j==1:h=.2
        values.append(np.cumsum(elapsed));decisions.append(chosen)
    return np.array(values),decisions
fig,axes=plt.subplots(1,2,figsize=(7.1,2.8))
sequence_rows=[]
for ax,kinds,title in zip(axes,[["reduce"]*6,assumptions["sequence"]["kinds"]],["Repeated reduction","Reduction + host-friendly probes"]):
    trace,decisions=sequence(kinds)
    for j in range(5):
        ax.plot(range(1,7),trace[j],marker="o",ms=3,color=COLORS[j],label=NAMES[j])
        for i,t in enumerate(trace[j]):sequence_rows.append([title,i+1,kinds[i],NAMES[j],f"{t:.6f}",decisions[j][i],"THEORETICAL"])
    ax.set(xticks=range(1,7),xlabel="Query position",ylabel="Cumulative estimated time (s)",title=title)
    ax.grid(alpha=.15)
axes[0].legend(fontsize=6,frameon=False)
fig.tight_layout();save(fig,"theory_sequences")
with (OUT/"sequence_projection.csv").open("w") as f:
    w=csv.writer(f,lineterminator="\n");w.writerow(["scenario","position","kind","configuration","cumulative_reference_s","location","status"]);w.writerows(sequence_rows)
summary["sequence_final_s"]={title:{n:float(sequence(k)[0][j,-1]) for j,n in enumerate(NAMES)} for title,k in [("repeat",["reduce"]*6),("mixed",assumptions["sequence"]["kinds"])]}

# Traffic is a separate normalized same-relation example, not inferred TPC-H bytes.
base=np.array([1.,.25,.001,.001,.001]);refill=np.array([0,0,0,1,1.])
next_read=np.array([0,.8,1,0,0.])
fig,axes=plt.subplots(1,2,figsize=(7.1,2.8))
axes[0].bar(range(5),base,color="#24749c",label="Required scan/result")
axes[0].bar(range(5),refill,bottom=base,color="#e3a13b",label="Refill")
axes[0].set(title="First offload/scan",ylabel="Host-interface bytes / input bytes")
axes[1].bar(range(5),next_read,color=COLORS)
axes[1].set(title="Next forced-HOST probe",ylabel="Storage bytes / input bytes")
for i in range(5):
    axes[0].text(i,base[i]+refill[i]+.025,f"{base[i]+refill[i]:.3f}",ha="center",fontsize=7)
    axes[1].text(i,next_read[i]+.025,f"{next_read[i]:.1f}",ha="center",fontsize=7)
for ax in axes:ax.set(xticks=range(5),xticklabels=SHORT,ylim=(0,1.25));ax.tick_params(axis="x",labelrotation=20)
axes[0].legend(fontsize=6,frameon=False,loc="upper center",ncol=2)
fig.tight_layout();save(fig,"theory_traffic")
with (OUT/"traffic_projection.csv").open("w") as f:
    w=csv.writer(f,lineterminator="\n");w.writerow(["configuration","required_bytes_per_input_byte","refill_bytes_per_input_byte","next_host_read_bytes_per_input_byte","status"])
    for j,n in enumerate(NAMES):w.writerow([n,base[j],refill[j],next_read[j],"THEORETICAL"])

budgets=np.array([8,16,32,64,128,256]);S=256.;A=16.
resident=np.minimum(S,budgets);spill=2*np.maximum(0,S-budgets)
fig,axes=plt.subplots(1,2,figsize=(7.1,2.7))
axes[0].plot(budgets,A+resident,"o-",color=COLORS[2],label="Reentrant sort: budgeted buffers + state")
axes[0].axhline(A+S,ls="--",color=COLORS[0],label="Full input materialization")
axes[0].set(ylabel="Modeled algorithm memory (MiB)",xlabel="Private work budget (MiB)",title="Budgeted state (not process RSS)")
axes[0].legend(fontsize=6,frameon=False)
axes[1].plot(budgets,spill,"o-",color=COLORS[3],label="One-pass spill write + replay")
axes[1].set(ylabel="Internal spill traffic (MiB)",xlabel="Private work budget (MiB)",title="256 MiB sort; ideal single replay")
axes[1].legend(fontsize=6,frameon=False)
for ax in axes:ax.set_xscale("log",base=2);ax.set_xticks(budgets,labels=list(map(str,budgets)));ax.grid(alpha=.15)
fig.tight_layout();save(fig,"theory_memory_spill")
with (OUT/"memory_projection.csv").open("w") as f:
    w=csv.writer(f,lineterminator="\n");w.writerow(["work_budget_MiB","algorithm_memory_MiB","spill_read_write_MiB","ideal_spill_seconds","status"])
    for m,r,b in zip(budgets,resident,spill):w.writerow([m,A+r,b,b/1000,"THEORETICAL"])

h=np.linspace(0,1,101);H=cold_probe*(1-lam*h);D=np.full_like(h,.65*cold_probe*(1+delta))
fig,ax=plt.subplots(figsize=(3.4,2.4))
ax.plot(h,H,label="HOST",color=COLORS[0]);ax.plot(h,D,label="CSD + refill",color=COLORS[3])
ax.plot(h,np.minimum(H,D),ls="--",lw=2,label="Full: candidate minimum",color=COLORS[4])
threshold=(1-.65*(1+delta))/lam
ax.axvline(threshold,ls=":",color="black",lw=.8)
ax.set(xlabel="Resident fraction h",ylabel="Estimated reference-scale time (s)",title=f"Illustrative crossover: h={threshold:.3f}")
ax.legend(fontsize=6,frameon=False);fig.tight_layout();save(fig,"theory_cache_crossover")
summary["illustrative_cache_crossover_fraction"]=float(threshold)
with (OUT/"cache_crossover_projection.csv").open("w") as f:
    w=csv.writer(f,lineterminator="\n");w.writerow(["resident_fraction","host_reference_s","device_refill_reference_s","full_reference_s","status"])
    for x,y,z in zip(h,H,D):w.writerow([f"{x:.2f}",f"{y:.6f}",f"{z:.6f}",f"{min(y,z):.6f}","THEORETICAL"])
(OUT/"summary.json").write_text(json.dumps(summary,indent=2)+"\n")
# Review gallery; individual figures remain the paper's export artifacts.
fig,axes=plt.subplots(3,2,figsize=(14,11))
for ax,stem in zip(axes.flat,["theory_suite_totals","theory_query_comparison",
                             "theory_sequences","theory_traffic",
                             "theory_memory_spill","theory_cache_crossover"]):
    ax.imshow(plt.imread(FIG/(stem+".png")));ax.axis("off")
fig.tight_layout(pad=.3)
fig.savefig(FIG/"theory_overview.png",dpi=180,bbox_inches="tight")
plt.close(fig)
print(json.dumps(summary,indent=2))
print("Generated six theoretical figure pairs and auditable model inputs/outputs; no experiments run.")
