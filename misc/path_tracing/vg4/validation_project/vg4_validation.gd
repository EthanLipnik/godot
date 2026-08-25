extends Node3D

var offscreen_contributors: Array[Node3D] = []

func _material(color: Color, metallic := 0.0, roughness := 0.5, emission := Color.BLACK, emission_energy := 1.0) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = color
	material.metallic = metallic
	material.roughness = roughness
	if emission != Color.BLACK:
		material.emission_enabled = true
		material.emission = emission
		material.emission_energy_multiplier = emission_energy
	return material

func _box(name: String, position: Vector3, size: Vector3, material: Material) -> MeshInstance3D:
	var node := MeshInstance3D.new()
	node.name = name
	var mesh := BoxMesh.new()
	mesh.size = size
	mesh.material = material
	node.mesh = mesh
	node.position = position
	add_child(node)
	return node

func _build_fixture() -> void:
	var environment_node := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.008, 0.01, 0.015)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.025, 0.03, 0.04)
	environment.ambient_light_energy = 0.3
	environment_node.environment = environment
	add_child(environment_node)

	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 1.5, 6.5)
	camera.fov = 52.0
	add_child(camera)
	camera.look_at(Vector3(0.0, 1.0, 0.0))
	camera.current = true

	_box("DiffuseReceiver", Vector3(0.0, -0.15, 0.0), Vector3(8.0, 0.2, 7.0), _material(Color(0.32, 0.34, 0.38), 0.0, 0.85))
	var mirror := _box("SharpMirror", Vector3(0.0, 1.15, -0.55), Vector3(3.1, 2.25, 0.12), _material(Color(0.88, 0.9, 0.95), 1.0, 0.01))
	mirror.rotation.y = deg_to_rad(-18.0)

	var emitter := _box("OffscreenEmitter", Vector3(-4.2, 1.15, 0.35), Vector3(1.0, 1.8, 0.4), _material(Color(0.8, 0.05, 0.02), 0.0, 0.35, Color(1.0, 0.045, 0.01), 14.0))
	offscreen_contributors.push_back(emitter)
	var light := OmniLight3D.new()
	light.name = "OffscreenEmitterLight"
	light.position = Vector3(-4.0, 1.3, 0.25)
	light.light_color = Color(1.0, 0.08, 0.025)
	light.light_energy = 8.0
	light.omni_range = 7.0
	light.shadow_enabled = true
	add_child(light)
	offscreen_contributors.push_back(light)

	var blocker := _box("OffscreenBlocker", Vector3(-2.65, 1.0, 0.15), Vector3(0.35, 1.9, 1.2), _material(Color(0.025, 0.028, 0.035), 0.0, 0.9))
	offscreen_contributors.push_back(blocker)
	var reflected_panel := _box("OffscreenReflectionContributor", Vector3(3.9, 1.2, -0.2), Vector3(1.1, 2.1, 0.25), _material(Color(0.02, 0.12, 0.95), 0.15, 0.12))
	offscreen_contributors.push_back(reflected_panel)

func _mean_rgb_difference(a: Image, b: Image) -> float:
	if a.is_empty() or b.is_empty() or a.get_size() != b.get_size():
		return 0.0
	var total := 0.0
	for y in a.get_height():
		for x in a.get_width():
			var delta := a.get_pixel(x, y) - b.get_pixel(x, y)
			total += absf(delta.r) + absf(delta.g) + absf(delta.b)
	return total / float(a.get_width() * a.get_height() * 3)

func _ready() -> void:
	_build_fixture()
	for _frame in 48:
		await get_tree().process_frame
	var retained := get_viewport().get_texture().get_image()
	for contributor in offscreen_contributors:
		contributor.position += Vector3(0.0, 100.0, 0.0)
	for _frame in 48:
		await get_tree().process_frame
	var removed := get_viewport().get_texture().get_image()
	var difference := _mean_rgb_difference(retained, removed)
	if retained.is_empty() or removed.is_empty():
		push_error("VG4 Metal fixture produced an empty capture.")
		get_tree().quit(2)
		return
	if difference < 0.00001:
		push_error("Off-screen emitter/blocker/reflection contributors were not retained by transport: %f" % difference)
		get_tree().quit(3)
		return
	print("VG4_METAL_FIXTURE_PATH=metal_primitive_as_masked_tlas")
	print("VG4_METAL_FIXTURE_ROLES=hidden_emitter,hidden_blocker,sharp_mirror,offscreen_contributor")
	print("VG4_METAL_FIXTURE_MEAN_RGB_DIFFERENCE=", difference)
	get_tree().quit()

