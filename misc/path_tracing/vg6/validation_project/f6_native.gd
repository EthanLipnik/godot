extends Node3D

const GRID := 33
const PURE_GRID := 17
const PURE_RESOURCE_COUNT := 5
const PURE_INSTANCES_PER_RESOURCE := 3
const PURE_BASE_ID := 0xF6000000
const PURE_FAR_DISTANCE := 22.0
const CLEAR_COLOR := Color(0.006, 0.008, 0.012)
var virtual_instance: VirtualGeometryInstance3D
var conventional_instance: MeshInstance3D
var camera: Camera3D
var pure_resources: Array = []
var pure_instances: Array = []
var pure_serialized_paths: Array[String] = []
var subviewport: SubViewport

func _make_grid() -> ArrayMesh:
	var vertices := PackedVector3Array()
	var normals := PackedVector3Array()
	var uvs := PackedVector2Array()
	var indices := PackedInt32Array()
	for y in GRID:
		for x in GRID:
			var uv := Vector2(float(x) / float(GRID - 1), float(y) / float(GRID - 1))
			var px := (uv.x - 0.5) * 4.0
			var py := (uv.y - 0.5) * 4.0
			vertices.push_back(Vector3(px, py, 0.12 * sin(px * 3.0) * cos(py * 2.0)))
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
	return mesh

func _make_pure_surface_mesh(color: Color) -> ArrayMesh:
	var vertices := PackedVector3Array()
	var normals := PackedVector3Array()
	var uvs := PackedVector2Array()
	var indices := PackedInt32Array()
	for y in PURE_GRID:
		for x in PURE_GRID:
			var uv := Vector2(float(x) / float(PURE_GRID - 1), float(y) / float(PURE_GRID - 1))
			vertices.push_back(Vector3((uv.x - 0.5) * 1.4, (uv.y - 0.5) * 1.4, 0.05 * sin(float(x * 3 + y))))
			normals.push_back(Vector3(0, 0, 1))
			uvs.push_back(uv)
	for y in PURE_GRID - 1:
		for x in PURE_GRID - 1:
			var a := y * PURE_GRID + x
			var b := a + 1
			var c := a + PURE_GRID
			var d := c + 1
			indices.append_array(PackedInt32Array([a, b, c, b, d, c]))
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_INDEX] = indices
	var mesh := ArrayMesh.new()
	# Keep two source surfaces in the fixture. compile_from_mesh selects one
	# surface per package, which exercises distinct material slots without ever
	# adding a conventional MeshInstance3D to the pure-VG phase.
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	var first := StandardMaterial3D.new()
	first.albedo_color = color
	first.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	first.roughness = 0.35
	first.cull_mode = BaseMaterial3D.CULL_DISABLED
	var second := StandardMaterial3D.new()
	second.albedo_color = color.lightened(0.22)
	second.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	second.metallic = 0.15
	second.roughness = 0.28
	second.cull_mode = BaseMaterial3D.CULL_DISABLED
	mesh.surface_set_material(0, first)
	mesh.surface_set_material(1, second)
	return mesh

func _mean_luminance(image: Image, from_x: int, to_x: int) -> float:
	var sum := 0.0
	var count := 0
	for y in range(40, image.get_height() - 40):
		for x in range(from_x, to_x):
			sum += image.get_pixel(x, y).get_luminance()
			count += 1
	return sum / maxf(1.0, float(count))

func _mirrored_difference(image: Image) -> float:
	var sum := 0.0
	var count := 0
	for y in range(40, image.get_height() - 40):
		for x in range(30, image.get_width() / 2 - 20):
			var a := image.get_pixel(x, y)
			var b := image.get_pixel(image.get_width() - 1 - x, y)
			sum += absf(a.r - b.r) + absf(a.g - b.g) + absf(a.b - b.b)
			count += 3
	return sum / maxf(1.0, float(count))

func _frame_image() -> Image:
	return get_viewport().get_texture().get_image()

