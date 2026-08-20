"""Validate the deterministic M2 character export and source evidence."""

import argparse
import hashlib
import json
import pathlib
import struct


def glb_document(path: pathlib.Path) -> dict:
	data = path.read_bytes()
	magic, version, total = struct.unpack_from("<4sII", data, 0)
	if magic != b"glTF" or version != 2 or total != len(data):
		raise ValueError("invalid GLB")
	length, kind = struct.unpack_from("<II", data, 12)
	if kind != 0x4E4F534A:
		raise ValueError("GLB has no JSON chunk")
	return json.loads(data[20 : 20 + length].decode("utf-8").rstrip(" \t\r\n\0"))


def main() -> None:
	parser = argparse.ArgumentParser()
	parser.add_argument("--first", required=True)
	parser.add_argument("--second", required=True)
	parser.add_argument("--export-manifest", required=True)
	parser.add_argument("--source-evidence", required=True)
	parser.add_argument("--output", required=True)
	args = parser.parse_args()
	first = pathlib.Path(args.first)
	second = pathlib.Path(args.second)
	manifest = json.loads(pathlib.Path(args.export_manifest).read_text(encoding="utf-8"))
	evidence = json.loads(pathlib.Path(args.source_evidence).read_text(encoding="utf-8"))
	document = glb_document(first)
	first_hash = hashlib.sha256(first.read_bytes()).hexdigest()
	second_hash = hashlib.sha256(second.read_bytes()).hexdigest()
	primitives = [primitive for mesh in document.get("meshes", []) for primitive in mesh.get("primitives", [])]
	target_counts = [len(primitive.get("targets", [])) for primitive in primitives]
	eight_weights = bool(primitives) and all(all(name in primitive.get("attributes", {}) for name in ("JOINTS_0", "WEIGHTS_0", "JOINTS_1", "WEIGHTS_1")) for primitive in primitives)
	retarget_values = list(evidence["retarget"].values())
	bone_names = {bone["name"] for bone in evidence["deform_bones"]}
	passed = all((
		first_hash == second_hash,
		evidence["embedded_scripts_disabled"],
		len(evidence["shape_keys"]) == 35,
		len(manifest["shape_keys"]) == 36,
		bool(target_counts) and min(target_counts) == 35,
		eight_weights,
		manifest["influences"]["output_maximum_influences"] <= 8,
		len(retarget_values) == len(set(retarget_values)),
		set(retarget_values) <= bone_names,
		evidence["normal_conversion"]["operation"] == "invert_green_channel",
		set(evidence["variants"]) == {"first_person", "third_person"},
		not any(not image["exists"] for material in evidence["materials"] for image in material["images"]),
	))
	report = {
		"milestone": "M2-character-foundation",
		"glb_sha256": first_hash,
		"byte_deterministic": first_hash == second_hash,
		"glb_bytes": first.stat().st_size,
		"vertices": manifest["vertices"],
		"deform_bones": len(evidence["deform_bones"]),
		"runtime_maximum_influences": manifest["influences"]["output_maximum_influences"],
		"morph_targets": min(target_counts) if target_counts else 0,
		"source_driver_count": len(evidence["shape_key_drivers"]),
		"missing_texture_count": sum(not image["exists"] for material in evidence["materials"] for image in material["images"]),
		"retarget_entries": len(evidence["retarget"]),
		"normal_conversion": evidence["normal_conversion"],
		"variants": evidence["variants"],
		"passed": passed,
	}
	pathlib.Path(args.output).write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
	print(json.dumps(report, sort_keys=True))
	if not passed:
		raise SystemExit(2)


if __name__ == "__main__":
	main()
