"""Synthetic metric and schema checks for the standalone CSV analyzer."""
import csv
from pathlib import Path
import tempfile
import unittest

from paths import ROOT
from analyze_presentmon import analyze, duration_stats


class AnalyzerTests(unittest.TestCase):
    def write_csv(self, directory, fields, rows):
        path = Path(directory) / "capture.csv"
        with path.open("w", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)
        return path

    def test_mean_and_low_are_different_metrics(self):
        result = duration_stats([16] * 98 + [32, 64], include_fps=True)
        self.assertEqual(result["p99_ms"], 32)
        self.assertEqual(result["p99_inverse_fps"], 31.25)
        self.assertEqual(result["slowest_1pct_mean_inverse_fps"], 15.625)
        self.assertGreater(result["mean_fps"], 60)

    def test_dominant_swapchain_warmup_and_unavailable_display(self):
        fields = ["Application", "ProcessID", "SwapChainAddress", "CPUStartTime",
                  "FrameTime", "DisplayedTime", "GPUTime", "GPUWait"]
        rows = [dict(zip(fields, ["bb-probe.exe", 1, "0xa", 100 + n, 16, 16, 12, 0]))
                for n in range(5)]
        rows[3]["DisplayedTime"] = "NA"
        rows.append(dict(zip(fields, ["bb-probe.exe", 1, "0xb", 0, 200, 200, 100, 50])))
        with tempfile.TemporaryDirectory() as directory:
            result = analyze(self.write_csv(directory, fields, rows), after=2, duration=2)
        self.assertEqual(result["selected_swapchain"], "0xa")
        self.assertEqual(result["rows_analyzed"], 2)
        self.assertEqual(result["cpu_intervals"]["mean_fps"], 62.5)
        self.assertEqual(result["displayed_rows"], 1)
        self.assertEqual(result["undisplayed_or_unavailable_rows"], 1)
        self.assertEqual(result["metrics"]["gpu_wait"]["mean_ms"], 0)

    def test_absolute_qpc_milliseconds_and_default_schema(self):
        fields = ["Application", "ProcessID", "SwapChainAddress", "CPUStartQPCTimeInMs",
                  "MsCPUBusy", "MsCPUWait", "MsGPUTime"]
        rows = [dict(zip(fields, ["bb-probe.exe", 2, "0xc", 123000 + n * 1000, 10, 6, 9]))
                for n in range(3)]
        with tempfile.TemporaryDirectory() as directory:
            result = analyze(self.write_csv(directory, fields, rows), after=1)
        self.assertEqual(result["rows_analyzed"], 2)
        self.assertEqual(result["cpu_intervals"]["mean_ms"], 16)
        self.assertEqual(result["observed_timestamp_span_seconds"], 1)

    def test_raw_qpc_requires_frequency_for_warmup(self):
        fields = ["Application", "CPUStartQPC", "CPUFrameTime"]
        rows = [dict(zip(fields, ["bb-probe.exe", 1000, 16]))]
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_csv(directory, fields, rows)
            with self.assertRaisesRegex(ValueError, "qpc-frequency"):
                analyze(path, after=1)

    def test_nonfinite_or_invalid_durations_are_excluded(self):
        result = duration_stats([None, -1, 0, float("nan"), float("inf"), 17], include_fps=True)
        self.assertEqual(result["samples"], 1)
        self.assertEqual(result["min_ms"], 17)

    def test_native_filter_recomputes_new_game_and_return_intervals(self):
        fields = ["session_elapsed_ms", "present_start_monotonic_ms", "interval_ms",
                  "is_game_frame", "is_reusing_frame", "acquire_ms", "flush_ms",
                  "submit_mutex_wait_ms", "present_call_ms"]
        rows = [dict(zip(fields, values)) for values in [
            [0, 999000, 0, 1, 0, 1, 2, 0, 1],
            [1000, 1000000, 1000, 1, 0, 1, 2, 0, 1],
            [1010, 1000010, 10, 1, 1, 1, 2, 0, 2],
            [1020, 1000020, 10, 0, 0, 1, 2, 0, 3],
            [1030, 1000030, 10, 1, 0, 1, 2, 0, 4],
        ]]
        with tempfile.TemporaryDirectory() as directory:
            result = analyze(self.write_csv(directory, fields, rows), after=1)
        self.assertEqual(result["rows_analyzed"], 4)
        self.assertEqual(result["all_presents"]["present_start_intervals"]["mean_ms"], 10)
        self.assertEqual(result["all_presents"]["present_return_intervals"]["mean_ms"], 11)
        self.assertEqual(result["new_game_presents"]["frame_rows"], 2)
        self.assertEqual(result["new_game_presents"]["present_start_intervals"]["mean_ms"], 30)
        self.assertEqual(result["new_game_presents"]["present_return_intervals"]["mean_ms"], 33)
        self.assertEqual(result["reused_rows"], 1)
        self.assertEqual(result["blank_rows"], 1)


if __name__ == "__main__":
    unittest.main()
