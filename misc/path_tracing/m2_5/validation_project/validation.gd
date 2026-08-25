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
	if "--validate-flux-raster-procedural-sky" in OS.get_cmdline_user_args():
		await _validate_flux_raster_procedural_sky()
		return
	if "--validate-flux-runtime-procedural-sky-toggle" in OS.get_cmdline_user_args():
		await _validate_flux_runtime_procedural_sky_toggle()
		return
	if "--validate-flux-mirror-caustic" in OS.get_cmdline_user_args():
		await _validate_flux_mirror_caustic()
		return
	if Engine.is_editor_hint():
		animate_deformation = false
		if "--validate-flux-editor-procedural-sky" in OS.get_cmdline_user_args():
			await _validate_flux_editor_procedural_sky()
			return
		_build_scene()
		if "--validate-flux-editor-overlay" in OS.get_cmdline_user_args():
			await _capture_editor_overlay_regression()
			return
		if "--validate-flux-editor" in OS.get_cmdline_user_args():
			await _capture_editor_viewport()
		return
	_build_scene()
	if "--validate-flux-reuse-warmup" in OS.get_cmdline_user_args():
		await _validate_flux_reuse_warmup()
		return
	if "--benchmark-flux" in OS.get_cmdline_user_args():
		await _run_benchmark()
		return
	if "--validate-flux-texture-transport" in OS.get_cmdline_user_args():
		await _validate_opaque_texture_transport()
		return
	if "--validate-flux-diffuse-transport" in OS.get_cmdline_user_args():
		await _validate_diffuse_transport()
		return
	if "--validate-flux-omni-diffuse-transport" in OS.get_cmdline_user_args():
		await _validate_omni_diffuse_transport()
		return
	if "--validate-flux-transport-culling" in OS.get_cmdline_user_args():
		await _validate_transport_culling()
		return
	if "--validate-flux-viewport-toggle" in OS.get_cmdline_user_args():
		await _validate_flux_viewport_toggle()
		return
	if "--validate-flux-temporal-detail" in OS.get_cmdline_user_args():
		await _validate_flux_temporal_detail()
		return
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", 1)
	for _frame in 12:
		await get_tree().process_frame
	animate_deformation = false
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", 0)
	for _frame in 8:
		await get_tree().process_frame
	var raster_image := get_viewport().get_texture().get_image()
	if raster_image.is_empty():
		push_error("Flux validation capture is empty.")
		get_tree().quit(2)
		return
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", 1)
	for _frame in 8:
		await get_tree().process_frame
	var flux_image := get_viewport().get_texture().get_image()
	var difference := _mean_absolute_rgb_difference(raster_image, flux_image)
	if difference < 0.0001:
		push_error("Flux renderer did not materially change the validation frame: %f" % difference)
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
	var disocclusion_difference := _mean_absolute_rgb_difference(flux_image, settled_camera_image)
	if moving_camera_image.is_empty() or settled_camera_image.is_empty() or disocclusion_difference < 0.001:
		push_error("Flux moving-camera disocclusion fixture did not produce a distinct valid frame: %f" % disocclusion_difference)
		get_tree().quit(5)
		return
	scene_camera.global_transform = original_camera_transform
	var raster_path := "user://flux_runtime_validation_raster.png"
	var flux_path := "user://flux_runtime_validation_flux.png"
	var moving_path := "user://flux_runtime_validation_disocclusion.png"
	if raster_image.save_png(raster_path) != OK or flux_image.save_png(flux_path) != OK or moving_camera_image.save_png(moving_path) != OK:
		push_error("Could not save flux validation captures.")
		get_tree().quit(4)
		return
	print("FLUX_VALIDATION_MEAN_ABS_RGB_DIFFERENCE=", difference)
	print("FLUX_VALIDATION_DISOCCLUSION_DIFFERENCE=", disocclusion_difference)
	print("FLUX_VALIDATION_CAPTURE=", ProjectSettings.globalize_path(flux_path))
	print("FLUX_VALIDATION_DISOCCLUSION_CAPTURE=", ProjectSettings.globalize_path(moving_path))
	get_tree().quit()

func _procedural_sky_side_luminance(image: Image, left_side: bool) -> float:
	var width := image.get_width()
	var height := image.get_height()
	var x_begin := int(width * (0.24 if left_side else 0.52))
	var x_end := int(width * (0.48 if left_side else 0.76))
	var y_begin := int(height * 0.28)
	var y_end := int(height * 0.72)
	var total := 0.0
	var count := 0
	for y in range(y_begin, y_end):
		for x in range(x_begin, x_end):
			total += image.get_pixel(x, y).get_luminance()
			count += 1
	return total / float(maxi(count, 1))

func _procedural_sky_shadow_energy(image: Image, left_side: bool) -> float:
	# The fixture's suspended sphere is centered on the floor. Sun/moon motion
	# moves its cast shadow into one of these two floor regions. Excluding the
	# centre avoids measuring the sphere silhouette itself.
	var width := image.get_width()
	var height := image.get_height()
	var x_begin := int(width * (0.18 if left_side else 0.57))
	var x_end := int(width * (0.43 if left_side else 0.82))
	var y_begin := int(height * 0.57)
	var y_end := int(height * 0.88)
	var mean := 0.0
	var count := 0
	for y in range(y_begin, y_end):
		for x in range(x_begin, x_end):
			mean += image.get_pixel(x, y).get_luminance()
			count += 1
	mean /= float(maxi(count, 1))
	var shadow := 0.0
	for y in range(y_begin, y_end):
		for x in range(x_begin, x_end):
			shadow += maxf(mean * 0.86 - image.get_pixel(x, y).get_luminance(), 0.0)
	return shadow / float(maxi(count, 1))

func _procedural_sky_highlight_energy(image: Image, left_side: bool) -> float:
	# The sphere's lit hemisphere is a direct-light receiver independent of the
	# floor shadow. It catches an ownership transition that might otherwise leave
	# a current PSSM map paired with an old raster directional-light buffer.
	var width := image.get_width()
	var height := image.get_height()
	var x_begin := int(width * (0.38 if left_side else 0.50))
	var x_end := int(width * (0.50 if left_side else 0.62))
	var y_begin := int(height * 0.40)
	var y_end := int(height * 0.61)
	var energy := 0.0
	var count := 0
	for y in range(y_begin, y_end):
		for x in range(x_begin, x_end):
			energy += image.get_pixel(x, y).get_luminance()
			count += 1
	return energy / float(maxi(count, 1))

func _procedural_sky_floor_banding_score(image: Image) -> float:
	# Measure broad, repeated PSSM bands away from the centered caster. The
	# smoothed row-to-row change removes the expected smooth perspective/sky
	# gradient and rises for the full-width dark stripes this fixture guards.
	if image.is_empty():
		return INF
	var width := image.get_width()
	var height := image.get_height()
	var x_begin := width / 16
	var x_end := width * 6 / 16
	var y_begin := height * 11 / 16
	var y_end := height * 15 / 16
	var rows: Array[float] = []
	for y in range(y_begin, y_end, 8):
		var row_luminance := 0.0
		for x in range(x_begin, x_end):
			row_luminance += image.get_pixel(x, y).get_luminance()
		rows.append(row_luminance / float(maxi(x_end - x_begin, 1)))
	var average_luminance := 0.0
	for value in rows:
		average_luminance += value
	average_luminance /= float(maxi(rows.size(), 1))
	var broad_band_groups := 0
	var in_band := false
	# Smooth the sub-pixel raster pattern first, then count spatially separate
	# broad curvature peaks. A real caster can create one edge; repeating PSSM
	# bands create several groups across this empty floor region.
	for index in range(2, rows.size() - 2):
		var previous := (rows[index - 2] + rows[index - 1] + rows[index]) / 3.0
		var next := (rows[index] + rows[index + 1] + rows[index + 2]) / 3.0
		var curvature := absf(previous - next) / maxf(average_luminance, 0.05)
		if curvature > 0.08:
			if not in_band:
				broad_band_groups += 1
			in_band = true
		else:
			in_band = false
	return float(broad_band_groups)

func _procedural_sky_grazing_stripe_score(image: Image) -> float:
	# Fine acne stripes survive a broad cascade-band metric. Measure the
	# high-frequency horizontal curvature across the unobstructed near floor.
	if image.is_empty():
		return INF
	var width := image.get_width()
	var height := image.get_height()
	var total_curvature := 0.0
	var total_luminance := 0.0
	var count := 0
	for y in range(int(height * 0.64), int(height * 0.93), 3):
		for x in range(int(width * 0.08) + 2, int(width * 0.92) - 2, 2):
			var previous := image.get_pixel(x - 2, y).get_luminance()
			var left := image.get_pixel(x - 1, y).get_luminance()
			var center := image.get_pixel(x, y).get_luminance()
			var right := image.get_pixel(x + 1, y).get_luminance()
			var next := image.get_pixel(x + 2, y).get_luminance()
			total_curvature += absf(previous - 4.0 * left + 6.0 * center - 4.0 * right + next)
			total_luminance += center
			count += 1
	return total_curvature / maxf(total_luminance, 0.05 * float(maxi(count, 1)))

