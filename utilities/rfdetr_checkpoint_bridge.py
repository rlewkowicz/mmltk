#!/usr/bin/env python3

import argparse
import json
from collections import OrderedDict
from pathlib import Path

import numpy as np
import torch


DTYPE_TO_NUMPY = {
    "float16": np.float16,
    "bfloat16": np.uint16,
    "float32": np.float32,
    "float64": np.float64,
    "bool": np.bool_,
    "uint8": np.uint8,
    "int8": np.int8,
    "int16": np.int16,
    "int32": np.int32,
    "int64": np.int64,
}

DTYPE_TO_TORCH = {
    "float16": torch.float16,
    "bfloat16": torch.bfloat16,
    "float32": torch.float32,
    "float64": torch.float64,
    "bool": torch.bool,
    "uint8": torch.uint8,
    "int8": torch.int8,
    "int16": torch.int16,
    "int32": torch.int32,
    "int64": torch.int64,
}


def _tensor_dtype_name(tensor: torch.Tensor) -> str:
    for name, dtype in DTYPE_TO_TORCH.items():
        if tensor.dtype == dtype:
            return name
    raise RuntimeError(f"unsupported RF-DETR checkpoint tensor dtype: {tensor.dtype}")


def _load_checkpoint_root(checkpoint_path: Path):
    checkpoint = torch.load(str(checkpoint_path), map_location="cpu", weights_only=False)
    if not isinstance(checkpoint, dict):
        state_dict_fn = getattr(checkpoint, "state_dict", None)
        if callable(state_dict_fn):
            model = state_dict_fn()
            if isinstance(model, dict):
                return {"model": model}
        raise RuntimeError(
            "RF-DETR checkpoint root is neither a dict nor a module with state_dict(): "
            f"{checkpoint_path} ({type(checkpoint).__name__})"
        )
    model = checkpoint.get("model")
    if isinstance(model, dict):
        return checkpoint
    state_dict = checkpoint.get("state_dict")
    if isinstance(state_dict, dict):
        return {"model": state_dict}
    if checkpoint and all(isinstance(name, str) and isinstance(tensor, torch.Tensor) for name, tensor in checkpoint.items()):
        return {"model": checkpoint}
    raise RuntimeError(f"RF-DETR checkpoint is missing dict['model'] or dict['state_dict']: {checkpoint_path}")


def _checkpoint_arg_value(args_obj, name: str):
    if args_obj is None:
        return None
    if isinstance(args_obj, dict):
        return args_obj.get(name)
    return getattr(args_obj, name, None)


def _coerce_bool(value):
    if isinstance(value, (bool, np.bool_)):
        return bool(value)
    return None


def _coerce_int(value):
    if isinstance(value, (bool, np.bool_)):
        return None
    if isinstance(value, (int, np.integer)):
        return int(value)
    if isinstance(value, (float, np.floating)) and float(value).is_integer():
        return int(value)
    return None


def _coerce_float(value):
    if isinstance(value, (bool, np.bool_)):
        return None
    if isinstance(value, (int, float, np.integer, np.floating)):
        return float(value)
    return None


def _extract_checkpoint_metadata(checkpoint) -> dict:
    args_obj = checkpoint.get("args")
    if args_obj is None:
        return {}

    metadata = {}
    field_specs = {
        "sum_group_losses": _coerce_bool,
        "use_varifocal_loss": _coerce_bool,
        "use_position_supervised_loss": _coerce_bool,
        "ia_bce_loss": _coerce_bool,
        "aux_loss": _coerce_bool,
        "mask_point_sample_ratio": _coerce_int,
        "focal_alpha": _coerce_float,
        "cls_loss_coef": _coerce_float,
        "bbox_loss_coef": _coerce_float,
        "giou_loss_coef": _coerce_float,
        "mask_ce_loss_coef": _coerce_float,
        "mask_dice_loss_coef": _coerce_float,
        "set_cost_class": _coerce_float,
        "set_cost_bbox": _coerce_float,
        "set_cost_giou": _coerce_float,
    }
    for name, coercer in field_specs.items():
        value = coercer(_checkpoint_arg_value(args_obj, name))
        if value is not None:
            metadata[name] = value
    return metadata


