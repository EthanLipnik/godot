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
	if "--validate-baked-visibility" in OS.get_cmdline_user_args():
		await _validate_baked_visibility()
		return
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
	if "--validate-hybrid-diffuse-transport" in OS.get_cmdline_user_args():
		await _validate_diffuse_transport()
		return
	if "--validate-hybrid-omni-diffuse-transport" in OS.get_cmdline_user_args():
		await _validate_omni_diffuse_transport()
		return
	if "--validate-hybrid-transport-culling" in OS.get_cmdline_user_args():
		await _validate_transport_culling()
		return
	if "--validate-hybrid-viewport-toggle" in OS.get_cmdline_user_args():
		await _validate_hybrid_viewport_toggle()
		return
	if "--validate-hybrid-temporal-detail" in OS.get_cmdline_user_args():
		await _validate_hybrid_temporal_detail()
		return
	ProjectSettings.set_setting("rendering/hybrid_renderer/mode", 1)
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
	ProjectSettings.set_setting("rendering/hybrid_renderer/mode", 1)
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

func _validate_hybrid_viewport_toggle() -> void:
	var viewport_rid := get_viewport().get_viewport_rid()
	var original_hybrid_mode := int(ProjectSettings.get_setting("rendering/hybrid_renderer/mode"))
	ProjectSettings.set_setting("rendering/hybrid_renderer/mode", 0)
	# Enabled viewports inherit the project mode. This begins disabled, then
	# changes live to Hybrid without changing the viewport override.
	RenderingServer.viewport_set_hybrid_renderer_enabled(viewport_rid, true)
	for _frame in 4:
		await get_tree().process_frame
	var raster_image := get_viewport().get_texture().get_image()
	ProjectSettings.set_setting("rendering/hybrid_renderer/mode", 1)
	for _frame in 4:
		await get_tree().process_frame
	var hybrid_image := get_viewport().get_texture().get_image()
	if raster_image.is_empty() or hybrid_image.is_empty():
		push_error("Hybrid viewport-toggle validation captured an empty frame.")
		get_tree().quit(7)
		return
	var raster_luminance := 0.0
	for y in raster_image.get_height():
		for x in raster_image.get_width():
			var pixel := raster_image.get_pixel(x, y)
			if not is_finite(pixel.r) or not is_finite(pixel.g) or not is_finite(pixel.b):
				push_error("Hybrid viewport-toggle validation captured non-finite raster output.")
				get_tree().quit(8)
				return
			raster_luminance += (pixel.r + pixel.g + pixel.b) / 3.0
	raster_luminance /= float(raster_image.get_width() * raster_image.get_height())
	var difference := _mean_absolute_rgb_difference(raster_image, hybrid_image)
	if raster_luminance <= 0.001 or difference < 0.0001:
		push_error("Hybrid viewport-toggle validation did not produce a non-black raster-to-hybrid transition: raster=%f difference=%f" % [raster_luminance, difference])
		get_tree().quit(9)
		return
	# Disabling remains an explicit viewport override even while the project mode
	# is enabled, which is the editor's opt-in behavior.
	RenderingServer.viewport_set_hybrid_renderer_enabled(viewport_rid, false)
	for _frame in 4:
		await get_tree().process_frame
	var disabled_image := get_viewport().get_texture().get_image()
	ProjectSettings.set_setting("rendering/hybrid_renderer/mode", original_hybrid_mode)
	if disabled_image.is_empty() or _mean_absolute_rgb_difference(hybrid_image, disabled_image) < 0.0001:
		push_error("Hybrid viewport-toggle validation did not preserve the explicit disabled override.")
		get_tree().quit(10)
		return
	print("HYBRID_VIEWPORT_TOGGLE_VALIDATION=PASS")
	get_tree().quit()

func _run_benchmark() -> void:
	ProjectSettings.set_setting("rendering/hybrid_renderer/mode", 1)
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

func _force_renderer_draws(viewport: SubViewport, count: int) -> void:
	# The temporal-detail fixture uses an UPDATE_ALWAYS SubViewport. Unlike an
	# unchanged on-demand root window, the subpixel capture-camera pulse also
	# marks its scene dirty for every force_draw. The 0.0001 m pulse is far below
	# one output pixel and the original pose is restored before every capture.
	var capture_camera := viewport.get_camera_3d()
	var base_position := capture_camera.position
	for _draw in count:
		capture_camera.position.x = base_position.x + (0.0001 if (_draw & 1) == 0 else -0.0001)
		RenderingServer.force_draw(false, 1.0 / 60.0)
		RenderingServer.force_sync()
		await get_tree().process_frame
	capture_camera.position = base_position
	RenderingServer.force_draw(false, 1.0 / 60.0)
	RenderingServer.force_sync()
	await get_tree().process_frame

