#!/bin/sh
# Prepare deterministic, non-proprietary replay inputs for the later Windows pass.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
repository_root=$(git -C "$script_dir" rev-parse --show-toplevel)
windows_dir="$repository_root/misc/path_tracing/windows_certification"
output_dir="$script_dir/out/windows_certification_bundle"
mkdir -p "$output_dir"

"$script_dir/run_scene_packet_validation.sh" >/dev/null
"$script_dir/run_shader_layout_validation.sh" >/dev/null

cp "$script_dir/out/scene_packet/reference.scene_packet" "$output_dir/reference.scene_packet"
cp "$script_dir/out/shader_layout/canonical_scene_packet_layout.spv" "$output_dir/canonical_scene_packet_layout.spv"
cp "$windows_dir/expected_results.json" "$output_dir/expected_results.json"
cp "$windows_dir/probe_environment.ps1" "$output_dir/probe_environment.ps1"
cp "$windows_dir/build_editor.ps1" "$output_dir/build_editor.ps1"

revision=$(git -C "$repository_root" rev-parse HEAD)
dirty=false
if [ -n "$(git -C "$repository_root" status --porcelain)" ]; then
	dirty=true
fi
packet_sha256=$(shasum -a 256 "$output_dir/reference.scene_packet" | awk '{print $1}')
spirv_sha256=$(shasum -a 256 "$output_dir/canonical_scene_packet_layout.spv" | awk '{print $1}')
{
	echo '{'
	printf '  "schema": 1,\n'
	printf '  "repository_revision": "%s",\n' "$revision"
	printf '  "working_tree_dirty_at_preparation": %s,\n' "$dirty"
	printf '  "required_scons": "4.10.1",\n'
	printf '  "reference_scene_packet_sha256": "%s",\n' "$packet_sha256"
	printf '  "canonical_layout_spirv_sha256": "%s",\n' "$spirv_sha256"
	printf '  "proprietary_dependencies_included": false\n'
	echo '}'
} > "$output_dir/bundle_manifest.json"

tar -czf "$script_dir/out/windows_certification_bundle.tar.gz" -C "$output_dir" .
cat "$output_dir/bundle_manifest.json"
