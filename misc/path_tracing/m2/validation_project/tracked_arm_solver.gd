class_name TrackedArmSolver
extends RefCounted

var filtered_wrist := Vector3.ZERO
var initialized := false
var response_hz := 20.0
var confidence_threshold := 0.55
var finger_pose: Array[Vector3] = []

func solve(shoulder: Vector3, observed_wrist: Vector3, pole: Vector3, upper_length: float, lower_length: float, confidence: float, delta: float) -> Dictionary:
	var target := observed_wrist if confidence >= confidence_threshold else shoulder + Vector3(0.25, -0.45, 0.05)
	if not initialized:
		filtered_wrist = target
		initialized = true
	var blend := 1.0 - exp(-response_hz * maxf(delta, 0.0))
	filtered_wrist = filtered_wrist.lerp(target, blend)
	var shoulder_to_wrist := filtered_wrist - shoulder
	var distance := clampf(shoulder_to_wrist.length(), absf(upper_length - lower_length) + 0.0001, upper_length + lower_length - 0.0001)
	var direction := shoulder_to_wrist.normalized()
	var pole_direction := (pole - shoulder).slide(direction).normalized()
	if pole_direction.length_squared() < 0.5:
		pole_direction = Vector3.UP.slide(direction).normalized()
	var along := (upper_length * upper_length - lower_length * lower_length + distance * distance) / (2.0 * distance)
	var perpendicular := sqrt(maxf(upper_length * upper_length - along * along, 0.0))
	var elbow := shoulder + direction * along + pole_direction * perpendicular
	var wrist := shoulder + direction * distance
	return {"shoulder": shoulder, "elbow": elbow, "wrist": wrist, "target": filtered_wrist}

func filter_fingers(observed: Array[Vector3], confidence: float, delta: float) -> Array[Vector3]:
	if finger_pose.size() != observed.size():
		finger_pose = observed.duplicate()
	var blend := 1.0 - exp(-response_hz * maxf(delta, 0.0))
	for index in observed.size():
		var target := observed[index] if confidence >= confidence_threshold else Vector3.ZERO
		finger_pose[index] = finger_pose[index].lerp(target, blend)
	return finger_pose
