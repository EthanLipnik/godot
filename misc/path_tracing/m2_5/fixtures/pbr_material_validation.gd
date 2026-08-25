extends Node3D

## Small product-neutral Flux material fixture.
##
## The textures are generated only for this deterministic renderer check; they
## are not production content. The fixture exercises shared opaque material
## records for brick, rough ground, metal, a low-roughness dielectric pane,
## and a matte control against a bright sky.

const CAPTURE_PATH := "user://flux_pbr_material_validation.png"

func _make_checker(size: int, a: Color, b: Color) -> ImageTexture:
	var image := Image.create(size, size, false, Image.FORMAT_RGBA8)
	for y in size:
		for x in size:
			var tile := ((x / 8) + (y / 8)) & 1
			image.set_pixel(x, y, a if tile == 0 else b)
	return ImageTexture.create_from_image(image)

func _make_brick_normal(size: int) -> ImageTexture:
	var image := Image.create(size, size, false, Image.FORMAT_RGBA8)
	for y in size:
		for x in size:
			var row := y / 16
			var offset := 16 if (row & 1) else 0
			var joint := ((x + offset) % 32 == 0) or (y % 16 == 0)
			image.set_pixel(x, y, Color(0.5, 0.5, 0.82, 1.0) if joint else Color(0.5, 0.5, 1.0, 1.0))
	return ImageTexture.create_from_image(image)

func _material(albedo: Color, metallic: float, roughness: float) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = albedo
	material.metallic = metallic
	material.roughness = roughness
	material.metallic_specular = 0.5
	return material

func _box(name: String, size: Vector3, position: Vector3, material: Material) -> MeshInstance3D:
	var mesh := BoxMesh.new()
	mesh.size = size
	mesh.material = material
	var instance := MeshInstance3D.new()
	instance.name = name
	instance.mesh = mesh
	instance.position = position
	add_child(instance)
	return instance

func _ready() -> void:
	# This standalone milestone project uses the current Flux enum value; the
	# consuming city project may select its full mode independently.
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", 1)
	ProjectSettings.set_setting("rendering/flux/ray_tracing/diagnostics/collect_gpu_timings", true)
	RenderingServer.viewport_set_flux_ray_tracing_enabled(get_viewport().get_viewport_rid(), true)

	var environment := WorldEnvironment.new()
	var environment_resource := Environment.new()
	environment_resource.background_mode = Environment.BG_SKY
	environment_resource.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	environment_resource.ambient_light_energy = 0.35
	var sky := Sky.new()
	var sky_material := ProceduralSkyMaterial.new()
	sky_material.sky_top_color = Color(0.04, 0.12, 0.32)
	sky_material.sky_horizon_color = Color(1.0, 0.52, 0.16)
	sky_material.ground_bottom_color = Color(0.01, 0.01, 0.015)
	sky_material.ground_horizon_color = Color(0.16, 0.10, 0.08)
	sky_material.sun_angle_max = 8.0
	sky_material.sun_curve = 0.08
	sky.sky_material = sky_material
	environment_resource.sky = sky
	environment.environment = environment_resource
	add_child(environment)

	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 2.6, 9.0)
	camera.look_at_from_position(camera.position, Vector3(0.0, 1.4, 0.0))
	camera.current = true
	add_child(camera)

	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-35.0, -25.0, 0.0)
	sun.light_energy = 1.7
	sun.shadow_enabled = true
	add_child(sun)

	var ground_material := _material(Color(0.16, 0.18, 0.22), 0.0, 0.78)
	ground_material.albedo_texture = _make_checker(128, Color(0.15, 0.17, 0.2), Color(0.19, 0.2, 0.22))
	_box("RoughGround", Vector3(12.0, 0.2, 12.0), Vector3(0.0, -0.1, 0.0), ground_material)

	var brick_material := _material(Color(0.65, 0.12, 0.045), 0.0, 0.58)
	brick_material.albedo_texture = _make_checker(128, Color(0.58, 0.08, 0.025), Color(0.78, 0.22, 0.08))
	brick_material.normal_enabled = true
	brick_material.normal_texture = _make_brick_normal(128)
	brick_material.uv1_scale = Vector3(0.5, 0.5, 1.0)
	_box("BrickWall", Vector3(5.0, 3.4, 0.18), Vector3(-2.0, 1.7, -1.2), brick_material)

	var metal_material := _material(Color(0.16, 0.19, 0.23), 0.92, 0.12)
	_box("MetalControl", Vector3(1.4, 1.4, 1.4), Vector3(2.4, 0.9, -0.5), metal_material).rotation_degrees.y = 24.0

	var pane_material := _material(Color(0.045, 0.10, 0.17), 0.0, 0.10)
	pane_material.metallic_specular = 0.5
	_box("DielectricPane", Vector3(1.55, 2.2, 0.06), Vector3(0.15, 1.45, 0.8), pane_material)

	var matte_material := _material(Color(0.045, 0.10, 0.17), 0.0, 0.85)
	_box("MatteControl", Vector3(1.55, 2.2, 0.06), Vector3(1.95, 1.45, 0.8), matte_material)

	var emitter_material := _material(Color(1.0, 0.8, 0.45), 0.0, 0.35)
	emitter_material.emission_enabled = true
	emitter_material.emission = Color(1.0, 0.55, 0.18)
	emitter_material.emission_energy_multiplier = 9.0
	_box("BrightEmitter", Vector3(2.0, 0.08, 1.3), Vector3(0.0, 4.5, -0.8), emitter_material).rotation_degrees.x = 180.0

	for _frame in 100:
		await get_tree().process_frame
	var image := get_viewport().get_texture().get_image()
	var diagnostics := RenderingServer.viewport_get_flux_diagnostics(get_viewport().get_viewport_rid())
	if image.is_empty() or not bool(diagnostics.get("valid", false)):
		push_error("Flux PBR fixture did not produce a valid image/diagnostic: %s" % JSON.stringify(diagnostics))
		get_tree().quit(2)
		return
	if image.save_png(CAPTURE_PATH) != OK:
		push_error("Flux PBR fixture could not save its capture.")
		get_tree().quit(3)
		return
	print("FLUX_PBR_FIXTURE_CAPTURE=", ProjectSettings.globalize_path(CAPTURE_PATH))
	print("FLUX_PBR_FIXTURE_DIAGNOSTICS=", JSON.stringify(diagnostics))
	print("FLUX_PBR_FIXTURE_EXPECTED_PANE_ROUGHNESS=0.10")
	print("FLUX_PBR_FIXTURE_EXPECTED_UV_SCALE_TOLERANCE=0.05")
	get_tree().quit()
