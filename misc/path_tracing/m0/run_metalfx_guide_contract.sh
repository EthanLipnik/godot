#!/bin/sh
# Build and run the MetalFX guide-contract spike.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
output_dir="$script_dir/out/metalfx_guide_contract"
mkdir -p "$output_dir"

xcrun -sdk macosx clang++ -std=c++17 -fobjc-arc \
	-framework Foundation -framework Metal -framework MetalFX \
	"$script_dir/metalfx_guide_contract.mm" -o "$output_dir/metalfx_guide_contract"

"$output_dir/metalfx_guide_contract" > "$script_dir/out/metalfx_guide_contract.json"
cat "$script_dir/out/metalfx_guide_contract.json"