func _create_temporal_capture_viewport() -> SubViewport:
	var capture_viewport := SubViewport.new()
	capture_viewport.name = "TemporalDetailCaptureViewport"
	capture_viewport.size = get_viewport().size
	capture_viewport.world_3d = get_viewport().world_3d
	capture_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	capture_viewport.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	capture_viewport.scaling_3d_scale = 0.5
	add_child(capture_viewport)
	var capture_camera := Camera3D.new()
	capture_camera.global_transform = scene_camera.global_transform
	capture_camera.fov = scene_camera.fov
	capture_camera.current = true
	capture_viewport.add_child(capture_camera)
	return capture_viewport

func _luminance(pixel: Color) -> float:
	return pixel.r * 0.2126 + pixel.g * 0.7152 + pixel.b * 0.0722

func _edge_detail_energy(image: Image, region: Rect2i) -> float:
	if image.is_empty():
		return 0.0
	var clipped := region.intersection(Rect2i(Vector2i.ZERO, image.get_size()))
	if clipped.size.x < 2 or clipped.size.y < 2:
		return 0.0
	var total := 0.0
	var sample_count := 0
	for y in range(clipped.position.y, clipped.end.y - 1):
		for x in range(clipped.position.x, clipped.end.x - 1):
			var center := _luminance(image.get_pixel(x, y))
			total += absf(center - _luminance(image.get_pixel(x + 1, y)))
			total += absf(center - _luminance(image.get_pixel(x, y + 1)))
			sample_count += 2
	return total / float(sample_count)

