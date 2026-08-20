#!/bin/sh
# Build and run the native Metal ray-tracing spike.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
output_dir="$script_dir/out/metal_rt_triangle"
mkdir -p "$output_dir"

xcrun -sdk macosx metal -std=metal3.2 -c "$script_dir/trace_triangle.metal" -o "$output_dir/trace_triangle.air"
xcrun -sdk macosx metallib "$output_dir/trace_triangle.air" -o "$output_dir/trace_triangle.metallib"
xcrun -sdk macosx clang++ -std=c++17 -fobjc-arc \
	-framework Foundation -framework Metal \
	"$script_dir/metal_rt_triangle.mm" -o "$output_dir/metal_rt_triangle"

"$output_dir/metal_rt_triangle" "$output_dir/trace_triangle.metallib" | tee "$script_dir/out/metal_rt_triangle.json"
