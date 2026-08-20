#!/bin/sh
# Build and validate the candidate cross-backend scene packet.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
output_dir="$script_dir/out/scene_packet"
mkdir -p "$output_dir"

xcrun -sdk macosx clang++ -std=c++17 -Wall -Wextra -Werror \
	"$script_dir/validate_scene_packet.cpp" -o "$output_dir/validate_scene_packet"

"$output_dir/validate_scene_packet" "$output_dir/reference.scene_packet" > "$script_dir/out/scene_packet_validation.json"
cat "$script_dir/out/scene_packet_validation.json"