func _validate_flux_editor_procedural_sky() -> void:
	# This deliberately renders through the editor's own SubViewport and mutates
	# AtmosphereSkyClock.current_time, matching inspector edits in an unsaved
	# scene. It must not be replaced with the game-viewport validation.
	var atmosphere := AtmosphereSkyMaterial.new()
	atmosphere.cloud_coverage = 0.0
	atmosphere.sun_disk_energy = 2.0
	atmosphere.moon_disk_size = 10.0
	atmosphere.moon_disk_energy = 50.0
	atmosphere.exposure = 1.0
	atmosphere.latitude = 0.0
	atmosphere.day_of_year = 80
	var procedural_sky := Sky.new()
	procedural_sky.process_mode = Sky.PROCESS_MODE_REALTIME
	procedural_sky.sky_material = atmosphere
	var environment_resource := Environment.new()
	environment_resource.background_mode = Environment.BG_SKY
	environment_resource.sky = procedural_sky
	environment_resource.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment_resource.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment_resource
	add_child(world_environment)
	var clock := AtmosphereSkyClock.new()
	clock.atmosphere = atmosphere
	clock.editor_preview_enabled = true
	clock.editor_preview_paused = true
	add_child(clock)

	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 0.0, 5.0)
	camera.look_at_from_position(camera.position, Vector3.ZERO)
	camera.fov = 46.0
	add_child(camera)
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.8, 0.8, 0.8)
	material.metallic = 0.0
	material.roughness = 1.0
	var floor_mesh := PlaneMesh.new()
	floor_mesh.size = Vector2(12.0, 12.0)
	floor_mesh.material = material
	var floor := MeshInstance3D.new()
	floor.mesh = floor_mesh
	floor.position.y = -1.35
	add_child(floor)
	var sphere_mesh := SphereMesh.new()
	sphere_mesh.radius = 0.72
	sphere_mesh.height = 1.44
	sphere_mesh.material = material
	var sphere := MeshInstance3D.new()
	sphere.mesh = sphere_mesh
	sphere.position = Vector3(0.0, -0.15, 0.0)
	add_child(sphere)

	await _wait_flux_frames(60)
	var editor_viewport := EditorInterface.get_editor_viewport_3d(0)
	var editor_camera := editor_viewport.get_camera_3d()
	editor_camera.global_transform = camera.global_transform
	editor_camera.fov = camera.fov
	# This is the same RenderingServer control used by the Flux editor toolbar,
	# but targets the editor SubViewport rather than the game viewport.
	var editor_viewport_rid := editor_viewport.get_viewport_rid()
	RenderingServer.viewport_set_flux_ray_tracing_enabled(editor_viewport_rid, false)

	var captures: Dictionary = {}
	var diagnostics: Dictionary = {}
	for capture_time in [9.0, 10.92, 12.34, 13.29, 15.0, 18.59, 1.54]:
		clock.current_time = capture_time
		await _wait_flux_frames(40)
		var image := editor_viewport.get_texture().get_image()
		captures[capture_time] = image
		diagnostics[capture_time] = RenderingServer.viewport_get_flux_diagnostics(editor_viewport_rid)

	# Re-enable the exact editor viewport and prove that the ray path uses the
	# same current daytime lobe without the raster-owned directional source.
	RenderingServer.viewport_set_flux_ray_tracing_enabled(editor_viewport_rid, true)
	clock.current_time = 9.0
	await _wait_flux_frames(40)
	var ray_morning := editor_viewport.get_texture().get_image()
	var ray_morning_diagnostics: Dictionary = RenderingServer.viewport_get_flux_diagnostics(editor_viewport_rid)
	clock.current_time = 15.0
	await _wait_flux_frames(40)
	var ray_afternoon := editor_viewport.get_texture().get_image()
	var ray_afternoon_diagnostics: Dictionary = RenderingServer.viewport_get_flux_diagnostics(editor_viewport_rid)

	# The low grazing view catches fine directional-shadow acne that may not be
	# visible in the normal editor camera.
	RenderingServer.viewport_set_flux_ray_tracing_enabled(editor_viewport_rid, false)
	editor_camera.look_at_from_position(Vector3(0.0, -0.82, 7.0), Vector3(0.0, -1.35, 0.0))
	editor_camera.fov = 58.0
	clock.current_time = 10.29
	await _wait_flux_frames(40)
	var raster_grazing := editor_viewport.get_texture().get_image()

	var raster_day_morning: Image = captures[9.0]
	var raster_1092: Image = captures[10.92]
	var raster_1234: Image = captures[12.34]
	var raster_midday: Image = captures[13.29]
	var raster_day_afternoon: Image = captures[15.0]
	var raster_evening: Image = captures[18.59]
	var raster_night: Image = captures[1.54]
	var raster_morning_shadow_left := _procedural_sky_shadow_energy(raster_day_morning, true)
	var raster_morning_shadow_right := _procedural_sky_shadow_energy(raster_day_morning, false)
	var raster_midday_shadow_left := _procedural_sky_shadow_energy(raster_midday, true)
	var raster_midday_shadow_right := _procedural_sky_shadow_energy(raster_midday, false)
	var raster_afternoon_shadow_left := _procedural_sky_shadow_energy(raster_day_afternoon, true)
	var raster_afternoon_shadow_right := _procedural_sky_shadow_energy(raster_day_afternoon, false)
	var raster_evening_shadow_left := _procedural_sky_shadow_energy(raster_evening, true)
	var raster_evening_shadow_right := _procedural_sky_shadow_energy(raster_evening, false)
	var raster_night_shadow_left := _procedural_sky_shadow_energy(raster_night, true)
	var raster_night_shadow_right := _procedural_sky_shadow_energy(raster_night, false)
	var raster_evening_highlight_left := _procedural_sky_highlight_energy(raster_evening, true)
	var raster_evening_highlight_right := _procedural_sky_highlight_energy(raster_evening, false)
	var raster_night_highlight_left := _procedural_sky_highlight_energy(raster_night, true)
	var raster_night_highlight_right := _procedural_sky_highlight_energy(raster_night, false)
	var ray_morning_shadow_left := _procedural_sky_shadow_energy(ray_morning, true)
	var ray_morning_shadow_right := _procedural_sky_shadow_energy(ray_morning, false)
	var ray_afternoon_shadow_left := _procedural_sky_shadow_energy(ray_afternoon, true)
	var ray_afternoon_shadow_right := _procedural_sky_shadow_energy(ray_afternoon, false)
	var raster_1092_band_score := _procedural_sky_floor_banding_score(raster_1092)
	var raster_1234_band_score := _procedural_sky_floor_banding_score(raster_1234)
	var raster_15_band_score := _procedural_sky_floor_banding_score(raster_day_afternoon)
	var raster_night_band_score := _procedural_sky_floor_banding_score(raster_night)
	var raster_grazing_stripe_score := _procedural_sky_grazing_stripe_score(raster_grazing)

	print("FLUX_EDITOR_SKY_RASTER_DAY_SHADOW=", raster_morning_shadow_left, "/", raster_morning_shadow_right, " midday=", raster_midday_shadow_left, "/", raster_midday_shadow_right, " afternoon=", raster_afternoon_shadow_left, "/", raster_afternoon_shadow_right)
	print("FLUX_EDITOR_SKY_RASTER_NIGHT_SHADOW=", raster_evening_shadow_left, "/", raster_evening_shadow_right, " ", raster_night_shadow_left, "/", raster_night_shadow_right)
	print("FLUX_EDITOR_SKY_RASTER_NIGHT_HIGHLIGHT=", raster_evening_highlight_left, "/", raster_evening_highlight_right, " ", raster_night_highlight_left, "/", raster_night_highlight_right)
	print("FLUX_EDITOR_SKY_RAY_DAY_SHADOW=", ray_morning_shadow_left, "/", ray_morning_shadow_right, " ", ray_afternoon_shadow_left, "/", ray_afternoon_shadow_right)
	print("FLUX_EDITOR_SKY_RASTER_BAND_SCORE=", raster_1092_band_score, "/", raster_1234_band_score, "/", raster_15_band_score, "/", raster_night_band_score)
	print("FLUX_EDITOR_SKY_GRAZING_STRIPE_SCORE=", raster_grazing_stripe_score)
	print("FLUX_EDITOR_SKY_MODES=", diagnostics[9.0].get("effective_mode", -1), "/", diagnostics[13.29].get("effective_mode", -1), "/", diagnostics[15.0].get("effective_mode", -1), "/", diagnostics[18.59].get("effective_mode", -1), "/", diagnostics[1.54].get("effective_mode", -1), " -> ", ray_morning_diagnostics.get("effective_mode", -1), "/", ray_afternoon_diagnostics.get("effective_mode", -1))

	var raster_modes_valid := int(diagnostics[9.0].get("effective_mode", -1)) == 0 and int(diagnostics[13.29].get("effective_mode", -1)) == 0 and int(diagnostics[15.0].get("effective_mode", -1)) == 0 and int(diagnostics[18.59].get("effective_mode", -1)) == 0 and int(diagnostics[1.54].get("effective_mode", -1)) == 0
	var raster_day_moves := raster_morning_shadow_right > raster_morning_shadow_left * 1.05 and raster_afternoon_shadow_left > raster_afternoon_shadow_right * 1.05
	var raster_midday_changes := absf(raster_midday_shadow_right - raster_morning_shadow_right) > 0.0001 or absf(raster_midday_shadow_left - raster_afternoon_shadow_left) > 0.0001
	var raster_night_moves := raster_evening_shadow_right > raster_evening_shadow_left * 1.05 and raster_night_highlight_right > raster_night_highlight_left * 1.01 and raster_evening_highlight_left > raster_evening_highlight_right * 1.01
	var ray_modes_valid := int(ray_morning_diagnostics.get("effective_mode", -1)) > 0 and int(ray_afternoon_diagnostics.get("effective_mode", -1)) > 0
	var ray_day_moves := ray_morning_shadow_right > ray_morning_shadow_left * 1.05 and ray_afternoon_shadow_left > ray_afternoon_shadow_right * 1.05
	var raster_band_free := raster_1092_band_score <= 2.0 and raster_1234_band_score <= 2.0 and raster_15_band_score <= 2.0 and raster_night_band_score <= 2.0
	var raster_grazing_stripe_free := raster_grazing_stripe_score <= 0.01
	if raster_day_morning.is_empty() or raster_1092.is_empty() or raster_1234.is_empty() or raster_midday.is_empty() or raster_day_afternoon.is_empty() or raster_evening.is_empty() or raster_night.is_empty() or ray_morning.is_empty() or ray_afternoon.is_empty() or raster_grazing.is_empty() or not raster_modes_valid or not raster_day_moves or not raster_midday_changes or not raster_night_moves or not ray_modes_valid or not ray_day_moves or not raster_band_free or not raster_grazing_stripe_free:
		push_error("Flux editor procedural-Sky validation failed.")
		get_tree().quit(46)
		return
	var capture_prefix := "user://flux_editor_procedural_sky_"
	if raster_day_morning.save_png(capture_prefix + "raster_9.png") != OK or raster_1092.save_png(capture_prefix + "raster_1092.png") != OK or raster_1234.save_png(capture_prefix + "raster_1234.png") != OK or raster_midday.save_png(capture_prefix + "raster_1329.png") != OK or raster_day_afternoon.save_png(capture_prefix + "raster_15.png") != OK or raster_evening.save_png(capture_prefix + "raster_1859.png") != OK or raster_night.save_png(capture_prefix + "raster_154.png") != OK or raster_grazing.save_png(capture_prefix + "raster_grazing_1029.png") != OK or ray_morning.save_png(capture_prefix + "ray_9.png") != OK or ray_afternoon.save_png(capture_prefix + "ray_15.png") != OK:
		push_error("Could not save Flux editor procedural-Sky captures.")
		get_tree().quit(47)
		return
	print("FLUX_EDITOR_SKY_CAPTURE_PREFIX=", ProjectSettings.globalize_path(capture_prefix))
	get_tree().quit()