func _validate_hybrid_temporal_detail() -> void:
	# Generic, self-contained high-frequency/morph fixture. It intentionally
	# uses force_draw rather than counting GDScript ticks: the engine diagnostic
	# at frames 60/120 is the assertion that MetalFX actually received history.
	animate_deformation = false
	var viewport := _create_temporal_capture_viewport()
	var image_region := Rect2i(Vector2i(int(viewport.size.x * 0.15), int(viewport.size.y * 0.15)), Vector2i(int(viewport.size.x * 0.70), int(viewport.size.y * 0.70)))
	var renderer_frames_before := Engine.get_frames_drawn()
	var detail_material := StandardMaterial3D.new()
	detail_material.albedo_color = Color.WHITE
	detail_material.albedo_texture = checker_texture
	detail_material.metallic = 0.12
	detail_material.roughness = 0.30
	var detail_mesh := SphereMesh.new()
	detail_mesh.radius = 1.18
	detail_mesh.height = 2.36
	detail_mesh.radial_segments = 64
	detail_mesh.rings = 32
	detail_mesh.material = detail_material
	var detail_instance := MeshInstance3D.new()
	detail_instance.name = "HighFrequencyOpaqueDetail"
	detail_instance.mesh = detail_mesh
	detail_instance.position = Vector3(0.0, 1.35, 0.55)
	add_child(detail_instance)

	RenderingServer.viewport_set_hybrid_renderer_mode(viewport.get_viewport_rid(), 0)
	viewport.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	await _force_renderer_draws(viewport, 8)
	var raster_image := viewport.get_texture().get_image()

	viewport.scaling_3d_mode = Viewport.SCALING_3D_MODE_METALFX_TEMPORAL
	await _force_renderer_draws(viewport, 132)
	var ordinary_metalfx_image := viewport.get_texture().get_image()

	RenderingServer.viewport_set_hybrid_renderer_mode(viewport.get_viewport_rid(), 2)
	await _force_renderer_draws(viewport, 132)
	var denoised_image := viewport.get_texture().get_image()
	var hybrid_renderer_frames := Engine.get_frames_drawn() - renderer_frames_before
	await _force_renderer_draws(viewport, 12)
	var denoised_static_repeat := viewport.get_texture().get_image()

	# A rigid displacement creates a true primary disocclusion in addition to
	# the blend-shape motion below. The renderer's periodic reactive diagnostic
	# must observe this transition while the static warmup stays nonreactive.
	var original_detail_position := detail_instance.position
	detail_instance.position.x += 0.85
	await _force_renderer_draws(viewport, 18)
	var disocclusion_image := viewport.get_texture().get_image()
	detail_instance.position = original_detail_position
	await _force_renderer_draws(viewport, 18)
	var returned_detail_image := viewport.get_texture().get_image()

	# Exercise the same blend-shape velocity path that skinned/morphing opaque
	# geometry uses, then freeze it and measure the residual after new history.
	animate_deformation = true
	await _force_renderer_draws(viewport, 12)
	var moving_image := viewport.get_texture().get_image()
	animate_deformation = false
	await _force_renderer_draws(viewport, 18)
	var settled_motion_image := viewport.get_texture().get_image()

	if raster_image.is_empty() or ordinary_metalfx_image.is_empty() or denoised_image.is_empty() or denoised_static_repeat.is_empty() or disocclusion_image.is_empty() or returned_detail_image.is_empty() or moving_image.is_empty() or settled_motion_image.is_empty():
		push_error("Hybrid temporal-detail validation captured an empty image.")
		get_tree().quit(32)
		return
	var raster_detail := _edge_detail_energy(raster_image, image_region)
	var ordinary_detail := _edge_detail_energy(ordinary_metalfx_image, image_region)
	var denoised_detail := _edge_detail_energy(denoised_image, image_region)
	var static_noise := _mean_absolute_rgb_difference_region(denoised_image, denoised_static_repeat, image_region)
	var disocclusion_delta := _mean_absolute_rgb_difference_region(denoised_static_repeat, disocclusion_image, image_region)
	var return_settle_delta := _mean_absolute_rgb_difference_region(denoised_static_repeat, returned_detail_image, image_region)
	var motion_delta := _mean_absolute_rgb_difference_region(denoised_static_repeat, moving_image, image_region)
	var motion_settle_delta := _mean_absolute_rgb_difference_region(moving_image, settled_motion_image, image_region)
	var denoised_detail_ratio := denoised_detail / maxf(raster_detail, 0.000001)
	if hybrid_renderer_frames < 120 or denoised_detail_ratio < 0.20 or static_noise > 0.080 or disocclusion_delta < 0.0005 or return_settle_delta > 0.220 or motion_delta < 0.0001 or motion_settle_delta > 0.220:
		push_error("Hybrid temporal-detail validation failed: renderer_frames=%d detail_ratio=%f static_noise=%f disocclusion=%f return_settle=%f motion_delta=%f motion_settle=%f" % [hybrid_renderer_frames, denoised_detail_ratio, static_noise, disocclusion_delta, return_settle_delta, motion_delta, motion_settle_delta])
		get_tree().quit(33)
		return
	var capture_prefix := "user://hybrid_temporal_detail_"
	if raster_image.save_png(capture_prefix + "raster.png") != OK or ordinary_metalfx_image.save_png(capture_prefix + "ordinary_metalfx.png") != OK or denoised_image.save_png(capture_prefix + "denoised.png") != OK or settled_motion_image.save_png(capture_prefix + "motion_settled.png") != OK:
		push_error("Could not save hybrid temporal-detail captures.")
		get_tree().quit(34)
		return
	print("HYBRID_TEMPORAL_FORCE_DRAW_REQUESTS=132")
	print("HYBRID_TEMPORAL_ACTUAL_RENDERER_FRAMES=", hybrid_renderer_frames)
	print("HYBRID_TEMPORAL_RASTER_EDGE_DETAIL=", raster_detail)
	print("HYBRID_TEMPORAL_ORDINARY_METALFX_EDGE_DETAIL=", ordinary_detail)
	print("HYBRID_TEMPORAL_DENOISED_EDGE_DETAIL=", denoised_detail)
	print("HYBRID_TEMPORAL_DENOISED_DETAIL_RATIO=", denoised_detail_ratio)
	print("HYBRID_TEMPORAL_STATIC_NOISE=", static_noise)
	print("HYBRID_TEMPORAL_DISOCCLUSION_DELTA=", disocclusion_delta)
	print("HYBRID_TEMPORAL_RETURN_SETTLE_DELTA=", return_settle_delta)
	print("HYBRID_TEMPORAL_MORPH_MOTION_DELTA=", motion_delta)
	print("HYBRID_TEMPORAL_MORPH_SETTLE_DELTA=", motion_settle_delta)
	print("HYBRID_TEMPORAL_CAPTURE_PREFIX=", ProjectSettings.globalize_path(capture_prefix))
	detail_instance.queue_free()
	viewport.queue_free()
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


func _positive_transport_delta(first: Image, second: Image, region: Rect2i) -> Color:
	if first.get_size() != second.get_size() or first.is_empty() or second.is_empty():
		return Color.BLACK
	var clipped := region.intersection(Rect2i(Vector2i.ZERO, first.get_size()))
	if clipped.size.x <= 0 or clipped.size.y <= 0:
		return Color.BLACK
	var sum := Color.BLACK
	for y in range(clipped.position.y, clipped.end.y):
		for x in range(clipped.position.x, clipped.end.x):
			var delta := second.get_pixel(x, y) - first.get_pixel(x, y)
			sum += Color(maxf(delta.r, 0.0), maxf(delta.g, 0.0), maxf(delta.b, 0.0), 0.0)
	return sum / float(clipped.size.x * clipped.size.y)


