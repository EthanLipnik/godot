@tool
extends Node3D

var animated_mesh: MeshInstance3D
var scene_camera: Camera3D
var animate_deformation := true
var deformation_phase := 0.0
var checker_material: StandardMaterial3D
var checker_texture: ImageTexture

func _make_opaque_checker_texture() -> ImageTexture:
	# A tiny generated texture makes this fixture self-contained while exercising
	# the same opaque UV0 albedo binding used by imported StandardMaterial3D data.
	var image := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	for y in image.get_height():
		for x in image.get_width():
			var dark_square := ((x >> 1) + (y >> 1)) & 1
			image.set_pixel(x, y, Color(0.03, 0.12, 0.85) if dark_square == 0 else Color(0.95, 0.26, 0.035))
	return ImageTexture.create_from_image(image)

func _process(delta: float) -> void:
	if Engine.is_editor_hint():
		return
	if animate_deformation and animated_mesh:
		deformation_phase += delta * 5.0
		animated_mesh.set_blend_shape_value(0, sin(deformation_phase) * 0.5 + 0.5)

func _ready() -> void:
	_build_scene()
	if Engine.is_editor_hint():
		animate_deformation = false
		if "--validate-hybrid-editor-overlay" in OS.get_cmdline_user_args():
			await _capture_editor_overlay_regression()
			return
		if "--validate-hybrid-editor" in OS.get_cmdline_user_args():
			await _capture_editor_viewport()
		return
	if "--benchmark-hybrid" in OS.get_cmdline_user_args():
		await _run_benchmark()
		return
	if "--validate-hybrid-texture-transport" in OS.get_cmdline_user_args():
		await _validate_opaque_texture_transport()
		return
	ProjectSettings.set_setting("rendering/hybrid_renderer/mode", 2)
	for _frame in 12:
		await get_tree().process_frame
	animate_deformation = false
	ProjectSettings.set_setting("rendering/hybrid_renderer/mode", 0)
	for _frame in 8:
		await get_tree().process_frame
	var raster_image := get_viewport().get_texture().get_image()
	if raster_image.is_empty():
		push_error("Hybrid validation capture is empty.")
		get_tree().quit(2)
		return
	ProjectSettings.set_setting("rendering/hybrid_renderer/mode", 2)
	for _frame in 8:
		await get_tree().process_frame
	var hybrid_image := get_viewport().get_texture().get_image()
	var difference := _mean_absolute_rgb_difference(raster_image, hybrid_image)
	if difference < 0.0001:
		push_error("Hybrid renderer did not materially change the validation frame: %f" % difference)
		get_tree().quit(3)
		return
	var original_camera_transform := scene_camera.global_transform
	scene_camera.position.x += 1.4
	scene_camera.look_at(Vector3(0.0, 0.8, 0.0))
	await get_tree().process_frame
	var moving_camera_image := get_viewport().get_texture().get_image()
	for _frame in 16:
		await get_tree().process_frame
	var settled_camera_image := get_viewport().get_texture().get_image()
	var disocclusion_difference := _mean_absolute_rgb_difference(hybrid_image, settled_camera_image)
	if moving_camera_image.is_empty() or settled_camera_image.is_empty() or disocclusion_difference < 0.001:
		push_error("Hybrid moving-camera disocclusion fixture did not produce a distinct valid frame: %f" % disocclusion_difference)
		get_tree().quit(5)
		return
	scene_camera.global_transform = original_camera_transform
	var raster_path := "user://hybrid_runtime_validation_raster.png"
	var hybrid_path := "user://hybrid_runtime_validation_hybrid.png"
	var moving_path := "user://hybrid_runtime_validation_disocclusion.png"
	if raster_image.save_png(raster_path) != OK or hybrid_image.save_png(hybrid_path) != OK or moving_camera_image.save_png(moving_path) != OK:
		push_error("Could not save hybrid validation captures.")
		get_tree().quit(4)
		return
	print("HYBRID_VALIDATION_MEAN_ABS_RGB_DIFFERENCE=", difference)
	print("HYBRID_VALIDATION_DISOCCLUSION_DIFFERENCE=", disocclusion_difference)
	print("HYBRID_VALIDATION_CAPTURE=", ProjectSettings.globalize_path(hybrid_path))
	print("HYBRID_VALIDATION_DISOCCLUSION_CAPTURE=", ProjectSettings.globalize_path(moving_path))
	get_tree().quit()

