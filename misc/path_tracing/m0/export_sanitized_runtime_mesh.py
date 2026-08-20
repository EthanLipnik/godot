"""Export one selected Blender mesh as a deterministic runtime GLB fixture."""

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
	parser.add_argument("--object-name", required=True)
	parser.add_argument("--output", required=True)
	parser.add_argument("--manifest", required=True)
	parser.add_argument("--max-influences", type=int, default=8)
	parser.add_argument("--morph-limit", type=int, default=8)
	return parser.parse_args(arguments)


def sha256(path: pathlib.Path) -> str:
	digest = hashlib.sha256()
	with path.open("rb") as source:
		for chunk in iter(lambda: source.read(1024 * 1024), b""):
			digest.update(chunk)
	return digest.hexdigest()


def duplicate_for_export(source_object: bpy.types.Object) -> bpy.types.Object:
	duplicate = source_object.copy()
	duplicate.data = source_object.data.copy()
	duplicate.name = "RuntimeMesh"
	duplicate.data.name = "RuntimeMeshData"
	duplicate.hide_viewport = False
	duplicate.hide_render = False
	bpy.context.scene.collection.objects.link(duplicate)
	duplicate.hide_set(False)
	duplicate.animation_data_clear()
	for modifier in list(duplicate.modifiers):
		if modifier.type != "ARMATURE":
			duplicate.modifiers.remove(modifier)
	return duplicate


def prune_influences(obj: bpy.types.Object, maximum: int, deform_group_names: set[str]) -> dict:
	if maximum < 1:
		raise ValueError("maximum influences must be positive")
	discarded_total = 0.0
	discarded_maximum = 0.0
	source_weight_sum_minimum = float("inf")
	source_weight_sum_maximum = 0.0
	input_maximum = 0
	vertices_pruned = 0
	for vertex in obj.data.vertices:
		memberships = [(membership.group, membership.weight) for membership in vertex.groups if membership.weight > 0.0]
		weighted = sorted(
			(
				(group_index, weight)
				for group_index, weight in memberships
				if obj.vertex_groups[group_index].name in deform_group_names
			),
			key=lambda item: (-item[1], item[0]),
		)
		for group_index, _ in memberships:
			if obj.vertex_groups[group_index].name not in deform_group_names:
				obj.vertex_groups[group_index].remove([vertex.index])
		input_maximum = max(input_maximum, len(weighted))
		kept = weighted[:maximum]
		source_total = sum(weight for _, weight in weighted)
		source_weight_sum_minimum = min(source_weight_sum_minimum, source_total)
		source_weight_sum_maximum = max(source_weight_sum_maximum, source_total)
		discarded = sum(weight for _, weight in weighted[maximum:])
		discarded_fraction = discarded / source_total if source_total > 0.0 else 0.0
		discarded_total += discarded_fraction
		discarded_maximum = max(discarded_maximum, discarded_fraction)
		vertices_pruned += len(weighted) > maximum
		for group_index, _ in weighted[maximum:]:
			obj.vertex_groups[group_index].remove([vertex.index])
		kept_total = sum(weight for _, weight in kept)
		if kept_total > 0.0:
			for group_index, weight in kept:
				obj.vertex_groups[group_index].add([vertex.index], weight / kept_total, "REPLACE")
	return {
		"input_maximum_influences": input_maximum,
		"output_maximum_influences": min(input_maximum, maximum),
		"vertices_pruned": vertices_pruned,
		"mean_discarded_weight": discarded_total / max(1, len(obj.data.vertices)),
		"maximum_discarded_weight": discarded_maximum,
		"source_weight_sum_minimum": source_weight_sum_minimum,
		"source_weight_sum_maximum": source_weight_sum_maximum,
	}


def prune_shape_keys(obj: bpy.types.Object, limit: int) -> list[str]:
	if not obj.data.shape_keys:
		return []
	obj.data.shape_keys.animation_data_clear()
	blocks = obj.data.shape_keys.key_blocks
	basis_name = blocks[0].name
	selected = {basis_name}
	selected.update(sorted(key.name for key in blocks[1:])[:limit])
	for key in list(blocks)[::-1]:
		if key.name not in selected:
			obj.shape_key_remove(key)
	basis = obj.data.shape_keys.key_blocks[0]
	for key in obj.data.shape_keys.key_blocks:
		key.mute = False
		if key != basis:
			key.relative_key = basis
	return [key.name for key in obj.data.shape_keys.key_blocks]


def main() -> None:
	args = parse_arguments()
	source_path = pathlib.Path(args.source).resolve()
	output_path = pathlib.Path(args.output).resolve()
	manifest_path = pathlib.Path(args.manifest).resolve()
	source_object = bpy.data.objects.get(args.object_name)
	if source_object is None or source_object.type != "MESH":
		raise RuntimeError(f"mesh object not found: {args.object_name}")
	runtime_object = duplicate_for_export(source_object)
	armatures = [
		modifier.object
		for modifier in runtime_object.modifiers
		if modifier.type == "ARMATURE" and modifier.object is not None
	]
	if len(armatures) != 1:
		raise RuntimeError(f"expected one armature modifier, found {len(armatures)}")
	deform_group_names = {bone.name for bone in armatures[0].data.bones if bone.use_deform}
	influence_report = prune_influences(runtime_object, args.max_influences, deform_group_names)
	shape_keys = prune_shape_keys(runtime_object, args.morph_limit)
	runtime_world_transform = runtime_object.matrix_world.copy()
	runtime_object.parent = armatures[0]
	runtime_object.parent_type = "OBJECT"
	runtime_object.matrix_world = runtime_world_transform

	bpy.ops.object.select_all(action="DESELECT")
	runtime_object.select_set(True)
	armatures[0].select_set(True)
	bpy.context.view_layer.objects.active = runtime_object
	output_path.parent.mkdir(parents=True, exist_ok=True)
	bpy.ops.export_scene.gltf(
		filepath=str(output_path),
		export_format="GLB",
		use_selection=True,
		export_materials="NONE",
		export_skins=True,
		export_influence_nb=args.max_influences,
		export_all_influences=True,
		export_morph=True,
		export_morph_normal=True,
		export_morph_tangent=False,
		export_morph_animation=False,
		export_animations=False,
		export_cameras=False,
		export_lights=False,
		export_extras=False,
		export_yup=True,
	)
	report = {
		"schema": 1,
		"source_sha256": sha256(source_path),
		"blender_version": bpy.app.version_string,
		"autoexec_fail": bpy.app.autoexec_fail,
		"source_object": args.object_name,
		"runtime_object": runtime_object.name,
		"runtime_visible": runtime_object.visible_get(),
		"runtime_parent": runtime_object.parent.name if runtime_object.parent else None,
		"vertices": len(runtime_object.data.vertices),
		"polygons": len(runtime_object.data.polygons),
		"armature": armatures[0].name,
		"deform_bone_count": len(deform_group_names),
		"shape_keys": shape_keys,
		"material_policy": "omitted_from_geometry_fixture",
		"influences": influence_report,
	}
	manifest_path.parent.mkdir(parents=True, exist_ok=True)
	manifest_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")


if __name__ == "__main__":
	main()
