#!/bin/sh
# Provision the pinned official Shader Slang compiler in ignored M0 output.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
tool_dir="$script_dir/out/tools/slang-2026.14"
archive="$script_dir/out/tools/slang-2026.14-macos-aarch64.zip"
url="https://github.com/shader-slang/slang/releases/download/v2026.14/slang-2026.14-macos-aarch64.zip"
expected_sha256="b8e2e277abd5cbe4591168ab4350ee46bd5631f705410208cb505c795fbab4b9"

if [ ! -x "$tool_dir/bin/slangc" ]; then
	mkdir -p "$script_dir/out/tools"
	if [ ! -f "$archive" ]; then
		curl -fL "$url" -o "$archive"
	fi
	actual_sha256=$(shasum -a 256 "$archive" | awk '{print $1}')
	if [ "$actual_sha256" != "$expected_sha256" ]; then
		echo "Shader Slang archive hash mismatch" >&2
		exit 2
	fi
	rm -rf "$tool_dir"
	mkdir -p "$tool_dir"
	unzip -q "$archive" -d "$tool_dir"
fi

"$tool_dir/bin/slangc" -version
