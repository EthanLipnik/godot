#!/usr/bin/env python3

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parent
VALIDATOR = ROOT / "validate_renderer_report.py"
STAGES = {
    "deformation": 0.1,
    "blas_build_or_refit": 0.2,
    "tlas_update": 0.1,
    "trace": 1.0,
    "guide_generation": 0.2,
    "reconstruction": 0.5,
    "composition": 0.1,
}


def execution() -> dict:
    return {
        "rendered_views": 2,
        "distinct_stereo_views": True,
        "dynamic_refit_correct": True,
        "guides_valid": True,
        "gpu_stage_ms": dict(STAGES),
    }


class RendererReportValidatorTests(unittest.TestCase):
    def validate(self, report: dict) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "report.json"
            path.write_text(json.dumps(report), encoding="utf-8")
            return subprocess.run(
                [sys.executable, str(VALIDATOR), str(path)],
                check=False,
                capture_output=True,
                text=True,
            )

    def test_accepts_both_measured_paths(self) -> None:
        report = {
            "schema": 1,
            "passed": True,
            "selected_path": "ray_query",
            "executions": {
                "ray_query": execution(),
                "rt_pipeline": execution(),
            },
        }
        self.assertEqual(self.validate(report).returncode, 0)

    def test_rejects_missing_rt_pipeline(self) -> None:
        report = {
            "schema": 1,
            "passed": True,
            "selected_path": "ray_query",
            "executions": {"ray_query": execution()},
        }
        self.assertNotEqual(self.validate(report).returncode, 0)

    def test_rejects_monoscopic_copy(self) -> None:
        copied = execution()
        copied["distinct_stereo_views"] = False
        report = {
            "schema": 1,
            "passed": True,
            "selected_path": "rt_pipeline",
            "executions": {
                "ray_query": execution(),
                "rt_pipeline": copied,
            },
        }
        self.assertNotEqual(self.validate(report).returncode, 0)


if __name__ == "__main__":
    unittest.main()
