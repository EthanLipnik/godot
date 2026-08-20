#!/bin/sh
# Prepare deterministic, non-proprietary replay inputs for the later Windows pass.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
repository_root=$(git -C "$script_dir" rev-parse --show-toplevel)
windows_dir="$repository_root/misc/path_tracing/windows_certification"
mkdir -p "$script_dir/out"
output_dir=$(mktemp -d "$script_dir/out/windows_certification_bundle.XXXXXX")
trap 'rm -rf "$output_dir"' EXIT HUP INT TERM

"$script_dir/run_scene_packet_validation.sh" >/dev/null
"$script_dir/run_shader_layout_validation.sh" >/dev/null
"$repository_root/misc/path_tracing/m1/run_scalar_closure_compile_gate.sh" >/dev/null

cp "$script_dir/out/scene_packet/reference.scene_packet" "$output_dir/reference.scene_packet"
cp "$script_dir/out/scene_packet/reference.scene_capture.ptc" "$output_dir/reference.scene_capture.ptc"
cp "$script_dir/out/shader_layout/canonical_scene_packet_layout.spv" "$output_dir/canonical_scene_packet_layout.spv"
cp "$repository_root/misc/path_tracing/m1/out/scalar_closure/scalar_closure.spv" "$output_dir/scalar_closure.spv"
cp "$windows_dir/expected_results.json" "$output_dir/expected_results.json"
cp "$windows_dir/probe_environment.ps1" "$output_dir/probe_environment.ps1"
cp "$windows_dir/build_editor.ps1" "$output_dir/build_editor.ps1"
cp "$windows_dir/run_renderer_certification.ps1" "$output_dir/run_renderer_certification.ps1"
cp "$windows_dir/validate_bundle.py" "$output_dir/validate_bundle.py"
cp "$windows_dir/validate_renderer_report.py" "$output_dir/validate_renderer_report.py"
cp "$windows_dir/README.md" "$output_dir/README.md"
cp "$repository_root/misc/path_tracing/m2/companion/Schemas/endpoint-message.schema.json" "$output_dir/endpoint-message.schema.json"
cp "$repository_root/misc/path_tracing/m2/companion/Schemas/stream-telemetry.schema.json" "$output_dir/stream-telemetry.schema.json"

revision=$(git -C "$repository_root" rev-parse HEAD)
dirty=false
if [ -n "$(git -C "$repository_root" status --porcelain)" ]; then
	dirty=true
fi
if [ "$dirty" = true ] && [ "${PATH_TRACING_ALLOW_DIRTY_BUNDLE:-0}" != 1 ]; then
	echo "refusing to prepare a Windows certification bundle from a dirty worktree" >&2
	exit 2
fi
packet_sha256=$(shasum -a 256 "$output_dir/reference.scene_packet" | awk '{print $1}')
capture_sha256=$(shasum -a 256 "$output_dir/reference.scene_capture.ptc" | awk '{print $1}')
spirv_sha256=$(shasum -a 256 "$output_dir/canonical_scene_packet_layout.spv" | awk '{print $1}')
closure_sha256=$(shasum -a 256 "$output_dir/scalar_closure.spv" | awk '{print $1}')
expected_sha256=$(shasum -a 256 "$output_dir/expected_results.json" | awk '{print $1}')
endpoint_schema_sha256=$(shasum -a 256 "$output_dir/endpoint-message.schema.json" | awk '{print $1}')
telemetry_schema_sha256=$(shasum -a 256 "$output_dir/stream-telemetry.schema.json" | awk '{print $1}')
probe_sha256=$(shasum -a 256 "$output_dir/probe_environment.ps1" | awk '{print $1}')
build_sha256=$(shasum -a 256 "$output_dir/build_editor.ps1" | awk '{print $1}')
run_sha256=$(shasum -a 256 "$output_dir/run_renderer_certification.ps1" | awk '{print $1}')
bundle_validator_sha256=$(shasum -a 256 "$output_dir/validate_bundle.py" | awk '{print $1}')
report_validator_sha256=$(shasum -a 256 "$output_dir/validate_renderer_report.py" | awk '{print $1}')
readme_sha256=$(shasum -a 256 "$output_dir/README.md" | awk '{print $1}')
{
	echo '{'
	printf '  "schema": 2,\n'
	printf '  "repository_revision": "%s",\n' "$revision"
	printf '  "working_tree_dirty_at_preparation": %s,\n' "$dirty"
	printf '  "required_scons": "4.10.1",\n'
	printf '  "artifacts": {\n'
	printf '    "reference.scene_packet": "%s",\n' "$packet_sha256"
	printf '    "reference.scene_capture.ptc": "%s",\n' "$capture_sha256"
	printf '    "canonical_scene_packet_layout.spv": "%s",\n' "$spirv_sha256"
	printf '    "scalar_closure.spv": "%s",\n' "$closure_sha256"
	printf '    "expected_results.json": "%s",\n' "$expected_sha256"
	printf '    "endpoint-message.schema.json": "%s",\n' "$endpoint_schema_sha256"
	printf '    "stream-telemetry.schema.json": "%s",\n' "$telemetry_schema_sha256"
	printf '    "probe_environment.ps1": "%s",\n' "$probe_sha256"
	printf '    "build_editor.ps1": "%s",\n' "$build_sha256"
	printf '    "run_renderer_certification.ps1": "%s",\n' "$run_sha256"
	printf '    "validate_bundle.py": "%s",\n' "$bundle_validator_sha256"
	printf '    "validate_renderer_report.py": "%s",\n' "$report_validator_sha256"
	printf '    "README.md": "%s"\n' "$readme_sha256"
	printf '  },\n'
	printf '  "proprietary_dependencies_included": false\n'
	echo '}'
} > "$output_dir/bundle_manifest.json"

python3 "$output_dir/validate_bundle.py" "$output_dir" >/dev/null
tar -czf "$script_dir/out/windows_certification_bundle.tar.gz" -C "$output_dir" .
cat "$output_dir/bundle_manifest.json"