func _validate_flux_runtime_procedural_sky_toggle() -> void:
	# The project starts with Flux enabled. Do not change that setting here:
	# RenderFlux resolves it at process start. Exercise the exact public
	# per-viewport control that the editor toolbar uses instead.
	var viewport_rid := get_viewport().get_viewport_rid()
	RenderingServer.viewport_set_flux_ray_tracing_enabled(viewport_rid, true)
	get_viewport().scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR

	var atmosphere := AtmosphereSkyMaterial.new()
	atmosphere.cloud_coverage = 0.0
	atmosphere.sun_disk_energy = 2.0
	atmosphere.moon_disk_size = 10.0
	atmosphere.moon_disk_energy = 50.0
	atmosphere.exposure = 1.0
	atmosphere.latitude = 0.0
	atmosphere.day_of_year = 80
	var procedural_sky := Sky.new()
	procedural_sky.process_mode = Sky.PROCESS_MODE_REALTIME
	procedural_sky.sky_material = atmosphere
	var environment_resource := Environment.new()
	environment_resource.background_mode = Environment.BG_SKY
	environment_resource.sky = procedural_sky
	environment_resource.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment_resource.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment_resource
	add_child(world_environment)

	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 0.0, 5.0)
	camera.look_at_from_position(camera.position, Vector3.ZERO)
	camera.current = true
	add_child(camera)
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.8, 0.8, 0.8)
	material.metallic = 0.0
	material.roughness = 1.0
	var floor_mesh := PlaneMesh.new()
	floor_mesh.size = Vector2(12.0, 12.0)
	floor_mesh.material = material
	var floor := MeshInstance3D.new()
	floor.mesh = floor_mesh
	floor.position.y = -1.35
	add_child(floor)
	var sphere_mesh := SphereMesh.new()
	sphere_mesh.radius = 0.72
	sphere_mesh.height = 1.44
	sphere_mesh.material = material
	var sphere := MeshInstance3D.new()
	sphere.mesh = sphere_mesh
	sphere.position = Vector3(0.0, -0.15, 0.0)
	add_child(sphere)

	# Full Flux owns the Sky direct-light path first. Then switch only this
	# viewport to raster in-process and keep advancing the same Sky resource.
	atmosphere.time_of_day = 9.0
	await _wait_flux_frames(40)
	var ray_day_morning := get_viewport().get_texture().get_image()
	var ray_day_morning_diagnostics: Dictionary = RenderingServer.viewport_get_flux_diagnostics(viewport_rid)
	atmosphere.time_of_day = 15.0
	await _wait_flux_frames(40)
	var ray_day_afternoon := get_viewport().get_texture().get_image()
	var ray_day_afternoon_diagnostics: Dictionary = RenderingServer.viewport_get_flux_diagnostics(viewport_rid)
	atmosphere.time_of_day = 18.59
	await _wait_flux_frames(40)
	var evening_moon_direction := atmosphere.get_moon_direction()
	var ray_night_evening := get_viewport().get_texture().get_image()
	var ray_night_evening_diagnostics: Dictionary = RenderingServer.viewport_get_flux_diagnostics(viewport_rid)

	RenderingServer.viewport_set_flux_ray_tracing_enabled(viewport_rid, false)
	await _wait_flux_frames(40)
	var raster_night_evening := get_viewport().get_texture().get_image()
	var raster_night_evening_diagnostics: Dictionary = RenderingServer.viewport_get_flux_diagnostics(viewport_rid)
	atmosphere.time_of_day = 1.54
	await _wait_flux_frames(40)
	var morning_moon_direction := atmosphere.get_moon_direction()
	var raster_night_morning := get_viewport().get_texture().get_image()
	var raster_night_morning_diagnostics: Dictionary = RenderingServer.viewport_get_flux_diagnostics(viewport_rid)
	atmosphere.time_of_day = 9.0
	await _wait_flux_frames(40)
	var raster_day_morning := get_viewport().get_texture().get_image()
	var raster_day_morning_diagnostics: Dictionary = RenderingServer.viewport_get_flux_diagnostics(viewport_rid)
	atmosphere.time_of_day = 15.0
	await _wait_flux_frames(40)
	var raster_day_afternoon := get_viewport().get_texture().get_image()
	var raster_day_afternoon_diagnostics: Dictionary = RenderingServer.viewport_get_flux_diagnostics(viewport_rid)

	RenderingServer.viewport_set_flux_ray_tracing_enabled(viewport_rid, true)
	await _wait_flux_frames(40)
	var ray_day_afternoon_return := get_viewport().get_texture().get_image()
	var ray_day_afternoon_return_diagnostics: Dictionary = RenderingServer.viewport_get_flux_diagnostics(viewport_rid)

	var ray_day_morning_shadow_left := _procedural_sky_shadow_energy(ray_day_morning, true)
	var ray_day_morning_shadow_right := _procedural_sky_shadow_energy(ray_day_morning, false)
	var ray_day_afternoon_shadow_left := _procedural_sky_shadow_energy(ray_day_afternoon, true)
	var ray_day_afternoon_shadow_right := _procedural_sky_shadow_energy(ray_day_afternoon, false)
	var ray_night_evening_shadow_left := _procedural_sky_shadow_energy(ray_night_evening, true)
	var ray_night_evening_shadow_right := _procedural_sky_shadow_energy(ray_night_evening, false)
	var raster_night_evening_shadow_left := _procedural_sky_shadow_energy(raster_night_evening, true)
	var raster_night_evening_shadow_right := _procedural_sky_shadow_energy(raster_night_evening, false)
	var raster_night_morning_shadow_left := _procedural_sky_shadow_energy(raster_night_morning, true)
	var raster_night_morning_shadow_right := _procedural_sky_shadow_energy(raster_night_morning, false)
	var raster_day_morning_shadow_left := _procedural_sky_shadow_energy(raster_day_morning, true)
	var raster_day_morning_shadow_right := _procedural_sky_shadow_energy(raster_day_morning, false)
	var raster_day_afternoon_shadow_left := _procedural_sky_shadow_energy(raster_day_afternoon, true)
	var raster_day_afternoon_shadow_right := _procedural_sky_shadow_energy(raster_day_afternoon, false)
	var ray_day_afternoon_return_shadow_left := _procedural_sky_shadow_energy(ray_day_afternoon_return, true)
	var ray_day_afternoon_return_shadow_right := _procedural_sky_shadow_energy(ray_day_afternoon_return, false)
	var ray_day_morning_highlight_left := _procedural_sky_highlight_energy(ray_day_morning, true)
	var ray_day_morning_highlight_right := _procedural_sky_highlight_energy(ray_day_morning, false)
	var ray_day_afternoon_highlight_left := _procedural_sky_highlight_energy(ray_day_afternoon, true)
	var ray_day_afternoon_highlight_right := _procedural_sky_highlight_energy(ray_day_afternoon, false)
	var raster_night_evening_highlight_left := _procedural_sky_highlight_energy(raster_night_evening, true)
	var raster_night_evening_highlight_right := _procedural_sky_highlight_energy(raster_night_evening, false)
	var raster_night_morning_highlight_left := _procedural_sky_highlight_energy(raster_night_morning, true)
	var raster_night_morning_highlight_right := _procedural_sky_highlight_energy(raster_night_morning, false)
	var raster_day_morning_highlight_left := _procedural_sky_highlight_energy(raster_day_morning, true)
	var raster_day_morning_highlight_right := _procedural_sky_highlight_energy(raster_day_morning, false)
	var raster_day_afternoon_highlight_left := _procedural_sky_highlight_energy(raster_day_afternoon, true)
	var raster_day_afternoon_highlight_right := _procedural_sky_highlight_energy(raster_day_afternoon, false)

	print("FLUX_RUNTIME_SKY_TOGGLE_RAY_DAY_SHADOW=", ray_day_morning_shadow_left, "/", ray_day_morning_shadow_right, " ", ray_day_afternoon_shadow_left, "/", ray_day_afternoon_shadow_right, " night=", ray_night_evening_shadow_left, "/", ray_night_evening_shadow_right, " return=", ray_day_afternoon_return_shadow_left, "/", ray_day_afternoon_return_shadow_right)
	print("FLUX_RUNTIME_SKY_TOGGLE_RASTER_NIGHT_SHADOW=", raster_night_evening_shadow_left, "/", raster_night_evening_shadow_right, " ", raster_night_morning_shadow_left, "/", raster_night_morning_shadow_right)
	print("FLUX_RUNTIME_SKY_TOGGLE_RASTER_DAY_SHADOW=", raster_day_morning_shadow_left, "/", raster_day_morning_shadow_right, " ", raster_day_afternoon_shadow_left, "/", raster_day_afternoon_shadow_right)
	print("FLUX_RUNTIME_SKY_TOGGLE_MOON_DIRECTION=", evening_moon_direction, " / ", morning_moon_direction)
	print("FLUX_RUNTIME_SKY_TOGGLE_HIGHLIGHT=ray_day ", ray_day_morning_highlight_left, "/", ray_day_morning_highlight_right, " ", ray_day_afternoon_highlight_left, "/", ray_day_afternoon_highlight_right, " raster_night ", raster_night_evening_highlight_left, "/", raster_night_evening_highlight_right, " ", raster_night_morning_highlight_left, "/", raster_night_morning_highlight_right, " raster_day ", raster_day_morning_highlight_left, "/", raster_day_morning_highlight_right, " ", raster_day_afternoon_highlight_left, "/", raster_day_afternoon_highlight_right)
	print("FLUX_RUNTIME_SKY_TOGGLE_MODES=", ray_day_morning_diagnostics.get("effective_mode", -1), "/", ray_day_afternoon_diagnostics.get("effective_mode", -1), "/", ray_night_evening_diagnostics.get("effective_mode", -1), " -> ", raster_night_evening_diagnostics.get("effective_mode", -1), "/", raster_night_morning_diagnostics.get("effective_mode", -1), "/", raster_day_morning_diagnostics.get("effective_mode", -1), "/", raster_day_afternoon_diagnostics.get("effective_mode", -1), " -> ", ray_day_afternoon_return_diagnostics.get("effective_mode", -1))

	var valid_modes := int(ray_day_morning_diagnostics.get("effective_mode", -1)) > 0 and int(ray_day_afternoon_diagnostics.get("effective_mode", -1)) > 0 and int(ray_night_evening_diagnostics.get("effective_mode", -1)) > 0 and int(raster_night_evening_diagnostics.get("effective_mode", -1)) == 0 and int(raster_night_morning_diagnostics.get("effective_mode", -1)) == 0 and int(raster_day_morning_diagnostics.get("effective_mode", -1)) == 0 and int(raster_day_afternoon_diagnostics.get("effective_mode", -1)) == 0 and int(ray_day_afternoon_return_diagnostics.get("effective_mode", -1)) > 0
	var ray_day_moves := ray_day_morning_shadow_right > ray_day_morning_shadow_left * 1.05 and ray_day_afternoon_shadow_left > ray_day_afternoon_shadow_right * 1.05
	var raster_night_moves := raster_night_evening_shadow_right > raster_night_evening_shadow_left * 1.05 and raster_night_morning_shadow_left > raster_night_morning_shadow_right * 1.05
	var raster_day_moves := raster_day_morning_shadow_right > raster_day_morning_shadow_left * 1.05 and raster_day_afternoon_shadow_left > raster_day_afternoon_shadow_right * 1.05
	# Night ray transport is deliberately stochastic environment sampling; the
	# exact ownership-transition equivalence is asserted on the daylight finite
	# lobe below. The raster night leg still proves the user's two clock values.
	var raster_night_transition_healthy := raster_night_evening_shadow_right > raster_night_evening_shadow_left * 1.05
	var transition_preserves_afternoon_direction := ray_day_afternoon_return_shadow_left > ray_day_afternoon_return_shadow_right * 1.05 and raster_day_afternoon_shadow_left > raster_day_afternoon_shadow_right * 1.05
	var highlights_move := ray_day_morning_highlight_left > ray_day_morning_highlight_right * 1.01 and ray_day_afternoon_highlight_right > ray_day_afternoon_highlight_left * 1.01 and raster_night_evening_highlight_left > raster_night_evening_highlight_right * 1.01 and raster_night_morning_highlight_right > raster_night_morning_highlight_left * 1.01 and raster_day_morning_highlight_left > raster_day_morning_highlight_right * 1.01 and raster_day_afternoon_highlight_right > raster_day_afternoon_highlight_left * 1.01
	if ray_day_morning.is_empty() or ray_day_afternoon.is_empty() or ray_night_evening.is_empty() or raster_night_evening.is_empty() or raster_night_morning.is_empty() or raster_day_morning.is_empty() or raster_day_afternoon.is_empty() or ray_day_afternoon_return.is_empty() or not valid_modes or not ray_day_moves or not raster_night_moves or not raster_day_moves or not raster_night_transition_healthy or not transition_preserves_afternoon_direction or not highlights_move:
		push_error("Flux runtime procedural-Sky toggle validation failed.")
		get_tree().quit(44)
		return
	var capture_prefix := "user://flux_runtime_sky_toggle_"
	if ray_day_morning.save_png(capture_prefix + "ray_day_morning.png") != OK or ray_day_afternoon.save_png(capture_prefix + "ray_day_afternoon.png") != OK or ray_night_evening.save_png(capture_prefix + "ray_night_evening.png") != OK or raster_night_evening.save_png(capture_prefix + "raster_night_evening.png") != OK or raster_night_morning.save_png(capture_prefix + "raster_night_morning.png") != OK or raster_day_morning.save_png(capture_prefix + "raster_day_morning.png") != OK or raster_day_afternoon.save_png(capture_prefix + "raster_day_afternoon.png") != OK or ray_day_afternoon_return.save_png(capture_prefix + "ray_day_afternoon_return.png") != OK:
		push_error("Could not save Flux runtime procedural-Sky toggle captures.")
		get_tree().quit(45)
		return
	print("FLUX_RUNTIME_SKY_TOGGLE_CAPTURE_PREFIX=", ProjectSettings.globalize_path(capture_prefix))
	get_tree().quit()

