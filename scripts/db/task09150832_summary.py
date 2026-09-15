#!/usr/bin/env python3
"""Summarize the raw task09150832 measurements into stable TSV/JSON files."""

from __future__ import annotations

import csv
import json
import math
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path


STAGES = {
    "T1-T2": (1, 2),
    "T4-T5": (4, 5),
    "T6-T7": (6, 7),
    "T8-T9": (8, 9),
    "T9-T10": (9, 10),
}
TICK_HZ = 2_500_000_000.0


def read_kv(path: Path) -> dict[str, float | str]:
    result: dict[str, float | str] = {}
    if not path.exists():
        return result
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        try:
            result[key] = float(value) if "." in value else int(value)
        except ValueError:
            result[key] = value
    return result


def number(value: object, default: float = 0.0) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def percentile(values: list[float], q: float) -> float:
    if not values:
        return 0.0
    return float(statistics.quantiles(values, n=100, method="inclusive")[int(q) - 1]) if len(values) > 1 else values[0]


def parse_profile(path: Path) -> dict[str, float]:
    samples: dict[str, list[float]] = defaultdict(list)
    if not path.exists():
        return {f"{stage}_{stat}": 0.0 for stage in STAGES for stat in ("avg_us", "p50_us", "p95_us", "p99_us")}
    with path.open(newline="", encoding="utf-8", errors="replace") as stream:
        rows = csv.DictReader((line for line in stream if not line.startswith("#")), delimiter="\t")
        for row in rows:
            if row.get("phase") != "parent_profile" or row.get("profile_valid") != "1":
                continue
            ticks = [number(row.get(f"profile_t{i}")) for i in range(12)]
            for stage, (left, right) in STAGES.items():
                if ticks[right] >= ticks[left] > 0:
                    samples[stage].append((ticks[right] - ticks[left]) * 1_000_000.0 / TICK_HZ)
    output: dict[str, float] = {}
    for stage in STAGES:
        values = samples[stage]
        output[f"{stage}_count"] = len(values)
        output[f"{stage}_avg_us"] = statistics.mean(values) if values else 0.0
        output[f"{stage}_p50_us"] = percentile(values, 50)
        output[f"{stage}_p95_us"] = percentile(values, 95)
        output[f"{stage}_p99_us"] = percentile(values, 99)
    return output


def parse_vmstat(path: Path) -> dict[str, float]:
    """Return median run queue and context-switch rates from vmstat samples."""
    run_queue: list[float] = []
    context_switches: list[float] = []
    if path.exists():
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            fields = line.split()
            if len(fields) < 17 or not fields[0].isdigit():
                continue
            try:
                run_queue.append(float(fields[0]))
                context_switches.append(float(fields[11]))
            except ValueError:
                continue
    return {
        "run_queue_r_avg": statistics.mean(run_queue) if run_queue else 0.0,
        "run_queue_r_p95": percentile(run_queue, 95),
        "context_switches_s_avg": statistics.mean(context_switches) if context_switches else 0.0,
        "context_switches_s_p95": percentile(context_switches, 95),
    }


def parse_a3(out: Path) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    pattern = re.compile(r"A3 internal .*?rw=(\w+) workers=(\d+) actual_workers=(\d+) actual_requests=(\d+) actual_spdk_submits=(\d+) outstanding_peak=(\d+) runtime_ns=(\d+) runtime_seconds=([0-9.]+) iops=([0-9.]+) errors=(\d+)")
    for path in sorted(out.glob("a3-*.log")):
        match = pattern.search(path.read_text(encoding="utf-8", errors="replace"))
        row: dict[str, object] = {"tag": path.stem, "file": path.name}
        if match:
            keys = ("rw", "workers", "actual_workers", "actual_requests", "actual_spdk_submits", "outstanding_peak", "runtime_ns", "runtime_seconds", "iops", "errors")
            for key, value in zip(keys, match.groups()):
                row[key] = float(value) if key in ("runtime_seconds", "iops") else int(value) if key != "rw" else value
        else:
            row["parse_error"] = True
        rows.append(row)
    return rows


