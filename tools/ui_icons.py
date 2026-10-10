"""Build and validate the ImageGen UI icon exports. Requires Pillow."""
from pathlib import Path
import hashlib
import json
import math
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageOps


ROOT = Path(__file__).resolve().parents[1]
PACK = ROOT / "textures" / "ui_icons"
SIZES = (64, 128, 256)
GUTTER = 4


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build():
    plan = json.loads((PACK / "icon_plan.json").read_text(encoding="utf-8"))
    records = json.loads((PACK / "generation_records.json").read_text(encoding="utf-8"))
    records = {entry["index"]: entry for entry in records["assets"]}
    assets = plan["assets"]
    missing = [a["id"] for a in assets if not (PACK / a["group"] / (a["id"] + ".png")).is_file()]
    if missing:
        raise RuntimeError("Missing source icons: " + ", ".join(missing))
    columns = math.ceil(math.sqrt(len(assets)))
    rows = math.ceil(len(assets) / columns)
    atlas_dir = PACK / "atlas"
    atlas_dir.mkdir(exist_ok=True)
    atlases = {size: Image.new("RGBA", (columns * (size + 2 * GUTTER), rows * (size + 2 * GUTTER))) for size in SIZES}
    manifest = {"generator": "Built-in image_gen.imagegen", "alpha": "straight", "foreground": "white", "sizes": list(SIZES), "gutter_px": GUTTER, "assets": [], "atlases": {}}
    preview = Image.new("RGB", (columns * 168, rows * 164 + 52), (18, 22, 30))
    draw = ImageDraw.Draw(preview)
    draw.text((18, 14), "NOX / 105 UI ICONS / WHITE, CYAN AND LAVENDER RUNTIME TINT", fill="white")
    for position, item in enumerate(assets):
        group, icon_id = item["group"], item["id"]
        source = PACK / group / (icon_id + ".png")
        record = records[item["index"]]
        if "source" in record:
            assert digest(source) == digest(Path(record["source"])), source
        image = Image.open(source).convert("RGBA")
        # Remove dark fringe remnants and tiny generated specks, retaining the generated silhouette.
        mask = ImageChops.multiply(image.getchannel("A"), ImageOps.grayscale(image.convert("RGB")))
        mask = mask.filter(ImageFilter.MedianFilter(5)).filter(ImageFilter.GaussianBlur(0.7))
        mask = mask.point(lambda value: 255 if value >= 128 else 0)
        bounds = mask.getbbox()
        if bounds is None:
            raise RuntimeError("Empty generated icon: " + icon_id)
        mask = mask.crop(bounds)
        entry = {"id": group + "/" + icon_id, "group": group, "usage": item["usage"], "source": source.relative_to(ROOT).as_posix(), "source_dimensions": list(image.size), "source_sha256": digest(source), "prompt": record["prompt"], "exports": {}, "atlas_rects": {}}
        for size in SIZES:
            footprint = int(size * 0.75)
            scale = footprint / max(mask.size)
            resized = mask.resize((max(1, round(mask.width * scale)), max(1, round(mask.height * scale))), Image.Resampling.LANCZOS)
            alpha = Image.new("L", (size, size))
            alpha.paste(resized, ((size - resized.width) // 2, (size - resized.height) // 2))
            icon = Image.new("RGBA", (size, size), (255, 255, 255, 0))
            icon.putalpha(alpha)
            destination = PACK / str(size) / group / (icon_id + ".png")
            destination.parent.mkdir(parents=True, exist_ok=True)
            icon.save(destination, optimize=True)
            entry["exports"][str(size)] = {"file": destination.relative_to(ROOT).as_posix(), "sha256": digest(destination)}
            stride = size + 2 * GUTTER
            left = (position % columns) * stride + GUTTER
            top = (position // columns) * stride + GUTTER
            # Paste all RGBA channels directly; alpha compositing would square the edge alpha.
            atlases[size].paste(icon, (left, top))
            width, height = atlases[size].size
            entry["atlas_rects"][str(size)] = {"pixels": [left, top, left + size, top + size], "uv_top_left": [left / width, top / height, (left + size) / width, (top + size) / height]}
            if size == 128:
                x, y = (position % columns) * 168, (position // columns) * 164 + 52
                preview.paste(icon.resize((92, 92), Image.Resampling.LANCZOS), (x + 38, y), icon.resize((92, 92), Image.Resampling.LANCZOS))
                small = icon.resize((32, 32), Image.Resampling.LANCZOS)
                for offset, color in ((20, (255, 255, 255)), (68, (89, 217, 255)), (116, (204, 158, 255))):
                    tinted = Image.new("RGBA", small.size, color + (0,))
                    tinted.putalpha(small.getchannel("A"))
                    preview.paste(tinted, (x + offset, y + 94), tinted)
                draw.text((x + 6, y + 132), group + "/" + icon_id, fill=(204, 211, 223))
        manifest["assets"].append(entry)
    for size, atlas in atlases.items():
        destination = atlas_dir / ("ui_icons_" + str(size) + ".png")
        atlas.save(destination, optimize=True)
        manifest["atlases"][str(size)] = {"file": destination.relative_to(ROOT).as_posix(), "dimensions": list(atlas.size), "sha256": digest(destination)}
    preview.save(PACK / "contact_sheet.png", optimize=True)
    write_json(PACK / "manifest.json", manifest)
    check()


def check():
    manifest = json.loads((PACK / "manifest.json").read_text(encoding="utf-8"))
    assert len(manifest["assets"]) == 105
    assert len({a["id"] for a in manifest["assets"]}) == 105
    atlases = {}
    for size, entry in manifest["atlases"].items():
        path = ROOT / entry["file"]
        assert digest(path) == entry["sha256"], path
        atlas = Image.open(path)
        assert atlas.mode == "RGBA" and list(atlas.size) == entry["dimensions"], path
        atlases[size] = atlas
    for entry in manifest["assets"]:
        assert digest(ROOT / entry["source"]) == entry["source_sha256"], entry["id"]
        for size, exported in entry["exports"].items():
            path = ROOT / exported["file"]
            assert digest(path) == exported["sha256"], path
            icon = Image.open(path)
            n = int(size)
            assert icon.mode == "RGBA" and icon.size == (n, n), path
            alpha = icon.getchannel("A")
            assert alpha.getextrema() == (0, 255), path
            box = alpha.point(lambda value: 255 if value >= 128 else 0).getbbox()
            assert box and min(box[0], box[1], n - box[2], n - box[3]) >= n * 0.10, path
            assert all(channel.getextrema() == (255, 255) for channel in icon.split()[:3]), path
            assert atlases[size].crop(entry["atlas_rects"][size]["pixels"]).tobytes() == icon.tobytes(), path
    for path in (PACK / "manifest.json", PACK / "icon_plan.json", PACK / "generation_records.json", PACK / "README.md", ROOT / "tools" / ".gitignore", Path(__file__)):
        raw = path.read_bytes()
        assert not raw.startswith(b"\xef\xbb\xbf"), path
        assert raw.decode("utf-8").isascii(), path
    print("Validated 105 preserved originals, 315 white RGBA exports and 3 exact atlases.")


if __name__ == "__main__":
    if sys.argv[1:] == ["--check"]:
        check()
    elif not sys.argv[1:]:
        build()
    else:
        raise SystemExit("Usage: python tools/ui_icons.py [--check]")