func _validate_flux_raster_procedural_sky() -> void:
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", 0)
	RenderingServer.viewport_set_flux_ray_tracing_enabled(get_viewport().get_viewport_rid(), false)
	get_viewport().scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR

	var atmosphere := AtmosphereSkyMaterial.new()
	atmosphere.cloud_coverage = 0.0
	atmosphere.sun_disk_energy = 2.0
	atmosphere.exposure = 1.0
	atmosphere.latitude = 0.0
	atmosphere.day_of_year = 80
	var procedural_sky := Sky.new()
	procedural_sky.process_mode = Sky.PROCESS_MODE_REALTIME
	procedural_sky.sky_material = atmosphere
	var environment_resource := Environment.new()
	environment_resource.background_mode = Environment.BG_SKY
	environment_resource.sky = procedural_sky
	environment_resource.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment_resource.reflected_light_source = Environment.REFLECTION_SOURCE_DISABLED
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment_resource
	add_child(world_environment)

	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 0.0, 5.0)
	camera.look_at_from_position(camera.position, Vector3.ZERO)
	camera.current = true
	add_child(camera)

	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.8, 0.8, 0.8)
	material.metallic = 0.0
	material.roughness = 1.0
	var floor_mesh := PlaneMesh.new()
	floor_mesh.size = Vector2(12.0, 12.0)
	floor_mesh.material = material
	var floor := MeshInstance3D.new()
	floor.mesh = floor_mesh
	floor.position.y = -1.35
	add_child(floor)
	var sphere_mesh := SphereMesh.new()
	sphere_mesh.radius = 0.72
	sphere_mesh.height = 1.44
	sphere_mesh.material = material
	var sphere := MeshInstance3D.new()
	sphere.mesh = sphere_mesh
	sphere.position = Vector3(0.0, -0.15, 0.0)
	add_child(sphere)

	atmosphere.time_of_day = 9.0
	for _frame in 24:
		await get_tree().process_frame
	var morning_direction := atmosphere.get_sun_direction()
	var morning := get_viewport().get_texture().get_image()
	# Environment rotation is applied at Sky lookup time. The renderer-owned
	# raster source must use the identical transform, so this 180-degree turn
	# moves the visible lobe and its cast-shadow family together.
	environment_resource.sky_rotation = Vector3(0.0, PI, 0.0)
	for _frame in 24:
		await get_tree().process_frame
	var rotated_morning := get_viewport().get_texture().get_image()
	environment_resource.sky_rotation = Vector3.ZERO
	for _frame in 24:
		await get_tree().process_frame
	atmosphere.time_of_day = 15.0
	for _frame in 24:
		await get_tree().process_frame
	var afternoon_direction := atmosphere.get_sun_direction()
	var afternoon := get_viewport().get_texture().get_image()
	var morning_left := _procedural_sky_side_luminance(morning, true)
	var morning_right := _procedural_sky_side_luminance(morning, false)
	var afternoon_left := _procedural_sky_side_luminance(afternoon, true)
	var afternoon_right := _procedural_sky_side_luminance(afternoon, false)
	var morning_shadow_left := _procedural_sky_shadow_energy(morning, true)
	var morning_shadow_right := _procedural_sky_shadow_energy(morning, false)
	var rotated_morning_shadow_left := _procedural_sky_shadow_energy(rotated_morning, true)
	var rotated_morning_shadow_right := _procedural_sky_shadow_energy(rotated_morning, false)
	var afternoon_shadow_left := _procedural_sky_shadow_energy(afternoon, true)
	var afternoon_shadow_right := _procedural_sky_shadow_energy(afternoon, false)
	atmosphere.sun_disk_energy = 0.0
	atmosphere.moon_disk_size = 10.0
	atmosphere.moon_disk_energy = 50.0
	atmosphere.time_of_day = 21.0
	for _frame in 24:
		await get_tree().process_frame
	var evening_moon_direction := atmosphere.get_moon_direction()
	var evening_moon := get_viewport().get_texture().get_image()
	atmosphere.time_of_day = 3.0
	for _frame in 24:
		await get_tree().process_frame
	var morning_moon_direction := atmosphere.get_moon_direction()
	var morning_moon := get_viewport().get_texture().get_image()
	var evening_moon_left := _procedural_sky_side_luminance(evening_moon, true)
	var evening_moon_right := _procedural_sky_side_luminance(evening_moon, false)
	var morning_moon_left := _procedural_sky_side_luminance(morning_moon, true)
	var morning_moon_right := _procedural_sky_side_luminance(morning_moon, false)
	var evening_moon_shadow_left := _procedural_sky_shadow_energy(evening_moon, true)
	var evening_moon_shadow_right := _procedural_sky_shadow_energy(evening_moon, false)
	var morning_moon_shadow_left := _procedural_sky_shadow_energy(morning_moon, true)
	var morning_moon_shadow_right := _procedural_sky_shadow_energy(morning_moon, false)
	print("FLUX_RASTER_PROCEDURAL_SKY_SHADOW_PROBE_DAY=", morning_shadow_left, "/", morning_shadow_right, " rotated=", rotated_morning_shadow_left, "/", rotated_morning_shadow_right, " afternoon=", afternoon_shadow_left, "/", afternoon_shadow_right)
	print("FLUX_RASTER_PROCEDURAL_SKY_SHADOW_PROBE_MOON=", evening_moon_shadow_left, "/", evening_moon_shadow_right, " ", morning_moon_shadow_left, "/", morning_moon_shadow_right)
	# The rendered sky remains an integration check, but its broad side windows
	# also include the floor and receiver. The directional source itself is
	# verified from the material API; the two floor probes are the raster-light
	# and shadow-family regression signal.
	if morning.is_empty() or rotated_morning.is_empty() or afternoon.is_empty() or evening_moon.is_empty() or morning_moon.is_empty() or morning_direction.x >= 0.0 or afternoon_direction.x <= 0.0 or morning_shadow_right <= morning_shadow_left * 1.05 or rotated_morning_shadow_left <= rotated_morning_shadow_right * 1.05 or afternoon_shadow_left <= afternoon_shadow_right * 1.05 or evening_moon_direction.x >= 0.0 or morning_moon_direction.x <= 0.0 or evening_moon_shadow_right <= evening_moon_shadow_left * 1.05 or morning_moon_shadow_left <= morning_moon_shadow_right * 1.05:
		push_error("Flux raster procedural-Sky direction failed: morning_dir=%s morning=%f/%f afternoon_dir=%s afternoon=%f/%f" % [morning_direction, morning_left, morning_right, afternoon_direction, afternoon_left, afternoon_right])
		get_tree().quit(41)
		return
	var capture_prefix := "user://flux_raster_procedural_sky_"
	if morning.save_png(capture_prefix + "morning.png") != OK or rotated_morning.save_png(capture_prefix + "morning_rotated.png") != OK or afternoon.save_png(capture_prefix + "afternoon.png") != OK or evening_moon.save_png(capture_prefix + "evening_moon.png") != OK or morning_moon.save_png(capture_prefix + "morning_moon.png") != OK:
		push_error("Could not save Flux raster procedural-Sky captures.")
		get_tree().quit(42)
		return
	print("FLUX_RASTER_PROCEDURAL_SKY_MORNING_DIRECTION=", morning_direction)
	print("FLUX_RASTER_PROCEDURAL_SKY_MORNING_LEFT_RIGHT=", morning_left, "/", morning_right)
	print("FLUX_RASTER_PROCEDURAL_SKY_MORNING_SHADOW_LEFT_RIGHT=", morning_shadow_left, "/", morning_shadow_right)
	print("FLUX_RASTER_PROCEDURAL_SKY_MORNING_ROTATED_SHADOW_LEFT_RIGHT=", rotated_morning_shadow_left, "/", rotated_morning_shadow_right)
	print("FLUX_RASTER_PROCEDURAL_SKY_AFTERNOON_DIRECTION=", afternoon_direction)
	print("FLUX_RASTER_PROCEDURAL_SKY_AFTERNOON_LEFT_RIGHT=", afternoon_left, "/", afternoon_right)
	print("FLUX_RASTER_PROCEDURAL_SKY_AFTERNOON_SHADOW_LEFT_RIGHT=", afternoon_shadow_left, "/", afternoon_shadow_right)
	print("FLUX_RASTER_PROCEDURAL_SKY_EVENING_MOON_DIRECTION=", evening_moon_direction)
	print("FLUX_RASTER_PROCEDURAL_SKY_EVENING_MOON_LEFT_RIGHT=", evening_moon_left, "/", evening_moon_right)
	print("FLUX_RASTER_PROCEDURAL_SKY_EVENING_MOON_SHADOW_LEFT_RIGHT=", evening_moon_shadow_left, "/", evening_moon_shadow_right)
	print("FLUX_RASTER_PROCEDURAL_SKY_MORNING_MOON_DIRECTION=", morning_moon_direction)
	print("FLUX_RASTER_PROCEDURAL_SKY_MORNING_MOON_LEFT_RIGHT=", morning_moon_left, "/", morning_moon_right)
	print("FLUX_RASTER_PROCEDURAL_SKY_MORNING_MOON_SHADOW_LEFT_RIGHT=", morning_moon_shadow_left, "/", morning_moon_shadow_right)
	print("FLUX_RASTER_PROCEDURAL_SKY_CAPTURE_PREFIX=", ProjectSettings.globalize_path(capture_prefix))
	get_tree().quit()

