class_name TrackedGestureTether
extends RefCounted

enum State { IDLE, CANDIDATE, ATTACHED }

var state := State.IDLE
var stable_frames := 0
var required_stable_frames := 3
var maximum_pinch_distance := 0.035
var minimum_confidence := 0.7
var anchor := Vector3.ZERO

func update(pinch_distance: float, confidence: float, release_requested: bool, candidate_anchor: Vector3) -> State:
	if release_requested or confidence < minimum_confidence:
		state = State.IDLE
		stable_frames = 0
		return state
	var pinching := pinch_distance <= maximum_pinch_distance
	match state:
		State.IDLE:
			if pinching:
				state = State.CANDIDATE
				stable_frames = 1
		State.CANDIDATE:
			if pinching:
				stable_frames += 1
				if stable_frames >= required_stable_frames:
					state = State.ATTACHED
					anchor = candidate_anchor
			else:
				state = State.IDLE
				stable_frames = 0
		State.ATTACHED:
			pass
	return state
