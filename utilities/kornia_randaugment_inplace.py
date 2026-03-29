#!/usr/bin/env python3

import argparse
import concurrent.futures
import json
import multiprocessing
import os
import queue as queue_module
import random
import sys
import tempfile
import time
import warnings
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

import numpy as np
import torch
from PIL import Image
import kornia.augmentation as K
from kornia.augmentation.auto import RandAugment
from tqdm import tqdm


# Verified against Kornia RandAugment via Context7 and the installed Kornia package.
# These are the default one-op candidate policies used by Kornia RandAugment.
FALLBACK_POLICY: List[List[Tuple[str, float, Optional[float]]]] = [
    [("auto_contrast", 0.0, 1.0)],
    [("equalize", 0.0, 1.0)],
    [("invert", 0.0, 1.0)],
    [("rotate", -30.0, 30.0)],
    [("posterize", 0.0, 4.0)],
    [("solarize", 0.0, 1.0)],
    [("solarize_add", 0.0, 0.43)],
    [("color", 0.1, 1.9)],
    [("contrast", 0.1, 1.9)],
    [("brightness", 0.1, 1.9)],
    [("sharpness", 0.1, 1.9)],
    [("shear_x", -0.3, 0.3)],
    [("shear_y", -0.3, 0.3)],
    [("translate_x", -0.1, 0.1)],
    [("translate_y", -0.1, 0.1)],
]

DEFAULT_EXTENSIONS = {
    ".jpg",
    ".jpeg",
    ".png",
    ".bmp",
    ".tif",
    ".tiff",
    ".webp",
}

RAND_GEOMETRIC_POLICY_NAMES = {
    "rotate",
    "shear_x",
    "shear_y",
    "translate_x",
    "translate_y",
}

LABEL_AWARE_MAX_ATTEMPTS = 8
NO_PROGRESS_TIMEOUT_SECONDS = 120.0
CUDA_UNSAFE_CPU_FALLBACK_OPS = frozenset({"random_erasing", "random_jpeg", "random_perspective"})
MAX_AUGMENTS_PER_IMAGE = 4


@dataclass(frozen=True)
class ImageJob:
    path: str
    op_indices: Tuple[int, ...]


@dataclass(frozen=True)
class OpSpec:
    name: str
    kind: str
    payload: object
    transforms_labels: bool


@dataclass(frozen=True)
class PreparedAugmenter:
    module: torch.nn.Module
    device: torch.device


@dataclass(frozen=True)
class RootPlan:
    root: Path
    images_total: int
    jobs: List[ImageJob]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Apply one medium-strength Kornia augmentation op in place to 75%% of images, updating paired .jsonl labels for geometric transforms."
    )
    parser.add_argument(
        "roots",
        nargs="*",
        help="One or more root directories containing images to augment in place, e.g. train val.",
    )
    parser.add_argument("--fraction", type=float, default=0.75, help="Fraction of images to augment. Default: 0.75")
    parser.add_argument(
        "--magnitude",
        type=int,
        default=15,
        help="RandAugment magnitude m in [1, 29] for the RandAugment-derived ops. Default: 15",
    )
    parser.add_argument("--workers", type=int, default=max(1, (os.cpu_count() or 1) - 1), help="Worker processes.")
    parser.add_argument("--batch-size", type=int, default=32, help="Batch size per op within each worker for image-only ops.")
    parser.add_argument("--device", default="cuda:0", help="Execution device, e.g. cuda:0 or cpu. Default: cuda:0")
    parser.add_argument("--size", type=int, default=432, help="Required image width/height. Default: 432")
    parser.add_argument(
        "--extensions",
        nargs="*",
        default=sorted(DEFAULT_EXTENSIONS),
        help="Image extensions to scan. Default: .jpg .jpeg .png .bmp .tif .tiff .webp",
    )
    parser.add_argument("--non-recursive", action="store_true", help="Do not recurse into subdirectories.")
    parser.add_argument("--dry-run", action="store_true", help="Show what would be changed without writing files.")
    parser.add_argument("--list-ops", action="store_true", help="List the augmentation op pool and exit.")
    return parser.parse_args()


def resolve_randaugment_policy() -> List[List[Tuple[str, float, Optional[float]]]]:
    try:
        from kornia.augmentation.auto.rand_augment.rand_augment import default_policy  # type: ignore

        resolved = []
        for subpolicy in default_policy:
            if len(subpolicy) != 1:
                raise RuntimeError(f"unexpected RandAugment subpolicy length: {len(subpolicy)}")
            name, low, high = subpolicy[0]
            resolved.append([(str(name), float(low), None if high is None else float(high))])
        if resolved:
            return resolved
    except Exception:
        pass
    return FALLBACK_POLICY