func _validate_flux_viewport_toggle() -> void:
	var viewport_rid := get_viewport().get_viewport_rid()
	var original_flux_mode := int(ProjectSettings.get_setting("rendering/flux/ray_tracing/enabled"))
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", 0)
	# Enabled viewports inherit the project mode. This begins disabled, then
	# changes live to Flux without changing the viewport override.
	RenderingServer.viewport_set_flux_ray_tracing_enabled(viewport_rid, true)
	for _frame in 4:
		await get_tree().process_frame
	var raster_image := get_viewport().get_texture().get_image()
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", 1)
	for _frame in 4:
		await get_tree().process_frame
	var flux_diagnostics: Dictionary = {}
	var trace_compaction: Dictionary = {}
	var compact_invariants: Dictionary = {}
	var compact_diagnostics_ready := false
	var dispatched_total := 0
	for _frame in 180:
		await get_tree().process_frame
		flux_diagnostics = RenderingServer.viewport_get_flux_diagnostics(viewport_rid)
		trace_compaction = flux_diagnostics.get("trace_compaction", {})
		compact_invariants = trace_compaction.get("invariants", {})
		dispatched_total = 0
		for trace_class in ["direct_only", "gi", "reflection", "exact_alpha", "complex_light"]:
			var class_counts: Dictionary = trace_compaction.get(trace_class, {})
			dispatched_total += int(class_counts.get("dispatched_count", 0))
		if bool(flux_diagnostics.get("valid", false)) and bool(flux_diagnostics.get("ray_effects_active", false)) and bool(flux_diagnostics.get("work_attribution_valid", false)) and bool(trace_compaction.get("active", false)) and not bool(trace_compaction.get("fallback", true)) and int(trace_compaction.get("active_pixel_count", 0)) > 0 and dispatched_total > 0 and bool(compact_invariants.get("sum_enqueued_equals_active", false)) and bool(compact_invariants.get("sum_dispatched_equals_active", false)) and bool(compact_invariants.get("inactive_excluded_from_trace", false)):
			compact_diagnostics_ready = true
			break
	if not compact_diagnostics_ready:
		push_error("Flux viewport-toggle validation did not observe a completed active compact trace diagnostic: %s" % JSON.stringify(flux_diagnostics))
		get_tree().quit(11)
		return
	var flux_image := get_viewport().get_texture().get_image()
	if raster_image.is_empty() or flux_image.is_empty():
		push_error("Flux viewport-toggle validation captured an empty frame.")
		get_tree().quit(7)
		return
	var raster_luminance := 0.0
	for y in raster_image.get_height():
		for x in raster_image.get_width():
			var pixel := raster_image.get_pixel(x, y)
			if not is_finite(pixel.r) or not is_finite(pixel.g) or not is_finite(pixel.b):
				push_error("Flux viewport-toggle validation captured non-finite raster output.")
				get_tree().quit(8)
				return
			raster_luminance += (pixel.r + pixel.g + pixel.b) / 3.0
	raster_luminance /= float(raster_image.get_width() * raster_image.get_height())
	var difference := _mean_absolute_rgb_difference(raster_image, flux_image)
	if raster_luminance <= 0.001 or difference < 0.0001:
		push_error("Flux viewport-toggle validation did not produce a non-black raster-to-flux transition: raster=%f difference=%f" % [raster_luminance, difference])
		get_tree().quit(9)
		return
	var submission_completion: Dictionary = flux_diagnostics.get("submission_completion", {})
	var diagnostic_snapshot := {
		"valid": flux_diagnostics.get("valid", false),
		"ray_effects_active": flux_diagnostics.get("ray_effects_active", false),
		"work_attribution_valid": flux_diagnostics.get("work_attribution_valid", false),
		"frame": flux_diagnostics.get("frame", 0),
		"submitted_frame": submission_completion.get("submitted_frame", 0),
		"observed_frame": submission_completion.get("observed_frame", 0),
		"timings_valid": flux_diagnostics.get("timings_valid", false),
		"timings_frame": flux_diagnostics.get("timings_frame", 0),
		"timings_ms": flux_diagnostics.get("timings_ms", {}),
		"transport": flux_diagnostics.get("transport", {}),
		"transport_complete": flux_diagnostics.get("transport_complete", false),
		"transport_revisions": flux_diagnostics.get("transport_revisions", {}),
		"trace_compaction": flux_diagnostics.get("trace_compaction", {}),
		"ray_work": flux_diagnostics.get("ray_work", {}),
		"direct_reservoir": flux_diagnostics.get("direct_reservoir", {}),
		"reusable_path_cache": flux_diagnostics.get("reusable_path_cache", {}),
		"diffuse_cache": flux_diagnostics.get("diffuse_cache", {}),
		"acceleration_structure": flux_diagnostics.get("acceleration_structure", {}),
		"environment_active": flux_diagnostics.get("environment_active", false),
		"environment_status": flux_diagnostics.get("environment_status", "disabled"),
		"environment_importance": flux_diagnostics.get("environment_importance", {}),
		"alpha": flux_diagnostics.get("alpha", {}),
		"materials": flux_diagnostics.get("materials", {})
	}
	print("FLUX_VIEWPORT_TOGGLE_DIAGNOSTICS_JSON=", JSON.stringify(diagnostic_snapshot))
	# Disabling remains an explicit viewport override even while the project mode
	# is enabled, which is the editor's opt-in behavior.
	RenderingServer.viewport_set_flux_ray_tracing_enabled(viewport_rid, false)
	for _frame in 4:
		await get_tree().process_frame
	var disabled_image := get_viewport().get_texture().get_image()
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", original_flux_mode)
	if disabled_image.is_empty() or _mean_absolute_rgb_difference(flux_image, disabled_image) < 0.0001:
		push_error("Flux viewport-toggle validation did not preserve the explicit disabled override.")
		get_tree().quit(10)
		return
	print("FLUX_VIEWPORT_TOGGLE_VALIDATION=PASS")
	get_tree().quit()

