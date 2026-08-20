"""Extract deterministic skeleton, morph-driver, and material evidence from an open Blender scene."""

import argparse
import hashlib
import json
import pathlib
import sys

import bpy


def arguments() -> argparse.Namespace:
	values = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
	parser = argparse.ArgumentParser()
	parser.add_argument("--source", required=True)
	parser.add_argument("--profile", required=True)
	parser.add_argument("--output", required=True)
	return parser.parse_args(values)


def sha256(path: pathlib.Path) -> str:
	digest = hashlib.sha256()
	with path.open("rb") as source:
		for chunk in iter(lambda: source.read(1024 * 1024), b""):
			digest.update(chunk)
	return digest.hexdigest()


def matrix_values(matrix) -> list[float]:
	return [round(float(matrix[row][column]), 9) for column in range(4) for row in range(4)]


def driver_record(curve) -> dict:
	driver = curve.driver
	variables = []
	for variable in sorted(driver.variables, key=lambda item: item.name):
		targets = []
		for target in variable.targets:
			targets.append({
				"id": target.id.name if target.id else None,
				"bone_target": target.bone_target,
				"data_path": target.data_path,
				"transform_type": target.transform_type,
				"transform_space": target.transform_space,
			})
		variables.append({"name": variable.name, "type": variable.type, "targets": targets})
	return {
		"data_path": curve.data_path,
		"array_index": curve.array_index,
		"type": driver.type,
		"expression": driver.expression,
		"variables": variables,
	}


def main() -> None:
	args = arguments()
	source_path = pathlib.Path(args.source).resolve()
	profile = json.loads(pathlib.Path(args.profile).read_text(encoding="utf-8"))
	mesh = bpy.data.objects.get(profile["source_object"])
	armature = bpy.data.objects.get(profile["armature"])
	if not mesh or mesh.type != "MESH" or not armature or armature.type != "ARMATURE":
		raise RuntimeError("profile mesh or armature was not found")
	deform_bones = [bone for bone in armature.data.bones if bone.use_deform]
	deform_names = {bone.name for bone in deform_bones}
	missing_retarget = sorted(set(profile["retarget"].values()) - deform_names)
	if missing_retarget:
		raise RuntimeError(f"retarget bones are absent: {missing_retarget}")
	bones = [{
		"name": bone.name,
		"parent": bone.parent.name if bone.parent and bone.parent.name in deform_names else None,
		"rest_local_column_major": matrix_values(bone.matrix_local),
	} for bone in sorted(deform_bones, key=lambda item: item.name)]
	shape_keys = []
	drivers = []
	if mesh.data.shape_keys:
		shape_keys = [key.name for key in mesh.data.shape_keys.key_blocks[1:]]
		if mesh.data.shape_keys.animation_data:
			drivers = [driver_record(curve) for curve in sorted(mesh.data.shape_keys.animation_data.drivers, key=lambda item: (item.data_path, item.array_index))]
	materials = []
	for material in sorted((item for item in mesh.data.materials if item), key=lambda item: item.name):
		images = []
		if material.node_tree:
			for node in material.node_tree.nodes:
				if node.type == "TEX_IMAGE" and node.image:
					resolved = pathlib.Path(bpy.path.abspath(node.image.filepath))
					if "<UDIM>" in resolved.name:
						tile_pattern = resolved.name.replace("<UDIM>", "[0-9][0-9][0-9][0-9]")
						tiles = sorted(resolved.parent.glob(tile_pattern))
					else:
						tiles = [resolved] if resolved.is_file() else []
					images.append({
						"name": node.image.name,
						"path": str(resolved),
						"exists": bool(tiles),
						"tiles": [{"path": str(tile), "sha256": sha256(tile)} for tile in tiles],
						"source_color_space": node.image.colorspace_settings.name,
					})
		materials.append({"name": material.name, "images": sorted(images, key=lambda item: (item["name"], item["path"]))})
	report = {
		"schema": 1,
		"source_sha256": sha256(source_path),
		"blender_version": bpy.app.version_string,
		"embedded_scripts_disabled": bpy.app.autoexec_fail,
		"source_object": mesh.name,
		"vertices": len(mesh.data.vertices),
		"polygons": len(mesh.data.polygons),
		"deform_bones": bones,
		"retarget": profile["retarget"],
		"shape_keys": shape_keys,
		"shape_key_drivers": drivers,
		"materials": materials,
		"normal_conversion": {
			"source": profile["normal_map_convention"],
			"runtime": profile["runtime_normal_map_convention"],
			"operation": "invert_green_channel",
		},
		"variants": profile["variants"],
		"license_notes": profile["license_notes"],
	}
	pathlib.Path(args.output).write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")


if __name__ == "__main__":
	main()
