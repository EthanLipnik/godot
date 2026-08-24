/**************************************************************************/
/*  test_flux_diagnostics.cpp                                             */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_flux_diagnostics)

#include "servers/rendering/rendering_server_types.h"

namespace TestFluxDiagnostics {

using RenderingServerTypes::FluxDiagnostics;

TEST_CASE("[RenderingServer][FluxDiagnostics] invalid snapshot has a stable complete schema") {
	const Dictionary diagnostics = FluxDiagnostics().to_dictionary();
	CHECK_FALSE(bool(diagnostics["valid"]));
	CHECK_EQ(int64_t(diagnostics["frame"]), 0);
	CHECK_EQ(int(diagnostics["effective_mode"]), 0);
	CHECK_FALSE(bool(diagnostics["ray_effects_active"]));
	CHECK_FALSE(bool(diagnostics["environment_active"]));
	CHECK_EQ(String(diagnostics["environment_status"]), "disabled");
	CHECK_EQ(int(diagnostics["primary_surface_version"]), 0);
	CHECK_FALSE(bool(diagnostics["ray_owned_shading"]));
	CHECK_FALSE(bool(diagnostics["transport_complete"]));

	const Dictionary admitted = diagnostics["admitted"];
	CHECK(admitted.has("geometry_count"));
	CHECK(admitted.has("surface_count"));
	CHECK(admitted.has("base_triangle_count"));
	CHECK(admitted.has("selected_triangle_count"));
	CHECK(admitted.has("canonical_material_count"));

	const Dictionary transport = diagnostics["transport"];
	CHECK(transport.has("retained_non_primary_geometry_count"));
	CHECK(transport.has("retains_non_primary_geometry"));

	const Dictionary materials = diagnostics["materials"];
	CHECK(materials.has("tier2"));
	CHECK(materials.has("capacity"));
	CHECK(materials.has("albedo"));
	CHECK(materials.has("normal"));
	CHECK(materials.has("orm"));
	CHECK(materials.has("emissive"));
	CHECK(materials.has("opacity"));
	CHECK(materials.has("alpha_occupancy"));
	CHECK_FALSE(bool(diagnostics["timings_valid"]));
}

TEST_CASE("[RenderingServer][FluxDiagnostics] exact mapping and conservative retention derivation") {
	FluxDiagnostics source;
	source.reset_for_frame(91, 2);
	source.ray_effects_active = true;
	source.environment_active = true;
	source.environment_status = "active";
	source.primary_surface_version = 1;
	source.ray_owned_shading = true;
	source.primary_surface_view_count = 2;
	source.transport_complete = true;
	source.transport_incomplete_reason = "complete";
	source.admitted_geometry_count = 12;
	source.admitted_surface_count = 9;
	source.admitted_base_triangle_count = 45000;
	source.admitted_selected_triangle_count = 23146;
	source.admitted_canonical_material_count = 16;
	source.transport_state = "bounded";
	source.transport_reason = "active";
	source.transport_max_distance = 150.0f;
	source.set_transport_counts(3, 8, 10);
	source.transport_selected_light_count = 4;
	source.transport_eligible_light_count = 7;
	source.material_tier2 = true;
	source.material_capacity = 2048;
	source.material_albedo = { 4, 4, 0 };
	source.material_normal = { 5, 4, 1 };
	source.material_orm = { 3, 3, 0 };
	source.material_emissive = { 2, 2, 0 };
	source.material_opacity = { 1, 1, 0 };
	source.material_alpha_occupancy = { 1, 1, 0 };

	const Dictionary diagnostics = source.to_dictionary();
	CHECK(bool(diagnostics["valid"]));
	CHECK_EQ(int64_t(diagnostics["frame"]), 91);
	CHECK_EQ(int(diagnostics["effective_mode"]), 2);
	CHECK_EQ(int(diagnostics["primary_surface_version"]), 1);
	CHECK(bool(diagnostics["ray_owned_shading"]));
	CHECK(bool(diagnostics["transport_complete"]));
	const Dictionary admitted = diagnostics["admitted"];
	CHECK_EQ(int64_t(admitted["selected_triangle_count"]), 23146);
	CHECK_EQ(int(admitted["canonical_material_count"]), 16);
	const Dictionary transport = diagnostics["transport"];
	CHECK_EQ(int(transport["retained_non_primary_geometry_count"]), 5);
	CHECK(bool(transport["retains_non_primary_geometry"]));
	const Dictionary materials = diagnostics["materials"];
	const Dictionary normal = materials["normal"];
	CHECK_EQ(int(normal["requested"]), 5);
	CHECK_EQ(int(normal["resident"]), 4);
	CHECK_EQ(int(normal["misses"]), 1);
}

TEST_CASE("[RenderingServer][FluxDiagnostics] raster reset clears active ray state and counters") {
	FluxDiagnostics diagnostics;
	diagnostics.reset_for_frame(4, 1);
	diagnostics.ray_effects_active = true;
	diagnostics.environment_active = true;
	diagnostics.admitted_geometry_count = 9;
	diagnostics.material_normal = { 3, 2, 1 };
	diagnostics.timings_valid = true;
	diagnostics.timings_ms.ray_effects = 8.0;

	diagnostics.reset_for_frame(5, 0);
	CHECK(diagnostics.valid);
	CHECK_EQ(diagnostics.effective_mode, 0);
	CHECK_FALSE(diagnostics.ray_effects_active);
	CHECK_FALSE(diagnostics.environment_active);
	CHECK_EQ(diagnostics.admitted_geometry_count, 0);
	CHECK_EQ(diagnostics.material_normal.requested, 0);
	CHECK_FALSE(diagnostics.timings_valid);
}

TEST_CASE("[RenderingServer][FluxDiagnostics] timing dictionary exposes every stable stage key") {
	FluxDiagnostics source;
	source.timings_valid = true;
	source.timings_ms = { 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0 };
	const Dictionary diagnostics = source.to_dictionary();
	CHECK(bool(diagnostics["timings_valid"]));
	const Dictionary timings = diagnostics["timings_ms"];
	CHECK_EQ(double(timings["blas"]), 1.0);
	CHECK_EQ(double(timings["tlas"]), 2.0);
	CHECK_EQ(double(timings["ray_shadows"]), 3.0);
	CHECK_EQ(double(timings["ray_effects"]), 4.0);
	CHECK_EQ(double(timings["spatial"]), 5.0);
	CHECK_EQ(double(timings["temporal"]), 6.0);
	CHECK_EQ(double(timings["composition"]), 7.0);
}

} // namespace TestFluxDiagnostics