func _run_benchmark() -> void:
	ProjectSettings.set_setting("rendering/hybrid_renderer/mode", 2)
	# Counter pass boundaries intentionally perturb the measured command stream.
	# The benchmark records the normal runtime path; validation mode records counters.
	ProjectSettings.set_setting("rendering/hybrid_renderer/diagnostics/collect_gpu_timings", false)
	for _frame in 60:
		await get_tree().process_frame
	var frame_times_ms: Array[float] = []
	var previous_tick := Time.get_ticks_usec()
	for _frame in 600:
		await get_tree().process_frame
		var tick := Time.get_ticks_usec()
		frame_times_ms.push_back(float(tick - previous_tick) / 1000.0)
		previous_tick = tick
	frame_times_ms.sort()
	var total := 0.0
	for frame_time in frame_times_ms:
		total += frame_time
	var mean := total / frame_times_ms.size()
	var p99 := frame_times_ms[int(ceil(frame_times_ms.size() * 0.99)) - 1]
	print("HYBRID_BENCHMARK_FRAMES=", frame_times_ms.size())
	print("HYBRID_BENCHMARK_MEAN_FRAME_MS=", mean)
	print("HYBRID_BENCHMARK_P99_FRAME_MS=", p99)
	get_tree().quit()

func _mean_absolute_rgb_difference(first: Image, second: Image) -> float:
	if first.get_size() != second.get_size() or first.is_empty() or second.is_empty():
		return 0.0
	var total := 0.0
	for y in first.get_height():
		for x in first.get_width():
			var a := first.get_pixel(x, y)
			var b := second.get_pixel(x, y)
			total += absf(a.r - b.r) + absf(a.g - b.g) + absf(a.b - b.b)
	return total / float(first.get_width() * first.get_height() * 3)

func _mean_absolute_rgb_difference_region(first: Image, second: Image, region: Rect2i) -> float:
	if first.get_size() != second.get_size() or first.is_empty() or second.is_empty():
		return 0.0
	var clipped := region.intersection(Rect2i(Vector2i.ZERO, first.get_size()))
	if clipped.size.x <= 0 or clipped.size.y <= 0:
		return 0.0
	var total := 0.0
	for y in range(clipped.position.y, clipped.end.y):
		for x in range(clipped.position.x, clipped.end.x):
			var a := first.get_pixel(x, y)
			var b := second.get_pixel(x, y)
			total += absf(a.r - b.r) + absf(a.g - b.g) + absf(a.b - b.b)
	return total / float(clipped.size.x * clipped.size.y * 3)

