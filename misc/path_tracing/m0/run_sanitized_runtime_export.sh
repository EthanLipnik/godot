#!/bin/sh
# Export the selected mesh twice and validate a deterministic sanitized GLB.

set -eu

if [ "$#" -ne 2 ]; then
	echo "usage: run_sanitized_runtime_export.sh <source.blend> <mesh-object-name>" >&2
	exit 1
fi

script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
output_dir="$script_dir/out/asset_export"
mkdir -p "$output_dir"

blender_executable=${BLENDER_EXECUTABLE:-/Applications/Blender.app/Contents/MacOS/Blender}
if [ ! -x "$blender_executable" ]; then
	echo "Blender executable not found: $blender_executable" >&2
	exit 1
fi

run_export() {
	output_path=$1
	manifest_path=$2
	log_path=$5
	if ! "$blender_executable" --background "$3" --disable-autoexec \
		--python "$script_dir/export_sanitized_runtime_mesh.py" -- \
		--source "$3" --object-name "$4" --output "$output_path" \
		--manifest "$manifest_path" --max-influences 8 --morph-limit 8 > "$log_path" 2>&1; then
		cat "$log_path" >&2
		return 1
	fi
}

run_export "$output_dir/runtime_mesh_first.glb" "$output_dir/runtime_mesh_manifest.json" "$1" "$2" "$output_dir/export_first.log"
run_export "$output_dir/runtime_mesh_second.glb" "$output_dir/runtime_mesh_manifest_second.json" "$1" "$2" "$output_dir/export_second.log"

python3 "$script_dir/validate_runtime_glb.py" \
	--first "$output_dir/runtime_mesh_first.glb" \
	--second "$output_dir/runtime_mesh_second.glb" \
	--manifest "$output_dir/runtime_mesh_manifest.json" \
	--output "$script_dir/out/sanitized_runtime_export.json"

cat "$script_dir/out/sanitized_runtime_export.json"
