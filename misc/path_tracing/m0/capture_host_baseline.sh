#!/bin/sh
# Privacy-safe host capture for renderer experiments.

set -eu

output_path=${1:-out/host_baseline.txt}
output_dir=$(dirname "$output_path")
mkdir -p "$output_dir"

{
	date -u '+captured_utc=%Y-%m-%dT%H:%M:%SZ'
	git rev-parse HEAD | sed 's/^/godot_revision=/'
	sw_vers | sed 's/^/os_/'
	uname -m | sed 's/^/architecture=/'
	system_profiler SPHardwareDataType SPDisplaysDataType 2>/dev/null | sed -E '/Serial Number|Hardware UUID|Provisioning UDID|Activation Lock Status/d'
	xcodebuild -version
	printf 'macos_sdk_version=%s\n' "$(xcrun --sdk macosx --show-sdk-version)"
	printf 'macos_sdk_path=%s\n' "$(xcrun --sdk macosx --show-sdk-path)"
	clang --version
	python3 --version
	for tool in scons glslangValidator spirv-val vulkaninfo slangc blender cmake ninja; do
		if command -v "$tool" >/dev/null 2>&1; then
			printf '%s=%s\n' "$tool" "$(command -v "$tool")"
		else
			printf '%s=missing\n' "$tool"
		fi
	done
	if [ -x /Applications/Blender.app/Contents/MacOS/Blender ]; then
		/Applications/Blender.app/Contents/MacOS/Blender --version | sed -n '1,2p' | sed 's/^/blender_app_/'
	fi
	if [ -x misc/path_tracing/m0/out/tools/slang-2026.14/bin/slangc ]; then
		misc/path_tracing/m0/out/tools/slang-2026.14/bin/slangc -version 2>&1 | sed 's/^/shader_slang_version=/'
	fi
	defaults read /System/Library/Frameworks/Metal.framework/Resources/Info CFBundleVersion 2>/dev/null | sed 's/^/metal_framework_version=/' || true
	defaults read /System/Library/Frameworks/MetalFX.framework/Resources/Info CFBundleVersion 2>/dev/null | sed 's/^/metalfx_framework_version=/' || true
} >"$output_path"

printf '%s\n' "$output_path"
