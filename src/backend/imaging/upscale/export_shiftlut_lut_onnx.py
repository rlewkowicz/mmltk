#!/usr/bin/env python3
"""Prepare checked ShiftLUT tables; the native generator owns the production graph."""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
from pathlib import Path
import re
import subprocess
import types

import numpy as np
import torch


_SOURCE_SHA256 = "4be9d7420763f82db81d614bcaf0fcc54331afceb669bc4d275c1223be0165ee"
_DEFINITIONS = {"LUTclip", "Query", "dwLUT", "pwLUT", "LUT", "inference"}


def _reference(source: Path):
    path = source / "LUT_test/sr/model_LUT.py"
    content = path.read_bytes()
    digest = hashlib.sha256(content).hexdigest()
    if digest != _SOURCE_SHA256:
        raise ValueError("unexpected upstream model_LUT.py; review its complete inference before updating the pinned digest")
    tree = ast.parse(content, filename=str(path))
    retained = []
    definitions = set()
    for node in tree.body:
        if isinstance(node, (ast.ClassDef, ast.FunctionDef)):
            if node.name not in _DEFINITIONS:
                raise ValueError(f"unexpected upstream definition {node.name}")
            retained.append(node)
            definitions.add(node.name)
        elif isinstance(node, (ast.Import, ast.ImportFrom)):
            # Only unrelated demo imports are omitted. Inference definitions
            # execute directly from the pinned source AST, unmodified.
            roots = {alias.name.split(".")[0] for alias in node.names} if isinstance(node, ast.Import) else {node.module}
            if roots <= {"PIL", "torch.autograd", "time"}:
                continue
            retained.append(node)
        elif isinstance(node, ast.Assign):
            if len(node.targets) != 1 or not isinstance(node.targets[0], ast.Name) or node.targets[0].id != "debug_oo":
                raise ValueError("unexpected upstream module state")
            retained.append(node)
        elif isinstance(node, ast.If) and ast.unparse(node.test) == "__name__ == '__main__'":
            continue
        else:
            raise ValueError("unexpected upstream module structure")
    if definitions != _DEFINITIONS:
        raise ValueError("missing upstream inference definitions")
    module = types.ModuleType("checked_shiftlut_reference")
    exec(compile(ast.Module(body=retained, type_ignores=[]), str(path), "exec"), module.__dict__)
    return module, digest


