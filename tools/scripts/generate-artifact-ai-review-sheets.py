#!/usr/bin/env python3
"""Generate high-resolution contact sheets for AI review of the v22 corpus."""

from __future__ import annotations

import argparse
import json
import textwrap
from collections import defaultdict
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont, ImageOps


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path("tests/artifact-blindset/corpus-v22.json"),
    )
    parser.add_argument(
        "--cache",
        type=Path,
        default=Path("build/artifact-corpus/v22-special-1280"),
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("build/artifact-corpus/v22-ai-review-sheets"),
    )
    parser.add_argument("--split", choices=("evaluation", "development", "all"), default="evaluation")
    parser.add_argument("--per-sheet", type=int, default=10)
    return parser.parse_args()


def fitted_preview(path: Path, width: int, height: int) -> Image.Image:
    with Image.open(path) as source:
        image = ImageOps.exif_transpose(source).convert("RGB")
        image.thumbnail((width, height), Image.Resampling.LANCZOS)
    canvas = Image.new("RGB", (width, height), (18, 18, 20))
    x = (width - image.width) // 2
    y = (height - image.height) // 2
    canvas.paste(image, (x, y))
    return canvas


def main() -> int:
    arguments = parse_arguments()
    if arguments.per_sheet < 1 or arguments.per_sheet > 12:
        raise SystemExit("--per-sheet must be between 1 and 12")
    document = json.loads(arguments.manifest.read_text(encoding="utf-8"))
    cases = [
        case for case in document["cases"]
        if arguments.split == "all" or case["split"] == arguments.split
    ]
    groups: dict[str, list[dict]] = defaultdict(list)
    for case in cases:
        groups[case["categoryLabel"]].append(case)

    arguments.output_dir.mkdir(parents=True, exist_ok=True)
    font = ImageFont.load_default(size=18)
    small_font = ImageFont.load_default(size=15)
    tile_width = 640
    image_height = 390
    header_height = 100
    tile_height = image_height + header_height
    columns = 2
    rows = (arguments.per_sheet + columns - 1) // columns
    index: list[dict] = []

    for label in sorted(groups):
        label_cases = groups[label]
        for batch_start in range(0, len(label_cases), arguments.per_sheet):
            batch = label_cases[batch_start : batch_start + arguments.per_sheet]
            sheet_number = batch_start // arguments.per_sheet + 1
            sheet_name = f"{label}-{sheet_number:02d}.jpg"
            sheet = Image.new("RGB", (tile_width * columns, tile_height * rows), (8, 8, 10))
            draw = ImageDraw.Draw(sheet)
            for slot, case in enumerate(batch, start=1):
                column = (slot - 1) % columns
                row = (slot - 1) // columns
                x = column * tile_width
                y = row * tile_height
                preview = fitted_preview(arguments.cache / case["filename"], tile_width, image_height)
                sheet.paste(preview, (x, y + header_height))
                title = case.get("commonsTitle", case["id"]).removeprefix("File:")
                lines = textwrap.wrap(title, width=64)[:2]
                draw.text((x + 12, y + 8), f"#{slot}  {case['id']}", fill=(255, 225, 70), font=font)
                draw.text((x + 12, y + 36), case["sourceCategory"], fill=(110, 205, 255), font=small_font)
                for line_index, line in enumerate(lines):
                    draw.text(
                        (x + 12, y + 58 + line_index * 18),
                        line,
                        fill=(235, 235, 235),
                        font=small_font,
                    )
                draw.rectangle((x, y, x + tile_width - 1, y + tile_height - 1), outline=(70, 70, 75), width=2)
                index.append({
                    "sheet": sheet_name,
                    "slot": slot,
                    "id": case["id"],
                    "filename": case["filename"],
                    "categoryLabel": case["categoryLabel"],
                    "sourceCategory": case["sourceCategory"],
                    "commonsTitle": case.get("commonsTitle", ""),
                    "sourcePage": case["sourcePage"],
                })
            sheet.save(arguments.output_dir / sheet_name, quality=92, subsampling=0)
            print(f"wrote {sheet_name}: {len(batch)} images")

    (arguments.output_dir / "index.json").write_text(
        json.dumps({"schemaVersion": 1, "split": arguments.split, "items": index}, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"wrote {len(index)} review items across {len({item['sheet'] for item in index})} sheets")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
