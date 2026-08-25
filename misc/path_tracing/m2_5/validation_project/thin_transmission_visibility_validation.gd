extends Node3D

## Off-camera thin-pane visibility validation. The panes are deliberately
## above the camera frustum but intersect the selected point-light paths to
## the center and right receivers. This validates ray visibility, not primary
## raster glass composition.

const CAPTURE_PATH := "user://flux_thin_transmission_visibility.png"
const NO_PANE_X := -1.8
const THIN_PANE_X := 0.0
const OPAQUE_PANE_X := 1.8

func _receiver(position: Vector3) -> void:
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.78, 0.78, 0.78)
	material.metallic = 0.0
	material.roughness = 0.72
	var mesh := BoxMesh.new()
	mesh.size = Vector3(1.2, 1.2, 0.08)
	mesh.material = material
	var node := MeshInstance3D.new()
	node.mesh = mesh
	node.position = position
	add_child(node)

func _pane(position: Vector3, thin: bool) -> void:
	var material := StandardMaterial3D.new()
	material.albedo_color = Color.WHITE
	material.metallic = 0.0
	material.roughness = 0.02
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	if thin:
		material.set_thin_transmission(0.9)
		material.set_thin_ior(1.5)
	var mesh := BoxMesh.new()
	mesh.size = Vector3(1.35, 0.04, 1.35)
	mesh.material = material
	var node := MeshInstance3D.new()
	node.name = "ThinPane" if thin else "OpaquePane"
	node.mesh = mesh
	node.position = position
	add_child(node)

func _mean_luminance(image: Image, rectangle: Rect2i) -> float:
	var sum := 0.0
	var count := 0
	for y in range(rectangle.position.y, rectangle.end.y):
		for x in range(rectangle.position.x, rectangle.end.x):
			var color := image.get_pixel(x, y)
			if not is_finite(color.r) or not is_finite(color.g) or not is_finite(color.b):
				return NAN
			sum += color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722
			count += 1
	return sum / max(count, 1)

func _ready() -> void:
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", 1)
	RenderingServer.viewport_set_flux_ray_tracing_enabled(get_viewport().get_viewport_rid(), true)

	var environment := WorldEnvironment.new()
	var environment_resource := Environment.new()
	environment_resource.background_mode = Environment.BG_COLOR
	environment_resource.background_color = Color.BLACK
	environment_resource.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment_resource.ambient_light_color = Color.BLACK
	environment_resource.ambient_light_energy = 0.0
	environment.environment = environment_resource
	add_child(environment)

	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 0.0, 8.0)
	camera.fov = 36.0
	camera.look_at_from_position(camera.position, Vector3.ZERO)
	camera.current = true
	add_child(camera)

	_receiver(Vector3(NO_PANE_X, 0.0, 0.0))
	_receiver(Vector3(THIN_PANE_X, 0.0, 0.0))
	_receiver(Vector3(OPAQUE_PANE_X, 0.0, 0.0))
	# The light-to-receiver segments pass through y=2.5, z=2.0. At this
	# height the narrow camera frustum cannot see either pane.
	_pane(Vector3(0.0, 2.5, 2.0), true)
	_pane(Vector3(0.9, 2.5, 2.0), false)

	var light := OmniLight3D.new()
	light.position = Vector3(0.0, 5.0, 4.0)
	light.light_energy = 18.0
	light.omni_range = 20.0
	light.shadow_enabled = true
	add_child(light)

	for _frame in 150:
		await get_tree().process_frame
	var diagnostics := RenderingServer.viewport_get_flux_diagnostics(get_viewport().get_viewport_rid())
	var materials: Dictionary = diagnostics.get("materials", {})
	if not bool(diagnostics.get("valid", false)) or int(materials.get("supported_thin_transmission_count", 0)) <= 0 or int(materials.get("unsupported_transmission_texture_count", -1)) != 0 or int(materials.get("unsupported_transmission_volume_count", -1)) != 0:
		push_error("Thin transmission diagnostics invalid: %s" % JSON.stringify(diagnostics))
		get_tree().quit(2)
		return
	var image := get_viewport().get_texture().get_image()
	if image.is_empty() or image.save_png(CAPTURE_PATH) != OK:
		push_error("Thin transmission fixture capture failed.")
		get_tree().quit(3)
		return
	# The receiver centers map to left/center/right at this fixed camera.
	var no_pane := _mean_luminance(image, Rect2i(155, 145, 80, 70))
	var thin_pane := _mean_luminance(image, Rect2i(280, 145, 80, 70))
	var opaque_pane := _mean_luminance(image, Rect2i(405, 145, 80, 70))
	if not is_finite(no_pane) or not is_finite(thin_pane) or not is_finite(opaque_pane) or not (no_pane > thin_pane + 0.002 and thin_pane > opaque_pane + 0.002):
		push_error("Thin visibility controls failed: no-pane=%f thin=%f opaque=%f" % [no_pane, thin_pane, opaque_pane])
		get_tree().quit(4)
		return
	print("FLUX_THIN_TRANSMISSION_VISIBILITY=PASS no_pane=%f thin=%f opaque=%f capture=%s" % [no_pane, thin_pane, opaque_pane, ProjectSettings.globalize_path(CAPTURE_PATH)])
	get_tree().quit()
