extends Node3D

# Pure virtual-geometry regression: no MeshInstance3D is present to mask the
# opaque-instance UMA mapping path. Five freshly serialized/reloaded resources
# are each instanced three times (ten instances share an existing resource).

const GRID := 17
const RESOURCE_COUNT := 5
const INSTANCES_PER_RESOURCE := 3

var camera: Camera3D
var retained_resources: Array[VirtualGeometry] = []

func _make_surface_mesh(color: Color) -> ArrayMesh:
	var vertices := PackedVector3Array()
	var normals := PackedVector3Array()
	var uvs := PackedVector2Array()
	var indices := PackedInt32Array()
	for y in GRID:
		for x in GRID:
			var uv := Vector2(float(x) / float(GRID - 1), float(y) / float(GRID - 1))
			vertices.push_back(Vector3((uv.x - 0.5) * 1.4, (uv.y - 0.5) * 1.4, 0.05 * sin(float(x * 3 + y))))
			normals.push_back(Vector3(0, 0, 1))
			uvs.push_back(uv)
	for y in GRID - 1:
		for x in GRID - 1:
			var a := y * GRID + x
			var b := a + 1
			var c := a + GRID
			var d := c + 1
			indices.append_array(PackedInt32Array([a, b, c, b, d, c]))
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_INDEX] = indices
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	var first := StandardMaterial3D.new()
	first.albedo_color = color
	first.roughness = 0.35
	first.cull_mode = BaseMaterial3D.CULL_DISABLED
	var second := StandardMaterial3D.new()
	second.albedo_color = color.lightened(0.22)
	second.metallic = 0.15
	second.roughness = 0.28
	second.cull_mode = BaseMaterial3D.CULL_DISABLED
	mesh.surface_set_material(0, first)
	mesh.surface_set_material(1, second)
	return mesh

func _ready() -> void:
	var environment_node := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.008, 0.01, 0.016)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.35, 0.38, 0.45)
	environment.ambient_light_energy = 0.9
	environment_node.environment = environment
	add_child(environment_node)
	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-35, -20, 0)
	light.light_energy = 1.7
	add_child(light)

	for resource_index in RESOURCE_COUNT:
		var mesh := _make_surface_mesh(Color.from_hsv(float(resource_index) / float(RESOURCE_COUNT), 0.65, 0.95))
		var source := VirtualGeometry.new()
		var compile_error := source.compile_from_mesh(mesh, resource_index & 1, 0xF600 + resource_index, 0xFA00 + resource_index, 8192)
		if compile_error != OK:
			push_error("F6 multi native compile failed: %d" % compile_error)
			get_tree().quit(2)
			return
		source.material_bindings = [mesh.surface_get_material(resource_index & 1)]
		var serialized_path := "user://f6_multi_%d.tres" % resource_index
		if ResourceSaver.save(source, serialized_path) != OK:
			push_error("F6 multi native save failed: %s" % serialized_path)
			get_tree().quit(3)
			return
		var loaded := ResourceLoader.load(serialized_path, "VirtualGeometry", ResourceLoader.CACHE_MODE_IGNORE) as VirtualGeometry
		if loaded == null or not loaded.is_valid_virtual_geometry() or not loaded.has_complete_material_bindings():
			push_error("F6 multi native reload failed: %s" % serialized_path)
			get_tree().quit(4)
			return
		source = null # Prove all instances below use the fresh serialized resource.
		retained_resources.append(loaded)
		DirAccess.remove_absolute(ProjectSettings.globalize_path(serialized_path))
		for shared_index in INSTANCES_PER_RESOURCE:
			var instance := VirtualGeometryInstance3D.new()
			instance.name = "PureVirtual_%d_%d" % [resource_index, shared_index]
			instance.virtual_geometry = loaded
			instance.semantic_instance_id = 0xF6000000 + resource_index * INSTANCES_PER_RESOURCE + shared_index + 1
			instance.position = Vector3((resource_index - 2) * 1.65, (shared_index - 1) * 1.65, 0)
			add_child(instance)

	camera = Camera3D.new()
	camera.fov = 48.0
	camera.position = Vector3(0, 0, 40)
	camera.current = true
	add_child(camera)
	await _run_fixture()

func _run_fixture() -> void:
	for _frame in 20:
		await get_tree().process_frame
	camera.position.z = 8.0
	for _frame in 55:
		await get_tree().process_frame
	camera.position.z = 40.0
	for _frame in 25:
		await get_tree().process_frame
	print("F6_MULTI_NATIVE result=pass unique_resources=%d instances=%d shared_instances=%d source_surfaces=2 pure_vg=true far_near_far=true" % [retained_resources.size(), RESOURCE_COUNT * INSTANCES_PER_RESOURCE, RESOURCE_COUNT * (INSTANCES_PER_RESOURCE - 1)])
	get_tree().quit(0)
