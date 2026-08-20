"""Validate deterministic structure of a sanitized GLB runtime fixture."""

import argparse
import hashlib
import json
import pathlib
import struct


def sha256(path: pathlib.Path) -> str:
	return hashlib.sha256(path.read_bytes()).hexdigest()


def read_glb_json(path: pathlib.Path) -> dict:
	data = path.read_bytes()
	if len(data) < 20:
		raise ValueError("truncated GLB")
	magic, version, total_length = struct.unpack_from("<4sII", data, 0)
	if magic != b"glTF" or version != 2 or total_length != len(data):
		raise ValueError("invalid GLB header")
	json_length, json_type = struct.unpack_from("<II", data, 12)
	if json_type != 0x4E4F534A or 20 + json_length > len(data):
		raise ValueError("invalid GLB JSON chunk")
	return json.loads(data[20 : 20 + json_length].decode("utf-8").rstrip(" \t\r\n\0"))


def main() -> None:
	parser = argparse.ArgumentParser()
	parser.add_argument("--first", required=True)
	parser.add_argument("--second", required=True)
	parser.add_argument("--manifest", required=True)
	parser.add_argument("--output", required=True)
	args = parser.parse_args()
	first_path = pathlib.Path(args.first)
	second_path = pathlib.Path(args.second)
	manifest = json.loads(pathlib.Path(args.manifest).read_text(encoding="utf-8"))
	document = read_glb_json(first_path)
	first_hash = sha256(first_path)
	second_hash = sha256(second_path)
	primitives = [primitive for mesh in document.get("meshes", []) for primitive in mesh.get("primitives", [])]
	has_eight_weight_streams = bool(primitives) and all(
		"JOINTS_0" in primitive.get("attributes", {})
		and "WEIGHTS_0" in primitive.get("attributes", {})
		and "JOINTS_1" in primitive.get("attributes", {})
		and "WEIGHTS_1" in primitive.get("attributes", {})
		for primitive in primitives
	)
	target_counts = [len(primitive.get("targets", [])) for primitive in primitives]
	passed = (
		first_hash == second_hash
		and has_eight_weight_streams
		and bool(target_counts)
		and min(target_counts) >= 3
		and len(document.get("skins", [])) == 1
		and manifest["influences"]["output_maximum_influences"] <= 8
		and manifest["autoexec_fail"]
	)
	report = {
		"experiment": "M0-sanitized-runtime-mesh",
		"glb_sha256": first_hash,
		"repeat_sha256": second_hash,
		"byte_deterministic": first_hash == second_hash,
		"glb_bytes": first_path.stat().st_size,
		"mesh_primitives": len(primitives),
		"skin_count": len(document.get("skins", [])),
		"has_eight_weight_streams": has_eight_weight_streams,
		"morph_targets_per_primitive": target_counts,
		"source_maximum_influences": manifest["influences"]["input_maximum_influences"],
		"runtime_maximum_influences": manifest["influences"]["output_maximum_influences"],
		"vertices_pruned": manifest["influences"]["vertices_pruned"],
		"mean_discarded_weight": manifest["influences"]["mean_discarded_weight"],
		"maximum_discarded_weight": manifest["influences"]["maximum_discarded_weight"],
		"shape_key_count": len(manifest["shape_keys"]),
		"embedded_scripts_disabled": manifest["autoexec_fail"],
		"materials_and_textures": manifest["material_policy"],
		"passed": passed,
	}
	pathlib.Path(args.output).write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
	if not passed:
		raise SystemExit(2)


if __name__ == "__main__":
	main()