def _asset_digest(root: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(root.iterdir(), key=lambda item: item.name):
        if path.is_file():
            digest.update(path.name.encode())
            digest.update(b"\0")
            digest.update(path.read_bytes())
    return digest.hexdigest()


def _table(reference, root: Path, name: str, domain: int, inputs: int) -> np.ndarray:
    metadata = json.loads((root / f"{name}.json").read_text())
    if metadata["IN"] != inputs or metadata["OUT"] != 16:
        raise ValueError(f"unexpected table shape for {name}")
    steps = {(int(item["in"]), int(item["out"])): int(item["step"]) for item in metadata["LUTs"]}
    if len(metadata["LUTs"]) != inputs * 16 or set(steps) != {(i, o) for o in range(16) for i in range(inputs)}:
        raise ValueError(f"incomplete or duplicated table metadata for {name}")
    result = np.empty((16, inputs, domain), dtype="<f4")
    with np.load(root / f"{name}.npz", allow_pickle=False) as arrays:
        if set(arrays.files) != {f"i{i}o{o}" for o in range(16) for i in range(inputs)}:
            raise ValueError(f"unexpected table members for {name}")
        for out in range(16):
            for inp in range(inputs):
                step = steps[inp, out]
                raw = arrays[f"i{inp}o{out}"]
                if step < 1 or step & (step - 1) or step > domain or raw.ndim != 1 or not np.issubdtype(raw.dtype, np.integer):
                    raise ValueError(f"invalid interpolation data for {name}")
                required = domain if step == 1 else (domain - 1) // step + 2
                if len(raw) < required or np.any(np.abs(raw.astype(np.int64)) > 32767):
                    raise ValueError(f"invalid interpolation bounds for {name}")
                # Upstream Query materializes every integer decision, including
                # sample boundaries and their immediate neighbors.
                values = torch.from_numpy(raw.astype(np.int32))
                coordinates = torch.arange(domain, dtype=torch.int32)
                result[out, inp] = reference.Query(values, coordinates, step).float().numpy()
    return result.reshape(-1)


def _table_specs():
    yield "DW0_LSB", 4, 9
    for stage in range(8):
        yield f"DW{stage}_MSB", 64, 9
        yield f"PW{stage}_MSB", 64, 16
    yield "UP_MSB", 64, 16


def _prepare(reference, root: Path, output: Path) -> None:
    parts = [_table(reference, root, name, domain, inputs) for name, domain, inputs in _table_specs()]
    offsets = np.load(root / "offset.npy", allow_pickle=False)
    if offsets.shape != (8, 2, 16) or np.any(np.abs(offsets) > 1) or np.any(offsets != np.trunc(offsets)):
        raise ValueError("unexpected ShiftLUT offsets")
    parts.append(offsets.astype("<f4").reshape(-1))
    np.concatenate(parts).astype("<f4", copy=False).tofile(output)


def _probes():
    generator = torch.Generator().manual_seed(0x5A17)
    for height, width in [(1, 1), (1, 7), (3, 1), (3, 5), (17, 19), (32, 32), (256, 256)]:
        for name in ("constant", "boundary", "random", "changed"):
            if name == "constant":
                image = torch.full((1, 3, height, width), 128.0)
            elif name == "boundary":
                image = (torch.arange(3 * height * width).reshape(1, 3, height, width) % 256).float()
            else:
                image = torch.randint(0, 256, (1, 3, height, width), generator=generator).float()
            yield f"{height}x{width}_{name}", image
            if name == "boundary" and height < 256:
                yield f"{height}x{width}_below", torch.nextafter(image, torch.full_like(image, float("-inf"))).clamp(0, 255)
                yield f"{height}x{width}_above", torch.nextafter(image, torch.full_like(image, float("inf"))).clamp(0, 255)


def _write_tile_oracles(reference, model, output: Path) -> None:
    means = np.array([0.485, 0.456, 0.406], dtype=np.float32).reshape(3, 1, 1)
    deviations = np.array([0.229, 0.224, 0.225], dtype=np.float32).reshape(3, 1, 1)
    for width, height in [(1, 1), (5, 3), (197, 193)]:
        for pattern in range(2):
            y, x = np.indices((height, width), dtype=np.uint32)
            source = np.stack(((x * 7 + y * 3 + pattern * 73) % 256,
                               ((x ^ y) + pattern * 41) % 256,
                               (x * y + pattern * 109) % 256,
                               (x * 11 + y * 19) % 256), axis=-1).astype(np.uint8)
            rgb = source[:, :, :3].transpose(2, 0, 1).astype(np.float64)
            numerator = (rgb * np.float64(np.float32(1.0 / 255.0)) - means.astype(np.float64)).astype(np.float32)
            normalized = numerator / deviations
            # Float64 exactly holds these FP32 products and sums before the
            # single FP32 rounding of CUDA fmaf at the input boundary.
            model_rgb = (normalized.astype(np.float64) * deviations.astype(np.float64) +
                         means.astype(np.float64)).astype(np.float32).clip(0, 1) * np.float32(255)
            restored = np.empty((height * 4, width * 4, 4), dtype=np.uint8)
            for top in range(0, height, 192):
                for left in range(0, width, 192):
                    axes = []
                    for origin, extent in ((top, height), (left, width)):
                        if extent == 1:
                            axes.append(np.zeros(256, dtype=np.int64))
                        else:
                            coordinate = (np.arange(256) + origin - 32) % (2 * (extent - 1))
                            axes.append(np.minimum(coordinate, 2 * (extent - 1) - coordinate))
                    tile = model_rgb[:, axes[0][:, None], axes[1][None, :]].copy()
                    with torch.inference_mode():
                        expected = reference.inference(model, torch.from_numpy(tile[None]).cuda() - 128).clamp(0, 255)
                    core_h, core_w = min(192, height - top) * 4, min(192, width - left) * 4
                    core = expected[0, :, 128:128 + core_h, 128:128 + core_w].cpu().numpy()
                    rgba = restored[top * 4:top * 4 + core_h, left * 4:left * 4 + core_w]
                    rgba[:, :, :3] = np.rint(core.transpose(1, 2, 0) * np.float32(1.0 / 255.0) * np.float32(255)).astype(np.uint8)
                    rgba[:, :, 3] = 255
            source.tofile(output / f"source_{width}_{height}_{pattern}.rgba")
            restored.tofile(output / f"expected_{width}_{height}_{pattern}.rgba")


def _verify(reference, root: Path, model_path: Path, generator: Path, output: Path, probes=None, tiled=True) -> None:
    if not torch.cuda.is_available():
        raise RuntimeError("ShiftLUT verification requires a GPU")
    expected_model = reference.LUT(str(root), stack=7, scale=4, DW_split_level=1).cuda().eval()
    if tiled:
        _write_tile_oracles(reference, expected_model, output)
    vectors = []
    for label, image in (_probes() if probes is None else probes):
        decisions = [[] for _ in range(16)]
        handles = []
        if image.shape[2] * image.shape[3] <= 1024:
            for index, operation in enumerate(expected_model.msb[1:]):
                def capture(module, arguments, index=index):
                    decisions[index].append((arguments[0] - 32).to(torch.int8).cpu().numpy().reshape(-1))
                handles.append(operation.register_forward_pre_hook(capture))
        try:
            with torch.inference_mode():
                expected = reference.inference(expected_model, image.cuda() - 128).clamp(0, 255).cpu()
        finally:
            for handle in handles:
                handle.remove()
        if handles:
            if any(len(branches) != 4 for branches in decisions):
                raise RuntimeError("unexpected upstream stage/rotation execution")
            np.concatenate([np.concatenate(branches) for branches in decisions]).tofile(output / f"{label}.decisions.i8")
        image.numpy().astype("<f4").tofile(output / f"{label}.input.f32")
        expected.numpy().astype("<f4").tofile(output / f"{label}.expected.f32")
        vectors.append(f"{label} {image.shape[2]} {image.shape[3]}")
    (output / "vectors.txt").write_text("\n".join(vectors) + "\n")
    del expected_model
    torch.cuda.empty_cache()
    # The installed pinned C++ ORT library is authoritative. No Python ORT
    # wheel, alternate evaluator, or duplicate Python graph is introduced.
    subprocess.run([str(generator), "--verify", str(model_path), str(output),
                    _asset_digest(root), _SOURCE_SHA256], check=True)


def _verify_rounding(reference, generator: Path, output: Path) -> None:
    # Synthetic interpolation tables run through the unmodified upstream LUT
    # implementation. Half-integer averages exercise both ties and both signs
    # before saturation, for depthwise and pointwise stages independently.
    for active in ("DW0_MSB", "PW0_MSB"):
        for sign in (-1, 1):
            case = output / f"ties_{active}_{sign}"
            root = case / "tables"
            root.mkdir(parents=True, exist_ok=True)
            np.save(root / "offset.npy", np.zeros((8, 2, 16), dtype=np.int32))
            for name, domain, inputs in _table_specs():
                step = 2 if name == active else 1
                values = sign * np.arange(domain // 2 + 1, dtype=np.int32) if step == 2 else np.zeros(domain, dtype=np.int32)
                np.savez(root / f"{name}.npz", **{f"i{i}o{o}": values for o in range(16) for i in range(inputs)})
                (root / f"{name}.json").write_text(json.dumps({
                    "IN": inputs, "OUT": 16,
                    "LUTs": [{"in": i, "out": o, "step": step} for o in range(16) for i in range(inputs)],
                }))
            interchange = case / "tables.f32"
            _prepare(reference, root, interchange)
            model = case / "synthetic.onnx"
            subprocess.run([str(generator), str(interchange), str(model), _asset_digest(root), _SOURCE_SHA256], check=True)
            probes = [(f"tie_{value}", torch.full((1, 3, 3, 5), float(value))) for value in (116, 124, 132, 140)]
            _verify(reference, root, model, generator, case, probes=probes, tiled=False)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--generator", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--validation", type=Path, required=True)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--export-only", action="store_true")
    modes.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    reference, source_digest = _reference(args.source)
    root = args.source / "LUT_test/LUTs/ShiftLUT_sr_s7_int"
    lut_digest = _asset_digest(root)
    args.validation.mkdir(parents=True, exist_ok=True)
    if args.export_only:
        interchange = args.validation / "tables.f32"
        _prepare(reference, root, interchange)
        temporary = args.validation / "ShiftLUT_fp32.onnx"
        subprocess.run([str(args.generator), str(interchange), str(temporary), lut_digest, source_digest], check=True)
        data = temporary.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        descriptor = Path(__file__).with_name("image_upscaler_runtime.cpp")
        text, count = re.subn(r'(\.filename = "ShiftLUT_fp32.onnx",.*?\.sha256 = ")[a-f0-9]{64}(")',
                             lambda match: match[1] + digest + match[2], descriptor.read_text(), count=1, flags=re.S)
        if count != 1:
            raise RuntimeError("missing authoritative ShiftLUT descriptor digest")
        args.output.write_bytes(data)
        descriptor.write_text(text)
    else:
        # Verification never writes the production asset or its descriptor.
        _verify(reference, root, args.output, args.generator, args.validation)
        _verify_rounding(reference, args.generator, args.validation)
    (args.validation / "source_manifest.json").write_text(json.dumps({
        "source_sha256": source_digest, "lut_sha256": lut_digest,
        "model_sha256": hashlib.sha256(args.output.read_bytes()).hexdigest(),
    }, indent=2) + "\n")


if __name__ == "__main__":
    main()