func _foreground_gate(image: Image, phase: String) -> bool:
	# Use an exact sample of the clear background in this target instead of
	# treating a nonempty indirect-command buffer as visual proof. The same gate
	# applies to root and SubViewport output and catches transparent/depth-only
	# submissions before a conventional mesh can mask them.
	var clear := image.get_pixel(0, 0)
	var foreground := 0
	var luminance_sum := 0.0
	var min_x := image.get_width()
	var min_y := image.get_height()
	var max_x := -1
	var max_y := -1
	var hue_bins := PackedInt32Array()
	hue_bins.resize(8)
	for y in image.get_height():
		for x in image.get_width():
			var pixel := image.get_pixel(x, y)
			var distance := absf(pixel.r - clear.r) + absf(pixel.g - clear.g) + absf(pixel.b - clear.b)
			if distance <= 0.08:
				continue
			foreground += 1
			luminance_sum += pixel.get_luminance()
			min_x = mini(min_x, x)
			min_y = mini(min_y, y)
			max_x = maxi(max_x, x)
			max_y = maxi(max_y, y)
			if pixel.s > 0.18:
				var hue_bin := mini(7, int(floor(pixel.h * 8.0)))
				hue_bins[hue_bin] += 1
	var image_area := image.get_width() * image.get_height()
	var coverage := float(foreground) / maxf(1.0, float(image_area))
	var bbox_width := max_x - min_x + 1
	var bbox_height := max_y - min_y + 1
	var local_contrast := luminance_sum / float(foreground) - clear.get_luminance() if foreground > 0 else 0.0
	var material_regions := 0
	for bin_count in hue_bins:
		if bin_count > image_area / 1000:
			material_regions += 1
	var passed := coverage > 0.012 and bbox_width > image.get_width() / 5 and bbox_height > image.get_height() / 8 and local_contrast > 0.045 and material_regions >= 3
	print("F6_NATIVE_VISUAL phase=%s result=%s clear=(%.6f,%.6f,%.6f) coverage=%.6f bbox=%dx%d local_contrast=%.6f material_regions=%d" % [phase, "pass" if passed else "fail", clear.r, clear.g, clear.b, coverage, bbox_width, bbox_height, local_contrast, material_regions])
	return passed

func _wait_frames(count: int) -> void:
	for _frame in count:
		await get_tree().process_frame

func _remove_pure_instances() -> void:
	for instance in pure_instances:
		if is_instance_valid(instance):
			instance.queue_free()
	pure_instances.clear()
	await get_tree().process_frame
	await get_tree().process_frame

func _load_pure_resources(path_prefix: String = "user://f6_native_pure") -> bool:
	pure_resources.clear()
	for resource_index in PURE_RESOURCE_COUNT:
		var mesh := _make_pure_surface_mesh(Color.from_hsv(float(resource_index) / float(PURE_RESOURCE_COUNT), 0.65, 0.95))
		var source := VirtualGeometry.new()
		var surface_index := resource_index & 1
		var compile_error := source.compile_from_mesh(mesh, surface_index, 0xF600 + resource_index, 0xFA00 + resource_index, 8192)
		if compile_error != OK:
			push_error("F6 pure-VG compile failed: %d" % compile_error)
			return false
		source.material_bindings = [mesh.surface_get_material(surface_index)]
		var serialized_path := "%s_%d.tres" % [path_prefix, resource_index]
		pure_serialized_paths.append(serialized_path)
		if ResourceSaver.save(source, serialized_path) != OK:
			push_error("F6 pure-VG save failed: %s" % serialized_path)
			return false
		# CACHE_MODE_IGNORE is intentional: this regression must exercise a fresh
		# Resource object rather than retaining the just-saved source instance.
		var loaded := ResourceLoader.load(serialized_path, "VirtualGeometry", ResourceLoader.CACHE_MODE_IGNORE) as VirtualGeometry
		if loaded == null or loaded == source or not loaded.is_valid_virtual_geometry() or not loaded.has_complete_material_bindings():
			push_error("F6 pure-VG reload failed: %s" % serialized_path)
			return false
		pure_resources.append(loaded)
	return pure_resources.size() == PURE_RESOURCE_COUNT

