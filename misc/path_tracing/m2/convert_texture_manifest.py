"""Create a deterministic, array-ready texture set and invert DirectX normal green."""

import argparse
import hashlib
import json
import pathlib
import shutil

from PIL import Image, __version__ as pillow_version


def sha256(path: pathlib.Path) -> str:
	return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
	parser = argparse.ArgumentParser()
	parser.add_argument("--source-evidence", required=True)
	parser.add_argument("--output-directory", required=True)
	parser.add_argument("--manifest", required=True)
	args = parser.parse_args()
	evidence = json.loads(pathlib.Path(args.source_evidence).read_text(encoding="utf-8"))
	output_directory = pathlib.Path(args.output_directory)
	output_directory.mkdir(parents=True, exist_ok=True)
	images_by_path = {}
	for material in evidence["materials"]:
		for image in material["images"]:
			for tile in image["tiles"]:
				record = images_by_path.setdefault(tile["path"], {"source_color_spaces": set(), "normal": False})
				record["source_color_spaces"].add(image["source_color_space"])
				record["normal"] |= "normal" in image["name"].lower() or "normal" in pathlib.Path(tile["path"]).name.lower()
	converted = []
	for source_name in sorted(images_by_path):
		source = pathlib.Path(source_name)
		metadata = images_by_path[source_name]
		output = output_directory / source.name
		operation = "copy_bytes"
		if metadata["normal"]:
			with Image.open(source) as image:
				converted_image = image.convert("RGBA")
				red, green, blue, alpha = converted_image.split()
				green = green.point(lambda value: 255 - value)
				Image.merge("RGBA", (red, green, blue, alpha)).save(output, format="PNG", compress_level=9, optimize=False)
			operation = "invert_green_channel"
		else:
			shutil.copyfile(source, output)
		converted.append({
			"source": str(source),
			"source_sha256": sha256(source),
			"output": output.name,
			"output_sha256": sha256(output),
			"source_color_spaces": sorted(metadata["source_color_spaces"]),
			"runtime_color_space": "linear" if "Non-Color" in metadata["source_color_spaces"] else "srgb",
			"operation": operation,
		})
	report = {
		"schema": 1,
		"pillow_version": pillow_version,
		"layout": "array_ready_udim_tiles",
		"textures": converted,
		"passed": bool(converted) and all(pathlib.Path(args.output_directory, item["output"]).is_file() for item in converted),
	}
	pathlib.Path(args.manifest).write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
	if not report["passed"]:
		raise SystemExit(2)


if __name__ == "__main__":
	main()
