#!/bin/sh
# Reproducible Godot path-tracing development baseline.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
repository_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
venv_dir="$script_dir/out/build-venv"

if [ ! -x "$venv_dir/bin/scons" ] || ! "$venv_dir/bin/scons" --version >/dev/null 2>&1; then
	python3 -m venv --clear "$venv_dir"
	"$venv_dir/bin/python" -m pip install 'scons==4.10.1'
fi

cd "$repository_root"
"$venv_dir/bin/scons" \
	platform=macos \
	arch=arm64 \
	target=editor \
	dev_build=yes \
	vulkan=no \
	angle=no \
	accesskit=no \
	-j"${JOBS:-12}"
