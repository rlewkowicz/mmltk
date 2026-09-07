#!/usr/bin/env python3
"""Export ShiftLUT's converted s7 lookup-table evaluator to ONNX."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
from typing import Final

import numpy as np
import torch
import torch.nn.functional as F
from torch import Tensor, nn


_MSB_DOMAIN: Final = 64
_LSB_DOMAIN: Final = 4
_CHANNELS: Final = 16
_STACKS: Final = 8
_SCALE: Final = 4
_DEFAULT_EXTENT: Final = 256


def _load_python_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load Python module {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _expanded_table(path: Path, domain: int) -> Tensor:
    with path.with_suffix(".json").open("r", encoding="utf-8") as stream:
        metadata = json.load(stream)
    arrays = np.load(path.with_suffix(".npz"))
    inputs = int(metadata["IN"])
    outputs = int(metadata["OUT"])
    steps = {
        (int(entry["in"]), int(entry["out"])): int(entry["step"])
        for entry in metadata["LUTs"]
    }
    dense = np.empty((outputs, inputs, domain), dtype=np.float32)
    coordinates = np.arange(domain, dtype=np.int64)
    for output in range(outputs):
        for input_channel in range(inputs):
            step = steps[(input_channel, output)]
            values = arrays[f"i{input_channel}o{output}"].astype(np.float32, copy=False)
            lower = coordinates // step
            upper = lower + 1
            fraction = (coordinates % step).astype(np.float32) / np.float32(step)
            if step == 1:
                dense[output, input_channel] = values[coordinates]
            else:
                dense[output, input_channel] = (
                    values[lower] * (np.float32(1.0) - fraction) + values[upper] * fraction
                )
    return torch.from_numpy(dense)


def _shift_kernel(offsets: np.ndarray) -> Tensor:
    if offsets.shape != (2, _CHANNELS):
        raise ValueError(f"expected one 2x{_CHANNELS} ShiftLUT offset matrix, received {offsets.shape}")
    kernel = torch.zeros((_CHANNELS, 1, 3, 3), dtype=torch.float32)
    for channel in range(_CHANNELS):
        dy = int(offsets[0, channel])
        dx = int(offsets[1, channel])
        if abs(dy) > 1 or abs(dx) > 1:
            raise ValueError(f"ShiftLUT offset ({dy}, {dx}) exceeds the supported 3x3 shift window")
        kernel[channel, 0, 1 + dy, 1 + dx] = 1.0
    return kernel


class _DenseDepthwiseLookup(nn.Module):
    def __init__(self, table: Tensor, input_channels: int) -> None:
        super().__init__()
        if table.ndim != 3 or table.shape[1] != 9:
            raise ValueError(f"depthwise table must have shape [output,9,value], received {tuple(table.shape)}")
        if input_channels not in (1, int(table.shape[0])):
            raise ValueError("depthwise input channels must be one or match the table outputs")
        self.input_channels = input_channels
        self.output_channels = int(table.shape[0])
        self.register_buffer("table", table.reshape(-1, table.shape[-1]).contiguous())

    def forward(self, image: Tensor) -> Tensor:
        batch, channels, height, width = image.shape
        padded = F.pad(image, (1, 1, 1, 1), mode="replicate")
        patches = torch.stack(
            [
                padded[:, :, y : y + height, x : x + width]
                for y in range(3)
                for x in range(3)
            ],
            dim=2,
        )
        if self.input_channels == 1:
            patches = patches.expand(batch, self.output_channels, 9, height, width)
        elif channels != self.output_channels:
            raise ValueError("ShiftLUT depthwise channel count changed")
        indices = patches.reshape(batch, self.output_channels * 9, height * width).to(torch.int64)
        values = torch.gather(self.table.unsqueeze(0).expand(batch, -1, -1), 2, indices)
        return torch.round(
            values.reshape(batch, self.output_channels, 9, height * width).sum(dim=2) / 9.0
        ).reshape(batch, self.output_channels, height, width)


class _DensePointwiseLookup(nn.Module):
    def __init__(self, table: Tensor) -> None:
        super().__init__()
        if table.ndim != 3 or table.shape[0] != _CHANNELS or table.shape[1] != _CHANNELS:
            raise ValueError(
                f"pointwise table must have shape [{_CHANNELS},{_CHANNELS},value], received {tuple(table.shape)}"
            )
        self.register_buffer("table", table.reshape(-1, table.shape[-1]).contiguous())

    def forward(self, image: Tensor) -> Tensor:
        batch, channels, height, width = image.shape
        if channels != _CHANNELS:
            raise ValueError("ShiftLUT pointwise channel count changed")
        indices = (
            image.unsqueeze(1)
            .expand(batch, _CHANNELS, _CHANNELS, height, width)
            .reshape(batch, _CHANNELS * _CHANNELS, height * width)
            .to(torch.int64)
        )
        values = torch.gather(self.table.unsqueeze(0).expand(batch, -1, -1), 2, indices)
        return torch.round(
            values.reshape(batch, _CHANNELS, _CHANNELS, height * width).sum(dim=2) / 16.0
        ).reshape(batch, _CHANNELS, height, width)


class _ShiftLutS7(nn.Module):
    def __init__(self, lut_root: Path) -> None:
        super().__init__()
        offsets = np.load(lut_root / "offset.npy")
        if offsets.shape != (_STACKS, 2, _CHANNELS):
            raise ValueError(f"expected ShiftLUT offsets with shape (8,2,16), received {offsets.shape}")
        self.lsb = _DenseDepthwiseLookup(_expanded_table(lut_root / "DW0_LSB", _LSB_DOMAIN), 1)
        self.msb_depthwise = nn.ModuleList(
            [
                _DenseDepthwiseLookup(
                    _expanded_table(lut_root / f"DW{index}_MSB", _MSB_DOMAIN),
                    1 if index == 0 else _CHANNELS,
                )
                for index in range(_STACKS)
            ]
        )
        self.msb_pointwise = nn.ModuleList(
            [
                _DensePointwiseLookup(_expanded_table(lut_root / f"PW{index}_MSB", _MSB_DOMAIN))
                for index in range(_STACKS)
            ]
        )
        self.shift_kernels = nn.ParameterList(
            [nn.Parameter(_shift_kernel(offsets[index]), requires_grad=False) for index in range(_STACKS)]
        )
        self.up = _DensePointwiseLookup(_expanded_table(lut_root / "UP_MSB", _MSB_DOMAIN))

    def forward(self, centered: Tensor) -> Tensor:
        low = torch.trunc(torch.remainder(centered, 4.0))
        high = torch.floor(centered / 4.0)
        for index in range(_STACKS):
            high = self.msb_depthwise[index](high + 32.0) + high
            if index == 0:
                low = torch.clamp(self.lsb(low) + low, 0.0, 3.0)
                high = high + low
            high = torch.clamp(high, -32.0, 31.0)
            high = F.conv2d(
                F.pad(high, (1, 1, 1, 1), mode="replicate"),
                self.shift_kernels[index],
                groups=_CHANNELS,
            )
            high = torch.clamp(self.msb_pointwise[index](high + 32.0) + high, -32.0, 31.0)
        restored = torch.clamp(self.up(high + 32.0) + high, -128.0, 127.0)
        return F.pixel_shuffle(restored, _SCALE)


class ShiftLutS7Inference(nn.Module):
    """The final `LUT_test/sr/model_LUT.py::inference` image contract."""

    def __init__(self, lut_root: Path) -> None:
        super().__init__()
        self.lut = _ShiftLutS7(lut_root)

    def forward(self, image: Tensor) -> Tensor:
        batch, channels, height, width = image.shape
        centered = torch.clamp(image - 128.0, -128.0, 127.0).reshape(
            batch * channels, 1, height, width
        )
        rotated = torch.cat(
            [
                centered,
                torch.rot90(centered, 1, (2, 3)),
                torch.rot90(centered, 2, (2, 3)),
                torch.rot90(centered, 3, (2, 3)),
            ],
            dim=0,
        )
        restored = self.lut(rotated)
        rotation_batch = batch * channels
        first, second, third, fourth = torch.split(restored, rotation_batch, dim=0)
        accumulated = (
            torch.clamp(first, -128.0, 127.0)
            + torch.clamp(torch.rot90(second, 3, (2, 3)), -128.0, 127.0)
            + torch.clamp(torch.rot90(third, 2, (2, 3)), -128.0, 127.0)
            + torch.clamp(torch.rot90(fourth, 1, (2, 3)), -128.0, 127.0)
        )
        return torch.clamp(
            accumulated.reshape(batch, channels, height * _SCALE, width * _SCALE) + 128.0,
            0.0,
            255.0,
        )


def _probe_inputs(extent: int) -> dict[str, Tensor]:
    generator = torch.Generator().manual_seed(0x5A17)
    gradient = torch.linspace(0.0, 255.0, extent).reshape(1, 1, 1, extent)
    edge = torch.zeros((1, 3, extent, extent), dtype=torch.float32)
    edge[:, :, :, extent // 2 :] = 255.0
    return {
        "constant": torch.full((1, 3, extent, extent), 128.0),
        "edge": edge,
        "gradient": gradient.expand(1, 3, extent, extent).contiguous(),
        "seeded": torch.randint(
            0,
            256,
            (1, 3, extent, extent),
            generator=generator,
            dtype=torch.int64,
        ).to(torch.float32),
    }


def _verify_reference(model: ShiftLutS7Inference, shiftlut_root: Path, extent: int) -> None:
    reference_code = _load_python_module(
        "shiftlut_reference_lut", shiftlut_root / "LUT_test/sr/model_LUT.py"
    )
    reference = reference_code.LUT(
        str(shiftlut_root / "LUT_test/LUTs/ShiftLUT_sr_s7_int"),
        stack=7,
        scale=4,
        DW_split_level=1,
    ).eval()
    with torch.inference_mode():
        for label, image in _probe_inputs(extent).items():
            expected = reference_code.inference(reference, image - 128.0).clamp(0.0, 255.0)
            actual = model(image)
            if not torch.equal(expected, actual):
                delta = torch.abs(expected - actual)
                raise RuntimeError(
                    f"{label} vectorized ShiftLUT mismatch: max={delta.max().item()} "
                    f"mean={delta.mean().item()}"
                )


def _asset_digest(lut_root: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(lut_root.iterdir(), key=lambda candidate: candidate.name):
        if not path.is_file():
            continue
        digest.update(path.name.encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
    return digest.hexdigest()


def _annotate_onnx(path: Path, lut_root: Path) -> None:
    import onnx

    model = onnx.load(path, load_external_data=True)
    del model.metadata_props[:]
    metadata = {
        "mmltk.input_range": "0..255 RGB FP32",
        "mmltk.output_range": "0..255 RGB FP32",
        "mmltk.precision": "strict-fp32",
        "mmltk.source_artifact": "LUT_test/LUTs/ShiftLUT_sr_s7_int",
        "mmltk.source_assets_sha256": _asset_digest(lut_root),
        "mmltk.source_inference": "LUT_test/sr/model_LUT.py::inference",
    }
    for key, value in metadata.items():
        entry = model.metadata_props.add()
        entry.key = key
        entry.value = value
    onnx.checker.check_model(model)
    onnx.save(model, path, save_as_external_data=False)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shiftlut-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--extent", type=int, default=_DEFAULT_EXTENT)
    parser.add_argument("--opset", type=int, default=18)
    parser.add_argument(
        "--export-only",
        action="store_true",
        help="skip reference probes; intended only while assembling a multi-phase change",
    )
    return parser.parse_args()


def main() -> None:
    args = _parse_args()
    shiftlut_root = args.shiftlut_root.resolve()
    lut_root = shiftlut_root / "LUT_test/LUTs/ShiftLUT_sr_s7_int"
    if args.extent <= 0:
        raise ValueError("--extent must be positive")
    if not lut_root.is_dir():
        raise FileNotFoundError(f"missing ShiftLUT s7 assets at {lut_root}")

    model = ShiftLutS7Inference(lut_root).eval()
    if not args.export_only:
        _verify_reference(model, shiftlut_root, min(args.extent, 16))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    sample = torch.zeros((1, 3, args.extent, args.extent), dtype=torch.float32)
    torch.onnx.export(
        model,
        (sample,),
        args.output,
        input_names=["image"],
        output_names=["upscaled"],
        opset_version=args.opset,
        dynamo=True,
        optimize=True,
        verify=False,
        external_data=False,
    )
    _annotate_onnx(args.output, lut_root)
    print(f"exported {args.output} ({args.output.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
