#!/bin/sh
# Read a Blender source scene without allowing embedded script execution.

set -eu

if [ "$#" -ne 1 ]; then
	echo "usage: run_blend_runtime_audit.sh <source.blend>" >&2
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

"$blender_executable" --background "$1" --disable-autoexec \
	--python "$script_dir/audit_blend_runtime_candidates.py" -- \
	--source "$1" --output "$output_dir/runtime_candidates.json"

cat "$output_dir/runtime_candidates.json"
