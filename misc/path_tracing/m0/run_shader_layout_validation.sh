#!/bin/sh
# Compile and validate the canonical packet layouts in MSL and GLSL/SPIR-V.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
output_dir="$script_dir/out/shader_layout"
mkdir -p "$output_dir"

xcrun -sdk macosx metal -std=metal3.2 -c "$script_dir/canonical_scene_packet_layout.metal" -o "$output_dir/layout.air"
xcrun -sdk macosx metallib "$output_dir/layout.air" -o "$output_dir/layout.metallib"
xcrun -sdk macosx clang++ -std=c++17 -Wall -Wextra -Werror -fobjc-arc \
	-framework Foundation -framework Metal \
	"$script_dir/validate_metal_packet_layout.mm" -o "$output_dir/validate_metal_packet_layout"
"$output_dir/validate_metal_packet_layout" "$output_dir/layout.metallib" > "$script_dir/out/metal_layout_validation.json"

glslangValidator -V --target-env vulkan1.2 -l -q --reflect-all-block-variables \
	"$script_dir/canonical_scene_packet_layout.comp.glsl" \
	-o "$output_dir/canonical_scene_packet_layout.spv" > "$output_dir/glsl_reflection.txt"
spirv-val --target-env vulkan1.2 "$output_dir/canonical_scene_packet_layout.spv"

require_reflection() {
	pattern=$1
	if ! grep -E "$pattern" "$output_dir/glsl_reflection.txt" >/dev/null; then
		echo "missing GLSL reflection: $pattern" >&2
		exit 2
	fi
}

require_reflection 'PacketLayoutProbe\.header\.magic: offset 0'
require_reflection 'PacketLayoutProbe\.guides\.schema_version: offset 80'
require_reflection 'PacketLayoutProbe\.camera\.view_index: offset 416'
require_reflection 'PacketLayoutProbe\.instance_record\.geometry_id: offset 560'
require_reflection 'PacketLayoutProbe\.material\.base_color_texture: offset 656'
require_reflection 'PacketLayoutProbe\.light\.light_id: offset 720'
require_reflection 'PacketLayoutProbe\.result: offset 736'

glslang_version=$(glslangValidator --version | sed -n '1p')
spirv_hash=$(shasum -a 256 "$output_dir/canonical_scene_packet_layout.spv" | awk '{print $1}')
{
	echo '{'
	printf '  "experiment": "M0-J-glsl-layout",\n'
	printf '  "compiler": "%s",\n' "$glslang_version"
	printf '  "spirv_sha256": "%s",\n' "$spirv_hash"
	printf '  "top_level_offsets": [0, 80, 144, 432, 592, 672, 736],\n'
	printf '  "passed": true\n'
	echo '}'
} > "$script_dir/out/glsl_layout_validation.json"

cat "$script_dir/out/metal_layout_validation.json"
cat "$script_dir/out/glsl_layout_validation.json"
