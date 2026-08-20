#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
swift test --package-path "$script_dir"
xcrun swiftc -typecheck -parse-as-library -target arm64-apple-xros27.0 -sdk "$(xcrun --sdk xros --show-sdk-path)" \
	"$script_dir/visionOS/RemoteXRCompanionApp.swift"
