/**************************************************************************/
/*  test_environment_portal_runtime.cpp                                  */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_environment_portal_runtime)

#include "scene/3d/environment_portal_3d.h"
#include "servers/rendering/path_tracing/environment_portal_runtime.h"

namespace TestEnvironmentPortalRuntime {

TEST_CASE("[PathTracing][EnvironmentPortal] authored portals are scenario-scoped, masked, stable, and revisioned") {
	EnvironmentPortal3D first;
	EnvironmentPortal3D second;
	EnvironmentPortal3D other_scenario;
	first.set_size(Vector2(2.0f, 4.0f));
	first.set_cull_mask(1);
	second.set_size(Vector2(3.0f, 5.0f));
	second.set_cull_mask(1);
	other_scenario.set_cull_mask(1);
	RendererPathTracing::EnvironmentPortalRuntime &runtime = RendererPathTracing::EnvironmentPortalRuntime::get_singleton();
	runtime.register_test_portal(&first, 7);
	runtime.register_test_portal(&second, 7);
	runtime.register_test_portal(&other_scenario, 8);
	const RendererPathTracing::EnvironmentPortalRuntimeResult initial = runtime.query(7, 1);
	CHECK_EQ(initial.registered_count, 2);
	CHECK_EQ(initial.selected_count, 2);
	CHECK_EQ(initial.portals.size(), 2);
	CHECK(initial.portals[0].portal_id < initial.portals[1].portal_id);
	CHECK(initial.generation != 0);
	CHECK_EQ(runtime.query(7, 2).selected_count, 0);
	CHECK_EQ(runtime.query(8, 1).selected_count, 1);
	first.set_proposal_weight(2.0f);
	runtime.register_test_portal(&first, 7);
	const RendererPathTracing::EnvironmentPortalRuntimeResult changed = runtime.query(7, 1);
	CHECK_NE(changed.generation, initial.generation);
	runtime.unregister_portal(first.get_instance_id());
	runtime.unregister_portal(second.get_instance_id());
	runtime.unregister_portal(other_scenario.get_instance_id());
}

} // namespace TestEnvironmentPortalRuntime
