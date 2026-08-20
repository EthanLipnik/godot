#!/bin/sh
# Compile the bounded closure/layout corpus through Slang to SPIR-V and MSL.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
output_dir="$script_dir/out/slang_feasibility"
slang_dir="$script_dir/out/tools/slang-2026.14"
mkdir -p "$output_dir"

"$script_dir/provision_slang.sh" >/dev/null
slangc="$slang_dir/bin/slangc"

"$slangc" "$script_dir/path_tracing_feasibility.slang" \
	-entry computeMain -stage compute -target spirv -profile glsl_460 \
	-emit-spirv-directly -g2 -reflection-json "$output_dir/reflection.json" \
	-o "$output_dir/path_tracing_feasibility.spv"
spirv-val --target-env vulkan1.2 "$output_dir/path_tracing_feasibility.spv"
if [ "$(jq '.parameters | length' "$output_dir/reflection.json")" -ne 2 ]; then
	echo "unexpected Slang resource reflection" >&2
	exit 2
fi

"$slangc" "$script_dir/path_tracing_feasibility.slang" \
	-entry computeMain -stage compute -target metal \
	-line-directive-mode standard -g2 -o "$output_dir/path_tracing_feasibility.metal"
xcrun -sdk macosx metal -std=metal3.2 -c "$output_dir/path_tracing_feasibility.metal" \
	-o "$output_dir/path_tracing_feasibility.air"
xcrun -sdk macosx metallib "$output_dir/path_tracing_feasibility.air" \
	-o "$output_dir/path_tracing_feasibility.metallib"

version=$("$slangc" -version 2>&1 | tr '\n' ' ')
spirv_sha256=$(shasum -a 256 "$output_dir/path_tracing_feasibility.spv" | awk '{print $1}')
msl_sha256=$(shasum -a 256 "$output_dir/path_tracing_feasibility.metal" | awk '{print $1}')
{
	echo '{'
	printf '  "experiment": "M0-E-slang-feasibility",\n'
	printf '  "compiler": "%s",\n' "$version"
	printf '  "targets": ["spirv", "metal"],\n'
	printf '  "corpus": ["canonical_layout", "bounded_diffuse_metallic_roughness_closure", "structured_buffers"],\n'
	printf '  "spirv_sha256": "%s",\n' "$spirv_sha256"
	printf '  "generated_msl_sha256": "%s",\n' "$msl_sha256"
	printf '  "gpu_performance_compared": false,\n'
	printf '  "decision": "feasible_for_further_evaluation_not_adopted",\n'
	printf '  "passed": true\n'
	echo '}'
} > "$script_dir/out/slang_feasibility.json"
cat "$script_dir/out/slang_feasibility.json"