def resolve_device(device_text: str) -> torch.device:
    try:
        device = torch.device(device_text)
    except Exception as exc:
        raise RuntimeError(f"invalid --device value: {device_text}") from exc

    if device.type == "cuda":
        if not torch.cuda.is_available():
            raise RuntimeError(f"--device {device_text} requested CUDA, but torch.cuda.is_available() is false")
        if device.index is not None and device.index >= torch.cuda.device_count():
            raise RuntimeError(
                f"--device {device_text} requested CUDA device {device.index}, but only {torch.cuda.device_count()} device(s) are available"
            )
    elif device.type != "cpu":
        raise RuntimeError(f"unsupported --device type: {device.type}")

    return device


def move_tensor_to_device(tensor: torch.Tensor, device: torch.device) -> torch.Tensor:
    if device.type == "cuda":
        if tensor.device.type != "cpu":
            return tensor.to(device, non_blocking=True)
        if not tensor.is_pinned():
            tensor = tensor.pin_memory()
        return tensor.to(device, non_blocking=True)
    return tensor.to(device)


def augmenter_device_for_op(op_name: str, requested_device: torch.device) -> torch.device:
    if requested_device.type == "cuda" and op_name in CUDA_UNSAFE_CPU_FALLBACK_OPS:
        return torch.device("cpu")
    return requested_device


