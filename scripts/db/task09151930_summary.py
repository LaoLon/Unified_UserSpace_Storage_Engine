#!/usr/bin/env python3
"""Summarize the stage-3 reactor/channel matrix and backend attribution logs."""

from __future__ import annotations

import csv
import json
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path


TICK_HZ = 2_500_000_000.0
STAGES = {"T4-T5": (4, 5), "T6-T7": (6, 7)}


def number(value: object, default: float = 0.0) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def read_kv(path: Path) -> dict[str, float | str]:
    values: dict[str, float | str] = {}
    if not path.exists():
        return values
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        try:
            values[key] = float(value) if "." in value else int(value)
        except ValueError:
            values[key] = value
    return values


def percentile(values: list[float], q: float) -> float:
    if not values:
        return 0.0
    if len(values) == 1:
        return values[0]
    return float(statistics.quantiles(values, n=100, method="inclusive")[int(q) - 1])


def parse_trace(path: Path) -> dict[str, object]:
    stage_values: dict[str, list[float]] = defaultdict(list)
    tb_values: dict[str, list[float]] = defaultdict(list)
    reactors: list[int] = []
    submit_cpus: list[int] = []
    complete_cpus: list[int] = []
    events: dict[int, dict[str, dict[str, str]]] = defaultdict(dict)
    if path.exists():
        with path.open(newline="", encoding="utf-8", errors="replace") as stream:
            rows = csv.DictReader((line for line in stream if not line.startswith("#")), delimiter="\t")
            for row in rows:
                parent = int(number(row.get("parent_id")))
                phase = row.get("phase", "")
                if parent:
                    events[parent][phase] = row
                if phase == "backend_tb1":
                    reactor = int(number(row.get("reactor")))
                    reactors.append(reactor)
                    submit_cpus.append(int(number(row.get("cpu"), -1)))
                elif phase == "backend_tb2":
                    complete_cpus.append(int(number(row.get("cpu"), -1)))
                if phase == "parent_profile" and row.get("profile_valid") == "1":
                    ticks = [number(row.get(f"profile_t{i}")) for i in range(12)]
                    for name, (left, right) in STAGES.items():
                        if ticks[right] >= ticks[left] > 0:
                            stage_values[name].append((ticks[right] - ticks[left]) * 1e6 / TICK_HZ)
    for request in events.values():
        tb0 = request.get("backend_tb0")
        tb1 = request.get("backend_tb1")
        tb2 = request.get("backend_tb2")
        if tb0 and tb1:
            tb_values["TB0-TB1_us"].append(max(0.0, (number(tb1["timestamp_ns"]) - number(tb0["timestamp_ns"])) / 1000.0))
        if tb1 and tb2:
            tb_values["TB1-TB2_us"].append(max(0.0, (number(tb2["timestamp_ns"]) - number(tb1["timestamp_ns"])) / 1000.0))
    result: dict[str, object] = {"trace_samples": len(reactors)}
    for name, values in {**stage_values, **tb_values}.items():
        result[f"{name}_count"] = len(values)
        result[f"{name}_avg_us"] = statistics.mean(values) if values else 0.0
        result[f"{name}_p50_us"] = percentile(values, 50)
        result[f"{name}_p95_us"] = percentile(values, 95)
        result[f"{name}_p99_us"] = percentile(values, 99)
    result["submit_cpu_set"] = ",".join(str(value) for value in sorted(set(submit_cpus)) if value >= 0)
    result["complete_cpu_set"] = ",".join(str(value) for value in sorted(set(complete_cpus)) if value >= 0)
    return result


def parse_tag(tag: str) -> dict[str, object]:
    match = re.match(r"(fixed|deployment)-d(\d+)-(randread|randwrite)-j(\d+)-r(\d+)$", tag)
    if not match:
        return {}
    group, reactors, rw, jobs, run = match.groups()
    return {"group": group, "data_reactors": int(reactors), "rw": rw,
            "jobs": int(jobs), "run": int(run)}


