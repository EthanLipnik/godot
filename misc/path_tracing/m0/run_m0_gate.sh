#!/bin/sh
# Run the complete portable-contract and Apple-reference M0 gate.

set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: run_m0_gate.sh <source.blend> <runtime-mesh-object-name>" >&2
	exit 1
fi

script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
repository_root=$(git -C "$script_dir" rev-parse --show-toplevel)
output_dir="$script_dir/out"

cd "$repository_root"
"$script_dir/capture_host_baseline.sh" "$output_dir/host_baseline.txt" >/dev/null
"$script_dir/build_macos_editor.sh"
"$script_dir/run_scene_packet_validation.sh"
"$script_dir/run_shader_layout_validation.sh"
"$script_dir/run_metal_rt_triangle.sh"
"$script_dir/run_two_bounce_path_tracer.sh"
"$script_dir/run_metalfx_guide_contract.sh"
"$script_dir/run_metalfx_temporal_stereo_quality.sh"
"$script_dir/run_slang_feasibility.sh"
"$script_dir/run_blend_runtime_audit.sh" "$1" > "$output_dir/asset_export/audit_gate.log"
"$script_dir/run_sanitized_runtime_export.sh" "$1" "$2"
"$script_dir/prepare_windows_certification_bundle.sh"

for report in \
	"$output_dir/scene_packet_validation.json" \
	"$output_dir/metal_layout_validation.json" \
	"$output_dir/glsl_layout_validation.json" \
	"$output_dir/metal_rt_triangle.json" \
	"$output_dir/two_bounce_path_tracer.json" \
	"$output_dir/metalfx_guide_contract.json" \
	"$output_dir/metalfx_temporal_stereo_quality.json" \
	"$output_dir/slang_feasibility.json" \
	"$output_dir/sanitized_runtime_export.json"; do
	if ! jq -e '.passed == true' "$report" >/dev/null; then
		echo "M0 report failed: $report" >&2
		exit 2
	fi
done

if [ ! -x "$repository_root/bin/godot.macos.editor.dev.arm64" ]; then
	echo "M0 macOS editor output is missing" >&2
	exit 2
fi

revision=$(git rev-parse HEAD)
dirty=false
if [ -n "$(git status --porcelain)" ]; then
	dirty=true
fi
{
	echo '{'
	printf '  "milestone": "M0",\n'
	printf '  "scope": "portable_contract_and_apple_reference",\n'
	printf '  "repository_revision_before_milestone_commit": "%s",\n' "$revision"
	printf '  "working_tree_dirty_before_milestone_commit": %s,\n' "$dirty"
	printf '  "windows_runtime_certified": false,\n'
	printf '  "renderer_parity_certified": false,\n'
	printf '  "passed": true\n'
	echo '}'
} > "$output_dir/m0_gate.json"

cat "$output_dir/m0_gate.json"
