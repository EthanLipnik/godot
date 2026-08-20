@tool
extends Node3D

var elapsed := 0.0
var validation_frames := 0

func _material(color: Color, metallic: float, roughness: float, emission := Color(0, 0, 0)) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = color
	material.metallic = metallic
	material.roughness = roughness
	material.emission_enabled = emission != Color(0, 0, 0)
	material.emission = emission
	material.emission_energy_multiplier = 3.0
	return material

func _triangle_mesh(vertices: PackedVector3Array, material: Material) -> ArrayMesh:
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_NORMAL] = PackedVector3Array([Vector3(0, 0, 1), Vector3(0, 0, 1), Vector3(0, 0, 1)])
	arrays[Mesh.ARRAY_INDEX] = PackedInt32Array([0, 1, 2])
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	mesh.surface_set_material(0, material)
	return mesh

func _character_mesh() -> ArrayMesh:
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([Vector3(-0.45, -0.8, 0), Vector3(0.45, -0.8, 0), Vector3(0, 0.85, 0)])
	arrays[Mesh.ARRAY_NORMAL] = PackedVector3Array([Vector3(0, 0, 1), Vector3(0, 0, 1), Vector3(0, 0, 1)])
	arrays[Mesh.ARRAY_INDEX] = PackedInt32Array([0, 1, 2])
	var bones := PackedInt32Array()
	var weights := PackedFloat32Array()
	for vertex in 3:
		for influence in 8:
			bones.push_back(0)
			weights.push_back(1.0 if influence == 0 else 0.0)
	arrays[Mesh.ARRAY_BONES] = bones
	arrays[Mesh.ARRAY_WEIGHTS] = weights
	var blend := []
	blend.resize(Mesh.ARRAY_MAX)
	blend[Mesh.ARRAY_VERTEX] = PackedVector3Array([Vector3(-0.5, 0, 0), Vector3(0.5, 0, 0), Vector3(0, 0.25, 0)])
	blend[Mesh.ARRAY_NORMAL] = PackedVector3Array([Vector3.ZERO, Vector3.ZERO, Vector3.ZERO])
	var mesh := ArrayMesh.new()
	mesh.add_blend_shape("Silhouette")
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays, [blend], {}, Mesh.ARRAY_FLAG_USE_8_BONE_WEIGHTS)
	mesh.surface_set_material(0, _material(Color(0.12, 0.22, 0.8), 0.15, 0.28, Color(0.01, 0.02, 0.08)))
	return mesh

func _ready() -> void:
	var skeleton: Skeleton3D = $Skeleton
	if skeleton.get_bone_count() == 0:
		skeleton.add_bone("Root")
		skeleton.set_bone_rest(0, Transform3D.IDENTITY)
	var skin := Skin.new()
	skin.set_bind_count(1)
	skin.set_bind_bone(0, 0)
	skin.set_bind_pose(0, Transform3D.IDENTITY)
	$Character.mesh = _character_mesh()
	$Character.skin = skin
	$Character.position = Vector3(0, 0.1, -0.7)
	$Mirror.mesh = _triangle_mesh(PackedVector3Array([Vector3(-2.5, -1.1, 0), Vector3(2.5, -1.1, 0), Vector3(0, 2.2, 0)]), _material(Color(0.92, 0.94, 1.0), 1.0, 0.02))
	$Mirror.position = Vector3(0, 0, -1.8)
	$Ground.mesh = _triangle_mesh(PackedVector3Array([Vector3(-4, -1, 1), Vector3(4, -1, 1), Vector3(0, -1, -5)]), _material(Color(0.16, 0.16, 0.18), 0.0, 0.75))
	set_process(true)

func _process(delta: float) -> void:
	elapsed += delta
	var phase := sin(elapsed * 1.7)
	$Character.set_blend_shape_value(0, phase * 0.5 + 0.5)
	$Skeleton.set_bone_pose_position(0, Vector3(phase * 0.22, 0, 0))
	validation_frames += 1
	if "--validate-deformation" in OS.get_cmdline_user_args() and validation_frames == 4:
		var baked: ArrayMesh = $Character.bake_mesh_from_current_deformation()
		var vertices: PackedVector3Array = baked.surface_get_arrays(0)[Mesh.ARRAY_VERTEX]
		var passed := baked != null and vertices.size() == 3 and vertices[0].x != -0.45
		print(JSON.stringify({"schema": 1, "scene": "animated_silhouette_mirror", "eight_weight_vertices": vertices.size(), "combined_deformation_changed": passed}))
		get_tree().quit(0 if passed else 1)
