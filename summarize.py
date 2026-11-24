# Copyright (c) 2026 Peter Fors
# SPDX-License-Identifier: MIT

import argparse
import csv
from collections import defaultdict
from pathlib import Path
from statistics import median


CASE_FIELDS = (
    "operation", "workload", "input_bytes", "placement", "input_offset",
    "output_shift", "output_bytes", "error_at", "implementation",
)

NUMERIC_FIELDS = {
    "input_bytes", "input_offset", "output_shift", "output_bytes", "error_at",
}

RESULT_FIELDS = (
    "cpu", "isa", *CASE_FIELDS, "trials", "calls_per_trial", "ns_per_call",
    "cycles_per_call", "cycles_per_input_byte", "input_gb_s",
    "min_ns_per_call", "max_ns_per_call",
)

EXPECTED_CASES = 4 * 5 * 23 * 3 + 6


def summarize(paths):
    cpu, isa = paths[0].stem.split("-")
    combined = defaultdict(list)

    for path in paths:
        samples = defaultdict(dict)
        with path.open(newline="") as source:
            for row in csv.DictReader(source):
                key = tuple(int(row[k]) if k in NUMERIC_FIELDS else row[k] for k in CASE_FIELDS)
                trial = int(row["trial"])
                repeats = int(row["repeats"])
                ns = int(row["elapsed_ns"]) / repeats
                cycles = int(row["cycles"]) / repeats
                if trial in samples[key] or not ns > 0 or not cycles > 0:
                    raise ValueError(f"{path}: invalid trial {trial} for {key}")
                samples[key][trial] = (repeats, ns, cycles)

        if len(samples) != 2 * EXPECTED_CASES:
            raise ValueError(f"{path}: got {len(samples)} series, expected {2 * EXPECTED_CASES}")

        if combined and samples.keys() != combined.keys():
            raise ValueError(f"{path}: cases differ across launches")

        for key, trials in samples.items():
            if sorted(trials) != list(range(5)) or len({v[0] for v in trials.values()}) != 1:
                raise ValueError(f"{path}: incomplete or inconsistent trials for {key}")
            combined[key].extend(trials.values())

    for key, trials in combined.items():
        if len({v[0] for v in trials}) != 1:
            raise ValueError(f"{cpu}-{isa}: inconsistent calls per trial for {key}")

        case = dict(zip(CASE_FIELDS, key))
        repeats, ns, cycles = zip(*trials)
        time = median(ns)
        count = median(cycles)
        valid = case["error_at"] < 0
        size = case["input_bytes"]
        yield {
            "cpu": cpu, "isa": isa, **case, "trials": len(trials),
            "calls_per_trial": repeats[0],
            "ns_per_call": f"{time:.6f}", "cycles_per_call": f"{count:.6f}",
            "cycles_per_input_byte": f"{count / size:.8f}" if valid else "",
            "input_gb_s": f"{size / time:.6f}" if valid else "",
            "min_ns_per_call": f"{min(ns):.6f}",
            "max_ns_per_call": f"{max(ns):.6f}",
        }


def main():
    parser = argparse.ArgumentParser(description="Check and summarize ffutf benchmark batches.")
    parser.add_argument("output", type=Path)
    parser.add_argument("captures", nargs="+", type=Path)
    args = parser.parse_args()
    results = []
    captures = defaultdict(list)
    seen = set()

    for path in args.captures:
        resolved = path.resolve()
        if resolved in seen:
            raise ValueError(f"duplicate capture: {path}")
        seen.add(resolved)
        captures[path.stem].append(path)

    for paths in captures.values():
        results.extend(summarize(paths))

    paired = defaultdict(set)
    workloads = defaultdict(set)
    for row in results:
        key = tuple(row[field] for field in ("cpu", "isa", *CASE_FIELDS[:-1]))
        paired[key].add(row["implementation"])
        workloads[row["cpu"], row["isa"]].add(key[2:])
    if any(methods != {"ffutf", "simdutf"} for methods in paired.values()):
        raise ValueError("missing paired implementation")

    for (_, isa), cases in workloads.items():
        if any(other_cases != cases for (_, other_isa), other_cases in workloads.items() if other_isa == isa):
            raise ValueError(f"hosts ran different cases for {isa}")

    with args.output.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=RESULT_FIELDS)
        writer.writeheader()
        writer.writerows(results)


if __name__ == "__main__":
    main()
