class_name PoseSpaceMorphDriver
extends RefCounted

var definitions: Array[Dictionary] = []
var weights: PackedFloat32Array

func configure(new_definitions: Array[Dictionary]) -> void:
	definitions = new_definitions.duplicate(true)
	weights.resize(definitions.size())
	weights.fill(0.0)

func evaluate(features: Dictionary, delta: float) -> PackedFloat32Array:
	for index in definitions.size():
		var definition := definitions[index]
		var input_value := float(features.get(definition.feature, 0.0))
		var low := float(definition.minimum)
		var high := float(definition.maximum)
		var normalized := clampf((input_value - low) / maxf(high - low, 0.000001), 0.0, 1.0)
		var target := normalized * normalized * (3.0 - 2.0 * normalized)
		var response_hz := maxf(float(definition.response_hz), 0.0)
		var blend := 1.0 - exp(-response_hz * maxf(delta, 0.0))
		weights[index] = lerpf(weights[index], target, blend)
	return weights