func _instantiate_pure_resources(parent: Node) -> bool:
	if pure_resources.size() != PURE_RESOURCE_COUNT:
		return false
	for resource_index in PURE_RESOURCE_COUNT:
		var resource: VirtualGeometry = pure_resources[resource_index]
		for shared_index in PURE_INSTANCES_PER_RESOURCE:
			var instance := VirtualGeometryInstance3D.new()
			instance.name = "PureVirtual_%d_%d" % [resource_index, shared_index]
			instance.virtual_geometry = resource
			instance.semantic_instance_id = PURE_BASE_ID + resource_index * PURE_INSTANCES_PER_RESOURCE + shared_index + 1
			instance.position = Vector3((resource_index - 2) * 1.65, (shared_index - 1) * 1.65, 0)
			parent.add_child(instance)
			pure_instances.append(instance)
	if pure_instances.size() != PURE_RESOURCE_COUNT * PURE_INSTANCES_PER_RESOURCE:
		return false
	# No opaque MeshInstance3D may participate in this phase. This catches an
	# accidental parity reference masking the pure virtual-geometry path.
	return parent.find_children("*", "MeshInstance3D", true, false).is_empty()

func _ready() -> void:
	var environment_node := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = CLEAR_COLOR
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.35, 0.38, 0.45)
	environment.ambient_light_energy = 0.8
	environment_node.environment = environment
	add_child(environment_node)

	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-25, -20, 0)
	light.light_energy = 1.5
	add_child(light)

	var mesh := _make_grid()
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.12, 0.62, 0.95)
	material.metallic = 0.15
	material.roughness = 0.32
	material.emission_enabled = true
	material.emission = Color(0.04, 0.35, 0.9)
	material.emission_energy_multiplier = 3.0
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	mesh.surface_set_material(0, material)

	var virtual_geometry := VirtualGeometry.new()
	var compile_error := virtual_geometry.compile_from_mesh(mesh, 0, 6001, 6002, 8192)
	if compile_error != OK:
		push_error("F6 native compile failed: %d" % compile_error)
		get_tree().quit(2)
		return
	virtual_geometry.material_bindings = [material]
	virtual_instance = VirtualGeometryInstance3D.new()
	virtual_instance.name = "NativeVirtualGeometry"
	virtual_instance.semantic_instance_id = 6003
	virtual_instance.virtual_geometry = virtual_geometry
	virtual_instance.position.x = -2.3
	add_child(virtual_instance)

	conventional_instance = MeshInstance3D.new()
	conventional_instance.name = "ConventionalArrayMeshReference"
	conventional_instance.mesh = mesh
	conventional_instance.position.x = 2.3
	add_child(conventional_instance)

	camera = Camera3D.new()
	camera.fov = 48.0
	camera.position = Vector3(0, 0, 55)
	camera.current = true
	add_child(camera)
	var conventional_pass := await _run_conventional_phase()
	if not conventional_pass:
		get_tree().quit(3)
		return
	# Remove the conventional reference and the first VG instance before the
	# crash-regression phase. The second phase is intentionally pure VG.
	if is_instance_valid(conventional_instance):
		conventional_instance.queue_free()
	if is_instance_valid(virtual_instance):
		virtual_instance.queue_free()
	await _wait_frames(3)
	var pure_pass := await _run_pure_vg_phase()
	if not pure_pass:
		get_tree().quit(4)
		return
	var subviewport_pass := await _run_pure_vg_subviewport_phase()
	get_tree().quit(0 if subviewport_pass else 5)

func _run_conventional_phase() -> bool:
	await _wait_frames(45)
	camera.position.z = 9.0
	await _wait_frames(100)
	var close_image := _frame_image()
	close_image.save_png("/tmp/flexi_f6_native_close.png")
	var half := close_image.get_width() / 2
	var virtual_luma := _mean_luminance(close_image, 20, half - 10)
	var conventional_luma := _mean_luminance(close_image, half + 10, close_image.get_width() - 20)
	var parity_difference := _mirrored_difference(close_image)
	virtual_instance.position.y = 0.22
	conventional_instance.position.y = 0.22
	await _wait_frames(8)
	var moved_image := _frame_image()
	var motion_difference := _mirrored_difference(moved_image)
	camera.position.z = 55.0
	await _wait_frames(60)
	_frame_image().save_png("/tmp/flexi_f6_native_coarsened.png")
	var passed := virtual_luma > 0.015 and conventional_luma > 0.015 and parity_difference < 0.16
	print("F6_NATIVE_METAL result=%s virtual_luma=%.6f conventional_luma=%.6f parity_difference=%.6f motion_parity=%.6f close=/tmp/flexi_f6_native_close.png coarsened=/tmp/flexi_f6_native_coarsened.png" % ["pass" if passed else "fail", virtual_luma, conventional_luma, parity_difference, motion_difference])
	return passed