func _add_diffuse_transport_box(name: String, size: Vector3, position: Vector3, albedo: Color) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = albedo
	material.roughness = 0.82
	var mesh := BoxMesh.new()
	mesh.size = size
	mesh.material = material
	var instance := MeshInstance3D.new()
	instance.name = name
	instance.mesh = mesh
	instance.position = position
	add_child(instance)
	return material


func _validate_diffuse_transport() -> void:
	# Isolate the physical secondary term: no raster light, a single one-sided
	# ceiling emitter, saturated red/green walls, and neutral receiving floor.
	# The A/B uses the same camera and primary raster; only Hybrid's
	# emissive-area/diffuse-transport term may introduce the colored deltas.
	animate_deformation = false
	get_viewport().scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	for child in get_children():
		child.queue_free()
	await get_tree().process_frame
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.0, 0.0, 0.0)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.0, 0.0, 0.0)
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment
	add_child(world_environment)
	scene_camera = Camera3D.new()
	scene_camera.position = Vector3(0.0, 1.7, 6.8)
	scene_camera.fov = 42.0
	scene_camera.look_at_from_position(scene_camera.position, Vector3(0.0, 1.65, -0.2))
	scene_camera.current = true
	add_child(scene_camera)
	_add_diffuse_transport_box("TransportFloor", Vector3(6.0, 0.08, 6.0), Vector3(0.0, 0.0, 0.0), Color(0.76, 0.76, 0.76))
	_add_diffuse_transport_box("TransportBack", Vector3(6.0, 4.0, 0.08), Vector3(0.0, 2.0, -3.0), Color(0.76, 0.76, 0.76))
	var red_material := _add_diffuse_transport_box("TransportRed", Vector3(0.08, 4.0, 6.0), Vector3(-3.0, 2.0, 0.0), Color(0.76, 0.76, 0.76))
	var green_material := _add_diffuse_transport_box("TransportGreen", Vector3(0.08, 4.0, 6.0), Vector3(3.0, 2.0, 0.0), Color(0.76, 0.76, 0.76))
	var emitter_material := StandardMaterial3D.new()
	emitter_material.albedo_color = Color(1.0, 0.95, 0.85)
	emitter_material.emission_enabled = true
	emitter_material.emission = Color(1.0, 0.95, 0.85)
	emitter_material.emission_energy_multiplier = 22.0
	var emitter_mesh := PlaneMesh.new()
	emitter_mesh.size = Vector2(1.4, 1.0)
	emitter_mesh.material = emitter_material
	var emitter := MeshInstance3D.new()
	emitter.name = "TransportCeilingEmitter"
	emitter.mesh = emitter_mesh
	emitter.position = Vector3(0.0, 3.94, -0.2)
	emitter.rotation_degrees.x = 180.0
	add_child(emitter)
	ProjectSettings.set_setting("rendering/hybrid_renderer/global_illumination/sample_count", 16)
	ProjectSettings.set_setting("rendering/hybrid_renderer/global_illumination/strength", 1.0)
	ProjectSettings.set_setting("rendering/hybrid_renderer/mode", 1)
	for _frame in 48:
		await get_tree().process_frame
	var neutral_hybrid := get_viewport().get_texture().get_image()
	# The neutral/color wall A/B holds the primary visible floor, camera, emitter,
	# and Forward+ direct term fixed. Resetting only the reconstruction history
	# makes the post-opaque transport delta observable without interpreting the
	# emitter's own warm direct spectrum as a wall bounce.
	ProjectSettings.set_setting("rendering/hybrid_renderer/mode", 0)
	for _frame in 12:
		await get_tree().process_frame
	red_material.albedo_color = Color(0.86, 0.012, 0.012)
	green_material.albedo_color = Color(0.012, 0.86, 0.012)
	ProjectSettings.set_setting("rendering/hybrid_renderer/mode", 1)
	for _frame in 48:
		await get_tree().process_frame
	var colored_hybrid := get_viewport().get_texture().get_image()
	var size := colored_hybrid.get_size()
	var left_floor := Rect2i(Vector2i(int(size.x * 0.27), int(size.y * 0.63)), Vector2i(int(size.x * 0.17), int(size.y * 0.20)))
	var right_floor := Rect2i(Vector2i(int(size.x * 0.56), int(size.y * 0.63)), Vector2i(int(size.x * 0.17), int(size.y * 0.20)))
	var left_delta := _positive_transport_delta(neutral_hybrid, colored_hybrid, left_floor)
	var right_delta := _positive_transport_delta(neutral_hybrid, colored_hybrid, right_floor)
	if left_delta.r <= left_delta.g + 0.00008 or right_delta.g <= right_delta.r + 0.00008:
		push_error("Diffuse transport did not transfer red/green wall energy to neutral floor: left=%s right=%s" % [left_delta, right_delta])
		get_tree().quit(16)
		return
	var base_path := "user://hybrid_diffuse_transport_"
	if neutral_hybrid.save_png(base_path + "neutral.png") != OK or colored_hybrid.save_png(base_path + "colored.png") != OK:
		push_error("Could not save diffuse transport captures.")
		get_tree().quit(17)
		return
	print("HYBRID_DIFFUSE_TRANSPORT_LEFT_DELTA=", left_delta)
	print("HYBRID_DIFFUSE_TRANSPORT_RIGHT_DELTA=", right_delta)
	print("HYBRID_DIFFUSE_TRANSPORT_CAPTURE_PREFIX=", ProjectSettings.globalize_path(base_path))
	get_tree().quit()