def build_op_specs(
    randaugment_policy: Sequence[List[Tuple[str, float, Optional[float]]]],
    required_size: int,
) -> List[OpSpec]:
    crop_resize = K.AugmentationSequential(
        K.RandomCrop((max(1, required_size - 24), max(1, required_size - 24)), p=1.0),
        K.Resize((required_size, required_size), p=1.0),
        data_keys=["input"],
    )

    op_specs = [
        OpSpec(
            f"randaugment_{subpolicy[0][0]}",
            "randaugment",
            subpolicy,
            subpolicy[0][0] in RAND_GEOMETRIC_POLICY_NAMES,
        )
        for subpolicy in randaugment_policy
    ]
    op_specs.extend(
        [
            OpSpec("color_jiggle", "module", K.ColorJiggle(0.1, 0.1, 0.1, 0.04, p=1.0), False),
            OpSpec("color_jitter", "module", K.ColorJitter(0.1, 0.1, 0.1, 0.04, p=1.0), False),
            OpSpec("random_affine", "module", K.RandomAffine(
                degrees=(-7.5, 7.5),
                translate=(0.03, 0.03),
                scale=(0.96, 1.04),
                shear=torch.tensor([[-4.0, 4.0], [0.0, 0.0]]),
                p=1.0,
                padding_mode="zeros",
            ), True),
            OpSpec("random_auto_contrast", "module", K.RandomAutoContrast(p=1.0), False),
            OpSpec("random_box_blur", "module", K.RandomBoxBlur(kernel_size=(5, 5), p=1.0), False),
            OpSpec("random_brightness", "module", K.RandomBrightness(brightness=(0.925, 1.075), p=1.0), False),
            OpSpec("random_channel_dropout", "module", K.RandomChannelDropout(num_drop_channels=1, p=1.0), False),
            OpSpec("random_channel_shuffle", "module", K.RandomChannelShuffle(p=1.0), False),
            OpSpec("random_clahe", "module", K.RandomClahe(clip_limit=(10.0, 25.0), grid_size=(8, 8), p=1.0), False),
            OpSpec("random_contrast", "module", K.RandomContrast(contrast=(0.925, 1.075), p=1.0), False),
            OpSpec("random_crop_resize", "module", crop_resize, True),
            OpSpec("random_equalize", "module", K.RandomEqualize(p=1.0), False),
            OpSpec("random_erasing", "module", K.RandomErasing(
                scale=(0.02, 0.07),
                ratio=(0.75, 1.4),
                value=0.0,
                p=1.0,
            ), False),
            OpSpec("random_fisheye", "module", K.RandomFisheye(
                center_x=torch.tensor([0.45, 0.55]),
                center_y=torch.tensor([0.45, 0.55]),
                gamma=torch.tensor([0.9, 1.1]),
                p=1.0,
            ), True),
            OpSpec("random_gamma", "module", K.RandomGamma(gamma=(0.925, 1.075), gain=(1.0, 1.0), p=1.0), False),
            OpSpec("random_gaussian_blur", "module", K.RandomGaussianBlur((5, 5), (0.3, 0.75), p=1.0), False),
            OpSpec("random_gaussian_illumination", "module", K.RandomGaussianIllumination(
                gain=(0.02, 0.07),
                center=(0.35, 0.65),
                sigma=(0.3, 0.6),
                sign=(-1.0, 1.0),
                p=1.0,
            ), False),
            OpSpec("random_gaussian_noise", "module", K.RandomGaussianNoise(mean=0.0, std=0.025, p=1.0), False),
            OpSpec("random_grayscale", "module", K.RandomGrayscale(p=1.0), False),
            OpSpec("random_horizontal_flip", "module", K.RandomHorizontalFlip(p=1.0), True),
            OpSpec("random_hue", "module", K.RandomHue(hue=(-0.04, 0.04), p=1.0), False),
            OpSpec("random_invert", "module", K.RandomInvert(p=1.0), False),
            OpSpec("random_jpeg", "module", K.RandomJPEG(jpeg_quality=(62.5, 80.0), p=1.0), False),
            OpSpec("random_jigsaw", "module", K.RandomJigsaw(grid=(4, 4), p=1.0), True),
            OpSpec("random_linear_corner_illumination", "module", K.RandomLinearCornerIllumination(
                gain=(0.02, 0.07),
                sign=(-1.0, 1.0),
                p=1.0,
            ), False),
            OpSpec("random_linear_illumination", "module", K.RandomLinearIllumination(
                gain=(0.02, 0.07),
                sign=(-1.0, 1.0),
                p=1.0,
            ), False),
            OpSpec("random_median_blur", "module", K.RandomMedianBlur((5, 5), p=1.0), False),
            OpSpec("random_motion_blur", "module", K.RandomMotionBlur(3, angle=(-10.0, 10.0), direction=(-0.25, 0.25), p=1.0), False),
            OpSpec("random_perspective", "module", K.RandomPerspective(0.125, p=1.0), True),
            OpSpec("random_planckian_jitter", "module", K.RandomPlanckianJitter(p=1.0), False),
            OpSpec("random_plasma_brightness", "module", K.RandomPlasmaBrightness(
                roughness=(0.2, 0.4),
                intensity=(0.05, 0.175),
                p=1.0,
            ), False),
            OpSpec("random_plasma_contrast", "module", K.RandomPlasmaContrast(roughness=(0.2, 0.4), p=1.0), False),
            OpSpec("random_plasma_shadow", "module", K.RandomPlasmaShadow(
                roughness=(0.2, 0.4),
                shade_intensity=(-0.3, -0.1),
                shade_quantity=(0.2, 0.45),
                p=1.0,
            ), False),
            OpSpec("random_posterize", "module", K.RandomPosterize(bits=(4.5, 6.0), p=1.0), False),
            OpSpec("random_rain", "module", K.RandomRain(
                number_of_drops=(300, 600),
                drop_height=(6, 12),
                drop_width=(-2, 2),
                p=1.0,
            ), False),
            OpSpec("random_resized_crop", "module", K.RandomResizedCrop(
                (required_size, required_size),
                scale=(0.85, 1.0),
                ratio=(0.925, 1.075),
                p=1.0,
            ), True),
            OpSpec("random_rgb_shift", "module", K.RandomRGBShift(0.075, 0.075, 0.075, p=1.0), False),
            OpSpec("random_rotation", "module", K.RandomRotation(degrees=(-10.0, 10.0), p=1.0), True),
            OpSpec("random_rotation90", "module", K.RandomRotation90(times=(1, 2), p=1.0), True),
            OpSpec("random_salt_and_pepper", "module", K.RandomSaltAndPepperNoise(
                amount=(0.01, 0.025),
                salt_vs_pepper=(0.4, 0.6),
                p=1.0,
            ), False),
            OpSpec("random_saturation", "module", K.RandomSaturation(saturation=(0.925, 1.075), p=1.0), False),
            OpSpec("random_sharpness", "module", K.RandomSharpness(sharpness=(0.2, 0.5), p=1.0), False),
            OpSpec("random_shear", "module", K.RandomShear(shear=torch.tensor([[-5.0, 5.0], [0.0, 0.0]]), p=1.0), True),
            OpSpec("random_snow", "module", K.RandomSnow(
                snow_coefficient=(0.15, 0.25),
                brightness=(1.1, 1.5),
                p=1.0,
            ), False),
            OpSpec("random_solarize", "module", K.RandomSolarize(
                thresholds=(0.35, 0.65),
                additions=(0.0, 0.1),
                p=1.0,
            ), False),
            OpSpec("random_thin_plate_spline", "module", K.RandomThinPlateSpline(scale=0.06, p=1.0), True),
            OpSpec("random_translate", "module", K.RandomTranslate(
                translate_x=(-0.04, 0.04),
                translate_y=(-0.04, 0.04),
                p=1.0,
            ), True),
            OpSpec("random_vertical_flip", "module", K.RandomVerticalFlip(p=1.0), True),
        ]
    )
    return op_specs


def collect_images(root: Path, recursive: bool, extensions: Sequence[str]) -> List[Path]:
    ext_set = {ext.lower() if ext.startswith(".") else f".{ext.lower()}" for ext in extensions}
    images: List[Path] = []
    if recursive:
        pending = [root]
        while pending:
            current = pending.pop()
            with os.scandir(current) as entries:
                dirs: List[Path] = []
                files: List[Path] = []
                for entry in entries:
                    if entry.name.startswith("."):
                        continue
                    if entry.is_dir(follow_symlinks=False):
                        dirs.append(Path(entry.path))
                    elif entry.is_file(follow_symlinks=False):
                        path = Path(entry.path)
                        if path.suffix.lower() in ext_set:
                            files.append(path)
                dirs.sort()
                files.sort()
                pending.extend(reversed(dirs))
                images.extend(files)
    else:
        with os.scandir(root) as entries:
            for entry in entries:
                if entry.name.startswith(".") or not entry.is_file(follow_symlinks=False):
                    continue
                path = Path(entry.path)
                if path.suffix.lower() in ext_set:
                    images.append(path)
    images.sort()
    return images


