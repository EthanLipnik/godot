/**************************************************************************/
/*  environment_portal_runtime.h                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                                                                        */
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                               */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"
#include "servers/rendering/path_tracing/indoor_lighting.h"

class EnvironmentPortal3D;

namespace RendererPathTracing {

// This data is scene-owned and may be shared by a stereo pair. It contains no
// screen-space history: callers must still keep per-eye reservoirs separate.
struct EnvironmentPortalRuntimeResult {
	Vector<EnvironmentPortal> portals;
	uint64_t generation = 0;
	uint32_t registered_count = 0;
	uint32_t selected_count = 0;
};

class EnvironmentPortalRuntime {
	struct Portal;

	Mutex *mutex = nullptr;
	HashMap<ObjectID, Portal> portals;

	EnvironmentPortalRuntime();
	~EnvironmentPortalRuntime();

public:
	static EnvironmentPortalRuntime &get_singleton();

	void register_portal(EnvironmentPortal3D *p_portal);
	void register_test_portal(EnvironmentPortal3D *p_portal, uint64_t p_scenario_id);
	void unregister_portal(ObjectID p_portal_id);
	EnvironmentPortalRuntimeResult query(uint64_t p_scenario_id, uint32_t p_visible_layers) const;
};

} // namespace RendererPathTracing
