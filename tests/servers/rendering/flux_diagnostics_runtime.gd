extends SceneTree

var viewport: SubViewport

func _initialize() -> void:
	ProjectSettings.set_setting("rendering/flux/ray_tracing/enabled", 1)
	ProjectSettings.set_setting("rendering/flux/ray_tracing/diagnostics/collect_gpu_timings", true)
	ProjectSettings.set_setting("rendering/flux/ray_tracing/environment_lighting/enabled", true)

	viewport = SubViewport.new()
	viewport.size = Vector2i(320, 180)
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	viewport.own_world_3d = true
	root.add_child(viewport)

	var scene := Node3D.new()
	viewport.add_child(scene)

	var environment := Environment.new()
	environment.background_mode = Environment.BG_SKY
	var sky := Sky.new()
	sky.sky_material = ProceduralSkyMaterial.new()
	environment.sky = sky
	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment
	scene.add_child(world_environment)

	var mesh_instance := MeshInstance3D.new()
	var mesh := BoxMesh.new()
	mesh.size = Vector3(2.0, 2.0, 2.0)
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.7, 0.2, 0.08)
	material.metallic = 0.25
	material.roughness = 0.4
	mesh.material = material
	mesh_instance.mesh = mesh
	scene.add_child(mesh_instance)

	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-50.0, -30.0, 0.0)
	light.shadow_enabled = true
	scene.add_child(light)

	var camera := Camera3D.new()
	camera.position = Vector3(0.0, 0.0, 5.0)
	camera.current = true
	scene.add_child(camera)

	RenderingServer.viewport_set_flux_ray_tracing_enabled(viewport.get_viewport_rid(), true)
	_run.call_deferred()

func _run() -> void:
	var invalid := RenderingServer.viewport_get_flux_diagnostics(RID())
	if invalid.valid or not invalid.has("admitted") or not invalid.has("materials") or not invalid.has("timings_ms"):
		push_error("Flux diagnostics invalid-RID schema is not stable: %s" % JSON.stringify(invalid))
		quit(5)
		return
	if RenderingServer.get_current_rendering_method() != "flux":
		for _frame in 4:
			await process_frame
		var non_flux := RenderingServer.viewport_get_flux_diagnostics(viewport.get_viewport_rid())
		if non_flux.valid:
			push_error("A non-Flux renderer returned active Flux diagnostics: %s" % JSON.stringify(non_flux))
			quit(6)
			return
		print("FLUX_DIAGNOSTICS_NON_FLUX=", JSON.stringify(non_flux))
		quit()
		return

	var diagnostics := {}
	for _frame in 180:
		await process_frame
		diagnostics = RenderingServer.viewport_get_flux_diagnostics(viewport.get_viewport_rid())
		if diagnostics.valid and diagnostics.ray_effects_active and diagnostics.timings_valid:
			break

	if not diagnostics.valid or diagnostics.effective_mode != 2 or not diagnostics.ray_effects_active:
		push_error("Flux diagnostics did not report an active completed frame: %s" % JSON.stringify(diagnostics))
		quit(2)
		return
	if diagnostics.primary_surface_version != 1 or not diagnostics.ray_owned_shading or diagnostics.primary_surface_view_count != 1 or not diagnostics.transport_complete:
		push_error("Flux diagnostics did not report complete ray-owned PrimarySurfaceV1 shading: %s" % JSON.stringify(diagnostics))
		quit(7)
		return
	if diagnostics.admitted.geometry_count < 1 or diagnostics.admitted.surface_count < 1 or diagnostics.admitted.selected_triangle_count < 1 or diagnostics.admitted.canonical_material_count < 1:
		push_error("Flux diagnostics did not expose admitted scene counts: %s" % JSON.stringify(diagnostics))
		quit(3)
		return
	if not diagnostics.timings_valid or diagnostics.timings_ms.ray_effects <= 0.0:
		push_error("Flux diagnostics did not expose a completed GPU timing capture: %s" % JSON.stringify(diagnostics))
		quit(4)
		return

	print("FLUX_DIAGNOSTICS_RUNTIME=", JSON.stringify(diagnostics))
	quit()