def compute_selected_count(total: int, fraction: float) -> int:
    if total <= 0:
        return 0
    if fraction <= 0.0:
        return 0
    if fraction >= 1.0:
        return total
    return max(1, min(total, int(round(total * fraction))))


def build_jobs(paths: Sequence[Path], policy_size: int, fraction: float) -> List[ImageJob]:
    rng = random.SystemRandom()
    selected_count = compute_selected_count(len(paths), fraction)
    if selected_count == 0:
        return []

    selected_paths = list(rng.sample(list(paths), selected_count))
    rng.shuffle(selected_paths)

    jobs: List[ImageJob] = []
    for path in selected_paths:
        augment_count = rng.randint(1, MAX_AUGMENTS_PER_IMAGE)
        op_indices = tuple(rng.randrange(policy_size) for _ in range(augment_count))
        jobs.append(ImageJob(str(path), op_indices))
    return jobs


def chunk_jobs(jobs: Sequence[ImageJob], worker_count: int) -> List[List[ImageJob]]:
    worker_count = max(1, worker_count)
    chunks: List[List[ImageJob]] = [[] for _ in range(min(worker_count, max(1, len(jobs))))]
    for index, job in enumerate(jobs):
        chunks[index % len(chunks)].append(job)
    return [chunk for chunk in chunks if chunk]


def build_root_plan(
    root: Path,
    recursive: bool,
    extensions: Sequence[str],
    policy_size: int,
    fraction: float,
) -> RootPlan:
    images = collect_images(root, recursive=recursive, extensions=extensions)
    if not images:
        raise RuntimeError(f"no images found under: {root}")
    jobs = build_jobs(images, policy_size, fraction)
    return RootPlan(root=root, images_total=len(images), jobs=jobs)


def load_rgb_image(path: Path, required_size: int) -> Tuple[np.ndarray, Optional[str]]:
    with Image.open(path) as image:
        original_format = image.format
        image = image.convert("RGB")
        if image.width != required_size or image.height != required_size:
            raise RuntimeError(
                f"expected {required_size}x{required_size}, got {image.width}x{image.height}: {path}"
            )
        return np.array(image, dtype=np.uint8, copy=True), original_format


def save_image_atomic(path: Path, image: np.ndarray, original_format: Optional[str]) -> None:
    pil_image = Image.fromarray(image)
    suffix = path.suffix.lower()
    save_kwargs: Dict[str, object] = {}
    if suffix in {".jpg", ".jpeg"} or (original_format and original_format.upper() == "JPEG"):
        save_kwargs["quality"] = 95
        save_kwargs["subsampling"] = 0

    with tempfile.NamedTemporaryFile(dir=path.parent, prefix=f".{path.name}.", suffix=path.suffix, delete=False) as tmp:
        tmp_path = Path(tmp.name)
    try:
        pil_image.save(tmp_path, format=original_format, **save_kwargs)
        os.replace(tmp_path, path)
    except Exception:
        tmp_path.unlink(missing_ok=True)
        raise


def label_path_for_image(path: Path) -> Path:
    return path.with_suffix(".jsonl")


def load_label_records(path: Path, required_size: int) -> List[Dict[str, object]]:
    if not path.is_file():
        raise RuntimeError(f"missing label file for image: {path}")

    records: List[Dict[str, object]] = []
    with path.open("r", encoding="utf-8") as handle:
        for line_number, raw_line in enumerate(handle, start=1):
            line = raw_line.strip()
            if not line:
                continue
            record = json.loads(line)
            if not isinstance(record, dict):
                raise RuntimeError(f"expected JSON object in {path}:{line_number}")
            if record.get("mask_rle_encoding") != "row_major_start_length":
                raise RuntimeError(f"unsupported mask_rle_encoding in {path}:{line_number}")
            image_size = record.get("image_size_wh")
            if image_size != [required_size, required_size]:
                raise RuntimeError(
                    f"expected image_size_wh [{required_size}, {required_size}] in {path}:{line_number}, got {image_size}"
                )
            bbox = record.get("bbox_xyxy")
            if not isinstance(bbox, list) or len(bbox) != 4:
                raise RuntimeError(f"expected bbox_xyxy length 4 in {path}:{line_number}")
            mask_rle = record.get("mask_rle")
            if not isinstance(mask_rle, str):
                raise RuntimeError(f"expected mask_rle string in {path}:{line_number}")
            records.append(record)
    if not records:
        raise RuntimeError(f"expected at least one label record in {path}")
    return records


