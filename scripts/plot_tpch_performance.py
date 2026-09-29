#!/usr/bin/env python3
"""Plot HOST versus CSD latency and speedup for the 22 TPC-H queries."""

from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np


def load_results(path: Path) -> tuple[list[str], np.ndarray, np.ndarray, np.ndarray]:
    queries: list[str] = []
    csd_ms: list[float] = []
    host_ms: list[float] = []
    speedups: list[float] = []
    with path.open(newline="", encoding="utf-8") as source:
        for row in csv.DictReader(source):
            queries.append(f"Q{int(row['query']):02d}")
            csd_ms.append(float(row["csd_ms"]))
            host_ms.append(float(row["host_ms"]))
            speedups.append(float(row["speedup"]))
    if len(queries) != 22:
        raise ValueError(f"expected 22 TPC-H rows, found {len(queries)}")
    return queries, np.asarray(csd_ms), np.asarray(host_ms), np.asarray(speedups)


def plot(input_path: Path, output_path: Path, pdf_path: Path) -> None:
    queries, csd_ms, host_ms, speedups = load_results(input_path)
    positions = np.arange(len(queries))
    width = 0.38

    plt.rcParams.update(
        {
            "font.size": 11,
            "axes.titlesize": 14,
            "axes.labelsize": 12,
            "figure.titlesize": 17,
        }
    )
    figure, (latency_axis, speedup_axis) = plt.subplots(
        2,
        1,
        figsize=(16, 9),
        sharex=True,
        gridspec_kw={"height_ratios": [1.55, 1.0], "hspace": 0.12},
    )

    latency_axis.bar(
        positions - width / 2,
        host_ms / 1000.0,
        width,
        label="PostgreSQL HOST",
        color="#4C78A8",
    )
    latency_axis.bar(
        positions + width / 2,
        csd_ms / 1000.0,
        width,
        label="CEMU CSD",
        color="#F58518",
    )
    latency_axis.set_yscale("log")
    latency_axis.set_ylabel("Wall-clock latency (s, log scale)")
    latency_axis.set_title("Per-query latency")
    latency_axis.grid(axis="y", which="both", alpha=0.25, linewidth=0.8)
    latency_axis.legend(loc="upper right", frameon=False, ncol=2)

    throughput_speedup = host_ms.sum() / csd_ms.sum()
    geomean_speedup = math.exp(np.log(speedups).mean())
    summary = (
        f"Total: HOST {host_ms.sum() / 1000:.2f}s, CSD {csd_ms.sum() / 1000:.2f}s\n"
        f"Throughput speedup: {throughput_speedup:.2f}x   "
        f"Geometric mean: {geomean_speedup:.2f}x"
    )
    latency_axis.text(
        0.012,
        0.965,
        summary,
        transform=latency_axis.transAxes,
        va="top",
        ha="left",
        bbox={"boxstyle": "round,pad=0.4", "facecolor": "white", "alpha": 0.88, "edgecolor": "#cccccc"},
    )

    speedup_colors = ["#2A9D8F" if value >= 1.0 else "#D1495B" for value in speedups]
    speedup_axis.bar(positions, speedups, width=0.72, color=speedup_colors)
    speedup_axis.axhline(1.0, color="#333333", linestyle="--", linewidth=1.2, label="Break-even (1x)")
    speedup_axis.set_ylabel("HOST / CSD speedup")
    speedup_axis.set_xlabel("TPC-H query")
    speedup_axis.set_title("CSD acceleration")
    speedup_axis.grid(axis="y", alpha=0.25, linewidth=0.8)
    speedup_axis.legend(loc="upper right", frameon=False)
    speedup_axis.set_ylim(0, max(speedups) * 1.14)
    for position, value in zip(positions, speedups):
        speedup_axis.text(
            position,
            value + max(speedups) * 0.018,
            f"{value:.2f}x",
            ha="center",
            va="bottom",
            rotation=90,
            fontsize=8,
        )

    speedup_axis.set_xticks(positions)
    speedup_axis.set_xticklabels(queries)
    figure.suptitle("TPC-H SF=1: PostgreSQL HOST vs FPGA-style CSD execution", y=0.985)
    figure.text(
        0.5,
        0.008,
        "CEMU TCG/VM-Freeze; 32 CSD lanes; PostgreSQL and OS caches cleared before every sample; restart time excluded.",
        ha="center",
        fontsize=9,
        color="#555555",
    )
    figure.subplots_adjust(top=0.92, bottom=0.1, left=0.07, right=0.985)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(output_path, dpi=220, bbox_inches="tight")
    figure.savefig(pdf_path, bbox_inches="tight")
    plt.close(figure)


def main() -> None:
    repository = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--input",
        type=Path,
        default=repository / "results" / "tpch_sf1_csd_performance.csv",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=repository / "results" / "tpch_sf1_host_vs_csd.png",
    )
    parser.add_argument(
        "--pdf",
        type=Path,
        default=repository / "results" / "tpch_sf1_host_vs_csd.pdf",
    )
    arguments = parser.parse_args()
    plot(arguments.input, arguments.output, arguments.pdf)


if __name__ == "__main__":
    main()
