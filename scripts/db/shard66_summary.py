#!/usr/bin/env python3
"""Summarize Shard-66A formal samples without mixing trace-on evidence."""

from __future__ import annotations

import csv
import json
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path


TAG_RE = re.compile(
    r"(shared-file|per-job-file)-b(4k|16k|64k)-j(1|8|16|32)-(randread|randwrite)-r(\d+)"
)


def number(value: object, default: float = 0.0) -> float:
    """Convert a numeric field while keeping malformed evidence visible to callers."""
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def read_kv(path: Path) -> dict[str, object]:
    """Read the key-value subset emitted by /usr/bin/time."""
    values: dict[str, object] = {}
    if not path.exists():
        return values
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key] = number(value.rstrip("%"), value)
    return values


def parse_tag(tag: str) -> dict[str, object]:
    """Decode one formal sample identity from its stable filename."""
    match = TAG_RE.fullmatch(tag)
    if not match:
        return {}
    model, block, jobs, rw, run = match.groups()
    return {
        "model": model,
        "block_size": block,
        "jobs": int(jobs),
        "rw": rw,
        "run": int(run),
    }


def fio_row(path: Path) -> dict[str, object]:
    """Extract latency, IOPS, and process CPU fields from one FIO sample."""
    data = json.loads(path.read_text(encoding="utf-8"))
    job = data.get("jobs", [{}])[0]
    read = job.get("read", {})
    write = job.get("write", {})
    timing = read_kv(path.with_suffix(".time"))
    row: dict[str, object] = {"tag": path.stem, "file": path.name}
    row.update(parse_tag(path.stem))
    row.update(
        {
            "read_iops": number(read.get("iops")),
            "write_iops": number(write.get("iops")),
            "read_avg_us": number(read.get("clat_ns", {}).get("mean")) / 1000.0,
            "write_avg_us": number(write.get("clat_ns", {}).get("mean")) / 1000.0,
            "read_p99_us": number(
                read.get("clat_ns", {}).get("percentile", {}).get("99.000000")
            )
            / 1000.0,
            "write_p99_us": number(
                write.get("clat_ns", {}).get("percentile", {}).get("99.000000")
            )
            / 1000.0,
            "fio_error": int(number(job.get("error"))),
            "cpu_percent": number(timing.get("cpu_percent")),
        }
    )
    return row


def write_tsv(path: Path, rows: list[dict[str, object]]) -> None:
    """Write rows with a stable tabular shape for later offline analysis."""
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    columns: list[str] = []
    for row in rows:
        for key in row:
            if key not in columns:
                columns.append(key)
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns, delimiter="\t", extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def medians(rows: list[dict[str, object]]) -> list[dict[str, object]]:
    """Compute medians independently for each file model, size, operation, and job count."""
    keys = ("model", "block_size", "rw", "jobs")
    groups: dict[tuple[object, ...], list[dict[str, object]]] = defaultdict(list)
    for row in rows:
        groups[tuple(row.get(key, "") for key in keys)].append(row)
    numeric = (
        "read_iops",
        "write_iops",
        "read_avg_us",
        "write_avg_us",
        "read_p99_us",
        "write_p99_us",
        "cpu_percent",
        "fio_error",
    )
    result: list[dict[str, object]] = []
    for group, members in sorted(groups.items(), key=lambda item: str(item[0])):
        row = {key: value for key, value in zip(keys, group)}
        row["samples"] = len(members)
        for key in numeric:
            row[key] = statistics.median([number(member.get(key)) for member in members])
        row["valid_samples"] = sum(int(number(member.get("fio_error"))) == 0 for member in members)
        result.append(row)
    return result


def main() -> int:
    """Summarize one output directory and return nonzero when no formal sample exists."""
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} OUT", file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    rows = [
        fio_row(path)
        for path in sorted(out.glob("*.json"))
        if parse_tag(path.stem)
    ]
    grouped = medians(rows)
    write_tsv(out / "formal_samples.tsv", rows)
    write_tsv(out / "formal_medians.tsv", grouped)
    summary = {
        "formal_sample_count": len(rows),
        "formal_group_count": len(grouped),
        "formal_medians": grouped,
        "trace_on_excluded": True,
    }
    (out / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True), encoding="utf-8"
    )
    print(f"summary: formal_samples={len(rows)} groups={len(grouped)} out={out}")
    return 0 if rows else 1


if __name__ == "__main__":
    raise SystemExit(main())