func _run_benchmark() -> void:
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", 1)
	# Counter pass boundaries intentionally perturb the measured command stream.
	# The benchmark records the normal runtime path; validation mode records counters.
	ProjectSettings.set_setting("rendering/flux/ray_tracing/diagnostics/collect_gpu_timings", false)
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
	print("FLUX_BENCHMARK_FRAMES=", frame_times_ms.size())
	print("FLUX_BENCHMARK_MEAN_FRAME_MS=", mean)
	print("FLUX_BENCHMARK_P99_FRAME_MS=", p99)
	get_tree().quit()

func _validate_flux_reuse_warmup() -> void:
	# Fixed camera and geometry exercise the cache-first GI replacement and the
	# per-pixel secondary convergence path without making a timing claim.
	animate_deformation = false
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", true)
	ProjectSettings.set_setting("rendering/flux/ray_tracing/diagnostics/collect_gpu_timings", false)
	await _wait_flux_frames(12)
	var cold: Dictionary = RenderingServer.viewport_get_flux_diagnostics(get_viewport().get_viewport_rid())
	await _wait_flux_frames(72)
	var warm: Dictionary = RenderingServer.viewport_get_flux_diagnostics(get_viewport().get_viewport_rid())
	var cold_work: Dictionary = cold.get("ray_work", {})
	var warm_work: Dictionary = warm.get("ray_work", {})
	var path_cache: Dictionary = warm.get("reusable_path_cache", {})
	var direct: Dictionary = warm.get("direct_reservoir", {})
	var warm_gi := int(warm_work.get("gi_fresh_ray_count", 0))
	var cold_gi := int(cold_work.get("gi_fresh_ray_count", 0))
	var staged := int(path_cache.get("staged_count", 0))
	var queried := int(path_cache.get("query_count", 0))
	var reused := int(path_cache.get("reused_candidate_count", 0))
	var temporal_di := int(direct.get("temporal_reuse_count", 0))
	print("FLUX_REUSE_WARMUP cold_gi=", cold_gi, " warm_gi=", warm_gi, " staged=", staged, " queried=", queried, " reused=", reused, " temporal_di=", temporal_di, " ray_work=", warm_work)
	if not bool(warm.get("work_attribution_valid", false)) or staged <= 0 or queried <= 0 or reused <= 0 or warm_gi >= cold_gi or temporal_di <= 0:
		push_error("Flux stationary reuse warmup did not replace transport rays: %s" % warm)
		get_tree().quit(48)
		return
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

func _validate_flux_temporal_detail() -> void:
	# Generic, self-contained high-frequency/morph fixture. It intentionally
	# uses force_draw rather than counting GDScript ticks: the engine diagnostic
	# at frames 60/120 is the assertion that MetalFX actually received history.
	# Keep one secondary estimator active so this fixture also proves that Flux
	# replays validated transport samples while MetalFX remains the sole image
	# denoiser. The moving phase must immediately reactivate exact secondary rays.
	ProjectSettings.set_setting("rendering/flux/ray_tracing/global_illumination/strength", 1.0)
	ProjectSettings.set_setting("rendering/flux/ray_tracing/global_illumination/sample_count", 1)
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

	RenderingServer.viewport_set_flux_ray_tracing_enabled(viewport.get_viewport_rid(), false)
	viewport.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	await _force_renderer_draws(viewport, 8)
	var raster_image := viewport.get_texture().get_image()

	viewport.scaling_3d_mode = Viewport.SCALING_3D_MODE_METALFX_TEMPORAL
	await _force_renderer_draws(viewport, 132)
	var ordinary_metalfx_image := viewport.get_texture().get_image()

	RenderingServer.viewport_set_flux_ray_tracing_enabled(viewport.get_viewport_rid(), true)
	await _force_renderer_draws(viewport, 132)
	var denoised_image := viewport.get_texture().get_image()
	var denoised_diagnostics: Dictionary = RenderingServer.viewport_get_flux_diagnostics(viewport.get_viewport_rid())
	var flux_renderer_frames := Engine.get_frames_drawn() - renderer_frames_before
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
	var moving_diagnostics: Dictionary = RenderingServer.viewport_get_flux_diagnostics(viewport.get_viewport_rid())
	animate_deformation = false
	await _force_renderer_draws(viewport, 18)
	var settled_motion_image := viewport.get_texture().get_image()

	if raster_image.is_empty() or ordinary_metalfx_image.is_empty() or denoised_image.is_empty() or denoised_static_repeat.is_empty() or disocclusion_image.is_empty() or returned_detail_image.is_empty() or moving_image.is_empty() or settled_motion_image.is_empty():
		push_error("Flux temporal-detail validation captured an empty image.")
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
	var metalfx_only := String(denoised_diagnostics.get("denoiser", "")) == "metalfx" and not bool(denoised_diagnostics.get("flux_image_reconstruction", true)) and String(moving_diagnostics.get("denoiser", "")) == "metalfx" and not bool(moving_diagnostics.get("flux_image_reconstruction", true))
	var static_ray_work: Dictionary = denoised_diagnostics.get("ray_work", {})
	var moving_ray_work: Dictionary = moving_diagnostics.get("ray_work", {})
	var static_converged_skips := int(static_ray_work.get("gi_converged_skip_count", 0)) + int(static_ray_work.get("reflection_converged_skip_count", 0))
	var static_fresh_secondary := int(static_ray_work.get("gi_fresh_ray_count", 0)) + int(static_ray_work.get("reflection_ray_count", 0))
	var moving_fresh_secondary := int(moving_ray_work.get("gi_fresh_ray_count", 0)) + int(moving_ray_work.get("reflection_ray_count", 0))
	var transport_schedule_valid := static_converged_skips > 0 and moving_fresh_secondary > static_fresh_secondary
	if flux_renderer_frames < 120 or denoised_detail_ratio < 0.20 or static_noise > 0.080 or disocclusion_delta < 0.0005 or return_settle_delta > 0.220 or motion_delta < 0.0001 or motion_settle_delta > 0.220 or not metalfx_only or not transport_schedule_valid:
		push_error("Flux temporal-detail validation failed: renderer_frames=%d detail_ratio=%f static_noise=%f disocclusion=%f return_settle=%f motion_delta=%f motion_settle=%f metalfx_only=%s static_skips=%d static_fresh=%d moving_fresh=%d transport_schedule_valid=%s" % [flux_renderer_frames, denoised_detail_ratio, static_noise, disocclusion_delta, return_settle_delta, motion_delta, motion_settle_delta, metalfx_only, static_converged_skips, static_fresh_secondary, moving_fresh_secondary, transport_schedule_valid])
		get_tree().quit(33)
		return
	var capture_prefix := "user://flux_temporal_detail_"
	if raster_image.save_png(capture_prefix + "raster.png") != OK or ordinary_metalfx_image.save_png(capture_prefix + "ordinary_metalfx.png") != OK or denoised_image.save_png(capture_prefix + "denoised.png") != OK or settled_motion_image.save_png(capture_prefix + "motion_settled.png") != OK:
		push_error("Could not save flux temporal-detail captures.")
		get_tree().quit(34)
		return
	print("FLUX_TEMPORAL_FORCE_DRAW_REQUESTS=132")
	print("FLUX_TEMPORAL_ACTUAL_RENDERER_FRAMES=", flux_renderer_frames)
	print("FLUX_TEMPORAL_RASTER_EDGE_DETAIL=", raster_detail)
	print("FLUX_TEMPORAL_ORDINARY_METALFX_EDGE_DETAIL=", ordinary_detail)
	print("FLUX_TEMPORAL_DENOISED_EDGE_DETAIL=", denoised_detail)
	print("FLUX_TEMPORAL_DENOISED_DETAIL_RATIO=", denoised_detail_ratio)
	print("FLUX_TEMPORAL_STATIC_NOISE=", static_noise)
	print("FLUX_TEMPORAL_DISOCCLUSION_DELTA=", disocclusion_delta)
	print("FLUX_TEMPORAL_RETURN_SETTLE_DELTA=", return_settle_delta)
	print("FLUX_TEMPORAL_MORPH_MOTION_DELTA=", motion_delta)
	print("FLUX_TEMPORAL_MORPH_SETTLE_DELTA=", motion_settle_delta)
	print("FLUX_TEMPORAL_STATIC_CONVERGED_SKIPS=", static_converged_skips)
	print("FLUX_TEMPORAL_STATIC_FRESH_SECONDARY=", static_fresh_secondary)
	print("FLUX_TEMPORAL_MOVING_FRESH_SECONDARY=", moving_fresh_secondary)
	print("FLUX_TEMPORAL_CAPTURE_PREFIX=", ProjectSettings.globalize_path(capture_prefix))
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


func _wait_flux_frames(frame_count: int) -> void:
	for _frame in frame_count:
		await get_tree().process_frame