def save_jsonl_atomic(path: Path, records: Sequence[Dict[str, object]]) -> None:
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix=f".{path.name}.", suffix=path.suffix, delete=False) as tmp:
        tmp_path = Path(tmp.name)
    try:
        with tmp_path.open("w", encoding="utf-8") as handle:
            for record in records:
                handle.write(json.dumps(record, separators=(",", ":")))
                handle.write("\n")
        os.replace(tmp_path, path)
    except Exception:
        tmp_path.unlink(missing_ok=True)
        raise


def decode_rle_row_major_start_length(rle: str, required_size: int) -> np.ndarray:
    flat = np.zeros(required_size * required_size, dtype=np.uint8)
    if not rle:
        return flat.reshape(required_size, required_size)

    for run in rle.split():
        start_text, length_text = run.split(":", 1)
        start = int(start_text)
        length = int(length_text)
        if start < 0 or length <= 0 or start + length > flat.size:
            raise RuntimeError(f"invalid RLE run '{run}' for {required_size}x{required_size} image")
        flat[start : start + length] = 1
    return flat.reshape(required_size, required_size)


def encode_rle_row_major_start_length(mask: np.ndarray) -> str:
    flat = np.ascontiguousarray(mask.astype(np.uint8)).reshape(-1)
    if flat.size == 0:
        return ""
    padded = np.concatenate((np.zeros(1, dtype=np.uint8), flat, np.zeros(1, dtype=np.uint8)))
    changes = np.flatnonzero(padded[1:] != padded[:-1])
    starts = changes[::2]
    ends = changes[1::2]
    return " ".join(f"{int(start)}:{int(end - start)}" for start, end in zip(starts, ends))


def bbox_xyxy_from_mask(mask: np.ndarray) -> Optional[List[int]]:
    ys, xs = np.nonzero(mask)
    if xs.size == 0 or ys.size == 0:
        return None
    return [int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())]


def records_to_mask_tensor(records: Sequence[Dict[str, object]], required_size: int) -> torch.Tensor:
    masks = [
        decode_rle_row_major_start_length(str(record["mask_rle"]), required_size).astype(np.float32)
        for record in records
    ]
    return torch.from_numpy(np.stack(masks, axis=0))


def rebuild_records_from_masks(
    records: Sequence[Dict[str, object]],
    masks: torch.Tensor,
    required_size: int,
) -> List[Dict[str, object]]:
    if masks.ndim != 3 or masks.shape[0] != len(records):
        raise RuntimeError(f"unexpected transformed mask shape: {tuple(masks.shape)}")

    binary_masks = masks.gt(0.5).cpu().numpy()
    rebuilt: List[Dict[str, object]] = []
    for record, mask in zip(records, binary_masks):
        bbox = bbox_xyxy_from_mask(mask)
        if bbox is None:
            continue
        updated = dict(record)
        updated["bbox_xyxy"] = bbox
        updated["mask_rle_encoding"] = "row_major_start_length"
        updated["mask_rle"] = encode_rle_row_major_start_length(mask)
        updated["image_size_wh"] = [required_size, required_size]
        rebuilt.append(updated)
    return rebuilt


def augment_image_and_labels(
    image_array: np.ndarray,
    records: Sequence[Dict[str, object]],
    augmenter: torch.nn.Module,
    required_size: int,
    device: torch.device,
) -> Tuple[np.ndarray, List[Dict[str, object]]]:
    image_tensor = torch.from_numpy(image_array).permute(2, 0, 1).unsqueeze(0).float().div_(255.0)
    mask_tensor = records_to_mask_tensor(records, required_size).unsqueeze(0)
    image_tensor = move_tensor_to_device(image_tensor, device).contiguous()
    mask_tensor = move_tensor_to_device(mask_tensor, device).contiguous()

    for _ in range(LABEL_AWARE_MAX_ATTEMPTS):
        params = augmenter.forward_parameters(image_tensor.shape)  # type: ignore[attr-defined]
        augmented_image = augmenter(image_tensor, params=params)
        augmented_masks = augmenter(mask_tensor, params=params)
        updated_records = rebuild_records_from_masks(records, augmented_masks.squeeze(0), required_size)
        if updated_records:
            augmented_np = (
                augmented_image.clamp_(0.0, 1.0)
                .mul(255.0)
                .round()
                .to(torch.uint8)
                .squeeze(0)
                .permute(1, 2, 0)
                .contiguous()
                .cpu()
                .numpy()
            )
            return augmented_np, updated_records

    raise RuntimeError(
        f"augmentation removed every instance after {LABEL_AWARE_MAX_ATTEMPTS} attempts on a labeled image"
    )


def build_augmenter(spec: OpSpec, magnitude: int, device: torch.device) -> PreparedAugmenter:
    if spec.kind == "randaugment":
        base_augmenter: torch.nn.Module = RandAugment(
            n=1,
            m=magnitude,
            policy=[spec.payload],  # type: ignore[list-item]
            transformation_matrix_mode="skip",
        )
    elif spec.kind == "module":
        base_augmenter = spec.payload
        if not isinstance(base_augmenter, torch.nn.Module):
            raise RuntimeError(f"op payload is not a torch module: {spec.name}")
    else:
        raise RuntimeError(f"unsupported op kind: {spec.kind}")

    op_device = augmenter_device_for_op(spec.name, device)
    module = base_augmenter if op_device.type == "cpu" else base_augmenter.to(op_device)
    return PreparedAugmenter(module=module, device=op_device)