def export_upstream(args: argparse.Namespace) -> None:
    checkpoint = _load_checkpoint_root(Path(args.input))
    model = checkpoint["model"]
    tensor_dir = Path(args.tensor_dir)
    tensor_dir.mkdir(parents=True, exist_ok=True)

    state_dict = []
    for index, (name, tensor) in enumerate(model.items()):
        if not isinstance(tensor, torch.Tensor):
            raise RuntimeError(f"RF-DETR checkpoint entry is not a tensor: {name}")
        prepared = tensor.detach().cpu().contiguous()
        tensor_path = tensor_dir / f"entry_{index:06d}.bin"
        dtype_name = _tensor_dtype_name(prepared)
        if dtype_name == "bfloat16":
            prepared.view(torch.uint16).numpy().tofile(tensor_path)
        else:
            prepared.numpy().tofile(tensor_path)
        state_dict.append(
            {
                "name": str(name),
                "tensor_path": str(tensor_path.relative_to(Path(args.manifest).parent)),
                "dtype": dtype_name,
                "sizes": list(prepared.shape),
            }
        )

    if not state_dict:
        raise RuntimeError(f"RF-DETR checkpoint state_dict is empty: {args.input}")

    manifest = {"state_dict": state_dict}
    metadata = _extract_checkpoint_metadata(checkpoint)
    if metadata:
        manifest["metadata"] = metadata
    manifest_path = Path(args.manifest)
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def write_upstream(args: argparse.Namespace) -> None:
    manifest_path = Path(args.manifest)
    payload = json.loads(manifest_path.read_text(encoding="utf-8"))
    entries = payload.get("state_dict")
    if not isinstance(entries, list) or not entries:
        raise RuntimeError(f"RF-DETR checkpoint manifest is missing state_dict entries: {manifest_path}")

    model = OrderedDict()
    for entry in entries:
        if not isinstance(entry, dict):
            raise RuntimeError(f"RF-DETR checkpoint manifest entry is not an object: {manifest_path}")
        name = entry.get("name")
        tensor_path = entry.get("tensor_path")
        dtype_name = entry.get("dtype")
        sizes = entry.get("sizes")
        if (
            not isinstance(name, str)
            or not isinstance(tensor_path, str)
            or not isinstance(dtype_name, str)
            or not isinstance(sizes, list)
        ):
            raise RuntimeError(
                f"RF-DETR checkpoint manifest entry is missing name/tensor_path/dtype/sizes: {manifest_path}"
            )
        if dtype_name not in DTYPE_TO_NUMPY or dtype_name not in DTYPE_TO_TORCH:
            raise RuntimeError(f"unsupported RF-DETR checkpoint tensor dtype: {dtype_name}")
        np_array = np.fromfile((manifest_path.parent / tensor_path).resolve(), dtype=DTYPE_TO_NUMPY[dtype_name])
        expected_shape = tuple(int(size) for size in sizes)
        if dtype_name == "bfloat16":
            tensor = torch.from_numpy(np_array.reshape(expected_shape)).view(torch.bfloat16).clone()
        else:
            tensor = torch.from_numpy(np_array.reshape(expected_shape)).to(DTYPE_TO_TORCH[dtype_name]).clone()
        model[name] = tensor.cpu().contiguous()

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    torch.save({"model": model}, str(output_path))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="RF-DETR upstream checkpoint bridge")
    subparsers = parser.add_subparsers(dest="command", required=True)

    export_parser = subparsers.add_parser("export-upstream")
    export_parser.add_argument("--input", required=True)
    export_parser.add_argument("--manifest", required=True)
    export_parser.add_argument("--tensor-dir", required=True)
    export_parser.set_defaults(func=export_upstream)

    write_parser = subparsers.add_parser("write-upstream")
    write_parser.add_argument("--output", required=True)
    write_parser.add_argument("--manifest", required=True)
    write_parser.set_defaults(func=write_upstream)

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    args.func(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