def fio_row(path: Path) -> dict[str, object]:
    data = json.loads(path.read_text(encoding="utf-8"))
    job = data.get("jobs", [{}])[0]
    read = job.get("read", {})
    write = job.get("write", {})
    stats = read_kv(path.with_suffix(".stats"))
    timing = read_kv(path.with_suffix(".time"))
    row: dict[str, object] = {"tag": path.stem, "file": path.name}
    row.update(parse_tag(path.stem))
    row.update({
        "read_iops": number(read.get("iops")), "write_iops": number(write.get("iops")),
        "read_avg_us": number(read.get("clat_ns", {}).get("mean")) / 1000.0,
        "write_avg_us": number(write.get("clat_ns", {}).get("mean")) / 1000.0,
        "read_p99_us": number(read.get("clat_ns", {}).get("percentile", {}).get("99.000000")) / 1000.0,
        "write_p99_us": number(write.get("clat_ns", {}).get("percentile", {}).get("99.000000")) / 1000.0,
        "fio_error": int(number(job.get("error"))),
        "spdk_submit": int(number(stats.get("spdk_submit"))),
        "spdk_complete": int(number(stats.get("spdk_complete"))),
        "spdk_reject": int(number(stats.get("spdk_reject"))),
        "errors": int(number(stats.get("errors"))),
        "cpu_percent_num": number(str(timing.get("cpu_percent", "")).rstrip("%")),
        "runtime_queue_depth_peak": int(number(stats.get("runtime_queue_depth_peak"))),
        "backend_outstanding_peak": int(number(stats.get("io_backend_outstanding_peak"))),
    })
    row.update(parse_trace(path.with_suffix(".trace.tsv")))
    for key, value in stats.items():
        if str(key).startswith("data_reactor_") or str(key).startswith("reactor_") or str(key).startswith("queue_reactor_"):
            row[key] = int(number(value))
    return row


def parse_raw(out: Path) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    pattern = re.compile(r"A2 blob .*?rw=(\w+) channels=(\d+) qd=(\d+) qd_per_channel=(\d+) iops=([0-9.]+) avg_us=([0-9.]+) p50_us=([0-9.]+) p99_us=([0-9.]+) errors=(\d+)")
    for path in sorted(out.glob("raw-blob-*.log")):
        match = pattern.search(path.read_text(encoding="utf-8", errors="replace"))
        row: dict[str, object] = {"tag": path.stem, "file": path.name}
        if match:
            rw, channels, qd, qdpc, iops, avg, p50, p99, errors = match.groups()
            row.update({"rw": rw, "channels": int(channels), "qd": int(qd), "qd_per_channel": int(qdpc),
                        "iops": float(iops), "avg_us": float(avg), "p50_us": float(p50),
                        "p99_us": float(p99), "errors": int(errors)})
        else:
            row["parse_error"] = True
        rows.append(row)
    return rows


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


def medians(rows: list[dict[str, object]], keys: tuple[str, ...]) -> list[dict[str, object]]:
    groups: dict[tuple[object, ...], list[dict[str, object]]] = defaultdict(list)
    for row in rows:
        groups[tuple(row.get(key, "") for key in keys)].append(row)
    numeric = ("read_iops", "write_iops", "read_avg_us", "write_avg_us", "read_p99_us", "write_p99_us",
               "T4-T5_avg_us", "T6-T7_avg_us", "TB0-TB1_us_avg_us", "TB1-TB2_us_avg_us", "cpu_percent_num",
               "fio_error", "spdk_submit", "spdk_complete", "spdk_reject", "errors")
    output: list[dict[str, object]] = []
    for group, members in sorted(groups.items(), key=lambda item: str(item[0])):
        row: dict[str, object] = {key: value for key, value in zip(keys, group)}
        row["samples"] = len(members)
        for key in numeric:
            values = [number(member.get(key)) for member in members]
            row[key] = statistics.median(values) if values else 0.0
        output.append(row)
    return output


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} OUT", file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    fio = [fio_row(path) for path in sorted(out.glob("fixed-*.json"))]
    fio += [fio_row(path) for path in sorted(out.glob("deployment-*.json"))]
    raw = parse_raw(out)
    fixed = medians([row for row in fio if row.get("group") == "fixed"], ("data_reactors", "rw", "jobs"))
    deployment = medians([row for row in fio if row.get("group") == "deployment"], ("data_reactors", "rw", "jobs"))
    write_tsv(out / "fio_samples.tsv", fio)
    write_tsv(out / "fixed_medians.tsv", fixed)
    write_tsv(out / "deployment_medians.tsv", deployment)
    write_tsv(out / "raw_blob.tsv", raw)
    summary = {"fio_samples": fio, "fixed_medians": fixed, "deployment_medians": deployment, "raw_blob": raw}
    (out / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True), encoding="utf-8")
    print(f"summary: fio={len(fio)} raw_blob={len(raw)} out={out}")
    return 0 if fio or raw else 1


if __name__ == "__main__":
    raise SystemExit(main())