def build_augmenters(op_specs: Sequence[OpSpec], magnitude: int, device: torch.device) -> List[PreparedAugmenter]:
    return [build_augmenter(spec, magnitude, device) for spec in op_specs]


def drain_progress_queue(progress_queue: object) -> int:
    drained = 0
    while True:
        try:
            drained += int(progress_queue.get_nowait())
        except queue_module.Empty:
            return drained


def augment_image_only_batch(
    arrays: Sequence[np.ndarray],
    augmenter: torch.nn.Module,
    device: torch.device,
) -> np.ndarray:
    batch = torch.from_numpy(np.stack(arrays, axis=0)).permute(0, 3, 1, 2).float().div_(255.0)
    batch = move_tensor_to_device(batch, device).contiguous()
    augmented_batch = augmenter(batch).clamp_(0.0, 1.0)
    return (
        augmented_batch.mul(255.0)
        .round()
        .to(torch.uint8)
        .permute(0, 2, 3, 1)
        .contiguous()
        .cpu()
        .numpy()
    )


def log_cuda_fallback(op_name: str, device: torch.device, exc: Exception) -> None:
    sys.stderr.write(
        f"warning: {op_name} failed on {device}; rerouting this op to cpu for the rest of the run: {type(exc).__name__}: {exc}\n"
    )
    sys.stderr.flush()


def apply_image_only_augment(
    image_array: np.ndarray,
    op_index: int,
    op_specs: Sequence[OpSpec],
    augmenters: List[PreparedAugmenter],
    magnitude: int,
) -> np.ndarray:
    prepared = augmenters[op_index]
    op_spec = op_specs[op_index]
    try:
        return augment_image_only_batch([image_array], prepared.module, prepared.device)[0]
    except Exception as exc:
        if prepared.device.type != "cuda":
            raise
        log_cuda_fallback(op_spec.name, prepared.device, exc)
        rebuilt = build_augmenter(op_spec, magnitude, torch.device("cpu"))
        augmenters[op_index] = rebuilt
        return augment_image_only_batch([image_array], rebuilt.module, rebuilt.device)[0]


def apply_label_aware_augment(
    image_array: np.ndarray,
    records: Sequence[Dict[str, object]],
    op_index: int,
    op_specs: Sequence[OpSpec],
    augmenters: List[PreparedAugmenter],
    magnitude: int,
    required_size: int,
) -> Tuple[np.ndarray, List[Dict[str, object]]]:
    prepared = augmenters[op_index]
    op_spec = op_specs[op_index]
    try:
        return augment_image_and_labels(
            image_array=image_array,
            records=records,
            augmenter=prepared.module,
            required_size=required_size,
            device=prepared.device,
        )
    except Exception as exc:
        if prepared.device.type != "cuda":
            raise
        log_cuda_fallback(op_spec.name, prepared.device, exc)
        rebuilt = build_augmenter(op_spec, magnitude, torch.device("cpu"))
        augmenters[op_index] = rebuilt
        return augment_image_and_labels(
            image_array=image_array,
            records=records,
            augmenter=rebuilt.module,
            required_size=required_size,
            device=rebuilt.device,
        )


def run_multi_augment_job(
    job: ImageJob,
    op_specs: Sequence[OpSpec],
    augmenters: List[PreparedAugmenter],
    magnitude: int,
    required_size: int,
    dry_run: bool,
) -> Counter[str]:
    path = Path(job.path)
    image_array, image_format = load_rgb_image(path, required_size)
    needs_labels = any(op_specs[op_index].transforms_labels for op_index in job.op_indices)
    current_records: Optional[List[Dict[str, object]]] = None
    if needs_labels:
        current_records = load_label_records(label_path_for_image(path), required_size)

    current_image = image_array
    op_counts: Counter[str] = Counter()
    for op_index in job.op_indices:
        op_spec = op_specs[op_index]
        op_counts[op_spec.name] += 1
        if op_spec.transforms_labels:
            if current_records is None:
                raise RuntimeError(f"missing label state for label-aware op: {op_spec.name}")
            current_image, current_records = apply_label_aware_augment(
                image_array=current_image,
                records=current_records,
                op_index=op_index,
                op_specs=op_specs,
                augmenters=augmenters,
                magnitude=magnitude,
                required_size=required_size,
            )
        else:
            current_image = apply_image_only_augment(
                image_array=current_image,
                op_index=op_index,
                op_specs=op_specs,
                augmenters=augmenters,
                magnitude=magnitude,
            )

    if not dry_run:
        save_image_atomic(path, current_image, image_format)
        if current_records is not None:
            save_jsonl_atomic(label_path_for_image(path), current_records)
    return op_counts