func _validate_opaque_texture_transport() -> void:
	if checker_material == null or checker_texture == null:
		push_error("Hybrid texture transport fixture has no checker material or texture.")
		get_tree().quit(12)
		return

	# The checker is deliberately behind the active camera. Its visible base color
	# must not perturb Forward+ primary raster output; it is positioned in the
	# glossy floor's ray-reflection path so only secondary hybrid evaluation can
	# introduce a color difference in the floor ROI below.
	animate_deformation = false
	# This A/B measures transport rather than MetalFX's intentionally changing
	# temporal reconstruction state. The normal validation retains MetalFX.
	get_viewport().scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	ProjectSettings.set_setting("rendering/hybrid_renderer/mode", 0)
	checker_material.albedo_texture = null
	for _frame in 16:
		await get_tree().process_frame
	var raster_scalar := get_viewport().get_texture().get_image()
	checker_material.albedo_texture = checker_texture
	for _frame in 16:
		await get_tree().process_frame
	var raster_textured := get_viewport().get_texture().get_image()
	var raster_control_difference := _mean_absolute_rgb_difference(raster_scalar, raster_textured)
	if raster_control_difference > 0.00005:
		push_error("Off-camera texture changed primary raster control: %f" % raster_control_difference)
		get_tree().quit(13)
		return

	ProjectSettings.set_setting("rendering/hybrid_renderer/mode", 2)
	checker_material.albedo_texture = null
	for _frame in 24:
		await get_tree().process_frame
	var hybrid_scalar := get_viewport().get_texture().get_image()
	checker_material.albedo_texture = checker_texture
	for _frame in 24:
		await get_tree().process_frame
	var hybrid_textured := get_viewport().get_texture().get_image()
	var floor_roi := Rect2i(Vector2i(int(hybrid_scalar.get_width() * 0.30), int(hybrid_scalar.get_height() * 0.54)), Vector2i(int(hybrid_scalar.get_width() * 0.40), int(hybrid_scalar.get_height() * 0.34)))
	var transport_difference := _mean_absolute_rgb_difference_region(hybrid_scalar, hybrid_textured, floor_roi)
	if transport_difference < 0.0002:
		push_error("Opaque UV0 texture did not measurably affect the secondary transport floor ROI: %f" % transport_difference)
		get_tree().quit(14)
		return

	var base_path := "user://hybrid_opaque_uv0_transport_"
	if raster_scalar.save_png(base_path + "raster_scalar.png") != OK or raster_textured.save_png(base_path + "raster_textured.png") != OK or hybrid_scalar.save_png(base_path + "hybrid_scalar.png") != OK or hybrid_textured.save_png(base_path + "hybrid_textured.png") != OK:
		push_error("Could not save opaque UV0 transport captures.")
		get_tree().quit(15)
		return
	print("HYBRID_UV0_TRANSPORT_RASTER_CONTROL_MAE=", raster_control_difference)
	print("HYBRID_UV0_TRANSPORT_FLOOR_ROI_MAE=", transport_difference)
	print("HYBRID_UV0_TRANSPORT_CAPTURE_PREFIX=", ProjectSettings.globalize_path(base_path))
	get_tree().quit()

func _capture_editor_viewport() -> void:
	for _frame in 60:
		await get_tree().process_frame
	var editor_viewport := EditorInterface.get_editor_viewport_3d(0)
	var editor_camera := editor_viewport.get_camera_3d()
	for _frame in 16:
		editor_camera.global_transform = scene_camera.global_transform
		editor_camera.fov = scene_camera.fov
		await get_tree().process_frame
	var image := editor_viewport.get_texture().get_image()
	if image.is_empty():
		push_error("Hybrid editor viewport capture is empty.")
		get_tree().quit(5)
		return
	var capture_path := "user://hybrid_runtime_validation_editor.png"
	if image.save_png(capture_path) != OK:
		push_error("Could not save hybrid editor viewport capture.")
		get_tree().quit(6)
		return
	# The editor viewport texture contains reconstructed scene color only. Capture
	# the root window separately so editor-owned post-scene overlays are regressible.
	var composite_image := get_tree().root.get_texture().get_image()
	if composite_image.is_empty():
		push_error("Hybrid editor composite capture is empty.")
		get_tree().quit(7)
		return
	var composite_path := "user://hybrid_runtime_validation_editor_composite.png"
	if composite_image.save_png(composite_path) != OK:
		push_error("Could not save hybrid editor composite capture.")
		get_tree().quit(8)
		return
	print("HYBRID_EDITOR_CAPTURE=", ProjectSettings.globalize_path(capture_path))
	print("HYBRID_EDITOR_COMPOSITE_CAPTURE=", ProjectSettings.globalize_path(composite_path))
	get_tree().quit()

