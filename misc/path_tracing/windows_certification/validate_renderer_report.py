#!/usr/bin/env python3

import json
import math
import pathlib
import sys


REQUIRED_PATHS = {"ray_query", "rt_pipeline"}
REQUIRED_STAGES = {
    "deformation",
    "blas_build_or_refit",
    "tlas_update",
    "trace",
    "guide_generation",
    "reconstruction",
    "composition",
}


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: validate_renderer_report.py <renderer-report.json>", file=sys.stderr)
        return 2
    report = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8-sig"))
    if report.get("schema") != 1 or report.get("passed") is not True:
        raise ValueError("renderer replay did not report a passing schema-1 result")
    executions = report.get("executions")
    if not isinstance(executions, dict) or set(executions) != REQUIRED_PATHS:
        raise ValueError("both ray_query and rt_pipeline executions are required")
    for path_name, execution in executions.items():
        if execution.get("rendered_views") != 2 or execution.get("distinct_stereo_views") is not True:
            raise ValueError(f"{path_name} did not prove two distinct stereo views")
        if execution.get("dynamic_refit_correct") is not True or execution.get("guides_valid") is not True:
            raise ValueError(f"{path_name} failed dynamic geometry or guide correctness")
        timings = execution.get("gpu_stage_ms")
        if not isinstance(timings, dict) or set(timings) != REQUIRED_STAGES:
            raise ValueError(f"{path_name} does not contain the exact required GPU stage timings")
        for stage, value in timings.items():
            if not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
                raise ValueError(f"{path_name}.{stage} has an invalid GPU timing")
    if report.get("selected_path") not in REQUIRED_PATHS:
        raise ValueError("selected_path must name a measured execution path")
    print(json.dumps({"schema": 1, "renderer_replay_valid": True, "passed": True}, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"renderer report validation failed: {error}", file=sys.stderr)
        raise SystemExit(1)
