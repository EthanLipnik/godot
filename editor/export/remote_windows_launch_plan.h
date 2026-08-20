/**************************************************************************/
/*  remote_windows_launch_plan.h                                          */
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

#pragma once

#include "core/error/error_list.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"

struct RemoteWindowsPreset {
	String host;
	String user;
	uint16_t port = 22;
	String remote_directory;
	String export_preset;
	String artifact_path;
	String engine_commit;
	String asset_manifest_sha256;
	String renderer_backend = "vulkan";
	bool export_template_available = false;
};

struct RemoteWindowsCommandPreview {
	Vector<String> transfer_argv;
	Vector<String> launch_argv;
	String manifest_json;
};

class RemoteWindowsLaunchPlan {
public:
	static Error validate(const RemoteWindowsPreset &p_preset, Vector<String> &r_diagnostics);
	static Error generate(const RemoteWindowsPreset &p_preset, RemoteWindowsCommandPreview &r_preview, Vector<String> &r_diagnostics);
};

class RemoteWindowsMockEndpoint {
public:
	enum State {
		STATE_IDLE,
		STATE_VALIDATED,
		STATE_TRANSFERRED,
		STATE_ENDPOINT_STARTED,
		STATE_XR_READY,
		STATE_ENGINE_LAUNCHED,
		STATE_CHANNELS_FORWARDED,
		STATE_READY,
		STATE_FAILED,
	};

	enum Event {
		EVENT_VALIDATE_OK,
		EVENT_TRANSFER_OK,
		EVENT_ENDPOINT_STARTED,
		EVENT_XR_SYSTEM_READY,
		EVENT_ENGINE_STARTED,
		EVENT_CHANNELS_FORWARDED,
		EVENT_HEALTHY,
		EVENT_FAILURE,
	};

private:
	State state = STATE_IDLE;
	Vector<String> transitions;

public:
	Error apply(Event p_event);
	State get_state() const { return state; }
	const Vector<String> &get_transitions() const { return transitions; }
};