def run_image_only_batch(
    batch_paths: Sequence[Path],
    formats: Sequence[Optional[str]],
    arrays: Sequence[np.ndarray],
    augmenter: torch.nn.Module,
    dry_run: bool,
    device: torch.device,
) -> None:
    if dry_run:
        return
    augmented_np = augment_image_only_batch(arrays=arrays, augmenter=augmenter, device=device)
    for image_path, image_format, image_array in zip(batch_paths, formats, augmented_np):
        save_image_atomic(image_path, image_array, image_format)


def run_worker(
    jobs: Sequence[ImageJob],
    randaugment_policy: Sequence[List[Tuple[str, float, Optional[float]]]],
    magnitude: int,
    batch_size: int,
    required_size: int,
    dry_run: bool,
    device_text: str,
    progress_queue: Optional[object] = None,
) -> Dict[str, object]:
    device = resolve_device(device_text)
    if device.type == "cuda":
        torch.cuda.set_device(device)
    torch.set_num_threads(1)
    if hasattr(torch, "set_num_interop_threads"):
        try:
            torch.set_num_interop_threads(1)
        except RuntimeError:
            pass
    warnings.filterwarnings(
        "ignore",
        message=r"torch\.meshgrid: in an upcoming release, it will be required to pass the indexing argument\.",
        category=UserWarning,
    )

    op_specs = build_op_specs(randaugment_policy, required_size)
    augmenters = build_augmenters(op_specs, magnitude, device)
    op_to_jobs: Dict[int, List[Path]] = defaultdict(list)
    sequence_jobs: List[ImageJob] = []
    for job in jobs:
        if len(job.op_indices) == 1 and not op_specs[job.op_indices[0]].transforms_labels:
            op_to_jobs[job.op_indices[0]].append(Path(job.path))
        else:
            sequence_jobs.append(job)

    augmented = 0
    op_counts: Counter[str] = Counter()
    pending_progress = 0

    def flush_progress(force: bool = False) -> None:
        nonlocal pending_progress
        if progress_queue is not None and pending_progress > 0 and (force or pending_progress >= 8):
            progress_queue.put(pending_progress)
            pending_progress = 0

    with torch.inference_mode():
        for op_index, paths in op_to_jobs.items():
            prepared_augmenter = augmenters[op_index]
            augmenter = prepared_augmenter.module
            op_spec = op_specs[op_index]
            op_name = op_spec.name
            op_device = prepared_augmenter.device

            for offset in range(0, len(paths), batch_size):
                batch_paths = paths[offset : offset + batch_size]
                arrays: List[np.ndarray] = []
                formats: List[Optional[str]] = []
                for path in batch_paths:
                    image_array, image_format = load_rgb_image(path, required_size)
                    arrays.append(image_array)
                    formats.append(image_format)

                if dry_run:
                    augmented += len(batch_paths)
                    op_counts[op_name] += len(batch_paths)
                    continue

                try:
                    run_image_only_batch(
                        batch_paths=batch_paths,
                        formats=formats,
                        arrays=arrays,
                        augmenter=augmenter,
                        dry_run=dry_run,
                        device=op_device,
                    )
                except Exception as exc:
                    if op_device.type != "cuda":
                        raise
                    log_cuda_fallback(op_name, op_device, exc)
                    rebuilt = build_augmenter(op_spec, magnitude, torch.device("cpu"))
                    augmenter = rebuilt.module
                    op_device = rebuilt.device
                    augmenters[op_index] = rebuilt
                    run_image_only_batch(
                        batch_paths=batch_paths,
                        formats=formats,
                        arrays=arrays,
                        augmenter=augmenter,
                        dry_run=dry_run,
                        device=op_device,
                    )

                augmented += len(batch_paths)
                op_counts[op_name] += len(batch_paths)
                pending_progress += len(batch_paths)
                flush_progress(force=True)

        for job in sequence_jobs:
            job_counts = run_multi_augment_job(
                job=job,
                op_specs=op_specs,
                augmenters=augmenters,
                magnitude=magnitude,
                required_size=required_size,
                dry_run=dry_run,
            )
            augmented += 1
            op_counts.update(job_counts)
            pending_progress += 1
            flush_progress(force=True)

    flush_progress(force=True)

    return {
        "augmented": augmented,
        "op_counts": dict(op_counts),
    }


def merge_counts(results: Sequence[Dict[str, object]]) -> Tuple[int, Counter[str]]:
    total_augmented = 0
    op_counts: Counter[str] = Counter()
    for result in results:
        total_augmented += int(result["augmented"])
        op_counts.update(result["op_counts"])  # type: ignore[arg-type]
    return total_augmented, op_counts