func _editor_overlay_line_width(image: Image) -> float:
	# Locate the editor's green/cyan world-origin line in the center viewport.
	# The score deliberately ignores low-saturation ray noise and scene lighting.
	var min_x := int(image.get_width() * 0.28)
	var max_x := int(image.get_width() * 0.76)
	var min_y := int(image.get_height() * 0.16)
	var max_y := int(image.get_height() * 0.95)
	var column_scores: Array[int] = []
	var highest_score := 0
	var peak_index := 0
	for x in range(min_x, max_x):
		var score := 0
		for y in range(min_y, max_y):
			var pixel := image.get_pixel(x, y)
			if pixel.g > pixel.r * 1.25 + 0.10 and pixel.g > pixel.b * 1.05 + 0.04:
				score += 1
		column_scores.append(score)
		if score > highest_score:
			highest_score = score
			peak_index = column_scores.size() - 1
	if highest_score == 0:
		return 0.0
	var threshold := maxi(1, int(ceil(highest_score * 0.30)))
	var left := peak_index
	while left > 0 and column_scores[left - 1] >= threshold:
		left -= 1
	var right := peak_index
	while right + 1 < column_scores.size() and column_scores[right + 1] >= threshold:
		right += 1
	return float(right - left + 1)

func _capture_editor_overlay_regression() -> void:
	var editor_viewport := EditorInterface.get_editor_viewport_3d(0)
	var editor_camera := editor_viewport.get_camera_3d()
	var original_hybrid_mode := int(ProjectSettings.get_setting("rendering/hybrid_renderer/mode"))
	var original_scaling_mode := editor_viewport.scaling_3d_mode
	var original_scaling_scale := editor_viewport.scaling_3d_scale
	editor_camera.global_transform = scene_camera.global_transform
	editor_camera.fov = scene_camera.fov

	var cases := [
		{"name": "native", "hybrid": 0, "scaling": Viewport.SCALING_3D_MODE_BILINEAR},
		{"name": "metalfx_temporal", "hybrid": 0, "scaling": Viewport.SCALING_3D_MODE_METALFX_TEMPORAL},
		{"name": "metalfx_denoised", "hybrid": 2, "scaling": Viewport.SCALING_3D_MODE_METALFX_TEMPORAL},
	]
	var captures: Dictionary = {}
	for capture_case in cases:
		ProjectSettings.set_setting("rendering/hybrid_renderer/mode", capture_case.hybrid)
		editor_viewport.scaling_3d_mode = capture_case.scaling
		editor_viewport.scaling_3d_scale = 0.67
		for _frame in 36:
			await get_tree().process_frame
		var image := get_tree().root.get_texture().get_image()
		if image.is_empty():
			push_error("Hybrid editor overlay regression capture is empty.")
			get_tree().quit(9)
			return
		var name: String = capture_case.name
		var path := "user://hybrid_editor_overlay_%s.png" % name
		if image.save_png(path) != OK:
			push_error("Could not save hybrid editor overlay regression capture.")
			get_tree().quit(10)
			return
		captures[name] = image
		print("HYBRID_EDITOR_OVERLAY_", name.to_upper(), "_LINE_WIDTH=", _editor_overlay_line_width(image))
		print("HYBRID_EDITOR_OVERLAY_", name.to_upper(), "_CAPTURE=", ProjectSettings.globalize_path(path))
		var orbit_transform := editor_camera.global_transform
		orbit_transform.basis = Basis(Vector3.UP, 0.035) * orbit_transform.basis
		editor_camera.global_transform = orbit_transform
		await get_tree().process_frame
		var orbit_image := get_tree().root.get_texture().get_image()
		for _frame in 16:
			await get_tree().process_frame
		var settled_image := get_tree().root.get_texture().get_image()
		var orbit_path := "user://hybrid_editor_overlay_%s_orbit.png" % name
		var settled_path := "user://hybrid_editor_overlay_%s_orbit_settled.png" % name
		if orbit_image.is_empty() or settled_image.is_empty() or orbit_image.save_png(orbit_path) != OK or settled_image.save_png(settled_path) != OK:
			push_error("Could not save hybrid editor overlay orbit regression capture.")
			get_tree().quit(11)
			return
		var orbit_width := _editor_overlay_line_width(orbit_image)
		var settled_width := _editor_overlay_line_width(settled_image)
		print("HYBRID_EDITOR_OVERLAY_", name.to_upper(), "_ORBIT_LINE_WIDTH=", orbit_width)
		print("HYBRID_EDITOR_OVERLAY_", name.to_upper(), "_ORBIT_SETTLED_LINE_WIDTH=", settled_width)
		print("HYBRID_EDITOR_OVERLAY_", name.to_upper(), "_ORBIT_LINE_WIDTH_DELTA=", absf(orbit_width - settled_width))
		editor_camera.global_transform = scene_camera.global_transform

	var native_image: Image = captures["native"]
	var denoised_image: Image = captures["metalfx_denoised"]
	print("HYBRID_EDITOR_OVERLAY_NATIVE_DENOISED_MEAN_ABS_RGB=", _mean_absolute_rgb_difference(native_image, denoised_image))
	ProjectSettings.set_setting("rendering/hybrid_renderer/mode", original_hybrid_mode)
	editor_viewport.scaling_3d_mode = original_scaling_mode
	editor_viewport.scaling_3d_scale = original_scaling_scale
	get_tree().quit()