func _validate_flux_mirror_caustic() -> void:
	# Geometric construction in the X/Y plane: the receiver is at (1, 0), the
	# source at (1, 2), and the finite mirror is x=0. Reflecting the source gives
	# (-1, 2), so the unique receiver->virtual-source line meets the mirror at
	# (0, 1), strictly inside its four-metre square. A small blocker interrupts
	# only the ordinary receiver->source segment, not either delta connection.
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
	scene_camera.position = Vector3(1.0, 2.35, 6.5)
	scene_camera.fov = 38.0
	scene_camera.look_at_from_position(scene_camera.position, Vector3(1.0, 0.0, 0.0))
	scene_camera.current = true
	add_child(scene_camera)
	var user_args := OS.get_cmdline_user_args()
	var feature_disabled := "--flux-caustic-disabled" in user_args
	var source_moved := "--flux-caustic-source-moved" in user_args
	var receiver_material := _add_diffuse_transport_box("CausticReceiver", Vector3(3.0, 0.08, 3.0), Vector3(1.0, -0.04, 0.0), Color(0.75, 0.75, 0.75))
	receiver_material.metallic = 0.0
	receiver_material.roughness = 0.8
	var mirror_material := StandardMaterial3D.new()
	mirror_material.albedo_color = Color(0.95, 0.95, 0.95)
	mirror_material.metallic = 1.0
	mirror_material.roughness = 0.0
	var mirror_mesh := PlaneMesh.new()
	mirror_mesh.size = Vector2(3.0, 3.0)
	mirror_mesh.material = mirror_material
	var mirror := MeshInstance3D.new()
	mirror.name = "CausticDeltaMirror"
	mirror.mesh = mirror_mesh
	mirror.position = Vector3(0.0, 1.0, 0.0)
	mirror.rotation_degrees.z = 90.0
	add_child(mirror)
	var source_material := StandardMaterial3D.new()
	source_material.albedo_color = Color(1.0, 0.9, 0.7)
	source_material.emission_enabled = true
	source_material.emission = Color(1.0, 0.9, 0.7)
	source_material.emission_energy_multiplier = 36.0
	var source_mesh := PlaneMesh.new()
	source_mesh.size = Vector2(0.7, 0.7)
	source_mesh.material = source_material
	var source := MeshInstance3D.new()
	source.name = "CausticFiniteEmitter"
	source.mesh = source_mesh
	source.position = Vector3(1.0, 2.32 if source_moved else 2.0, 0.0)
	source.rotation_degrees.z = 90.0
	add_child(source)
	_add_diffuse_transport_box("CausticDirectBlocker", Vector3(0.22, 0.20, 1.1), Vector3(1.0, 1.0, 0.0), Color(0.05, 0.05, 0.05))
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", true)
	ProjectSettings.set_setting("rendering/flux/ray_tracing/global_illumination/strength", 0.0)
	# The feature-off leg is a fresh process with --flux-caustic-disabled. Do
	# not mutate this setting in-process: RenderFlux intentionally caches it.
	ProjectSettings.set_setting("rendering/flux/ray_tracing/bidirectional_caustics/enabled", true)
	ProjectSettings.set_setting("rendering/flux/ray_tracing/bidirectional_caustics/delta_roughness_threshold", 0.02)
	ProjectSettings.set_setting("rendering/flux/ray_tracing/bidirectional_caustics/max_candidates", 1)
	await _wait_flux_frames(48)
	var enabled_image := get_viewport().get_texture().get_image()
	var diagnostics: Dictionary = RenderingServer.viewport_get_flux_diagnostics(get_viewport().get_viewport_rid())
	var caustic: Dictionary = diagnostics.get("bidirectional_caustic", {})
	if feature_disabled:
		if bool(caustic.get("active", false)) or int(caustic.get("candidate_count", 0)) != 0 or int(caustic.get("valid_count", 0)) != 0 or int(caustic.get("contributed_count", 0)) != 0 or int(caustic.get("visibility_ray_count", 0)) != 0:
			push_error("Flux fresh-process feature-off control still performed caustic work: %s" % caustic)
			get_tree().quit(41)
			return
	elif not bool(caustic.get("active", false)) or int(caustic.get("mirror_triangle_count", 0)) <= 0 or int(caustic.get("source_triangle_count", 0)) <= 0 or int(caustic.get("valid_count", 0)) <= 0 or int(caustic.get("contributed_count", 0)) <= 0 or int(caustic.get("visibility_ray_count", 0)) < 2:
		push_error("Flux planar mirror caustic diagnostics were not active/valid: %s" % caustic)
		get_tree().quit(42)
		return
	var capture_name := "disabled" if feature_disabled else ("moved" if source_moved else "enabled")
	var capture_path := "user://flux_mirror_caustic_%s.png" % capture_name
	if enabled_image.is_empty() or enabled_image.save_png(capture_path) != OK:
		push_error("Could not save planar mirror caustic captures.")
		get_tree().quit(43)
		return
	print("FLUX_MIRROR_CAUSTIC_DIAGNOSTICS=", caustic)
	print("FLUX_MIRROR_CAUSTIC_CAPTURE=", ProjectSettings.globalize_path(capture_path))
	get_tree().quit()


func _validate_diffuse_transport() -> void:
	# Isolate the physical secondary term: no raster light, a single one-sided
	# ceiling emitter, saturated red/green walls, and neutral receiving floor.
	# The A/B uses the same camera and primary raster; only Flux's
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
	ProjectSettings.set_setting("rendering/flux/ray_tracing/global_illumination/sample_count", 16)
	ProjectSettings.set_setting("rendering/flux/ray_tracing/global_illumination/strength", 1.0)
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", 1)
	for _frame in 48:
		await get_tree().process_frame
	var neutral_flux := get_viewport().get_texture().get_image()
	# The neutral/color wall A/B holds the primary visible floor, camera, emitter,
	# and Forward+ direct term fixed. Resetting only the reconstruction history
	# makes the post-opaque transport delta observable without interpreting the
	# emitter's own warm direct spectrum as a wall bounce.
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", 0)
	for _frame in 12:
		await get_tree().process_frame
	red_material.albedo_color = Color(0.86, 0.012, 0.012)
	green_material.albedo_color = Color(0.012, 0.86, 0.012)
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", 1)
	for _frame in 48:
		await get_tree().process_frame
	var colored_flux := get_viewport().get_texture().get_image()
	var size := colored_flux.get_size()
	var left_floor := Rect2i(Vector2i(int(size.x * 0.27), int(size.y * 0.63)), Vector2i(int(size.x * 0.17), int(size.y * 0.20)))
	var right_floor := Rect2i(Vector2i(int(size.x * 0.56), int(size.y * 0.63)), Vector2i(int(size.x * 0.17), int(size.y * 0.20)))
	var left_delta := _positive_transport_delta(neutral_flux, colored_flux, left_floor)
	var right_delta := _positive_transport_delta(neutral_flux, colored_flux, right_floor)
	if left_delta.r <= left_delta.g + 0.00008 or right_delta.g <= right_delta.r + 0.00008:
		push_error("Diffuse transport did not transfer red/green wall energy to neutral floor: left=%s right=%s" % [left_delta, right_delta])
		get_tree().quit(16)
		return
	var base_path := "user://flux_diffuse_transport_"
	if neutral_flux.save_png(base_path + "neutral.png") != OK or colored_flux.save_png(base_path + "colored.png") != OK:
		push_error("Could not save diffuse transport captures.")
		get_tree().quit(17)
		return
	print("FLUX_DIFFUSE_TRANSPORT_LEFT_DELTA=", left_delta)
	print("FLUX_DIFFUSE_TRANSPORT_RIGHT_DELTA=", right_delta)
	print("FLUX_DIFFUSE_TRANSPORT_CAPTURE_PREFIX=", ProjectSettings.globalize_path(base_path))
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
	ProjectSettings.set_setting("rendering/flux/ray_tracing/global_illumination/sample_count", 16)
	ProjectSettings.set_setting("rendering/flux/ray_tracing/global_illumination/strength", 1.0)
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", 1)
	for _frame in 48:
		await get_tree().process_frame
	var neutral_flux := get_viewport().get_texture().get_image()
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", 0)
	for _frame in 12:
		await get_tree().process_frame
	red_material.albedo_color = Color(0.86, 0.012, 0.012)
	green_material.albedo_color = Color(0.012, 0.86, 0.012)
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", 1)
	for _frame in 48:
		await get_tree().process_frame
	var colored_flux := get_viewport().get_texture().get_image()
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", 0)
	for _frame in 12:
		await get_tree().process_frame
	ProjectSettings.set_setting("rendering/flux/ray_tracing/global_illumination/strength", 0.0)
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", 1)
	for _frame in 32:
		await get_tree().process_frame
	var no_secondary_flux := get_viewport().get_texture().get_image()
	var size := colored_flux.get_size()
	var left_floor := Rect2i(Vector2i(int(size.x * 0.27), int(size.y * 0.63)), Vector2i(int(size.x * 0.17), int(size.y * 0.20)))
	var right_floor := Rect2i(Vector2i(int(size.x * 0.56), int(size.y * 0.63)), Vector2i(int(size.x * 0.17), int(size.y * 0.20)))
	var left_delta := _positive_transport_delta(neutral_flux, colored_flux, left_floor)
	var right_delta := _positive_transport_delta(neutral_flux, colored_flux, right_floor)
	var secondary_delta := _mean_absolute_rgb_difference_region(no_secondary_flux, colored_flux, left_floor) + _mean_absolute_rgb_difference_region(no_secondary_flux, colored_flux, right_floor)
	if left_delta.r <= left_delta.g + 0.00008 or right_delta.g <= right_delta.r + 0.00008 or secondary_delta <= 0.0002:
		push_error("Omni secondary transport failed: left=%s right=%s GI-on/off=%f" % [left_delta, right_delta, secondary_delta])
		get_tree().quit(18)
		return
	var base_path := "user://flux_omni_diffuse_transport_"
	if neutral_flux.save_png(base_path + "neutral.png") != OK or colored_flux.save_png(base_path + "colored.png") != OK or no_secondary_flux.save_png(base_path + "no_secondary.png") != OK:
		push_error("Could not save Omni diffuse transport captures.")
		get_tree().quit(19)
		return
	print("FLUX_OMNI_DIFFUSE_TRANSPORT_LEFT_DELTA=", left_delta)
	print("FLUX_OMNI_DIFFUSE_TRANSPORT_RIGHT_DELTA=", right_delta)
	print("FLUX_OMNI_DIFFUSE_TRANSPORT_GI_ON_OFF=", secondary_delta)
	print("FLUX_OMNI_DIFFUSE_TRANSPORT_CAPTURE_PREFIX=", ProjectSettings.globalize_path(base_path))
	get_tree().quit()