def main() -> int:
    args = parse_args()

    if args.fraction < 0.0 or args.fraction > 1.0:
        raise RuntimeError(f"--fraction must be in [0.0, 1.0]. Got {args.fraction}")
    if args.magnitude <= 0 or args.magnitude >= 30:
        raise RuntimeError(f"--magnitude must be in [1, 29]. Got {args.magnitude}")
    if args.batch_size <= 0:
        raise RuntimeError(f"--batch-size must be positive. Got {args.batch_size}")
    if args.workers <= 0:
        raise RuntimeError(f"--workers must be positive. Got {args.workers}")
    if args.size <= 0:
        raise RuntimeError(f"--size must be positive. Got {args.size}")

    randaugment_policy = resolve_randaugment_policy()
    op_specs = build_op_specs(randaugment_policy, args.size)
    if args.list_ops:
        for spec in op_specs:
            print(spec.name)
        print(f"ops_available={len(op_specs)}")
        return 0
    device = resolve_device(args.device)

    if not args.roots:
        raise RuntimeError("at least one root directory is required unless --list-ops is used")

    roots = [Path(root).resolve() for root in args.roots]
    for root in roots:
        if not root.is_dir():
            raise RuntimeError(f"image root is not a directory: {root}")

    root_plans: List[RootPlan] = []
    for root in roots:
        root_plans.append(
            build_root_plan(
                root=root,
                recursive=not args.non_recursive,
                extensions=args.extensions,
                policy_size=len(op_specs),
                fraction=args.fraction,
            )
        )

    total_images = sum(plan.images_total for plan in root_plans)
    jobs = [job for plan in root_plans for job in plan.jobs]
    if not jobs:
        print(f"images_total={total_images}")
        print("images_selected=0")
        print("images_augmented=0")
        return 0

    effective_workers = 1 if device.type == "cuda" else args.workers
    task_count = 1 if device.type == "cuda" else min(len(jobs), max(1, effective_workers * 8))
    chunks = chunk_jobs(jobs, task_count)
    results: List[Dict[str, object]] = []

    class LocalProgressQueue:
        def __init__(self, progress: tqdm) -> None:
            self.progress = progress

        def put(self, value: int) -> None:
            self.progress.update(int(value))

    with tqdm(total=len(jobs), unit="img", desc="augment", dynamic_ncols=True) as progress:
        if len(chunks) == 1:
            results.append(
                run_worker(
                    chunks[0],
                    randaugment_policy,
                    args.magnitude,
                    args.batch_size,
                    args.size,
                    args.dry_run,
                    str(device),
                    LocalProgressQueue(progress),
                )
            )
        else:
            ctx = multiprocessing.get_context("spawn")
            manager = ctx.Manager()
            executor: Optional[concurrent.futures.ProcessPoolExecutor] = None
            fast_shutdown = False
            try:
                progress_queue = manager.Queue()
                executor = concurrent.futures.ProcessPoolExecutor(
                    max_workers=min(args.workers, len(chunks)),
                    mp_context=ctx,
                )
                pending_futures = {
                    executor.submit(
                        run_worker,
                        chunk,
                        randaugment_policy,
                            args.magnitude,
                            args.batch_size,
                            args.size,
                            args.dry_run,
                            str(device),
                            progress_queue,
                        )
                        for chunk in chunks
                    }
                last_progress_time = time.monotonic()
                while pending_futures:
                    done, pending_futures = concurrent.futures.wait(
                        pending_futures,
                        timeout=0.2,
                        return_when=concurrent.futures.FIRST_COMPLETED,
                    )
                    drained = drain_progress_queue(progress_queue)
                    if drained > 0:
                        progress.update(drained)
                        last_progress_time = time.monotonic()
                    for future in done:
                        results.append(future.result())
                        last_progress_time = time.monotonic()
                    if pending_futures and time.monotonic() - last_progress_time > NO_PROGRESS_TIMEOUT_SECONDS:
                        fast_shutdown = True
                        raise RuntimeError(
                            f"no progress for {NO_PROGRESS_TIMEOUT_SECONDS:.0f}s with {len(pending_futures)} worker tasks still running; likely a stuck or pathological augmentation op"
                        )
                drained = drain_progress_queue(progress_queue)
                if drained > 0:
                    progress.update(drained)
            finally:
                if executor is not None:
                    executor.shutdown(wait=not fast_shutdown, cancel_futures=fast_shutdown)
                manager.shutdown()

    augmented_count, op_counts = merge_counts(results)
    print(f"roots={len(root_plans)}")
    for plan in root_plans:
        print(f"root={plan.root}")
        print(f"root_images_total={plan.images_total}")
        print(f"root_images_selected={len(plan.jobs)}")
    print(f"images_total={total_images}")
    print(f"images_selected={len(jobs)}")
    print(f"images_augmented={augmented_count}")
    print(f"fraction_requested={args.fraction:.4f}")
    print(f"magnitude={args.magnitude}")
    print(f"ops_available={len(op_specs)}")
    print(f"device={device}")
    print(f"effective_workers={effective_workers}")
    print(f"mode={'dry-run' if args.dry_run else 'in-place'}")
    for op_name in sorted(op_counts):
        print(f"{op_name}={op_counts[op_name]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
