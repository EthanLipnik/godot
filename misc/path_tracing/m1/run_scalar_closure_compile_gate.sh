#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
output_dir="$script_dir/out/scalar_closure"
mkdir -p "$output_dir"

xcrun -sdk macosx metal -std=metal3.2 -c "$script_dir/scalar_closure.metal" -o "$output_dir/scalar_closure.air"
glslangValidator -V --target-env vulkan1.2 "$script_dir/scalar_closure.comp.glsl" -o "$output_dir/scalar_closure.spv"
spirv-val --target-env vulkan1.2 "$output_dir/scalar_closure.spv"

metal_hash=$(shasum -a 256 "$output_dir/scalar_closure.air" | awk '{print $1}')
spirv_hash=$(shasum -a 256 "$output_dir/scalar_closure.spv" | awk '{print $1}')
printf '{"experiment":"M1-paired-scalar-closure","metal_air_sha256":"%s","spirv_sha256":"%s","passed":true}\n' "$metal_hash" "$spirv_hash"
