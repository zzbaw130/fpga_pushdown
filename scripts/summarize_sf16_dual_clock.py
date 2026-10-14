#!/usr/bin/env python3
"""Compare native HOST query time with CEMU's FPGA/Flash device-time model.

The result is a cross-platform projection, never a measured CSD hardware
speedup: native PostgreSQL runs on this server, while CSD time comes from the
CEMU parameterized scheduler and excludes VM-Freeze functional C++ wall time.
"""

from __future__ import annotations

import argparse
import csv
import math
import re
from pathlib import Path


TIME = re.compile(r"^Time:\s+([0-9.]+)\s+ms", re.MULTILINE)
PROFILE_LINE = re.compile(r"CSD profile status [^\n]+")
PROFILE_FIELD = re.compile(r"\b([a-z_]+)=([0-9]+)\b")
CACHE_RESIDENT = re.compile(r"Resident Pages:[^\n]*\s([0-9.]+)%")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csd_dir", type=Path,
                        help="test_csd_tpch_execution.sh output directory")
    parser.add_argument("native_dir", type=Path,
                        help="benchmark_native_host_tpch.sh output directory")
    parser.add_argument("--expected-queries", type=int, default=22)
    parser.add_argument("--max-cached-percent", type=float, default=1.0,
                        help="reject native samples with a warmer cache")
    parser.add_argument("--output-csv", type=Path)
    args = parser.parse_args()
    if not math.isfinite(args.max_cached_percent) or not 0 <= args.max_cached_percent <= 100:
        parser.error("--max-cached-percent must be between 0 and 100")

    native = {}
    with (args.native_dir / "timings.csv").open(newline="", encoding="utf-8") as source:
        for row in csv.DictReader(source):
            query = int(row["query"])
            if query in native:
                raise ValueError(f"Q{query}: duplicate native measurement")
            native[query] = row
    if len(native) != args.expected_queries:
        raise ValueError(f"expected {args.expected_queries} native rows, got {len(native)}")
    if args.expected_queries == 22 and set(native) != set(range(1, 23)):
        raise ValueError("full TPC-H comparison requires Q1 through Q22")

    rows = []
    for query, reference in sorted(native.items()):
        raw_path = args.csd_dir / f"q{query}.csd.raw"
        csd_path = args.csd_dir / f"q{query}.csd.txt"
        golden_path = args.csd_dir / f"q{query}.host.txt"
        raw = raw_path.read_text(encoding="utf-8")
        if "ERROR:" in raw or csd_path.read_bytes() != golden_path.read_bytes():
            raise ValueError(f"Q{query}: CSD failed or is not byte-exact")
        if int(reference["rc"]) or int(reference["exact"]):
            raise ValueError(f"Q{query}: native HOST failed or is not byte-exact")
        cache_log = (args.native_dir /
                     f"q{query}.cache-before.log").read_text(encoding="utf-8")
        cache_match = CACHE_RESIDENT.search(cache_log)
        if cache_match is None:
            raise ValueError(f"Q{query}: missing native cache-state evidence")
        cached_pct = float(cache_match.group(1))
        if cached_pct > args.max_cached_percent:
            raise ValueError(f"Q{query}: native cache {cached_pct}% exceeds limit")
        timings = [float(value) for value in TIME.findall(raw)]
        if not timings or not all(math.isfinite(value) for value in timings):
            raise ValueError(f"Q{query}: no finite CEMU guest query time")
        csd_ms = sum(timings)
        native_ms = float(reference["native_ms"])
        if csd_ms <= 0 or not math.isfinite(native_ms) or native_ms <= 0:
            raise ValueError(f"Q{query}: non-positive query time")
        profiles = [dict((key, int(value)) for key, value in
                         PROFILE_FIELD.findall(line))
                    for line in PROFILE_LINE.findall(raw)]
        required = {"materialized_batch_peak_bytes", "fdm_reserved_bytes",
                    "operator_memory_bytes", "operator_peak_bytes",
                    "spill_peak_bytes", "spill_failed_required_bytes"}
        if not profiles or any(not required <= profile.keys() for profile in profiles):
            raise ValueError(f"Q{query}: CSD profile is missing")
        if any(profile["spill_failed_required_bytes"] for profile in profiles):
            raise ValueError(f"Q{query}: CSD reported a spill allocation failure")
        conservative_sum = max(profile["fdm_reserved_bytes"] -
                               profile["operator_memory_bytes"] +
                               profile["operator_peak_bytes"] +
                               profile["materialized_batch_peak_bytes"]
                               for profile in profiles)
        rows.append({
            "query": query,
            "csd_model_ms": round(csd_ms, 3),
            "native_host_ms": round(native_ms, 3),
            "native_cached_percent": cached_pct,
            "projected_ratio": round(native_ms / csd_ms, 6),
            "materialized_batch_peak_bytes": max(
                profile["materialized_batch_peak_bytes"] for profile in profiles),
            "fdm_reserved_bytes": max(profile["fdm_reserved_bytes"]
                                      for profile in profiles),
            "fdm_plus_stl_peak_sum_bytes": conservative_sum,
            "spill_peak_bytes": max(profile["spill_peak_bytes"]
                                    for profile in profiles),
        })

    fieldnames = list(rows[0])
    if args.output_csv:
        with args.output_csv.open("w", newline="", encoding="utf-8") as sink:
            writer = csv.DictWriter(sink, fieldnames=fieldnames,
                                    lineterminator="\n")
            writer.writeheader()
            writer.writerows(rows)
    total_csd = sum(row["csd_model_ms"] for row in rows)
    total_host = sum(row["native_host_ms"] for row in rows)
    offline_oracle = sum(min(row["csd_model_ms"], row["native_host_ms"])
                         for row in rows)
    csd_faster = sum(row["csd_model_ms"] < row["native_host_ms"]
                     for row in rows)
    geomean = math.exp(sum(math.log(row["native_host_ms"] /
                                   row["csd_model_ms"]) for row in rows) / len(rows))
    print(f"queries={len(rows)} exact={len(rows)}")
    print(f"CSD model total={total_csd:.3f} ms")
    print(f"Native HOST total={total_host:.3f} ms")
    print(f"Cross-platform projected throughput ratio={total_host / total_csd:.4f}x")
    print(f"Cross-platform projected geometric mean={geomean:.4f}x")
    print(f"CSD-faster queries={csd_faster}/{len(rows)}")
    print(f"Offline per-query oracle ratio={total_host / offline_oracle:.4f}x (not AUTO)")
    print(f"Largest C++ Batch estimate={max(row['materialized_batch_peak_bytes'] for row in rows):,} B")
    print(f"Largest FDM plus disjoint peak sum={max(row['fdm_plus_stl_peak_sum_bytes'] for row in rows):,} B")
    print("Caution: the sum omits other STL allocations and does not prove a physical 4 GiB bound.")
    print("Warning: this ratio is not measured same-hardware CSD speedup.")


if __name__ == "__main__":
    main()