def fio_row(path: Path) -> dict[str, object]:
    data = json.loads(path.read_text(encoding="utf-8"))
    job = data.get("jobs", [{}])[0]
    read = job.get("read", {})
    write = job.get("write", {})
    stats = read_kv(path.with_suffix(".stats"))
    timing = read_kv(path.with_suffix(".time"))
    name = path.stem
    match = re.match(r"(scale|affinity|ordering)-(.+)", name)
    row: dict[str, object] = {"tag": name, "file": path.name}
    if match:
        row["family"] = match.group(1)
        row["case"] = match.group(2)
    row.update({
        "read_iops": number(read.get("iops")),
        "write_iops": number(write.get("iops")),
        "iops": number(read.get("iops")) + number(write.get("iops")),
        "read_avg_us": number(read.get("clat_ns", {}).get("mean")) / 1000.0,
        "write_avg_us": number(write.get("clat_ns", {}).get("mean")) / 1000.0,
        "read_p99_us": number(read.get("clat_ns", {}).get("percentile", {}).get("99.000000")) / 1000.0,
        "write_p99_us": number(write.get("clat_ns", {}).get("percentile", {}).get("99.000000")) / 1000.0,
        "fio_error": int(job.get("error", 0)),
        "spdk_submit": int(number(stats.get("spdk_submit"))),
        "spdk_complete": int(number(stats.get("spdk_complete"))),
        "spdk_reject": int(number(stats.get("spdk_reject"))),
        "runtime_queue_depth_peak": int(number(stats.get("runtime_queue_depth_peak"))),
        "runtime_queue_wait_ns": int(number(stats.get("runtime_queue_wait_ns"))),
        "cpu_percent": timing.get("cpu_percent", ""),
        "cpu_percent_num": number(str(timing.get("cpu_percent", "")).rstrip("%")),
        "max_rss_kb": int(number(timing.get("max_rss_kb"))),
        "voluntary_ctxt_switches": int(number(timing.get("voluntary_ctxt_switches"))),
        "involuntary_ctxt_switches": int(number(timing.get("involuntary_ctxt_switches"))),
    })
    row.update(parse_vmstat(path.with_suffix(".vmstat")))
    row.update(parse_profile(path.with_suffix(".trace.tsv")))
    for key in ("ticket_admit", "ticket_dependency_waits", "ticket_pending_peak", "ticket_scan_items_total", "ticket_active_list_length_peak", "io_copy_bytes", "io_copy_ops", "errors"):
        row[key] = int(number(stats.get(key)))
    for metric in ("admit_lock_wait", "admit_scan", "admit_insert", "dependency_wait", "complete_lock_wait", "complete_scan", "wake_processing"):
        for suffix in ("avg_ns", "p50_ns", "p95_ns", "p99_ns", "count"):
            row[f"ticket_{metric}_{suffix}"] = int(number(stats.get(f"ticket_{metric}_{suffix}")))
    for bucket in ("0", "1_4", "5_8", "9_16", "17_32", "33_64", "gt64"):
        row[f"admission_scan_{bucket}"] = int(number(stats.get(f"ticket_admission_scan_length_{bucket}")))
        row[f"completion_scan_{bucket}"] = int(number(stats.get(f"ticket_completion_scan_length_{bucket}")))
    for key, value in stats.items():
        if str(key).startswith("data_reactor_") and str(key).endswith("_submit"):
            row[key] = int(number(value))
    return row


def write_tsv(path: Path, rows: list[dict[str, object]]) -> None:
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


def median_rows(rows: list[dict[str, object]], key_fn, numeric_keys: list[str]) -> list[dict[str, object]]:
    groups: dict[object, list[dict[str, object]]] = defaultdict(list)
    for row in rows:
        groups[key_fn(row)].append(row)
    output = []
    for group, members in sorted(groups.items(), key=lambda item: str(item[0])):
        result = {"group": "|".join(str(part) for part in group) if isinstance(group, tuple) else str(group), "samples": len(members)}
        for key in numeric_keys:
            values = [number(member.get(key)) for member in members if member.get(key) not in (None, "")]
            result[key] = statistics.median(values) if values else 0.0
        output.append(result)
    return output


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} OUT", file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    a3 = parse_a3(out)
    fio = [fio_row(path) for path in sorted(out.glob("*.json")) if path.name != "summary.json"]
    write_tsv(out / "a3.tsv", a3)
    write_tsv(out / "fio_samples.tsv", fio)
    numeric = ["iops", "read_iops", "write_iops", "read_avg_us", "write_avg_us", "read_p99_us", "write_p99_us", "T1-T2_avg_us", "T4-T5_avg_us", "T6-T7_avg_us", "T8-T9_avg_us", "T9-T10_avg_us", "ticket_admit", "ticket_dependency_waits", "ticket_pending_peak", "runtime_queue_depth_peak", "run_queue_r_avg", "run_queue_r_p95", "context_switches_s_avg", "context_switches_s_p95", "voluntary_ctxt_switches", "involuntary_ctxt_switches", "cpu_percent_num", "spdk_submit", "spdk_complete", "fio_error"]
    scaling = [row for row in fio if row.get("family") == "scale"]
    ordering = [row for row in fio if row.get("family") == "ordering"]
    affinity = [row for row in fio if row.get("family") == "affinity"]
    key_scale = lambda row: tuple(str(row.get("case", "")).rsplit("-r", 1)[0].split("-"))
    key_order = lambda row: tuple(str(row.get("case", "")).rsplit("-r", 1)[0].split("-"))
    write_tsv(out / "scaling_medians.tsv", median_rows(scaling, key_scale, numeric))
    write_tsv(out / "ordering_medians.tsv", median_rows(ordering, key_order, numeric))
    write_tsv(out / "affinity.tsv", affinity)
    summary = {"a3": a3, "fio_samples": fio, "scaling_medians": median_rows(scaling, key_scale, numeric), "ordering_medians": median_rows(ordering, key_order, numeric), "affinity": affinity}
    (out / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True), encoding="utf-8")
    if not a3 and not fio:
        print("no measurements found", file=sys.stderr)
        return 1
    print(f"summary: a3={len(a3)} fio={len(fio)} out={out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
