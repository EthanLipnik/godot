"""Read-only Blender scene inventory for choosing a sanitized runtime mesh.

Run Blender with --disable-autoexec. This script never evaluates or invokes
embedded text blocks, operators supplied by the scene, or external scripts.
"""

import argparse
import hashlib
import json
import pathlib
import sys

import bpy


def parse_arguments() -> argparse.Namespace:
	arguments = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
	parser = argparse.ArgumentParser()
	parser.add_argument("--source", required=True)
	parser.add_argument("--output", required=True)
	return parser.parse_args(arguments)


def sha256(path: pathlib.Path) -> str:
	digest = hashlib.sha256()
	with path.open("rb") as source:
		for chunk in iter(lambda: source.read(1024 * 1024), b""):
			digest.update(chunk)
	return digest.hexdigest()


def mesh_record(obj: bpy.types.Object) -> dict:
	mesh = obj.data
	maximum_influences = 0
	vertices_over_four = 0
	vertices_over_eight = 0
	for vertex in mesh.vertices:
		influences = sum(1 for group in vertex.groups if group.weight > 0.0)
		maximum_influences = max(maximum_influences, influences)
		vertices_over_four += influences > 4
		vertices_over_eight += influences > 8
	shape_keys = []
	if mesh.shape_keys:
		shape_keys = [key.name for key in mesh.shape_keys.key_blocks]
	return {
		"name": obj.name,
		"data_name": mesh.name,
		"visible_viewport": obj.visible_get(),
		"vertices": len(mesh.vertices),
		"polygons": len(mesh.polygons),
		"materials": [slot.material.name if slot.material else None for slot in obj.material_slots],
		"vertex_groups": len(obj.vertex_groups),
		"maximum_influences": maximum_influences,
		"vertices_over_four_influences": vertices_over_four,
		"vertices_over_eight_influences": vertices_over_eight,
		"shape_keys": shape_keys,
		"modifiers": [{"name": modifier.name, "type": modifier.type} for modifier in obj.modifiers],
	}


def main() -> None:
	args = parse_arguments()
	source_path = pathlib.Path(args.source).resolve()
	output_path = pathlib.Path(args.output).resolve()
	meshes = sorted(
		(mesh_record(obj) for obj in bpy.data.objects if obj.type == "MESH"),
		key=lambda record: record["name"],
	)
	report = {
		"schema": 1,
		"source_sha256": sha256(source_path),
		"blender_version": bpy.app.version_string,
		"autoexec_fail": bpy.app.autoexec_fail,
		"object_count": len(bpy.data.objects),
		"mesh_count": len(meshes),
		"armatures": sorted(obj.name for obj in bpy.data.objects if obj.type == "ARMATURE"),
		"meshes": meshes,
	}
	output_path.parent.mkdir(parents=True, exist_ok=True)
	output_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")


if __name__ == "__main__":
	main()