func _validate_omni_diffuse_transport() -> void:
	# This is deliberately separate from the emitter fixture: there is no
	# emissive triangle, so red/green floor transfer can only originate from the
	# bounded secondary Omni-light evaluation.
	animate_deformation = false
	get_viewport().scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	for child in get_children():
		child.queue_free()
	await get_tree().process_frame
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color.BLACK
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color.BLACK
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment
	add_child(world_environment)
	scene_camera = Camera3D.new()
	scene_camera.position = Vector3(0.0, 1.7, 6.8)
	scene_camera.fov = 42.0
	scene_camera.look_at_from_position(scene_camera.position, Vector3(0.0, 1.65, -0.2))
	scene_camera.current = true
	add_child(scene_camera)
	_add_diffuse_transport_box("OmniTransportFloor", Vector3(6.0, 0.08, 6.0), Vector3(0.0, 0.0, 0.0), Color(0.76, 0.76, 0.76))
	_add_diffuse_transport_box("OmniTransportBack", Vector3(6.0, 4.0, 0.08), Vector3(0.0, 2.0, -3.0), Color(0.76, 0.76, 0.76))
	var red_material := _add_diffuse_transport_box("OmniTransportRed", Vector3(0.08, 4.0, 6.0), Vector3(-3.0, 2.0, 0.0), Color(0.76, 0.76, 0.76))
	var green_material := _add_diffuse_transport_box("OmniTransportGreen", Vector3(0.08, 4.0, 6.0), Vector3(3.0, 2.0, 0.0), Color(0.76, 0.76, 0.76))
	var omni := OmniLight3D.new()
	omni.name = "TransportOmni"
	omni.position = Vector3(0.0, 3.65, -0.2)
	omni.light_color = Color(1.0, 0.95, 0.85)
	omni.light_energy = 2.5
	omni.omni_range = 5.0
	omni.shadow_enabled = true
	add_child(omni)
	ProjectSettings.set_setting("rendering/hybrid_renderer/global_illumination/sample_count", 16)
	ProjectSettings.set_setting("rendering/hybrid_renderer/global_illumination/strength", 1.0)
	ProjectSettings.set_setting("rendering/hybrid_renderer/mode", 1)
	for _frame in 48:
		await get_tree().process_frame
	var neutral_hybrid := get_viewport().get_texture().get_image()
	ProjectSettings.set_setting("rendering/hybrid_renderer/mode", 0)
	for _frame in 12:
		await get_tree().process_frame
	red_material.albedo_color = Color(0.86, 0.012, 0.012)
	green_material.albedo_color = Color(0.012, 0.86, 0.012)
	ProjectSettings.set_setting("rendering/hybrid_renderer/mode", 1)
	for _frame in 48:
		await get_tree().process_frame
	var colored_hybrid := get_viewport().get_texture().get_image()
	ProjectSettings.set_setting("rendering/hybrid_renderer/mode", 0)
	for _frame in 12:
		await get_tree().process_frame
	ProjectSettings.set_setting("rendering/hybrid_renderer/global_illumination/strength", 0.0)
	ProjectSettings.set_setting("rendering/hybrid_renderer/mode", 1)
	for _frame in 32:
		await get_tree().process_frame
	var no_secondary_hybrid := get_viewport().get_texture().get_image()
	var size := colored_hybrid.get_size()
	var left_floor := Rect2i(Vector2i(int(size.x * 0.27), int(size.y * 0.63)), Vector2i(int(size.x * 0.17), int(size.y * 0.20)))
	var right_floor := Rect2i(Vector2i(int(size.x * 0.56), int(size.y * 0.63)), Vector2i(int(size.x * 0.17), int(size.y * 0.20)))
	var left_delta := _positive_transport_delta(neutral_hybrid, colored_hybrid, left_floor)
	var right_delta := _positive_transport_delta(neutral_hybrid, colored_hybrid, right_floor)
	var secondary_delta := _mean_absolute_rgb_difference_region(no_secondary_hybrid, colored_hybrid, left_floor) + _mean_absolute_rgb_difference_region(no_secondary_hybrid, colored_hybrid, right_floor)
	if left_delta.r <= left_delta.g + 0.00008 or right_delta.g <= right_delta.r + 0.00008 or secondary_delta <= 0.0002:
		push_error("Omni secondary transport failed: left=%s right=%s GI-on/off=%f" % [left_delta, right_delta, secondary_delta])
		get_tree().quit(18)
		return
	var base_path := "user://hybrid_omni_diffuse_transport_"
	if neutral_hybrid.save_png(base_path + "neutral.png") != OK or colored_hybrid.save_png(base_path + "colored.png") != OK or no_secondary_hybrid.save_png(base_path + "no_secondary.png") != OK:
		push_error("Could not save Omni diffuse transport captures.")
		get_tree().quit(19)
		return
	print("HYBRID_OMNI_DIFFUSE_TRANSPORT_LEFT_DELTA=", left_delta)
	print("HYBRID_OMNI_DIFFUSE_TRANSPORT_RIGHT_DELTA=", right_delta)
	print("HYBRID_OMNI_DIFFUSE_TRANSPORT_GI_ON_OFF=", secondary_delta)
	print("HYBRID_OMNI_DIFFUSE_TRANSPORT_CAPTURE_PREFIX=", ProjectSettings.globalize_path(base_path))
	get_tree().quit()

