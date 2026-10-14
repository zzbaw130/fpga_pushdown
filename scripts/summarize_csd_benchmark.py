#!/usr/bin/env python3
"""Validate paired guest timings; the HOST side includes TCG CPU emulation."""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="benchmark_csd_tpch.exp CSV")
    parser.add_argument("--expected-queries", type=int, default=22)
    parser.add_argument("--output-csv", type=Path,
                        help="write query,csd_ms,host_ms,speedup for plotting")
    args = parser.parse_args()

    pairs: dict[int, dict[str, dict[str, str]]] = {}
    with args.input.open(newline="", encoding="utf-8") as source:
        for row in csv.DictReader(source):
            query = int(row["query"])
            mode = row["mode"].lower()
            if mode not in {"host", "csd", "auto"}:
                raise ValueError(f"Q{query}: unknown mode {mode!r}")
            if mode in pairs.setdefault(query, {}):
                raise ValueError(f"Q{query}: duplicate {mode} measurement")
            if int(row["rc"]) != 0 or int(row["match"]) != 0:
                raise ValueError(f"Q{query} {mode}: failed or result mismatch")
            if not math.isfinite(float(row["guest_ms"])) or \
                    float(row["guest_ms"]) <= 0:
                raise ValueError(f"Q{query} {mode}: invalid guest timing")
            pairs[query][mode] = row

    if len(pairs) != args.expected_queries:
        raise ValueError(f"expected {args.expected_queries} query pairs, got {len(pairs)}")
    if args.expected_queries == 22 and set(pairs) != set(range(1, 23)):
        raise ValueError("full TPC-H benchmark must contain Q1 through Q22")
    plot_rows = []
    modes = set()
    for query, measurements in sorted(pairs.items()):
        device_modes = set(measurements) - {"host"}
        if "host" not in measurements or len(device_modes) != 1:
            raise ValueError(f"Q{query}: expected exactly one HOST and one device path")
        mode = device_modes.pop()
        modes.add(mode)
        host_ms = float(measurements["host"]["guest_ms"])
        csd_ms = float(measurements[mode]["guest_ms"])
        plot_rows.append({"query": query, "csd_ms": csd_ms,
                          "host_ms": host_ms, "speedup": host_ms / csd_ms})
    if len(modes) != 1:
        raise ValueError(f"mixed device modes in one benchmark: {sorted(modes)}")

    total_host = sum(row["host_ms"] for row in plot_rows)
    total_device = sum(row["csd_ms"] for row in plot_rows)
    summary = {
        "mode": modes.pop(),
        "interpretation": "same-guest TCG ratio, not native-HOST or hardware speedup",
        "query_count": len(plot_rows),
        "host_guest_ms": total_host,
        "device_guest_ms": total_device,
        "total_speedup": total_host / total_device,
        "geomean_speedup": math.exp(sum(math.log(row["speedup"])
                                          for row in plot_rows) / len(plot_rows)),
        "minimum_speedup": min(row["speedup"] for row in plot_rows),
        "maximum_speedup": max(row["speedup"] for row in plot_rows),
    }
    if args.output_csv:
        with args.output_csv.open("w", newline="", encoding="utf-8") as sink:
            writer = csv.DictWriter(sink,
                                    fieldnames=["query", "csd_ms", "host_ms", "speedup"],
                                    lineterminator="\n")
            writer.writeheader()
            writer.writerows(plot_rows)
    print(json.dumps(summary, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