func _run_pure_vg_phase() -> bool:
	pure_serialized_paths.clear()
	if not _load_pure_resources():
		return false
	if not _instantiate_pure_resources(self):
		return false
	var unique_count := pure_resources.size()
	var instance_count := pure_instances.size()
	var shared_count := 0
	for resource_index in PURE_RESOURCE_COUNT:
		for shared_index in PURE_INSTANCES_PER_RESOURCE:
			var instance: VirtualGeometryInstance3D = pure_instances[resource_index * PURE_INSTANCES_PER_RESOURCE + shared_index]
			if instance.virtual_geometry == pure_resources[resource_index] and shared_index > 0:
				shared_count += 1
	await _wait_frames(20)
	camera.position.z = PURE_FAR_DISTANCE
	await _wait_frames(55)
	var far_a_pass := _foreground_gate(_frame_image(), "root_far_a")
	camera.position.z = 8.0
	await _wait_frames(55)
	var near_pass := _foreground_gate(_frame_image(), "root_near")
	camera.position.z = PURE_FAR_DISTANCE
	await _wait_frames(25)
	var far_b_pass := _foreground_gate(_frame_image(), "root_far_b")
	# Unload every instance/resource, then load the serialized packages again and
	# submit the same topology. This is bounded and catches stale UMA pointers.
	await _remove_pure_instances()
	pure_resources.clear()
	await _wait_frames(2)
	for serialized_path in pure_serialized_paths:
		var reloaded := ResourceLoader.load(serialized_path, "VirtualGeometry", ResourceLoader.CACHE_MODE_IGNORE) as VirtualGeometry
		if reloaded == null or not reloaded.is_valid_virtual_geometry() or not reloaded.has_complete_material_bindings():
			return false
		pure_resources.append(reloaded)
	if not _instantiate_pure_resources(self):
		return false
	await _wait_frames(20)
	camera.position.z = 8.0
	await _wait_frames(45)
	camera.position.z = PURE_FAR_DISTANCE
	await _wait_frames(20)
	var no_conventional_mesh := find_children("*", "MeshInstance3D", true, false).is_empty()
	var passed := far_a_pass and near_pass and far_b_pass and unique_count == PURE_RESOURCE_COUNT and instance_count == PURE_RESOURCE_COUNT * PURE_INSTANCES_PER_RESOURCE and shared_count == 10 and pure_resources.size() == PURE_RESOURCE_COUNT and pure_instances.size() == PURE_RESOURCE_COUNT * PURE_INSTANCES_PER_RESOURCE and no_conventional_mesh
	print("F6_NATIVE_PURE_VG result=%s unique_resources=%d instances=%d shared_instances=%d unload_reload=true far_near_far=true conventional_mesh_instances=%d pure_vg=true" % ["pass" if passed else "fail", unique_count, instance_count, shared_count, find_children("*", "MeshInstance3D", true, false).size()])
	return passed

