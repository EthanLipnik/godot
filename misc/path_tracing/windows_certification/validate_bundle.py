#!/usr/bin/env python3

import hashlib
import json
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: validate_bundle.py <bundle-directory>", file=sys.stderr)
        return 2
    root = pathlib.Path(sys.argv[1]).resolve()
    manifest_path = root / "bundle_manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("schema") != 2:
        raise ValueError("bundle manifest schema 2 is required")
    if manifest.get("proprietary_dependencies_included") is not False:
        raise ValueError("certification bundle must not contain proprietary dependencies")
    artifacts = manifest.get("artifacts")
    if not isinstance(artifacts, dict) or not artifacts:
        raise ValueError("bundle manifest has no artifacts")
    for relative, expected in artifacts.items():
        if pathlib.PurePath(relative).is_absolute() or ".." in pathlib.PurePath(relative).parts:
            raise ValueError(f"unsafe artifact path: {relative}")
        artifact = root / relative
        actual = hashlib.sha256(artifact.read_bytes()).hexdigest()
        if actual != expected:
            raise ValueError(f"SHA-256 mismatch for {relative}: {actual} != {expected}")
    actual_files = {
        path.relative_to(root).as_posix()
        for path in root.rglob("*")
        if path.is_file()
    }
    expected_files = set(artifacts) | {"bundle_manifest.json"}
    if actual_files != expected_files:
        raise ValueError(
            f"bundle file set differs from manifest: extra={sorted(actual_files - expected_files)}, "
            f"missing={sorted(expected_files - actual_files)}"
        )
    expected_results = json.loads((root / "expected_results.json").read_text(encoding="utf-8"))
    if expected_results.get("schema") != 2:
        raise ValueError("expected-results schema 2 is required")
    report = {
        "schema": 1,
        "artifact_count": len(artifacts),
        "repository_revision": manifest.get("repository_revision"),
        "passed": True,
    }
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"bundle validation failed: {error}", file=sys.stderr)
        raise SystemExit(1)
