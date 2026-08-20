#!/bin/sh
# Build and run the deterministic two-bounce Metal validation corpus.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
output_dir="$script_dir/out/two_bounce"
mkdir -p "$output_dir"

xcrun -sdk macosx metal -std=metal3.2 -c "$script_dir/two_bounce_path_tracer.metal" -o "$output_dir/two_bounce_path_tracer.air"
xcrun -sdk macosx metallib "$output_dir/two_bounce_path_tracer.air" -o "$output_dir/two_bounce_path_tracer.metallib"
xcrun -sdk macosx clang++ -std=c++17 -Wall -Wextra -Werror -fobjc-arc \
	-framework Foundation -framework Metal \
	"$script_dir/two_bounce_path_tracer.mm" -o "$output_dir/two_bounce_path_tracer"

"$output_dir/two_bounce_path_tracer" "$output_dir/two_bounce_path_tracer.metallib" "$output_dir" \
	> "$script_dir/out/two_bounce_path_tracer.json"
cat "$script_dir/out/two_bounce_path_tracer.json"
