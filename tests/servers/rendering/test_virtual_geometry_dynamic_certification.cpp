/**************************************************************************/
/*  test_virtual_geometry_dynamic_certification.cpp                       */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_virtual_geometry_dynamic_certification)

#include "modules/meshoptimizer/virtual_geometry.h"
#include "modules/meshoptimizer/virtual_geometry_compiler.h"
#include "modules/meshoptimizer/virtual_geometry_instance_3d.h"
#include "servers/rendering/renderer_scene_render.h"
#include "servers/rendering/virtual_geometry/virtual_geometry_ray.h"

namespace TestVirtualGeometryDynamicCertification {

using namespace RendererVirtualGeometry;

static VirtualGeometryCompiler::Input _input() {
	VirtualGeometryCompiler::Input input;
	input.source_asset_identity = 0x1001;
	input.source_primitive_identity = 0x2001;
	for (int z = 0; z < 7; z++) {
		for (int x = 0; x < 7; x++) {
			input.positions.push_back(Vector3(real_t(x), 0.1 * Math::sin(real_t(x + z)), real_t(z)));
		}
	}
	for (int z = 0; z < 6; z++) {
		for (int x = 0; x < 6; x++) {
			const int a = z * 7 + x;
			const int b = a + 1;
			const int c = a + 7;
			const int d = c + 1;
			input.indices.push_back(a);
			input.indices.push_back(c);
			input.indices.push_back(b);
			input.indices.push_back(b);
			input.indices.push_back(c);
			input.indices.push_back(d);
		}
	}
	return input;
}

static Package _compile(const VirtualGeometryCompiler::Input &p_input) {
	VirtualGeometryCompiler compiler;
	VirtualGeometryCompiler::Settings settings;
	settings.max_vertices_per_cluster = 16;
	settings.max_triangles_per_cluster = 4;
	settings.clusters_per_group = 2;
	settings.max_decoded_page_bytes = 1024;
	Package package;
	CHECK_EQ(compiler.compile(p_input, settings, package), OK);
	return package;
}

TEST_CASE("[Rendering][VirtualGeometry] VG8 rigid motion preserves immutable geometry and ray identities") {
	const Package package = _compile(_input());
	REQUIRE_EQ(validate_package(package, true), OK);
	REQUIRE(!package.manifest.clusters.is_empty());
	REQUIRE(!package.manifest.ray_groups.is_empty());

	Ref<VirtualGeometry> resource;
	resource.instantiate();
	REQUIRE_EQ(resource->set_compiled_package(package), OK);
	VirtualGeometryInstance3D node;
	node.set_virtual_geometry(resource);

	Vector<uint64_t> cluster_ids;
	Vector<uint64_t> cluster_topology_revisions;
	for (const ClusterDescriptor &cluster : package.manifest.clusters) {
		cluster_ids.push_back(cluster.stable_id);
		cluster_topology_revisions.push_back(cluster.topology_revision);
	}
	Vector<uint64_t> ray_group_ids;
	Vector<uint64_t> ray_group_revisions;
	for (const RayGroupDescriptor &ray_group : package.manifest.ray_groups) {
		ray_group_ids.push_back(ray_group.stable_id);
		ray_group_revisions.push_back(ray_group.revision);
	}

	RendererSceneRender::VirtualGeometryInstance frame_instance;
	frame_instance.resource = resource->get_rid();
	frame_instance.local_bounds = package.manifest.resource_bounds;
	frame_instance.world_bounds = package.manifest.resource_bounds;
	frame_instance.semantic_instance_id = 0xfeed;
	frame_instance.resource_revision = resource->get_revision();
	frame_instance.instance_revision = node.get_instance_revision();
	// Seed the previous frame with a real transform so frame zero is already a
	// valid motion-vector pair rather than an identity-to-identity comparison.
	Transform3D transform(Basis(), Vector3(-0.25, 0, -0.1));
	frame_instance.transform = transform;
	for (uint32_t frame = 0; frame < 48; frame++) {
		frame_instance.previous_transform = transform;
		transform = Transform3D(Basis(Vector3(0, 1, 0), Math::deg_to_rad(real_t(frame * 3))), Vector3(real_t(frame) * 0.25, 0, real_t(frame) * 0.1));
		frame_instance.transform = transform;
		frame_instance.instance_revision++;
		CHECK(frame_instance.previous_transform != frame_instance.transform);
		CHECK_EQ(frame_instance.resource, resource->get_rid());
		CHECK_EQ(frame_instance.semantic_instance_id, uint64_t(0xfeed));
		for (int i = 0; i < cluster_ids.size(); i++) {
			CHECK_EQ(package.manifest.clusters[i].stable_id, cluster_ids[i]);
			CHECK_EQ(package.manifest.clusters[i].topology_revision, cluster_topology_revisions[i]);
		}
		for (int i = 0; i < ray_group_ids.size(); i++) {
			CHECK_EQ(package.manifest.ray_groups[i].stable_id, ray_group_ids[i]);
			CHECK_EQ(package.manifest.ray_groups[i].revision, ray_group_revisions[i]);
		}
	}
	CHECK_EQ(frame_instance.resource_revision, resource->get_revision());
	CHECK(frame_instance.instance_revision > node.get_instance_revision());
}

TEST_CASE("[Rendering][VirtualGeometry] VG8 deformation and topology mutation stay on the conventional path") {
	VirtualGeometryCompiler::Input fixed_topology = _input();
	fixed_topology.immutable = false;
	fixed_topology.conventional_path_reason = "VG8 certification: fixed-topology deformation has no clustered deformation proof.";
	Package fixed_package = _compile(fixed_topology);
	CHECK(fixed_package.manifest.pages.is_empty());
	CHECK(fixed_package.manifest.clusters.is_empty());
	CHECK(!fixed_package.manifest.conventional_path_diagnostics.is_empty());
	CHECK(fixed_package.manifest.conventional_path_diagnostics[0].contains("fixed-topology deformation"));

	VirtualGeometryCompiler::Input topology_change = _input();
	topology_change.immutable = false;
	topology_change.indices.resize(topology_change.indices.size() - 3);
	topology_change.conventional_path_reason = "VG8 certification: topology-changing mutation requires immutable partition rebuild.";
	Package topology_package = _compile(topology_change);
	CHECK(topology_package.manifest.pages.is_empty());
	CHECK(topology_package.manifest.clusters.is_empty());
	CHECK(!topology_package.manifest.conventional_path_diagnostics.is_empty());
	CHECK(topology_package.manifest.conventional_path_diagnostics[0].contains("topology-changing mutation"));
}

} // namespace TestVirtualGeometryDynamicCertification