func _run_pure_vg_subviewport_phase() -> bool:
	# Detach the root-phase instances before constructing an independent 3D
	# world. This ensures the SubViewport submission is the only VG workload.
	await _remove_pure_instances()
	pure_resources.clear()
	await _wait_frames(2)
	subviewport = SubViewport.new()
	subviewport.name = "PureVirtualSubViewport"
	subviewport.size = Vector2i(800, 450)
	subviewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	subviewport.world_3d = World3D.new()
	add_child(subviewport)
	var sub_scene := Node3D.new()
	sub_scene.name = "PureVirtualSubViewportScene"
	subviewport.add_child(sub_scene)
	var sub_environment_node := WorldEnvironment.new()
	var sub_environment := Environment.new()
	sub_environment.background_mode = Environment.BG_COLOR
	sub_environment.background_color = CLEAR_COLOR
	sub_environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	sub_environment.ambient_light_color = Color(0.35, 0.38, 0.45)
	sub_environment.ambient_light_energy = 0.8
	sub_environment_node.environment = sub_environment
	sub_scene.add_child(sub_environment_node)
	var sub_light := DirectionalLight3D.new()
	sub_light.rotation_degrees = Vector3(-35, -20, 0)
	sub_light.light_energy = 1.7
	sub_scene.add_child(sub_light)
	var sub_camera := Camera3D.new()
	sub_camera.fov = 48.0
	sub_camera.position = Vector3(0, 0, 40)
	sub_camera.current = true
	sub_scene.add_child(sub_camera)
	if not _load_pure_resources("user://f6_native_pure_subviewport"):
		return false
	var unique_count := pure_resources.size()
	if not _instantiate_pure_resources(sub_scene):
		return false
	var instance_count := pure_instances.size()
	var shared_count := 0
	for resource_index in PURE_RESOURCE_COUNT:
		for shared_index in PURE_INSTANCES_PER_RESOURCE:
			var instance: VirtualGeometryInstance3D = pure_instances[resource_index * PURE_INSTANCES_PER_RESOURCE + shared_index]
			if instance.virtual_geometry == pure_resources[resource_index] and shared_index > 0:
				shared_count += 1
	await _wait_frames(30)
	sub_camera.position.z = PURE_FAR_DISTANCE
	await _wait_frames(55)
	var far_a_pass := _foreground_gate(subviewport.get_texture().get_image(), "subviewport_far_a")
	sub_camera.position.z = 8.0
	await _wait_frames(55)
	# Reading the target after UPDATE_ALWAYS frames makes this a real native
	# viewport submission, rather than merely constructing an offscreen tree.
	var near_image := subviewport.get_texture().get_image()
	var rendered := near_image.get_width() == 800 and near_image.get_height() == 450
	var near_pass := rendered and _foreground_gate(near_image, "subviewport_near")
	sub_camera.position.z = PURE_FAR_DISTANCE
	await _wait_frames(25)
	var far_b_pass := _foreground_gate(subviewport.get_texture().get_image(), "subviewport_far_b")
	await _remove_pure_instances()
	pure_resources.clear()
	await _wait_frames(2)
	for serialized_path in pure_serialized_paths:
		if not serialized_path.begins_with("user://f6_native_pure_subviewport_"):
			continue
		var reloaded := ResourceLoader.load(serialized_path, "VirtualGeometry", ResourceLoader.CACHE_MODE_IGNORE) as VirtualGeometry
		if reloaded == null or not reloaded.is_valid_virtual_geometry() or not reloaded.has_complete_material_bindings():
			return false
		pure_resources.append(reloaded)
	if pure_resources.size() != PURE_RESOURCE_COUNT or not _instantiate_pure_resources(sub_scene):
		return false
	await _wait_frames(25)
	sub_camera.position.z = 8.0
	await _wait_frames(45)
	sub_camera.position.z = PURE_FAR_DISTANCE
	await _wait_frames(20)
	var no_conventional_mesh := sub_scene.find_children("*", "MeshInstance3D", true, false).is_empty()
	var passed := far_a_pass and near_pass and far_b_pass and rendered and subviewport.render_target_update_mode == SubViewport.UPDATE_ALWAYS and unique_count == PURE_RESOURCE_COUNT and instance_count == PURE_RESOURCE_COUNT * PURE_INSTANCES_PER_RESOURCE and shared_count == 10 and pure_resources.size() == PURE_RESOURCE_COUNT and pure_instances.size() == PURE_RESOURCE_COUNT * PURE_INSTANCES_PER_RESOURCE and no_conventional_mesh
	print("F6_NATIVE_PURE_VG_SUBVIEWPORT result=%s unique_resources=%d instances=%d shared_instances=%d unload_reload=true far_near_far=true update_always=true conventional_mesh_instances=%d" % ["pass" if passed else "fail", unique_count, instance_count, shared_count, sub_scene.find_children("*", "MeshInstance3D", true, false).size()])
	await _remove_pure_instances()
	pure_resources.clear()
	if is_instance_valid(subviewport):
		subviewport.queue_free()
	await _wait_frames(2)
	for serialized_path in pure_serialized_paths:
		DirAccess.remove_absolute(ProjectSettings.globalize_path(serialized_path))
	return passed