func _validate_transport_culling() -> void:
	# This fixture is intentionally independent from the bilateral Omni test:
	# it validates one off-camera source and one unrelated far opaque candidate.
	animate_deformation = false
	ProjectSettings.set_setting("rendering/hybrid_renderer/transport_culling/enabled", true)
	ProjectSettings.set_setting("rendering/hybrid_renderer/transport_culling/max_distance", 64.0)
	var transport_distance := float(ProjectSettings.get_setting("rendering/hybrid_renderer/transport_culling/max_distance"))
	if not bool(ProjectSettings.get_setting("rendering/hybrid_renderer/transport_culling/enabled")) or not is_equal_approx(transport_distance, 64.0):
		push_error("Hybrid transport-culling fixture could not configure enabled D=64 state.")
		get_tree().quit(20)
		return
	get_viewport().scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	for child in get_children():
		child.queue_free()
	await get_tree().process_frame
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color.BLACK
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color.BLACK
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment
	add_child(world_environment)
	scene_camera = Camera3D.new()
	scene_camera.position = Vector3(0.0, 1.7, 6.8)
	scene_camera.fov = 42.0
	scene_camera.look_at_from_position(scene_camera.position, Vector3(0.0, 1.65, -0.2))
	scene_camera.current = true
	add_child(scene_camera)
	_add_diffuse_transport_box("TransportCullFloor", Vector3(6.0, 0.08, 6.0), Vector3(0.0, 0.0, 0.0), Color(0.76, 0.76, 0.76))
	_add_diffuse_transport_box("TransportCullBack", Vector3(6.0, 4.0, 0.08), Vector3(0.0, 2.0, -3.0), Color(0.76, 0.76, 0.76))
	_add_diffuse_transport_box("TransportCullRed", Vector3(0.08, 4.0, 6.0), Vector3(-3.0, 2.0, 0.0), Color(0.86, 0.012, 0.012))
	_add_diffuse_transport_box("TransportCullGreen", Vector3(0.08, 4.0, 6.0), Vector3(3.0, 2.0, 0.0), Color(0.012, 0.86, 0.012))
	_add_diffuse_transport_box("TransportCullFarOpaque", Vector3(2.0, 2.0, 2.0), Vector3(200.0, 1.0, 0.0), Color(0.76, 0.76, 0.76))
	var omni := OmniLight3D.new()
	omni.name = "TransportCullOffCameraOmni"
	omni.position = Vector3(6.0, 2.0, -0.2)
	omni.light_color = Color(1.0, 0.95, 0.85)
	omni.light_energy = 2.5
	omni.omni_range = 9.0
	omni.shadow_enabled = true
	add_child(omni)
	print("HYBRID_TRANSPORT_CULLING_FIXTURE_OFF_CAMERA_OMNI=", omni.position, " range=", omni.omni_range)
	print("HYBRID_TRANSPORT_CULLING_FIXTURE_FAR_MESH=TransportCullFarOpaque position=", Vector3(200.0, 1.0, 0.0), " D=", transport_distance)
	ProjectSettings.set_setting("rendering/hybrid_renderer/global_illumination/sample_count", 16)
	ProjectSettings.set_setting("rendering/hybrid_renderer/global_illumination/strength", 1.0)
	ProjectSettings.set_setting("rendering/hybrid_renderer/mode", 1)
	for _frame in 48:
		await get_tree().process_frame
	var gi_on := get_viewport().get_texture().get_image()
	ProjectSettings.set_setting("rendering/hybrid_renderer/mode", 0)
	for _frame in 12:
		await get_tree().process_frame
	ProjectSettings.set_setting("rendering/hybrid_renderer/global_illumination/strength", 0.0)
	ProjectSettings.set_setting("rendering/hybrid_renderer/mode", 1)
	for _frame in 32:
		await get_tree().process_frame
	var gi_off := get_viewport().get_texture().get_image()
	var size := gi_on.get_size()
	var left_floor := Rect2i(Vector2i(int(size.x * 0.27), int(size.y * 0.63)), Vector2i(int(size.x * 0.17), int(size.y * 0.20)))
	var right_floor := Rect2i(Vector2i(int(size.x * 0.56), int(size.y * 0.63)), Vector2i(int(size.x * 0.17), int(size.y * 0.20)))
	var transport_delta := _mean_absolute_rgb_difference_region(gi_off, gi_on, left_floor) + _mean_absolute_rgb_difference_region(gi_off, gi_on, right_floor)
	if transport_delta <= 0.0002:
		push_error("Hybrid transport-culling fixture GI-on/off floor ROI MAE was too small: %f" % transport_delta)
		get_tree().quit(21)
		return
	var base_path := "user://hybrid_transport_culling_"
	if gi_on.save_png(base_path + "gi_on.png") != OK or gi_off.save_png(base_path + "gi_off.png") != OK:
		push_error("Could not save hybrid transport-culling captures.")
		get_tree().quit(22)
		return
	print("HYBRID_TRANSPORT_CULLING_GI_ON_OFF=", transport_delta)
	print("HYBRID_TRANSPORT_CULLING_CAPTURE_PREFIX=", ProjectSettings.globalize_path(base_path))
	get_tree().quit()

