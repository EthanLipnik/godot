#!/bin/sh
# Build and run the animated MetalFX temporal/stereo acceptance harness.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
output_dir="$script_dir/out/metalfx_temporal_stereo"
mkdir -p "$output_dir"

xcrun -sdk macosx clang++ -std=c++17 -Wall -Wextra -Werror -fobjc-arc \
	-framework Foundation -framework Metal -framework MetalFX \
	"$script_dir/metalfx_temporal_stereo_quality.mm" -o "$output_dir/metalfx_temporal_stereo_quality"

"$output_dir/metalfx_temporal_stereo_quality" "$output_dir" > "$script_dir/out/metalfx_temporal_stereo_quality.json"
cat "$script_dir/out/metalfx_temporal_stereo_quality.json"
