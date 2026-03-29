#!/usr/bin/env python3

import argparse
import math
import multiprocessing
import random
from pathlib import Path
from typing import Dict, List, Sequence, Tuple

import numpy as np
import torch
from PIL import Image, ImageDraw, ImageFont

import kornia_randaugment_inplace as aug


Record = Dict[str, object]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Render one sample per Kornia augmentation op against a red background with a green circle."
    )
    parser.add_argument(
        "--output-dir",
        default="build/augment_probe",
        help="Directory to write per-op outputs and the contact sheet. Default: build/augment_probe",
    )
    parser.add_argument("--size", type=int, default=432, help="Synthetic probe image size. Default: 432")
    parser.add_argument("--magnitude", type=int, default=15, help="RandAugment magnitude. Default: 15")
    parser.add_argument("--device", default="cpu", help="Execution device, e.g. cpu or cuda:0. Default: cpu")
    parser.add_argument("--seed", type=int, default=1337, help="Random seed. Default: 1337")
    parser.add_argument("--columns", type=int, default=4, help="Contact-sheet column count. Default: 4")
    parser.add_argument("--timeout-seconds", type=float, default=20.0, help="Per-op timeout. Default: 20")
    return parser.parse_args()


def make_probe_scene(size: int) -> Tuple[np.ndarray, List[Record]]:
    image = np.zeros((size, size, 3), dtype=np.uint8)
    image[:, :, 0] = 180

    center_xy = size // 2
    radius = max(24, size // 5)
    yy, xx = np.ogrid[:size, :size]
    mask = ((xx - center_xy) ** 2 + (yy - center_xy) ** 2) <= radius ** 2
    image[mask] = np.array([0, 220, 0], dtype=np.uint8)

    bbox = [
        center_xy - radius,
        center_xy - radius,
        center_xy + radius,
        center_xy + radius,
    ]
    record: Record = {
        "class": "probe_circle",
        "bbox_xyxy": bbox,
        "mask_rle_encoding": "row_major_start_length",
        "mask_rle": aug.encode_rle_row_major_start_length(mask.astype(np.uint8)),
        "image_size_wh": [size, size],
        "category_id": 1,
    }
    return image, [record]


def render_overlay(image_array: np.ndarray, records: Sequence[Record], size: int) -> Image.Image:
    base = Image.fromarray(image_array).convert("RGBA")
    overlay = np.zeros((size, size, 4), dtype=np.uint8)
    draw = ImageDraw.Draw(base)

    for record in records:
        mask = aug.decode_rle_row_major_start_length(str(record["mask_rle"]), size).astype(bool)
        overlay[mask] = np.array([0, 255, 255, 72], dtype=np.uint8)
        bbox = record.get("bbox_xyxy")
        if isinstance(bbox, list) and len(bbox) == 4:
            draw.rectangle([int(v) for v in bbox], outline=(255, 255, 0, 255), width=3)

    composed = Image.alpha_composite(base, Image.fromarray(overlay))
    return composed.convert("RGB")


def add_caption(image: Image.Image, caption: str) -> Image.Image:
    font = ImageFont.load_default()
    text_padding = 8
    text_height = 22
    canvas = Image.new("RGB", (image.width, image.height + text_height), (18, 18, 18))
    canvas.paste(image, (0, 0))
    draw = ImageDraw.Draw(canvas)
    draw.text((text_padding, image.height + 4), caption, fill=(240, 240, 240), font=font)
    return canvas


def save_contact_sheet(images: Sequence[Image.Image], columns: int, output_path: Path) -> None:
    if not images:
        raise RuntimeError("no images available for contact sheet")
    columns = max(1, columns)
    tile_width = max(image.width for image in images)
    tile_height = max(image.height for image in images)
    rows = math.ceil(len(images) / columns)
    sheet = Image.new("RGB", (columns * tile_width, rows * tile_height), (12, 12, 12))

    for index, image in enumerate(images):
        row = index // columns
        col = index % columns
        sheet.paste(image, (col * tile_width, row * tile_height))

    output_path.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(output_path)


def run_probe_op(
    spec: aug.OpSpec,
    image_array: np.ndarray,
    records: Sequence[Record],
    size: int,
    magnitude: int,
    device: torch.device,
) -> Tuple[np.ndarray, List[Record], torch.device]:
    prepared = aug.build_augmenter(spec, magnitude, device)
    try:
        if spec.transforms_labels:
            output_image, output_records = aug.augment_image_and_labels(
                image_array=image_array,
                records=records,
                augmenter=prepared.module,
                required_size=size,
                device=prepared.device,
            )
            return output_image, output_records, prepared.device

        output_batch = aug.augment_image_only_batch(
            arrays=[image_array],
            augmenter=prepared.module,
            device=prepared.device,
        )
        return output_batch[0], list(records), prepared.device
    except Exception:
        if prepared.device.type != "cuda":
            raise
        rebuilt = aug.build_augmenter(spec, magnitude, torch.device("cpu"))
        if spec.transforms_labels:
            output_image, output_records = aug.augment_image_and_labels(
                image_array=image_array,
                records=records,
                augmenter=rebuilt.module,
                required_size=size,
                device=rebuilt.device,
            )
            return output_image, output_records, rebuilt.device

        output_batch = aug.augment_image_only_batch(
            arrays=[image_array],
            augmenter=rebuilt.module,
            device=rebuilt.device,
        )
        return output_batch[0], list(records), rebuilt.device


def make_status_tile(size: int, caption: str, detail: str) -> Image.Image:
    image = Image.new("RGB", (size, size), (56, 10, 10))
    draw = ImageDraw.Draw(image)
    font = ImageFont.load_default()
    draw.text((12, 12), detail[:120], fill=(255, 220, 220), font=font)
    return add_caption(image, caption)


def render_single_op(
    spec_index: int,
    size: int,
    magnitude: int,
    device_text: str,
    seed: int,
    tile_path: str,
) -> None:
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    device = aug.resolve_device(device_text)
    randaugment_policy = aug.resolve_randaugment_policy()
    op_specs = aug.build_op_specs(randaugment_policy, size)
    spec = op_specs[spec_index]
    base_image, base_records = make_probe_scene(size)
    probe_image, probe_records, used_device = run_probe_op(
        spec=spec,
        image_array=base_image,
        records=base_records,
        size=size,
        magnitude=magnitude,
        device=device,
    )
    tile = add_caption(render_overlay(probe_image, probe_records, size), f"{spec_index + 1:03d} {spec.name} [{used_device}]")
    Path(tile_path).parent.mkdir(parents=True, exist_ok=True)
    tile.save(tile_path)


def main() -> int:
    args = parse_args()
    if args.columns <= 0:
        raise RuntimeError(f"--columns must be positive. Got {args.columns}")
    if args.size <= 0:
        raise RuntimeError(f"--size must be positive. Got {args.size}")
    if args.magnitude <= 0 or args.magnitude >= 30:
        raise RuntimeError(f"--magnitude must be in [1, 29]. Got {args.magnitude}")
    if args.timeout_seconds <= 0:
        raise RuntimeError(f"--timeout-seconds must be positive. Got {args.timeout_seconds}")

    random.seed(args.seed)
    np.random.seed(args.seed)
    torch.manual_seed(args.seed)
    device = aug.resolve_device(args.device)

    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    base_image, base_records = make_probe_scene(args.size)
    base_overlay = add_caption(render_overlay(base_image, base_records, args.size), "base")
    base_overlay.save(output_dir / "000_base.png")

    randaugment_policy = aug.resolve_randaugment_policy()
    op_specs = aug.build_op_specs(randaugment_policy, args.size)
    rendered_tiles: List[Image.Image] = [base_overlay]
    ctx = multiprocessing.get_context("spawn")

    for index, spec in enumerate(op_specs, start=1):
        tile_path = output_dir / f"{index:03d}_{spec.name}.png"
        print(f"rendering {index}/{len(op_specs)} {spec.name}")
        process = ctx.Process(
            target=render_single_op,
            args=(index - 1, args.size, args.magnitude, str(device), args.seed + index, str(tile_path)),
        )
        process.start()
        process.join(args.timeout_seconds)
        if process.is_alive():
            process.terminate()
            process.join()
            tile = make_status_tile(args.size, f"{index:03d} {spec.name} [timeout]", f"timed out after {args.timeout_seconds:.1f}s")
            tile.save(tile_path)
            rendered_tiles.append(tile)
            continue
        if process.exitcode != 0:
            detail = f"process failed with exit code {process.exitcode}"
            tile = make_status_tile(args.size, f"{index:03d} {spec.name} [failed]", detail)
            tile.save(tile_path)
            rendered_tiles.append(tile)
            continue
        rendered_tiles.append(Image.open(tile_path).convert("RGB"))

    save_contact_sheet(rendered_tiles, args.columns, output_dir / "contact_sheet.png")
    print(f"ops_rendered={len(op_specs)}")
    print(f"output_dir={output_dir}")
    print(f"contact_sheet={output_dir / 'contact_sheet.png'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
