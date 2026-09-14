#!/usr/bin/env python3
"""Summarize parent T0..T11 profiles and reject non-monotonic samples."""

from __future__ import annotations

import csv
import sys
from pathlib import Path


def percentile(values: list[int], fraction: float) -> int:
    """Return the nearest-rank percentile for one stage duration list."""
    if not values:
        return 0
    values = sorted(values)
    index = max(0, min(len(values) - 1, int((len(values) - 1) * fraction)))
    return values[index]


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print(f"usage: {sys.argv[0]} TRACE.tsv [ticks_hz]", file=sys.stderr)
        return 2
    path = Path(sys.argv[1])
    ticks_hz = int(sys.argv[2]) if len(sys.argv) == 3 else 0
    stage_values: list[list[int]] = [[] for _ in range(11)]
    profile_count = 0
    invalid = 0
    child_total = 0
    dropped = 0
    with path.open(newline="", encoding="utf-8") as stream:
        rows = csv.DictReader((line for line in stream if not line.startswith("#")),
                              delimiter="\t")
        for row in rows:
            if row.get("phase") != "parent_profile":
                continue
            profile_count += 1
            child_total += int(row.get("profile_child_count", "0"))
            ticks = [int(row.get(f"profile_t{i}", "0")) for i in range(12)]
            valid = row.get("profile_valid", "0") == "1"
            if not valid or any(ticks[i] and ticks[i + 1] and ticks[i] > ticks[i + 1]
                                for i in range(11)):
                invalid += 1
                continue
            for i in range(11):
                if ticks[i] and ticks[i + 1]:
                    stage_values[i].append(ticks[i + 1] - ticks[i])
        stream.seek(0)
        for line in stream:
            if line.startswith("#") and "dropped=" in line:
                dropped += int(line.split("dropped=", 1)[1].split()[0])

    print(f"profile_count={profile_count}")
    print(f"profile_invalid={invalid}")
    print(f"profile_child_total={child_total}")
    print(f"trace_dropped={dropped}")
    print(f"ticks_hz={ticks_hz}")
    for i, values in enumerate(stage_values):
        name = f"T{i}-T{i + 1}"
        total = sum(values)
        count = len(values)
        avg = total / count if count else 0
        p50 = percentile(values, 0.50)
        p95 = percentile(values, 0.95)
        p99 = percentile(values, 0.99)
        avg_us = avg * 1e6 / ticks_hz if ticks_hz else 0
        p99_us = p99 * 1e6 / ticks_hz if ticks_hz else 0
        print(f"stage={name} count={count} total_ticks={total} avg_ticks={avg:.2f} "
              f"p50_ticks={p50} p95_ticks={p95} p99_ticks={p99} "
              f"avg_us={avg_us:.3f} p99_us={p99_us:.3f}")
    return 0 if invalid == 0 and dropped == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
