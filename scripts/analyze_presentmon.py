"""Summarize PresentMon CSV data without equating its lows to NVIDIA's overlay.

All duration columns are milliseconds. PresentMon v2 CPUStartTime defaults to
seconds; use --time-unit milliseconds if the capture changed that option.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import csv
import json
import math
from pathlib import Path
import statistics
import sys


def number(value):
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    return result if math.isfinite(result) else None


def first_number(row, names):
    for name in names:
        value = number(row.get(name))
        if value is not None:
            return value
    return None


def quantile(sorted_values, fraction):
    """Nearest-rank quantile: reproducible for short captures too."""
    return sorted_values[max(0, math.ceil(len(sorted_values) * fraction) - 1)]


def duration_stats(values, include_fps=False, allow_zero=False):
    ordered = sorted(value for value in values if value is not None and math.isfinite(value)
                     and (value >= 0 if allow_zero else value > 0))
    if not ordered:
        return {"samples": 0}
    mean = statistics.fmean(ordered)
    worst_count = max(1, math.ceil(len(ordered) * 0.01))
    worst_mean = statistics.fmean(ordered[-worst_count:])
    p99 = quantile(ordered, 0.99)
    result = {
        "samples": len(ordered), "mean_ms": mean, "min_ms": ordered[0],
        "median_ms": statistics.median(ordered),
        "stddev_ms": statistics.pstdev(ordered),
        "p95_ms": quantile(ordered, 0.95), "p99_ms": p99,
        "p99_9_ms": quantile(ordered, 0.999), "max_ms": ordered[-1],
        "slowest_1pct_samples": worst_count,
        "slowest_1pct_mean_ms": worst_mean,
        "over_25ms": sum(value > 25 for value in ordered),
        "over_33_333ms": sum(value > 1000 / 30 for value in ordered),
        "over_50ms": sum(value > 50 for value in ordered),
    }
    if include_fps:
        result.update(
            mean_fps=1000 / mean, p99_inverse_fps=1000 / p99,
            slowest_1pct_mean_inverse_fps=1000 / worst_mean,
        )
    return result


def timestamp_seconds(row, time_unit="auto", qpc_frequency=None):
    for name, scale in (
        ("CPUStartTimeInSeconds", 1), ("TimeInSeconds", 1),
        ("CPUStartTimeInMs", 0.001), ("CPUStartQPCTimeInMs", 0.001),
        ("CPUStartQPCTime", 0.001),
        ("CPUStartTime", 0.001 if time_unit == "milliseconds" else 1),
    ):
        value = number(row.get(name))
        if value is not None:
            return value * scale
    if qpc_frequency:
        value = number(row.get("CPUStartQPC"))
        if value is not None:
            return value / qpc_frequency
    return None


def cpu_duration(row):
    value = first_number(row, ("CPUFrameTime", "FrameTime", "MsBetweenAppStart",
                               "MsBetweenPresents", "msBetweenPresents"))
    if value is not None:
        return value
    busy = first_number(row, ("CPUBusy", "MsCPUBusy"))
    wait = first_number(row, ("CPUWait", "MsCPUWait"))
    return busy + wait if busy is not None and wait is not None else None


def load_csv(path):
    with Path(path).open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        if not reader.fieldnames:
            raise ValueError("CSV has no header")
        columns = [name.strip() for name in reader.fieldnames]
        rows = []
        for row in reader:
            if None in row:
                raise ValueError("CSV row has more fields than the header")
            rows.append({name.strip(): (value or "").strip() for name, value in row.items()})
    return columns, rows


def analyze_native(path, columns, rows, after=0, duration=None):
    """Native timestamps measure host API pacing; they do not measure scanout."""
    retained = []
    for row in rows:
        elapsed = number(row.get("session_elapsed_ms"))
        started = number(row.get("present_start_monotonic_ms"))
        if elapsed is None or started is None:
            continue
        seconds = elapsed / 1000
        if seconds >= after and (duration is None or seconds < after + duration):
            retained.append(row)
    if not retained:
        raise ValueError("No native frames remain after warmup/time filtering")
    retained.sort(key=lambda row: number(row["present_start_monotonic_ms"]))
    game_rows = [row for row in retained if number(row.get("is_game_frame")) == 1
                 and number(row.get("is_reusing_frame")) == 0]

    def pacing(selected):
        starts = [number(row["present_start_monotonic_ms"]) for row in selected]
        returns = [number(row["present_start_monotonic_ms"]) + number(row["present_call_ms"])
                   for row in selected if number(row.get("present_call_ms")) is not None]
        return {
            "frame_rows": len(selected),
            "present_start_intervals": duration_stats([b - a for a, b in zip(starts, starts[1:])],
                                                       include_fps=True),
            "present_return_intervals": duration_stats([b - a for a, b in zip(returns, returns[1:])],
                                                        include_fps=True),
            "timings": {name: duration_stats([number(row.get(name)) for row in selected], allow_zero=True)
                        for name in ("acquire_ms", "flush_ms", "submit_mutex_wait_ms", "present_call_ms")
                        if name in columns},
        }

    return {
        "file": str(Path(path).resolve()), "schema": "native_present_api_timings",
        "columns": columns, "input_rows": len(rows), "rows_analyzed": len(retained),
        "warmup_seconds": after, "requested_duration_seconds": duration,
        "first_elapsed_seconds": number(retained[0]["session_elapsed_ms"]) / 1000,
        "last_elapsed_seconds": number(retained[-1]["session_elapsed_ms"]) / 1000,
        "all_presents": pacing(retained), "new_game_presents": pacing(game_rows),
        "reused_rows": sum(number(row.get("is_reusing_frame")) == 1 for row in retained),
        "blank_rows": sum(number(row.get("is_game_frame")) == 0 for row in retained),
        "notes": [
            "--after uses session_elapsed_ms measured from native CSV open, not an absolute clock.",
            "Intervals are recomputed within each filtered stream; the boundary interval is excluded.",
            "New game presents require is_game_frame=1 and is_reusing_frame=0.",
            "Present return = present_start_monotonic_ms + present_call_ms; this is host API return, "
            "not GPU completion, actual display delivery, or scanout.",
            "p99 uses nearest rank; slowest 1% averages ceil(N * 0.01) longest intervals before inversion.",
            "NVIDIA overlay's exact 1% Low formula/window is unknown; these are independent statistics.",
            "Native data does not establish whether a frame was dropped by the display system.",
        ],
    }


def analyze(path, process="bb-probe.exe", pid=None, swapchain=None, after=0,
            duration=None, time_unit="auto", qpc_frequency=None):
    columns, all_rows = load_csv(path)
    if "session_elapsed_ms" in columns and "present_start_monotonic_ms" in columns:
        return analyze_native(path, columns, all_rows, after, duration)
    groups = defaultdict(list)
    for row in all_rows:
        application = row.get("Application", "")
        basename = application.replace("\\", "/").rsplit("/", 1)[-1]
        if process and basename.casefold() != process.casefold():
            continue
        if pid is not None and row.get("ProcessID") != str(pid):
            continue
        chain = row.get("SwapChainAddress", "")
        if swapchain and chain.casefold() != swapchain.casefold():
            continue
        groups[(application, row.get("ProcessID", ""), chain)].append(row)
    if not groups:
        raise ValueError("No rows match the selected process/PID/swapchain")
    key, selected = max(groups.items(), key=lambda item: len(item[1]))
    timed_rows = [(row, timestamp_seconds(row, time_unit, qpc_frequency)) for row in selected]
    valid_times = [time for _, time in timed_rows if time is not None]
    if (after or duration is not None) and not valid_times:
        raise ValueError("Warmup filtering needs CPUStartTime, a time column, or --qpc-frequency")
    start = min(valid_times) if valid_times else None
    retained = [(row, time) for row, time in timed_rows if start is None or (
        time is not None and time - start >= after
        and (duration is None or time - start < after + duration))]
    if not retained:
        raise ValueError("No frames remain after warmup/time filtering")
    rows = [row for row, _ in retained]
    retained_times = [time for _, time in retained if time is not None]
    cpu = [cpu_duration(row) for row in rows]
    cpu_source = next((name for name in ("CPUFrameTime", "FrameTime", "MsBetweenAppStart",
                                        "MsBetweenPresents", "msBetweenPresents")
                       if name in columns), "CPUBusy + CPUWait")
    if not any(value is not None and value > 0 for value in cpu) and retained_times:
        ordered_times = sorted(retained_times)
        cpu = [(b - a) * 1000 for a, b in zip(ordered_times, ordered_times[1:])]
        cpu_source = "CPUStartTime differences"
    display_name = next((name for name in ("DisplayedTime", "MsBetweenDisplayChange",
                                           "msBetweenDisplayChange") if name in columns), None)
    displayed = [number(row.get(display_name)) for row in rows] if display_name else []
    explicit_dropped = sum(row.get("Dropped", "").casefold() in ("1", "true", "yes")
                           for row in rows)
    metrics = {}
    for name, aliases in (
        ("gpu_time", ("GPUTime", "MsGPUTime")),
        ("gpu_busy", ("GPUBusy", "MsGPUBusy", "msGPUActive")),
        ("gpu_wait", ("GPUWait", "MsGPUWait")),
        ("cpu_busy", ("CPUBusy", "MsCPUBusy")),
        ("cpu_wait", ("CPUWait", "MsCPUWait")),
        ("display_latency", ("DisplayLatency", "MsDisplayLatency")),
    ):
        if any(alias in columns for alias in aliases):
            metrics[name] = duration_stats([first_number(row, aliases) for row in rows], allow_zero=True)
    result = {
        "file": str(Path(path).resolve()), "columns": columns,
        "input_rows": len(all_rows), "matching_swapchains": len(groups),
        "selected_application": key[0], "selected_pid": key[1], "selected_swapchain": key[2],
        "selected_rows_before_filter": len(selected), "rows_analyzed": len(rows),
        "warmup_seconds": after, "requested_duration_seconds": duration,
        "observed_timestamp_span_seconds": max(retained_times) - min(retained_times)
            if retained_times else None,
        "cpu_interval_source": cpu_source,
        "cpu_intervals": duration_stats(cpu, include_fps=True),
        "present_modes": dict(Counter(row.get("PresentMode", "unknown") for row in rows)),
        "explicit_dropped_rows": explicit_dropped, "metrics": metrics,
        "notes": [
            "p99 uses nearest-rank frame-duration quantiles; mean FPS is 1000 / mean duration.",
            "The slowest 1% metric averages ceil(N * 0.01) longest durations, then inverts.",
            "NVIDIA overlay's exact 1% Low formula/window is unknown; these are independent statistics.",
            "CPU frame intervals describe app work starts, not confirmed screen delivery.",
            "Vulkan/Other CPU and GPU metrics have PresentMon instrumentation limitations.",
        ],
    }
    if display_name:
        result.update(
            display_duration_source=display_name,
            display_durations=duration_stats(displayed, include_fps=True),
            displayed_rows=sum(value is not None and value > 0 for value in displayed),
            undisplayed_or_unavailable_rows=sum(value is None or value <= 0 for value in displayed),
        )
        result["notes"].append(
            "DisplayedTime is time visible in milliseconds, not a timestamp. NA means not displayed "
            "or unavailable; capture boundaries may lack complete display tracking."
        )
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__ + " Native present timing CSV is also supported.")
    parser.add_argument("csv", type=Path)
    parser.add_argument("--process", default="bb-probe.exe", help="Empty string selects any process")
    parser.add_argument("--pid", type=int)
    parser.add_argument("--swapchain")
    parser.add_argument("--after", type=float, default=0, help="Seconds after first selected sample")
    parser.add_argument("--duration", type=float, help="Seconds to analyze after warmup")
    parser.add_argument("--time-unit", choices=("auto", "seconds", "milliseconds"), default="auto")
    parser.add_argument("--qpc-frequency", type=float, help="Required for raw CPUStartQPC warmup filtering")
    args = parser.parse_args()
    if args.after < 0 or (args.duration is not None and args.duration <= 0):
        parser.error("--after must be nonnegative and --duration must be positive")
    if args.qpc_frequency is not None and args.qpc_frequency <= 0:
        parser.error("--qpc-frequency must be positive")
    try:
        result = analyze(args.csv, args.process, args.pid, args.swapchain, args.after,
                         args.duration, args.time_unit, args.qpc_frequency)
    except (OSError, ValueError) as error:
        print(f"Analysis failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
