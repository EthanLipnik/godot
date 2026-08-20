/**************************************************************************/
/*  remote_windows_launch_plan.cpp                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY    */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "remote_windows_launch_plan.h"

#include "core/io/json.h"
#include "core/string/char_utils.h"

static bool _is_hex(const String &p_value, uint32_t p_length) {
	if (p_value.length() != int(p_length)) {
		return false;
	}
	for (int index = 0; index < p_value.length(); index++) {
		const char32_t character = p_value[index];
		if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F'))) {
			return false;
		}
	}
	return true;
}

static bool _safe_endpoint_component(const String &p_value) {
	if (p_value.is_empty()) {
		return false;
	}
	for (int index = 0; index < p_value.length(); index++) {
		const char32_t character = p_value[index];
		if (!(is_ascii_alphanumeric_char(character) || character == '.' || character == '-' || character == '_')) {
			return false;
		}
	}
	return true;
}

Error RemoteWindowsLaunchPlan::validate(const RemoteWindowsPreset &p_preset, Vector<String> &r_diagnostics) {
	r_diagnostics.clear();
	if (!_safe_endpoint_component(p_preset.host)) {
		r_diagnostics.push_back("Remote host must be a DNS name or IPv4 address without credentials or shell metacharacters.");
	}
	if (!_safe_endpoint_component(p_preset.user)) {
		r_diagnostics.push_back("Remote user is missing or contains shell metacharacters.");
	}
	if (p_preset.port == 0) {
		r_diagnostics.push_back("Remote SSH port must be nonzero.");
	}
	if (p_preset.remote_directory.is_empty() || p_preset.remote_directory.contains("..") || p_preset.remote_directory.contains_char('\n') || p_preset.remote_directory.contains_char('\r')) {
		r_diagnostics.push_back("Remote directory must be explicit and may not traverse parents or contain line breaks.");
	}
	if (p_preset.export_preset.is_empty()) {
		r_diagnostics.push_back("A Windows export preset is required.");
	}
	if (!p_preset.export_template_available) {
		r_diagnostics.push_back("The selected Windows export template is unavailable.");
	}
	if (p_preset.artifact_path.is_empty()) {
		r_diagnostics.push_back("A local exported artifact path is required.");
	}
	if (!_is_hex(p_preset.engine_commit, 40)) {
		r_diagnostics.push_back("Engine commit must be a full 40-character hexadecimal revision.");
	}
	if (!_is_hex(p_preset.asset_manifest_sha256, 64)) {
		r_diagnostics.push_back("Asset manifest must be a 64-character SHA-256 digest.");
	}
	if (p_preset.renderer_backend != "vulkan") {
		r_diagnostics.push_back("The remote VR PC preset must explicitly request the Vulkan renderer backend.");
	}
	return r_diagnostics.is_empty() ? OK : ERR_INVALID_PARAMETER;
}

Error RemoteWindowsLaunchPlan::generate(const RemoteWindowsPreset &p_preset, RemoteWindowsCommandPreview &r_preview, Vector<String> &r_diagnostics) {
	r_preview = {};
	const Error validation_error = validate(p_preset, r_diagnostics);
	if (validation_error != OK) {
		return validation_error;
	}
	Dictionary manifest;
	manifest["schema"] = 1;
	manifest["engine_commit"] = p_preset.engine_commit.to_lower();
	manifest["asset_manifest_sha256"] = p_preset.asset_manifest_sha256.to_lower();
	manifest["export_preset"] = p_preset.export_preset;
	manifest["renderer_backend"] = p_preset.renderer_backend;
	manifest["artifact_name"] = p_preset.artifact_path.get_file();
	r_preview.manifest_json = JSON::stringify(manifest, "", true);
	const String endpoint = p_preset.user + "@" + p_preset.host;
	r_preview.transfer_argv = { "scp", "-P", itos(p_preset.port), p_preset.artifact_path, endpoint + ":" + p_preset.remote_directory };
	r_preview.launch_argv = { "ssh", "-p", itos(p_preset.port), endpoint, "powershell.exe", "-NoProfile", "-NonInteractive", "-File",
		p_preset.remote_directory.path_join("launch_godot_vr.ps1"), "-Manifest", p_preset.remote_directory.path_join("launch-manifest.json") };
	return OK;
}

Error RemoteWindowsMockEndpoint::apply(Event p_event) {
	if (p_event == EVENT_FAILURE) {
		state = STATE_FAILED;
		transitions.push_back("failed");
		return OK;
	}
	const Event expected[] = { EVENT_VALIDATE_OK, EVENT_TRANSFER_OK, EVENT_ENDPOINT_STARTED, EVENT_XR_SYSTEM_READY,
		EVENT_ENGINE_STARTED, EVENT_CHANNELS_FORWARDED, EVENT_HEALTHY };
	const char *names[] = { "validated", "transferred", "endpoint_started", "xr_ready", "engine_launched", "channels_forwarded", "ready" };
	if (state == STATE_FAILED || state == STATE_READY || uint32_t(state) >= sizeof(expected) / sizeof(expected[0]) || p_event != expected[state]) {
		return ERR_INVALID_PARAMETER;
	}
	state = State(uint32_t(state) + 1);
	transitions.push_back(names[uint32_t(state) - 1]);
	return OK;
}