func _build_scene() -> void:
	if not "--validate-hybrid-no-environment" in OS.get_cmdline_user_args():
		var environment := WorldEnvironment.new()
		var environment_resource := Environment.new()
		environment_resource.background_mode = Environment.BG_COLOR
		environment_resource.background_color = Color(0.025, 0.04, 0.075)
		environment_resource.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
		environment_resource.ambient_light_color = Color(0.3, 0.38, 0.5)
		environment_resource.ambient_light_energy = 0.45
		environment.environment = environment_resource
		add_child(environment)

	scene_camera = Camera3D.new()
	scene_camera.position = Vector3(0.0, 2.8, 7.5)
	scene_camera.look_at_from_position(scene_camera.position, Vector3(0.0, 0.8, 0.0))
	scene_camera.current = true
	add_child(scene_camera)

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-52.0, -28.0, 0.0)
	sun.light_energy = 1.5
	sun.shadow_enabled = true
	add_child(sun)

	var emitter_material := StandardMaterial3D.new()
	emitter_material.albedo_color = Color(1.0, 0.9, 0.72)
	emitter_material.emission_enabled = true
	emitter_material.emission = Color(1.0, 0.82, 0.58)
	emitter_material.emission_energy_multiplier = 12.0
	var emitter_mesh := PlaneMesh.new()
	emitter_mesh.size = Vector2(2.0, 1.4)
	emitter_mesh.material = emitter_material
	var emitter := MeshInstance3D.new()
	emitter.mesh = emitter_mesh
	emitter.position = Vector3(0.0, 4.5, -0.8)
	emitter.rotation_degrees.x = 180.0
	add_child(emitter)

	var floor_material := StandardMaterial3D.new()
	floor_material.albedo_color = Color(0.12, 0.13, 0.15)
	floor_material.metallic = 0.75
	floor_material.roughness = 0.18
	var floor_mesh := BoxMesh.new()
	floor_mesh.size = Vector3(12.0, 0.2, 12.0)
	floor_mesh.material = floor_material
	var floor_instance := MeshInstance3D.new()
	floor_instance.mesh = floor_mesh
	floor_instance.position.y = -0.1
	add_child(floor_instance)

	var sphere_material := StandardMaterial3D.new()
	sphere_material.albedo_color = Color(0.8, 0.08, 0.035)
	sphere_material.metallic = 0.15
	sphere_material.roughness = 0.22
	var sphere_mesh := SphereMesh.new()
	sphere_mesh.radius = 1.0
	sphere_mesh.height = 2.0
	sphere_mesh.material = sphere_material
	var sphere := MeshInstance3D.new()
	sphere.mesh = sphere_mesh
	sphere.position = Vector3(-1.3, 1.0, 0.0)
	add_child(sphere)

	var box_material := StandardMaterial3D.new()
	box_material.albedo_color = Color(0.04, 0.32, 0.8)
	box_material.metallic = 0.8
	box_material.roughness = 0.12
	var box_mesh := BoxMesh.new()
	box_mesh.size = Vector3(1.6, 1.6, 1.6)
	box_mesh.material = box_material
	var box := MeshInstance3D.new()
	box.mesh = box_mesh
	box.position = Vector3(1.35, 0.8, -0.4)
	box.rotation_degrees = Vector3(0.0, 28.0, 0.0)
	add_child(box)

	checker_material = StandardMaterial3D.new()
	checker_material.albedo_color = Color.WHITE
	checker_texture = _make_opaque_checker_texture()
	if not "--benchmark-hybrid-scalar" in OS.get_cmdline_user_args():
		checker_material.albedo_texture = checker_texture
	checker_material.roughness = 0.72
	var checker_mesh := BoxMesh.new()
	checker_mesh.size = Vector3(5.0, 3.0, 0.12)
	checker_mesh.material = checker_material
	var checker := MeshInstance3D.new()
	checker.name = "OpaqueUV0SecondaryChecker"
	checker.mesh = checker_mesh
	checker.position = Vector3(0.0, 3.7, 10.2)
	add_child(checker)

	# Behind the camera: absent from raster visibility but required in the ray scene.
	var offscreen_mesh := BoxMesh.new()
	offscreen_mesh.size = Vector3(2.0, 2.0, 2.0)
	offscreen_mesh.material = box_material
	var offscreen_reflector := MeshInstance3D.new()
	offscreen_reflector.mesh = offscreen_mesh
	offscreen_reflector.position = Vector3(0.0, 1.0, 10.0)
	add_child(offscreen_reflector)

	var deforming_mesh := ArrayMesh.new()
	deforming_mesh.add_blend_shape("Lift")
	deforming_mesh.blend_shape_mode = Mesh.BLEND_SHAPE_MODE_RELATIVE
	var base_arrays: Array = []
	base_arrays.resize(Mesh.ARRAY_MAX)
	base_arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([
		Vector3(-0.5, 0.0, 0.0), Vector3(0.5, 0.0, 0.0), Vector3(0.5, 1.0, 0.0), Vector3(-0.5, 1.0, 0.0)
	])
	base_arrays[Mesh.ARRAY_NORMAL] = PackedVector3Array([Vector3.BACK, Vector3.BACK, Vector3.BACK, Vector3.BACK])
	base_arrays[Mesh.ARRAY_INDEX] = PackedInt32Array([0, 1, 2, 0, 2, 3])
	var lift_arrays: Array = []
	lift_arrays.resize(Mesh.ARRAY_MAX)
	lift_arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([
		Vector3.ZERO, Vector3.ZERO, Vector3(0.0, 0.45, 0.0), Vector3(0.0, 0.45, 0.0)
	])
	lift_arrays[Mesh.ARRAY_NORMAL] = PackedVector3Array([Vector3.ZERO, Vector3.ZERO, Vector3.ZERO, Vector3.ZERO])
	deforming_mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, base_arrays, [lift_arrays])
	deforming_mesh.surface_set_material(0, sphere_material)
	animated_mesh = MeshInstance3D.new()
	animated_mesh.mesh = deforming_mesh
	animated_mesh.position = Vector3(0.0, 0.02, 1.0)
	add_child(animated_mesh)
