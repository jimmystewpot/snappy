#!/usr/bin/env python3
"""Compare two Google Benchmark JSON result files.

Extracts median aggregates (or single-run entries if aggregates are absent),
prints a Markdown comparison table and suite geometric mean deltas, and exits
with code 1 if any suite geomean regresses by more than --max-regression-pct,
if zero throughput is detected in any comparison benchmark, or if benchmarks
present in the baseline (before) are missing in the comparison (after).
"""

import argparse
import json
import math
import sys
from typing import Any, Callable, Dict, List, Tuple


def load_benchmark_entries(json_path: str) -> Dict[str, Dict[str, Any]]:
    """Load benchmarks from a Google Benchmark JSON file.

    Extracts aggregate_name == 'median' if aggregates are present,
    otherwise all single-run entries.
    """
    with open(json_path, "r", encoding="utf-8") as f:
        data = json.load(f)

    raw_benchmarks = data.get("benchmarks", [])
    has_aggregates = any("aggregate_name" in b for b in raw_benchmarks)
    if has_aggregates:
        filtered = [b for b in raw_benchmarks if b.get("aggregate_name") == "median"]
    else:
        filtered = raw_benchmarks

    result: Dict[str, Dict[str, Any]] = {}
    for b in filtered:
        name = b.get("name", "")
        if name:
            result[name] = b
    return result


def get_throughput_mb_s(entry: Dict[str, Any]) -> float:
    """Return throughput in MB/s (10^6 bytes/sec)."""
    bps = entry.get("bytes_per_second")
    if bps is not None and bps > 0:
        return float(bps) / 1e6
    return 0.0


SUITES: List[Tuple[str, Callable[[str, Dict[str, Any]], bool]]] = [
    ("BM_Flat", lambda name, b: name.startswith("BM_Flat") or name.startswith("BM_ZFlat")),
    ("BM_UFlat", lambda name, b: name.startswith("BM_UFlat")),
    ("BM_UIOVec", lambda name, b: name.startswith("BM_UIOVec")),
    ("BM_UValidate", lambda name, b: name.startswith("BM_UValidate")),
    ("small-input (_200)", lambda name, b: "_200" in name or "_200" in str(b.get("label", ""))),
]


def compare_benchmarks(
    before_path: str, after_path: str, max_regression_pct: float
) -> int:
    before_entries = load_benchmark_entries(before_path)
    after_entries = load_benchmark_entries(after_path)

    common_names = sorted(set(before_entries.keys()) & set(after_entries.keys()))
    missing_in_after = sorted(set(before_entries.keys()) - set(after_entries.keys()))

    if not common_names and not missing_in_after:
        print(
            f"Error: No benchmarks found in {before_path}",
            file=sys.stderr,
        )
        return 1

    has_error = False

    if missing_in_after:
        print(f"### Warning: Missing Benchmarks in After\n")
        print(
            f"{len(missing_in_after)} benchmark(s) present in before were missing in after:"
        )
        for m in missing_in_after:
            print(f"- {m}")
        print()
        has_error = True

    # Print individual benchmark table
    print("| Benchmark | Before (MB/s) | After (MB/s) | Delta (%) |")
    print("| :--- | ---: | ---: | ---: |")

    benchmark_deltas: Dict[str, Tuple[float, float, float]] = {}

    for name in common_names:
        b_before = before_entries[name]
        b_after = after_entries[name]
        before_mb = get_throughput_mb_s(b_before)
        after_mb = get_throughput_mb_s(b_after)

        if before_mb > 0:
            ratio = after_mb / before_mb
            delta_pct = (ratio - 1.0) * 100.0
            benchmark_deltas[name] = (before_mb, after_mb, delta_pct)
            print(f"| {name} | {before_mb:.2f} | {after_mb:.2f} | {delta_pct:+.2f}% |")
        else:
            print(f"| {name} | {before_mb:.2f} | {after_mb:.2f} | N/A |")

    print("\n### Suite Geomean Summary\n")
    print("| Suite | Before (MB/s) | After (MB/s) | Delta (%) | Status |")
    print("| :--- | ---: | ---: | ---: | :--- |")

    for suite_name, matcher in SUITES:
        before_suite_benchmarks = [
            name
            for name in before_entries
            if matcher(name, before_entries[name])
        ]
        if not before_suite_benchmarks:
            continue

        matching_names = [
            name
            for name in common_names
            if name in benchmark_deltas
            and matcher(name, before_entries[name])
        ]

        missing_in_suite = set(before_suite_benchmarks) - set(matching_names)

        before_valid = [
            get_throughput_mb_s(before_entries[n])
            for n in before_suite_benchmarks
            if get_throughput_mb_s(before_entries[n]) > 0
        ]
        before_geomean = (
            math.exp(sum(math.log(x) for x in before_valid) / len(before_valid))
            if before_valid
            else 0.0
        )

        if not matching_names:
            has_error = True
            status = "FAIL (entire suite missing in after)"
            print(
                f"| {suite_name} | {before_geomean:.2f} | 0.00 | -100.00% | {status} |"
            )
            continue

        if missing_in_suite:
            has_error = True
            status = f"FAIL ({len(missing_in_suite)} benchmark(s) missing in after)"
            print(
                f"| {suite_name} | {before_geomean:.2f} | 0.00 | -100.00% | {status} |"
            )
            continue

        zero_throughput_names = [
            n for n in matching_names if benchmark_deltas[n][1] <= 0
        ]
        if zero_throughput_names:
            has_error = True
            status = (
                f"FAIL (zero throughput in {len(zero_throughput_names)} benchmark(s))"
            )
            print(
                f"| {suite_name} | {before_geomean:.2f} | 0.00 | -100.00% | {status} |"
            )
            continue

        ratios = [
            benchmark_deltas[n][1] / benchmark_deltas[n][0] for n in matching_names
        ]
        log_ratios = [math.log(r) for r in ratios]
        geomean_ratio = math.exp(sum(log_ratios) / len(log_ratios))
        suite_delta_pct = (geomean_ratio - 1.0) * 100.0

        after_geomean = math.exp(
            sum(math.log(benchmark_deltas[n][1]) for n in matching_names)
            / len(matching_names)
        )

        regressed = suite_delta_pct < -max_regression_pct
        if regressed:
            has_error = True
            status = (
                f"FAIL (regression {abs(suite_delta_pct):.2f}% > {max_regression_pct:.2f}%)"
            )
        else:
            status = "PASS"

        print(
            f"| {suite_name} | {before_geomean:.2f} | {after_geomean:.2f} | {suite_delta_pct:+.2f}% | {status} |"
        )

    print()
    if has_error:
        print(
            f"Result: FAILED - regressions, missing benchmarks, or zero throughput detected\n"
        )
        return 1
    else:
        print(
            f"Result: PASSED - all suite geomeans within threshold (<= {max_regression_pct:.2f}% regression)\n"
        )
        return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare two Google Benchmark JSON files and check for regressions."
    )
    parser.add_argument("before", help="Path to baseline (before) benchmark JSON file")
    parser.add_argument("after", help="Path to comparison (after) benchmark JSON file")
    parser.add_argument(
        "--max-regression-pct",
        type=float,
        default=5.0,
        help="Maximum allowed suite geomean regression percentage (default: 5.0)",
    )
    args = parser.parse_args()

    return compare_benchmarks(args.before, args.after, args.max_regression_pct)


if __name__ == "__main__":
    sys.exit(main())
