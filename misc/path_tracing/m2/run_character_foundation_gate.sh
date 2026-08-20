#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
	echo "usage: run_character_foundation_gate.sh <source.blend>" >&2
	exit 1
fi

script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
output_dir="$script_dir/out/character"
profile="$script_dir/external_character_profile.json"
source_file=$1
blender=${BLENDER_EXECUTABLE:-/Applications/Blender.app/Contents/MacOS/Blender}
mkdir -p "$output_dir"

object_name=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["source_object"])' "$profile")
run_export() {
	output=$1
	manifest=$2
	log=$3
	"$blender" --background "$source_file" --disable-autoexec --python "$script_dir/../m0/export_sanitized_runtime_mesh.py" -- \
		--source "$source_file" --object-name "$object_name" --output "$output" --manifest "$manifest" \
		--max-influences 8 --morph-limit 35 > "$log" 2>&1
}

run_export "$output_dir/runtime_character_first.glb" "$output_dir/export_manifest.json" "$output_dir/export_first.log"
run_export "$output_dir/runtime_character_second.glb" "$output_dir/export_manifest_second.json" "$output_dir/export_second.log"
"$blender" --background "$source_file" --disable-autoexec --python "$script_dir/inspect_character_source.py" -- \
	--source "$source_file" --profile "$profile" --output "$output_dir/source_evidence.json" > "$output_dir/inspect.log" 2>&1

python3 "$script_dir/validate_character_foundation.py" \
	--first "$output_dir/runtime_character_first.glb" --second "$output_dir/runtime_character_second.glb" \
	--export-manifest "$output_dir/export_manifest.json" --source-evidence "$output_dir/source_evidence.json" \
	--output "$output_dir/result.json"

texture_python=${M2_PYTHON:-python3}
"$texture_python" "$script_dir/convert_texture_manifest.py" \
	--source-evidence "$output_dir/source_evidence.json" --output-directory "$output_dir/textures" \
	--manifest "$output_dir/texture_manifest.json"