func _validate_transport_culling() -> void:
	# This fixture is intentionally independent from the bilateral Omni test:
	# it validates one off-camera source and one unrelated far opaque candidate.
	animate_deformation = false
	ProjectSettings.set_setting("rendering/flux/ray_tracing/transport_culling/enabled", true)
	ProjectSettings.set_setting("rendering/flux/ray_tracing/transport_culling/max_distance", 64.0)
	var transport_distance := float(ProjectSettings.get_setting("rendering/flux/ray_tracing/transport_culling/max_distance"))
	if not bool(ProjectSettings.get_setting("rendering/flux/ray_tracing/transport_culling/enabled")) or not is_equal_approx(transport_distance, 64.0):
		push_error("Flux transport-culling fixture could not configure enabled D=64 state.")
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
	print("FLUX_TRANSPORT_CULLING_FIXTURE_OFF_CAMERA_OMNI=", omni.position, " range=", omni.omni_range)
	print("FLUX_TRANSPORT_CULLING_FIXTURE_FAR_MESH=TransportCullFarOpaque position=", Vector3(200.0, 1.0, 0.0), " D=", transport_distance)
	ProjectSettings.set_setting("rendering/flux/ray_tracing/global_illumination/sample_count", 16)
	ProjectSettings.set_setting("rendering/flux/ray_tracing/global_illumination/strength", 1.0)
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", 1)
	for _frame in 48:
		await get_tree().process_frame
	var gi_on := get_viewport().get_texture().get_image()
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", 0)
	for _frame in 12:
		await get_tree().process_frame
	ProjectSettings.set_setting("rendering/flux/ray_tracing/global_illumination/strength", 0.0)
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", 1)
	for _frame in 32:
		await get_tree().process_frame
	var gi_off := get_viewport().get_texture().get_image()
	var size := gi_on.get_size()
	var left_floor := Rect2i(Vector2i(int(size.x * 0.27), int(size.y * 0.63)), Vector2i(int(size.x * 0.17), int(size.y * 0.20)))
	var right_floor := Rect2i(Vector2i(int(size.x * 0.56), int(size.y * 0.63)), Vector2i(int(size.x * 0.17), int(size.y * 0.20)))
	var transport_delta := _mean_absolute_rgb_difference_region(gi_off, gi_on, left_floor) + _mean_absolute_rgb_difference_region(gi_off, gi_on, right_floor)
	if transport_delta <= 0.0002:
		push_error("Flux transport-culling fixture GI-on/off floor ROI MAE was too small: %f" % transport_delta)
		get_tree().quit(21)
		return
	var base_path := "user://flux_transport_culling_"
	if gi_on.save_png(base_path + "gi_on.png") != OK or gi_off.save_png(base_path + "gi_off.png") != OK:
		push_error("Could not save flux transport-culling captures.")
		get_tree().quit(22)
		return
	print("FLUX_TRANSPORT_CULLING_GI_ON_OFF=", transport_delta)
	print("FLUX_TRANSPORT_CULLING_CAPTURE_PREFIX=", ProjectSettings.globalize_path(base_path))
	get_tree().quit()

func _capture_baked_visibility_frame(anchor_enabled: bool) -> Image:
	var anchor := get_node_or_null("BakedVisibilityAnchor") as BakedVisibilityVolume3D
	if anchor == null:
		return Image.new()
	anchor.enabled = anchor_enabled
	ProjectSettings.set_setting("rendering/occlusion_culling/baked_visibility/diagnostics", true)
	ProjectSettings.set_setting("rendering/flux/ray_tracing/transport_culling/enabled", true)
	ProjectSettings.set_setting("rendering/flux/ray_tracing/transport_culling/max_distance", anchor.transport_distance)
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", 1)
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
	# receiver. Their removal must change the receiver ROI through the flux
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
		push_error("Flux texture transport fixture has no checker material or texture.")
		get_tree().quit(12)
		return

	# The checker is deliberately behind the active camera. Its visible base color
	# must not perturb Forward+ primary raster output; it is positioned in the
	# glossy floor's ray-reflection path so only secondary flux evaluation can
	# introduce a color difference in the floor ROI below.
	animate_deformation = false
	# This A/B measures transport rather than MetalFX's intentionally changing
	# temporal reconstruction state. The normal validation retains MetalFX.
	get_viewport().scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", 0)
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

	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", 1)
	checker_material.albedo_texture = null
	for _frame in 24:
		await get_tree().process_frame
	var flux_scalar := get_viewport().get_texture().get_image()
	checker_material.albedo_texture = checker_texture
	for _frame in 24:
		await get_tree().process_frame
	var flux_textured := get_viewport().get_texture().get_image()
	var floor_roi := Rect2i(Vector2i(int(flux_scalar.get_width() * 0.30), int(flux_scalar.get_height() * 0.54)), Vector2i(int(flux_scalar.get_width() * 0.40), int(flux_scalar.get_height() * 0.34)))
	var transport_difference := _mean_absolute_rgb_difference_region(flux_scalar, flux_textured, floor_roi)
	if transport_difference < 0.0002:
		push_error("Opaque UV0 texture did not measurably affect the secondary transport floor ROI: %f" % transport_difference)
		get_tree().quit(14)
		return

	var base_path := "user://flux_opaque_uv0_transport_"
	if raster_scalar.save_png(base_path + "raster_scalar.png") != OK or raster_textured.save_png(base_path + "raster_textured.png") != OK or flux_scalar.save_png(base_path + "flux_scalar.png") != OK or flux_textured.save_png(base_path + "flux_textured.png") != OK:
		push_error("Could not save opaque UV0 transport captures.")
		get_tree().quit(15)
		return
	print("FLUX_UV0_TRANSPORT_RASTER_CONTROL_MAE=", raster_control_difference)
	print("FLUX_UV0_TRANSPORT_FLOOR_ROI_MAE=", transport_difference)
	print("FLUX_UV0_TRANSPORT_CAPTURE_PREFIX=", ProjectSettings.globalize_path(base_path))
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
		push_error("Flux editor viewport capture is empty.")
		get_tree().quit(5)
		return
	var capture_path := "user://flux_runtime_validation_editor.png"
	if image.save_png(capture_path) != OK:
		push_error("Could not save flux editor viewport capture.")
		get_tree().quit(6)
		return
	# The editor viewport texture contains reconstructed scene color only. Capture
	# the root window separately so editor-owned post-scene overlays are regressible.
	var composite_image := get_tree().root.get_texture().get_image()
	if composite_image.is_empty():
		push_error("Flux editor composite capture is empty.")
		get_tree().quit(7)
		return
	var composite_path := "user://flux_runtime_validation_editor_composite.png"
	if composite_image.save_png(composite_path) != OK:
		push_error("Could not save flux editor composite capture.")
		get_tree().quit(8)
		return
	print("FLUX_EDITOR_CAPTURE=", ProjectSettings.globalize_path(capture_path))
	print("FLUX_EDITOR_COMPOSITE_CAPTURE=", ProjectSettings.globalize_path(composite_path))
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
	var original_flux_mode := int(ProjectSettings.get_setting("rendering/flux/ray_tracing/enabled"))
	var original_scaling_mode := editor_viewport.scaling_3d_mode
	var original_scaling_scale := editor_viewport.scaling_3d_scale
	editor_camera.global_transform = scene_camera.global_transform
	editor_camera.fov = scene_camera.fov

	var cases := [
		{"name": "native", "flux": 0, "scaling": Viewport.SCALING_3D_MODE_BILINEAR},
		{"name": "metalfx_temporal", "flux": 0, "scaling": Viewport.SCALING_3D_MODE_METALFX_TEMPORAL},
		{"name": "metalfx_denoised", "flux": 2, "scaling": Viewport.SCALING_3D_MODE_METALFX_TEMPORAL},
	]
	var captures: Dictionary = {}
	for capture_case in cases:
		ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", capture_case.flux)
		editor_viewport.scaling_3d_mode = capture_case.scaling
		editor_viewport.scaling_3d_scale = 0.67
		for _frame in 36:
			await get_tree().process_frame
		var image := get_tree().root.get_texture().get_image()
		if image.is_empty():
			push_error("Flux editor overlay regression capture is empty.")
			get_tree().quit(9)
			return
		var name: String = capture_case.name
		var path := "user://flux_editor_overlay_%s.png" % name
		if image.save_png(path) != OK:
			push_error("Could not save flux editor overlay regression capture.")
			get_tree().quit(10)
			return
		captures[name] = image
		print("FLUX_EDITOR_OVERLAY_", name.to_upper(), "_LINE_WIDTH=", _editor_overlay_line_width(image))
		print("FLUX_EDITOR_OVERLAY_", name.to_upper(), "_CAPTURE=", ProjectSettings.globalize_path(path))
		var orbit_transform := editor_camera.global_transform
		orbit_transform.basis = Basis(Vector3.UP, 0.035) * orbit_transform.basis
		editor_camera.global_transform = orbit_transform
		await get_tree().process_frame
		var orbit_image := get_tree().root.get_texture().get_image()
		for _frame in 16:
			await get_tree().process_frame
		var settled_image := get_tree().root.get_texture().get_image()
		var orbit_path := "user://flux_editor_overlay_%s_orbit.png" % name
		var settled_path := "user://flux_editor_overlay_%s_orbit_settled.png" % name
		if orbit_image.is_empty() or settled_image.is_empty() or orbit_image.save_png(orbit_path) != OK or settled_image.save_png(settled_path) != OK:
			push_error("Could not save flux editor overlay orbit regression capture.")
			get_tree().quit(11)
			return
		var orbit_width := _editor_overlay_line_width(orbit_image)
		var settled_width := _editor_overlay_line_width(settled_image)
		print("FLUX_EDITOR_OVERLAY_", name.to_upper(), "_ORBIT_LINE_WIDTH=", orbit_width)
		print("FLUX_EDITOR_OVERLAY_", name.to_upper(), "_ORBIT_SETTLED_LINE_WIDTH=", settled_width)
		print("FLUX_EDITOR_OVERLAY_", name.to_upper(), "_ORBIT_LINE_WIDTH_DELTA=", absf(orbit_width - settled_width))
		editor_camera.global_transform = scene_camera.global_transform

	var native_image: Image = captures["native"]
	var denoised_image: Image = captures["metalfx_denoised"]
	print("FLUX_EDITOR_OVERLAY_NATIVE_DENOISED_MEAN_ABS_RGB=", _mean_absolute_rgb_difference(native_image, denoised_image))
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", original_flux_mode)
	editor_viewport.scaling_3d_mode = original_scaling_mode
	editor_viewport.scaling_3d_scale = original_scaling_scale
	get_tree().quit()

func _build_scene() -> void:
	if not "--validate-flux-no-environment" in OS.get_cmdline_user_args():
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
	if not "--benchmark-flux-scalar" in OS.get_cmdline_user_args():
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