func _capture_baked_visibility_frame(anchor_enabled: bool) -> Image:
	var anchor := get_node_or_null("BakedVisibilityAnchor") as BakedVisibilityVolume3D
	if anchor == null:
		return Image.new()
	anchor.enabled = anchor_enabled
	ProjectSettings.set_setting("rendering/occlusion_culling/baked_visibility/diagnostics", true)
	ProjectSettings.set_setting("rendering/hybrid_renderer/transport_culling/enabled", true)
	ProjectSettings.set_setting("rendering/hybrid_renderer/transport_culling/max_distance", anchor.transport_distance)
	ProjectSettings.set_setting("rendering/hybrid_renderer/mode", 1)
	for _frame in 48:
		await get_tree().process_frame
	return get_viewport().get_texture().get_image()

func _validate_baked_visibility() -> void:
	# This scene is deliberately pre-authored rather than assembled at runtime so
	# the editor baker serializes stable relative NodePaths into its .bvis payload.
	var anchor := get_node_or_null("BakedVisibilityAnchor") as BakedVisibilityVolume3D
	var blocker := get_node_or_null("VisibleCompartment/CertifiedOpaqueBlocker") as MeshInstance3D
	var contributor := get_node_or_null("TransportCompartment/HiddenReflectionGIContributor") as MeshInstance3D
	var transport_light := get_node_or_null("TransportCompartment/OffCameraTransportLight") as OmniLight3D
	var dynamic_mesh := get_node_or_null("VisibleCompartment/VisibleDynamicGeometry") as MeshInstance3D
	if anchor == null or blocker == null or contributor == null or transport_light == null or dynamic_mesh == null:
		push_error("Baked-visibility fixture has an incomplete authored node layout.")
		get_tree().quit(30)
		return
	if anchor.data == null or not anchor.data.is_valid():
		push_error("Baked-visibility fixture needs a generated .bvis. Run --headless --path misc/path_tracing/m2_5/validation_project --bake-visibility=res://baked_visibility_fixture.tscn --bake-visibility-strict --bake-visibility-require-anchor first.")
		get_tree().quit(31)
		return
	if not is_equal_approx(anchor.transport_distance, 14.0):
		push_error("Baked-visibility fixture requires authored transport distance D=14.")
		get_tree().quit(32)
		return
	print("BAKED_VISIBILITY_FIXTURE_DYNAMIC_NODE=", dynamic_mesh.get_path())
	print("BAKED_VISIBILITY_FIXTURE_TRANSPORT_NODE=", contributor.get_path(), " light=", transport_light.get_path())

	# A valid bake must preserve the visible image relative to ordinary runtime
	# transport culling while reducing only conservative static candidate sets.
	var disabled_image := await _capture_baked_visibility_frame(false)
	var enabled_image := await _capture_baked_visibility_frame(true)
	var stats: Dictionary = anchor.get_runtime_stats()
	var registered_geometry := int(stats.get("registered_static_geometry", 0))
	var primary_geometry := int(stats.get("primary_geometry", 0))
	var transport_geometry := int(stats.get("transport_geometry", 0))
	var transport_geometry_eligible := int(stats.get("transport_geometry_eligible", 0))
	if not bool(stats.get("available", false)) or not bool(stats.get("active", false)) or registered_geometry <= 0 or transport_geometry_eligible <= 0:
		push_error("Baked-visibility runtime statistics were unavailable or inactive: %s" % stats)
		get_tree().quit(33)
		return
	if float(primary_geometry) / float(registered_geometry) > 0.75 or float(transport_geometry) / float(transport_geometry_eligible) > 0.90:
		push_error("Baked-visibility fixture did not meet reduction gates: primary=%d/%d transport=%d/%d" % [primary_geometry, registered_geometry, transport_geometry, transport_geometry_eligible])
		get_tree().quit(34)
		return
	var enabled_delta := _mean_absolute_rgb_difference(disabled_image, enabled_image)
	if disabled_image.is_empty() or enabled_image.is_empty() or enabled_delta > 0.02:
		push_error("Baked-visibility enabled/disabled capture diverged: %f" % enabled_delta)
		get_tree().quit(35)
		return

	# The contributor and positional light are primary-hidden behind the
	# separating wall, but their right-edge transport path reaches the visible
	# receiver. Their removal must change the receiver ROI through the hybrid
	# closure, never through primary raster admission.
	var contributor_material := contributor.get_active_material(0) as StandardMaterial3D
	if contributor_material == null:
		push_error("Baked-visibility fixture contributor has no StandardMaterial3D.")
		get_tree().quit(36)
		return
	var original_energy := contributor_material.emission_energy_multiplier
	var original_light_energy := transport_light.light_energy
	contributor_material.emission_energy_multiplier = 0.0
	transport_light.light_energy = 0.0
	var no_transport_image := await _capture_baked_visibility_frame(true)
	contributor_material.emission_energy_multiplier = original_energy
	transport_light.light_energy = original_light_energy
	var receiver_roi := Rect2i(Vector2i(int(enabled_image.get_width() * 0.25), int(enabled_image.get_height() * 0.46)), Vector2i(int(enabled_image.get_width() * 0.50), int(enabled_image.get_height() * 0.38)))
	var hidden_transport_delta := _mean_absolute_rgb_difference_region(no_transport_image, enabled_image, receiver_roi)
	if hidden_transport_delta <= 0.0002:
		push_error("Hidden contributor/light did not affect the visible receiver ROI: %f" % hidden_transport_delta)
		get_tree().quit(37)
		return

	# Runtime identity validates opaque blocker material state. Turning this
	# certified blocker transparent must detach the baked result and match the
	# anchor-disabled fail-open image instead of retaining an unsafe PVS.
	var blocker_material := blocker.get_active_material(0) as StandardMaterial3D
	if blocker_material == null:
		push_error("Baked-visibility fixture blocker has no StandardMaterial3D.")
		get_tree().quit(38)
		return
	var original_transparency := blocker_material.transparency
	blocker_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	var fail_open_image := await _capture_baked_visibility_frame(true)
	blocker_material.transparency = original_transparency
	var fail_open_delta := _mean_absolute_rgb_difference(disabled_image, fail_open_image)
	if fail_open_image.is_empty() or fail_open_delta > 0.02:
		push_error("Certified transparent blocker did not recover fail-open behavior: %f" % fail_open_delta)
		get_tree().quit(39)
		return
	var base_path := "user://baked_visibility_"
	if disabled_image.save_png(base_path + "disabled.png") != OK or enabled_image.save_png(base_path + "enabled.png") != OK or no_transport_image.save_png(base_path + "transport_off.png") != OK or fail_open_image.save_png(base_path + "transparent_blocker.png") != OK:
		push_error("Could not save baked-visibility validation captures.")
		get_tree().quit(40)
		return
	print("BAKED_VISIBILITY_PRIMARY_GEOMETRY=", primary_geometry, "/", registered_geometry)
	print("BAKED_VISIBILITY_TRANSPORT_GEOMETRY=", transport_geometry, "/", transport_geometry_eligible)
	print("BAKED_VISIBILITY_ENABLED_DISABLED_MAE=", enabled_delta)
	print("BAKED_VISIBILITY_HIDDEN_TRANSPORT_RECEIVER_ROI_MAE=", hidden_transport_delta)
	print("BAKED_VISIBILITY_TRANSPARENT_BLOCKER_FAIL_OPEN_MAE=", fail_open_delta)
	print("BAKED_VISIBILITY_CAPTURE_PREFIX=", ProjectSettings.globalize_path(base_path))
	get_tree().quit()

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

	ProjectSettings.set_setting("rendering/hybrid_renderer/mode", 1)
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
