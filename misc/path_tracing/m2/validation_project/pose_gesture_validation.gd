@tool
extends Node3D

const MORPH_COUNT := 35
const PoseDriver = preload("res://pose_space_morph_driver.gd")
const GestureTether = preload("res://tracked_gesture_tether.gd")
const ArmSolver = preload("res://tracked_arm_solver.gd")

var driver = PoseDriver.new()
var tether = GestureTether.new()
var arm_solver = ArmSolver.new()
var elapsed := 0.0
var validation_frames := 0
var maximum_weight_step := 0.0
var previous_weights := PackedFloat32Array()
var activated := PackedByteArray()
var gesture_sequence_passed := false
var maximum_arm_step := 0.0
var maximum_wrist_error := 0.0
var maximum_finger_step := 0.0
var previous_elbow := Vector3.ZERO
var previous_fingers: Array[Vector3] = []
var arm_finite := true

func _material(color: Color, metallic: float, roughness: float, emission := Color(0, 0, 0)) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = color
	material.metallic = metallic
	material.roughness = roughness
	material.emission_enabled = emission != Color(0, 0, 0)
	material.emission = emission
	material.emission_energy_multiplier = 2.0
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
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([Vector3(-0.55, -0.9, 0), Vector3(0.55, -0.9, 0), Vector3(0, 1.0, 0)])
	arrays[Mesh.ARRAY_NORMAL] = PackedVector3Array([Vector3(0, 0, 1), Vector3(0, 0, 1), Vector3(0, 0, 1)])
	arrays[Mesh.ARRAY_INDEX] = PackedInt32Array([0, 1, 2])
	var bones := PackedInt32Array()
	var bone_weights := PackedFloat32Array()
	for vertex in 3:
		for influence in 8:
			bones.push_back(0)
			bone_weights.push_back(1.0 if influence == 0 else 0.0)
	arrays[Mesh.ARRAY_BONES] = bones
	arrays[Mesh.ARRAY_WEIGHTS] = bone_weights
	var blend_shapes: Array = []
	var mesh := ArrayMesh.new()
	for index in MORPH_COUNT:
		mesh.add_blend_shape("Pose_%02d" % index)
		var blend := []
		blend.resize(Mesh.ARRAY_MAX)
		var magnitude := 1.5 if index == 0 else 0.002 + float(index % 7) * 0.001
		blend[Mesh.ARRAY_VERTEX] = PackedVector3Array([Vector3(-magnitude, 0, 0), Vector3(magnitude, 0, 0), Vector3(0, magnitude, 0)])
		blend[Mesh.ARRAY_NORMAL] = PackedVector3Array([Vector3.ZERO, Vector3.ZERO, Vector3.ZERO])
		blend_shapes.push_back(blend)
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays, blend_shapes, {}, Mesh.ARRAY_FLAG_USE_8_BONE_WEIGHTS)
	mesh.surface_set_material(0, _material(Color(0.15, 0.22, 0.72), 0.15, 0.28, Color(0.005, 0.01, 0.03)))
	return mesh

func _ready() -> void:
	if $Skeleton.get_bone_count() == 0:
		$Skeleton.add_bone("Root")
		$Skeleton.set_bone_rest(0, Transform3D.IDENTITY)
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
	var definitions: Array[Dictionary] = []
	for index in MORPH_COUNT:
		definitions.push_back({"feature": "pose_%02d" % index, "minimum": -0.35, "maximum": 0.65, "response_hz": 18.0})
	driver.configure(definitions)
	previous_weights.resize(MORPH_COUNT)
	activated.resize(MORPH_COUNT)
	set_process(true)

