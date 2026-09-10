#!/usr/bin/env python3
"""Summarize an HFS request trace without requiring external dependencies."""

from __future__ import annotations

import csv
import statistics
import sys
from collections import Counter, defaultdict


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} TRACE.tsv", file=sys.stderr)
        return 2
    phases = Counter()
    operations = Counter()
    buffers = Counter()
    parents = set()
    children = set()
    copyBytes = Counter()
    lockClasses = Counter()
    wakeReasons = Counter()
    metadataOps = Counter()
    durations = defaultdict(list)
    dropped = 0
    with open(sys.argv[1], newline="", encoding="utf-8") as stream:
        lines = []
        for line in stream:
            if line.startswith("#"):
                marker = "dropped="
                if marker in line:
                    try:
                        dropped += int(line.split(marker, 1)[1].split()[0])
                    except ValueError:
                        pass
                continue
            if line.strip():
                lines.append(line)
        for row in csv.DictReader(lines, delimiter="\t"):
            phase = row["phase"]
            phases[phase] += 1
            operations[row["operation"]] += 1
            buffers[row["buffer_kind"]] += 1
            parent = int(row["parent_id"])
            request = int(row["request_id"])
            if parent:
                parents.add(parent)
            if phase == "spdk_submit" and request:
                children.add(request)
            if phase in ("copy_in_end", "copy_out_end"):
                copyBytes[phase] += int(row["bytes"])
            if row.get("lock_class", "none") != "none":
                lockClasses[row["lock_class"]] += 1
            if row.get("wake_reason", "none") != "none":
                wakeReasons[row["wake_reason"]] += 1
            if row.get("metadata_op", "none") != "none":
                metadataOps[row["metadata_op"]] += 1
            duration = int(row["duration_ns"])
            if duration:
                durations[phase].append(duration)

    submit = phases["spdk_submit"]
    complete = phases["spdk_complete"]
    print(f"logical_io_count={phases['io_enter']}")
    print(f"spdk_child_count={submit}")
    print(f"request_amplification={submit / phases['io_enter']:.3f}"
          if phases["io_enter"] else "request_amplification=0")
    print(f"spdk_submit_complete={submit}/{complete}")
    print(f"copy_in_bytes={copyBytes['copy_in_end']}")
    print(f"copy_out_bytes={copyBytes['copy_out_end']}")
    print(f"trace_dropped={dropped}")
    print(f"parent_ids={len(parents)} child_ids={len(children)}")
    print("phase_counts=" + ",".join(f"{key}:{value}"
                                    for key, value in sorted(phases.items())))
    print("buffer_counts=" + ",".join(f"{key}:{value}"
                                     for key, value in sorted(buffers.items())))
    print("lock_class_counts=" + ",".join(
        f"{key}:{value}" for key, value in sorted(lockClasses.items())))
    print("wake_reason_counts=" + ",".join(
        f"{key}:{value}" for key, value in sorted(wakeReasons.items())))
    print("metadata_op_counts=" + ",".join(
        f"{key}:{value}" for key, value in sorted(metadataOps.items())))
    for phase, values in sorted(durations.items()):
        print(f"{phase}_p50_ns={int(statistics.median(values))}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