func _process(delta: float) -> void:
	elapsed += delta
	var features := {}
	for index in MORPH_COUNT:
		features["pose_%02d" % index] = sin(elapsed * 1.4 + float(index) * TAU / MORPH_COUNT)
	var weights: PackedFloat32Array = driver.evaluate(features, delta)
	for index in MORPH_COUNT:
		$Character.set_blend_shape_value(index, weights[index])
		if validation_frames > 10:
			maximum_weight_step = maxf(maximum_weight_step, absf(weights[index] - previous_weights[index]))
		activated[index] = 1 if activated[index] != 0 or weights[index] > 0.7 else 0
		previous_weights[index] = weights[index]
	$Skeleton.set_bone_pose_position(0, Vector3(sin(elapsed) * 0.18, 0, 0))
	var tracking_confidence := 0.1 if validation_frames >= 300 and validation_frames < 330 else 1.0
	var observed_wrist := Vector3(0.55 + sin(elapsed * 1.8) * 0.12, 1.1 + cos(elapsed * 1.3) * 0.08, 0.1)
	var arm: Dictionary = arm_solver.solve(Vector3(0, 1.35, 0), observed_wrist, Vector3(0, 0.8, 0.8), 0.38, 0.36, tracking_confidence, delta)
	var elbow: Vector3 = arm.elbow
	var wrist: Vector3 = arm.wrist
	if validation_frames > 1:
		maximum_arm_step = maxf(maximum_arm_step, elbow.distance_to(previous_elbow))
	previous_elbow = elbow
	maximum_wrist_error = maxf(maximum_wrist_error, wrist.distance_to(arm.target))
	arm_finite = arm_finite and elbow.is_finite() and wrist.is_finite()
	var observed_fingers: Array[Vector3] = []
	for joint in 26:
		observed_fingers.push_back(Vector3(float(joint) * 0.002, sin(elapsed + joint * 0.1) * 0.01, 0))
	var fingers: Array[Vector3] = arm_solver.filter_fingers(observed_fingers, tracking_confidence, delta)
	if previous_fingers.size() == fingers.size():
		for joint in fingers.size():
			maximum_finger_step = maxf(maximum_finger_step, fingers[joint].distance_to(previous_fingers[joint]))
	previous_fingers = fingers.duplicate()
	validation_frames += 1
	if validation_frames == 2:
		gesture_sequence_passed = tether.update(0.02, 1.0, false, Vector3(1, 2, 3)) == GestureTether.State.CANDIDATE
	if validation_frames == 3:
		gesture_sequence_passed = gesture_sequence_passed and tether.update(0.02, 1.0, false, Vector3(1, 2, 3)) == GestureTether.State.CANDIDATE
	if validation_frames == 4:
		gesture_sequence_passed = gesture_sequence_passed and tether.update(0.02, 1.0, false, Vector3(1, 2, 3)) == GestureTether.State.ATTACHED and tether.anchor == Vector3(1, 2, 3)
	if validation_frames == 5:
		gesture_sequence_passed = gesture_sequence_passed and tether.update(0.02, 0.1, false, Vector3.ZERO) == GestureTether.State.IDLE
	if "--validate-morph-toggle" in OS.get_cmdline_user_args() and validation_frames == 30:
		var before_mesh: ArrayMesh = $Character.bake_mesh_from_current_deformation()
		var before: Vector3 = before_mesh.surface_get_arrays(0)[Mesh.ARRAY_VERTEX][0]
		var before_weight: float = $Character.get_blend_shape_value(0)
		$Character.set_blend_shape_value(0, 0.0 if before_weight >= 0.5 else 1.0)
		var after_mesh: ArrayMesh = $Character.bake_mesh_from_current_deformation()
		var after: Vector3 = after_mesh.surface_get_arrays(0)[Mesh.ARRAY_VERTEX][0]
		print(JSON.stringify({"before_weight": before_weight, "before_vertex": before, "after_weight": $Character.get_blend_shape_value(0), "after_vertex": after, "changed": before != after}))
		get_tree().quit(0 if before != after else 1)
	if "--validate-m2-pose-gesture" in OS.get_cmdline_user_args() and validation_frames == 720:
		var active_count := 0
		for value in activated:
			active_count += value
		var finite_weights := true
		for value in previous_weights:
			finite_weights = finite_weights and is_finite(value) and value >= 0.0 and value <= 1.0
		var baked: ArrayMesh = $Character.bake_mesh_from_current_deformation()
		var deformation_changed: bool = baked != null and baked.surface_get_arrays(0)[Mesh.ARRAY_VERTEX][0].x != -0.55
		var passed: bool = active_count == MORPH_COUNT and finite_weights and maximum_weight_step < 0.1 and gesture_sequence_passed and deformation_changed and arm_finite and maximum_arm_step < 0.1 and maximum_wrist_error < 0.03 and maximum_finger_step < 0.02
		print(JSON.stringify({"schema": 1, "scene": "pose_gesture_validation", "morphs_activated": active_count, "maximum_weight_step": maximum_weight_step, "gesture_tether": gesture_sequence_passed, "combined_deformation_changed": deformation_changed, "arm_finite": arm_finite, "maximum_arm_step": maximum_arm_step, "maximum_wrist_error": maximum_wrist_error, "maximum_finger_step": maximum_finger_step, "passed": passed}))
		get_tree().quit(0 if passed else 1)
