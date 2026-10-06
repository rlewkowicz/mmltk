#!/usr/bin/env python3
"""Bounded independent RF-DETR evidence. Execute only through ./mmltk.

This imports the read-only pinned source. Archives contain ordinary tensors,
not traced/scripted models. Stock AMP and the explicitly controlled FP32 matcher
are separate executions; neither is a Match-Free oracle or a dataset AP result.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager, nullcontext
import hashlib
import importlib.metadata as metadata
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

REVISION = "e9a138f70cc14e3fddca029a0683055e34ab8f35"
FORMAT = "mmltk.rfdetr.upstream-oracle.v1"
ROOT = Path("/opt/rfdetr-oracle")
MAX_MANIFEST = 32 * 1024 * 1024
MAX_ARCHIVE = 4 * 1024**3


def digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def json_write(path, value):
    data = json.dumps(value, sort_keys=True, indent=2, allow_nan=False).encode()
    if len(data) > MAX_MANIFEST:
        raise RuntimeError("oracle manifest exceeds its declared bound")
    temporary = path.with_suffix(path.suffix + ".partial")
    with temporary.open("wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)


def payload():
    import torch
    import torchvision
    names = {"torch", "torchvision", "triton"}
    names.update(distribution.metadata["Name"] for distribution in metadata.distributions()
                 if distribution.metadata["Name"].lower().startswith("nvidia-"))
    return {"distributions": {name: metadata.version(name) for name in names},
            "torch": torch.__version__, "torchvision": torchvision.__version__,
            "cuda": torch.version.cuda}


def check_environment():
    current = payload()
    expected = json.loads((ROOT / "payload.json").read_text())
    if current != expected:
        raise RuntimeError(f"authoritative NGC payload replaced: {current!r} != {expected!r}")
    manifest = json.loads((ROOT / "nvidia-payload.json").read_text())
    if not current["torch"].startswith(manifest["versions"]["torch"]):
        raise RuntimeError("oracle Torch does not match docker/nvidia-payload.json")
    from packaging.requirements import Requirement
    from packaging.utils import canonicalize_name
    additions = []
    for line in (ROOT / "requirements.txt").read_text().splitlines():
        if line and not line.startswith("#"):
            name, version = line.split("==")
            if metadata.version(name) != version:
                raise RuntimeError(f"incompatible oracle dependency {name}: expected {version}")
            additions.append(canonicalize_name(name))
    # Check the selected genuine import closure without asking pip to resolve or
    # modify it. Donor Torch/vision/Triton/CUDA stay an indivisible authority.
    authoritative = {canonicalize_name(name) for name in current["distributions"]}
    pending = [(name, "") for name in additions] + [("fsspec", "http"), ("torchmetrics", "detection"), ("jsonargparse", "signatures")]
    seen = set()
    while pending:
        name, extra = pending.pop()
        if (name, extra) in seen or name in authoritative:
            continue
        seen.add((name, extra))
        for declaration in metadata.requires(name) or []:
            requirement = Requirement(declaration)
            if requirement.marker and not requirement.marker.evaluate({"extra": extra}):
                continue
            version = metadata.version(requirement.name)
            if not requirement.specifier.contains(version, prereleases=True):
                raise RuntimeError(f"incompatible dependency closure: {name}[{extra}] requires {requirement}, installed {version}")
            dependency = canonicalize_name(requirement.name)
            pending.extend((dependency, selected) for selected in {"", *requirement.extras})
    # Genuine imports exercise the numerical and Lightning dependency closure.
    import transformers  # noqa: F401
    import pytorch_lightning  # noqa: F401
    import torchmetrics  # noqa: F401
    import torch_linear_assignment  # noqa: F401
    current["dependency_closure"] = {name: metadata.version(name) for name, _ in sorted(seen)}
    return current


def verify_source(source):
    def git(*args):
        return subprocess.check_output(["git", "-c", f"safe.directory={source}", "-C", str(source), *args], text=True).strip()
    if git("rev-parse", "HEAD") != REVISION or git("status", "--porcelain", "--untracked-files=all"):
        raise RuntimeError(f"upstream must be a clean checkout of {REVISION}")
    if git("diff", "--name-only", "HEAD"):
        raise RuntimeError("upstream source changed")
    return {str(path.relative_to(source)): digest(path) for path in sorted((source / "src/rfdetr").rglob("*")) if path.is_file()}


class Archive:
    """One bounded tensor shard; explicit inventory preserves undefined gradients."""
    def __init__(self):
        self.tensors = {}
        self.inventory = {}

    def add(self, name, value):
        import torch
        if name in self.inventory:
            raise RuntimeError(f"duplicate oracle tensor {name}")
        if value is None:
            self.inventory[name] = {"defined": False}
            return
        if not isinstance(value, torch.Tensor):
            raise TypeError(name)
        # torchvision's dataset transforms can return Image tensor subclasses;
        # the archive boundary owns only ordinary Tensor objects.
        detached = value.detach().as_subclass(torch.Tensor)
        tensor = detached.cpu().contiguous().clone()
        self.tensors[name] = tensor
        finite = torch.isfinite(tensor) if tensor.is_floating_point() else torch.ones_like(tensor, dtype=torch.bool)
        self.inventory[name] = {"defined": True, "shape": list(value.shape), "dtype": str(value.dtype),
                                "device": str(value.device), "stride": list(value.stride()),
                                "finite_count": int(finite.sum()), "elements": value.numel()}
        if tensor.is_floating_point():
            self.tensors["finite/" + name] = finite

    def write(self, directory, name):
        import torch
        path = directory / (name + ".pt")
        size = sum(value.numel() * value.element_size() for value in self.tensors.values())
        if size > MAX_ARCHIVE:
            raise RuntimeError(f"tensor shard {name} exceeds its bound")
        torch.save(self.tensors, path)
        if path.stat().st_size > MAX_ARCHIVE:
            raise RuntimeError(f"serialized tensor shard {name} exceeds its bound")
        return {"file": path.name, "sha256": digest(path), "bytes": path.stat().st_size,
                "tensors": self.inventory}


def output_tensors(outputs):
    layers = [("main", outputs)]
    layers += [(f"aux.{i}", layer) for i, layer in enumerate(outputs.get("aux_outputs", []))]
    if "enc_outputs" in outputs:
        layers.append(("enc", outputs["enc_outputs"]))
    for label, layer in layers:
        if set(layer) - {"pred_logits", "pred_boxes", "pred_masks", "aux_outputs", "enc_outputs"}:
            raise RuntimeError(f"unexpected upstream output inventory in {label}: {set(layer)}")
        for key in ("pred_logits", "pred_boxes", "pred_masks"):
            if key not in layer:
                continue
            if isinstance(layer[key], dict):
                for operand, value in layer[key].items():
                    yield f"{label}/{key}/{operand}", value
            else:
                yield f"{label}/{key}", layer[key]


class Observe:
    """Observe results without modifying equations, draws, or batching eligibility."""
    def __init__(self):
        self.indices = []
        self.encoder = []
        self.matcher = []
        self.uncertain = []
        self.direct = []

    def __enter__(self):
        import torch
        self.rand, self.topk, self.profile = torch.rand, torch.topk, sys.getprofile()
        def rand(*args, **kwargs):
            result = self.rand(*args, **kwargs)
            frame = sys._getframe(1)
            names = []
            while frame is not None:
                names.append((frame.f_code.co_name, frame.f_code.co_filename))
                frame = frame.f_back
            if any(name == "get_uncertain_point_coords_with_randomness" for name, _ in names):
                self.uncertain.append(result.detach().clone())
            elif any(path.endswith("/rfdetr/models/matcher.py") for _, path in names):
                self.matcher.append(result.detach().clone())
            return result
        def topk(*args, **kwargs):
            result = self.topk(*args, **kwargs)
            if sys._getframe(1).f_code.co_filename.endswith("/rfdetr/models/transformer.py"):
                self.encoder.append(result.indices.detach().clone())
            return result
        def profile(frame, event, result):
            if event == "return" and frame.f_code.co_filename.endswith("/rfdetr/models/criterion.py"):
                if frame.f_code.co_name == "forward" and "all_indices" in frame.f_locals:
                    self.indices = frame.f_locals["all_indices"]
                if frame.f_code.co_name == "loss_masks" and "point_logits" in frame.f_locals:
                    self.direct.append({name: frame.f_locals[name].detach().clone() for name in ("point_coords", "point_logits", "point_labels")})
            if self.profile is not None:
                self.profile(frame, event, result)
        try:
            torch.rand, torch.topk = rand, topk
            sys.setprofile(profile)
        except BaseException:
            torch.rand, torch.topk = self.rand, self.topk
            sys.setprofile(self.profile)
            raise
        return self

    def __exit__(self, *args):
        import torch
        torch.rand, torch.topk = self.rand, self.topk
        sys.setprofile(self.profile)

    def save(self, archive, prefix=""):
        for group, indices in enumerate(self.encoder):
            archive.add(prefix + f"encoder/{group}", indices)
        for layer, matches in enumerate(self.indices):
            for image, (query, target) in enumerate(matches):
                archive.add(prefix + f"assignment/{layer}/{image}/query", query)
                archive.add(prefix + f"assignment/{layer}/{image}/target", target)
        if self.matcher:
            if len(self.matcher) != len(self.indices) or len(self.uncertain) != 2 * len(self.indices) or len(self.direct) != len(self.indices):
                raise RuntimeError("upstream mask sampling inventory changed; cannot map explicit native samples")
            for layer, coordinates in enumerate(self.matcher):
                archive.add(prefix + f"samples/{layer}/matcher", coordinates)
                archive.add(prefix + f"samples/{layer}/candidates", self.uncertain[layer * 2])
                archive.add(prefix + f"samples/{layer}/remainder", self.uncertain[layer * 2 + 1])
                for key, value in self.direct[layer].items():
                    archive.add(prefix + f"direct/{layer}/{key}", value)


def normalized_input(batch, resolution, offset=0):
    import torch
    pixels = torch.arange(batch * 3 * resolution * resolution, dtype=torch.int64).reshape(batch, 3, resolution, resolution)
    pixels = ((pixels * 37 + pixels // resolution * 17 + offset) % 256).float() / 255
    return (pixels - torch.tensor([.485, .456, .406])[None, :, None, None]) / torch.tensor([.229, .224, .225])[None, :, None, None]


def targets_for(batch, resolution, segmentation):
    import torch
    targets = []
    for image in range(batch):
        count = 2 if image == 0 else 0  # mixed background at batch two
        value = {"labels": torch.tensor([1, 3][:count], dtype=torch.int64, device="cuda"),
                 "boxes": torch.tensor([[.31, .34, .22, .28], [.72, .65, .19, .24]][:count], dtype=torch.float32, device="cuda").reshape(count, 4)}
        if segmentation:
            masks = torch.zeros(count, resolution, resolution, dtype=torch.bool, device="cuda")
            for row in range(count):
                lo, hi = (resolution * (row + 1) // 5, resolution * (row + 2) // 5)
                masks[row, lo:hi, lo:hi + 3] = True
            value["masks"] = masks
        targets.append(value)
    return targets


def save_targets(archive, targets):
    for image, target in enumerate(targets):
        for name, value in target.items():
            archive.add(f"target/{image}/{name}", value)


def float_operands(value):
    """Explicit controlled matcher boundary, not a replacement matcher formula."""
    import torch
    if isinstance(value, torch.Tensor):
        return value.float() if value.is_floating_point() else value
    if isinstance(value, dict):
        return {key: float_operands(item) for key, item in value.items()}
    if isinstance(value, list):
        return [float_operands(item) for item in value]
    return value


@contextmanager
def fp32_matcher_costs():
    """Cast cost operands only; preserve original sampling and matcher dispatch."""
    import torch
    from rfdetr.models import matcher
    def promote(original, caller_only=False):
        def invoke(*args, **kwargs):
            if caller_only and not sys._getframe(1).f_code.co_filename.endswith("/rfdetr/models/matcher.py"):
                return original(*args, **kwargs)
            with torch.autocast("cuda", enabled=False):
                return original(*(float_operands(value) for value in args), **{key: float_operands(value) for key, value in kwargs.items()})
        return invoke
    operations = [(matcher.HungarianMatcher, "_focal_classification_cost"),
                  (matcher, "box_cxcywh_to_xyxy"), (matcher, "batch_sigmoid_ce_loss"),
                  (matcher, "batch_dice_loss"), (torch, "cdist")]
    originals = []
    try:
        for owner, name in operations:
            original = getattr(owner, name)
            originals.append((owner, name, original))
            setattr(owner, name, promote(original, owner is torch))
        yield
    finally:
        for owner, name, original in reversed(originals):
            setattr(owner, name, original)


def save_losses(archive, prefix, losses, weights):
    import torch
    # RFDETRModelModule.training_step's actual weighted reduction order.
    total = torch.stack([losses[key] * weights[key] for key in losses if key in weights]).sum()
    for key, value in losses.items():
        archive.add(prefix + ("loss/" if key in weights else "statistic/") + key, value)
    archive.add(prefix + "loss/total", total)
    return total


def stock_update_evidence(archive, optimizer, names, gradients, max_norm):
    """Actual torch.optim on the same initial AMP state; bounded to one update."""
    import torch
    names_by_id = {id(value): key for key, value in names.items()}
    shadow = {key: value.detach().clone().requires_grad_(True) for key, value in names.items()}
    groups = [{**{key: value for key, value in group.items() if key != "params"},
               "params": [shadow[names_by_id[id(value)]] for value in group["params"]]}
              for group in optimizer.param_groups]
    stock_optimizer = torch.optim.AdamW(groups, foreach=False, fused=False)
    for key, gradient in zip(names, gradients, strict=True):
        shadow[key].grad = None if gradient is None else gradient.detach().clone()
    archive.add("stock/clip_norm", torch.nn.utils.clip_grad_norm_(shadow.values(), max_norm, foreach=False, error_if_nonfinite=True))
    for key, parameter in shadow.items():
        archive.add("stock/clipped/" + key, parameter.grad)
    stock_optimizer.step()
    for key, parameter in shadow.items():
        archive.add("stock/updated/" + key, parameter)
        for field in ("step", "exp_avg", "exp_avg_sq"):
            archive.add(f"stock/optimizer/{key}/{field}", stock_optimizer.state.get(parameter, {}).get(field))


def run_case(directory, model, initial, mc, tc, defaults, preset, dtype_name, batch, training, steps):
    import torch
    from rfdetr.models.lwdetr import build_criterion_from_config
    from rfdetr.training.param_groups import get_param_dict
    from rfdetr._namespace import _namespace_from_configs
    from rfdetr.utilities.tensors import NestedTensor
    dtype = {"fp32": torch.float32, "fp16": torch.float16, "bf16": torch.bfloat16}[dtype_name]
    model.load_state_dict(initial, strict=True)
    model.train(training)
    criterion, _ = build_criterion_from_config(mc, tc, defaults)
    criterion.train(training)
    controlled, _ = build_criterion_from_config(mc, tc, defaults)
    controlled.train(training)
    names = dict(model.named_parameters())
    trainable = {key: parameter for key, parameter in names.items() if parameter.requires_grad}
    optimizer = torch.optim.AdamW(get_param_dict(_namespace_from_configs(mc, tc, defaults), model),
                                 lr=tc.lr, weight_decay=tc.weight_decay, foreach=False, fused=False)
    name_by_id = {id(value): key for key, value in names.items()}
    groups = [{"names": [name_by_id[id(value)] for value in group["params"]],
               "lr": group["lr"], "weight_decay": group["weight_decay"],
               "betas": list(group["betas"]), "eps": group["eps"], "amsgrad": group["amsgrad"]}
              for group in optimizer.param_groups]
    identity = f"{preset}-{dtype_name}-b{batch}-{'train' if training else 'eval'}"
    records = []
    for step in range(steps):
        archive = Archive()
        torch.manual_seed(731 + step)
        images = normalized_input(batch, mc.resolution, step * 11).cuda().requires_grad_(True)
        mask = torch.zeros(batch, mc.resolution, mc.resolution, dtype=torch.bool, device="cuda")
        targets = targets_for(batch, mc.resolution, mc.segmentation_head)
        save_targets(archive, targets)
        archive.add("input", images)
        archive.add("input_mask", mask)
        optimizer.zero_grad(set_to_none=True)
        with Observe() as forward_observation, torch.autocast("cuda", dtype=dtype, enabled=dtype != torch.float32):
            execution_policy = {"autocast_enabled": torch.is_autocast_enabled("cuda"), "autocast_dtype": str(torch.get_autocast_dtype("cuda"))}
            outputs = model(NestedTensor(images, mask))
        forward_observation.save(archive)
        raw_outputs = dict(output_tensors(outputs))
        for key, value in raw_outputs.items():
            archive.add("output/" + key, value)
        if step == 0:
            terms = []
            for key, value in raw_outputs.items():
                cotangent = (torch.arange(value.numel(), device=value.device, dtype=torch.float32).reshape(value.shape) % 19 - 9) / max(value.numel(), 1)
                cotangent = cotangent.to(value.dtype)
                archive.add("cotangent/" + key, cotangent)
                terms.append((value * cotangent).sum())
            vjps = torch.autograd.grad(sum(terms), [images, *trainable.values()], allow_unused=True, retain_graph=training)
            for key, value in zip(["input", *("parameter/" + key for key in trainable)], vjps, strict=True):
                archive.add("vjp/" + key, value)
        if training:
            rng = torch.cuda.get_rng_state()
            with Observe() as stock, torch.autocast("cuda", dtype=dtype, enabled=dtype != torch.float32):
                stock_losses = criterion(outputs, targets)
                stock_total = save_losses(archive, "stock/", stock_losses, criterion.weight_dict)
            stock.save(archive, "stock/")
            # Preserve stock losses/assignments/VJPs independently. The controlled
            # branch consumes the same initial RNG, targets and model tensors.
            stock_gradients = torch.autograd.grad(stock_total, list(trainable.values()), allow_unused=True, retain_graph=True)
            for key, value in zip(trainable, stock_gradients, strict=True):
                archive.add("stock/gradient/" + key, value)
            if dtype == torch.float32:
                # The multi-update FP32 trajectory follows untouched stock calls.
                observation, losses = stock, stock_losses
                total = save_losses(archive, "", losses, criterion.weight_dict)
            else:
                if steps != 1:
                    raise RuntimeError("AMP stock update evidence requires an identical initial state")
                stock_update_evidence(archive, optimizer, trainable, stock_gradients, tc.clip_max_norm)
                torch.cuda.set_rng_state(rng)
                with fp32_matcher_costs(), Observe() as observation, torch.autocast("cuda", dtype=dtype):
                    losses = controlled(outputs, targets)
                    total = save_losses(archive, "", losses, controlled.weight_dict)
            observation.save(archive)
            # K=1, no scaler: fixed scale one separates model/criterion and update
            # arithmetic from the independently covered dynamic-scaler owner.
            total.backward()
            for key, parameter in trainable.items():
                archive.add("gradient/" + key, parameter.grad)
            norm = torch.nn.utils.clip_grad_norm_(trainable.values(), tc.clip_max_norm, foreach=False, error_if_nonfinite=True)
            archive.add("clip_norm", norm)
            for key, parameter in trainable.items():
                archive.add("clipped/" + key, parameter.grad)
            optimizer.step()
            for key, parameter in trainable.items():
                archive.add("updated/" + key, parameter)
                state = optimizer.state.get(parameter, {})
                for field in ("step", "exp_avg", "exp_avg_sq"):
                    archive.add(f"optimizer/{key}/{field}", state.get(field))
            model.eval()
            held_criterion = criterion if dtype == torch.float32 else controlled
            held_criterion.eval()
            held = normalized_input(batch, mc.resolution, 97).cuda()
            archive.add("held/input", held)
            torch.manual_seed(1701)
            with torch.no_grad(), (nullcontext() if dtype == torch.float32 else fp32_matcher_costs()), Observe() as held_observation, torch.autocast("cuda", dtype=dtype, enabled=dtype != torch.float32):
                held_outputs = model(NestedTensor(held, mask))
                held_losses = held_criterion(held_outputs, targets)
                save_losses(archive, "held/", held_losses, held_criterion.weight_dict)
            held_observation.save(archive, "held/")
            for key, value in output_tensors(held_outputs):
                archive.add("held/output/" + key, value)
            model.train(True)
            held_criterion.train(True)
        record = archive.write(directory, f"{identity}-step{step}")
        record["step"] = step
        records.append(record)
        print(f"upstream oracle produced {identity} step={step}", flush=True)
    return {"id": identity, "preset": preset, "precision": dtype_name, "batch": batch,
            "execution_policy": execution_policy,
            "comparison_policy": "untouched_stock" if dtype == torch.float32 or not training else "upstream_with_fp32_matcher_boundary",
            "training": training, "resolution": mc.resolution, "groups": mc.group_detr,
            "classes": mc.num_classes + 1, "trainable": list(trainable), "parameter_groups": groups,
            "loss_weights": criterion.weight_dict, "clip_max_norm": tc.clip_max_norm,
            "optimizer": {"lr": tc.lr, "lr_encoder": tc.lr_encoder, "weight_decay": tc.weight_decay,
                          "lr_vit_layer_decay": tc.lr_vit_layer_decay, "lr_component_decay": tc.lr_component_decay},
            "steps": records}


def constructor_evidence(model):
    import torch
    state = model.state_dict()
    names = ["refpoint_embed.weight", "bbox_embed.layers.2.weight", "bbox_embed.layers.2.bias"]
    zero = {name: bool(torch.count_nonzero(state[name]) == 0) for name in names}
    copies = {}
    for group in range(model.group_detr):
        for suffix in ("weight", "bias"):
            copies[f"transformer.enc_out_class_embed.{group}.{suffix}"] = torch.equal(state[f"transformer.enc_out_class_embed.{group}.{suffix}"], state[f"class_embed.{suffix}"])
        for layer in range(3):
            for suffix in ("weight", "bias"):
                key = f"transformer.enc_out_bbox_embed.{group}.layers.{layer}.{suffix}"
                copies[key] = torch.equal(state[key], state[f"bbox_embed.layers.{layer}.{suffix}"])
    if not all(zero.values()) or not all(copies.values()):
        raise RuntimeError("pinned upstream constructor zero/copy invariants changed")
    return {"zero": zero, "copy": copies}


def resize_evidence(directory):
    import numpy as np
    import torch
    from PIL import Image
    from torchvision.transforms import InterpolationMode
    from torchvision.transforms.v2 import functional as tv, ToImage, ToDtype
    from rfdetr.datasets._torchvision import Resize
    from rfdetr.datasets.transforms import Normalize
    archive = Archive()
    cases = [("identity", 13, 17, 13, 17), ("enlarge", 7, 11, 19, 27),
             ("shrink", 37, 53, 11, 17), ("odd_aspect", 9, 41, 23, 13),
             ("high_frequency", 64, 65, 13, 17)]
    for name, height, width, out_height, out_width in cases:
        yy, xx = np.indices((height, width))
        rgb = np.stack([(xx * 31 + yy * 17) % 256, ((xx + yy) % 2) * 255, (xx * yy * 13) % 256], -1).astype(np.uint8)
        image = Image.fromarray(rgb, "RGB")
        source = tv.to_dtype(tv.to_image(image), torch.float32, scale=True)
        archive.add(name + "/rgb8", torch.from_numpy(rgb.copy()))
        archive.add(name + "/input", source)
        for antialias in (False, True):
            value = tv.resize(source, [out_height, out_width], interpolation=InterpolationMode.BILINEAR, antialias=antialias)
            archive.add(name + ("/torch_antialias" if antialias else "/torch"), value)
        pil, target = Resize((out_height, out_width))(image, None)
        pil_tensor = ToImage()(pil)
        pil_float = ToDtype(torch.float32, scale=True)(pil_tensor)
        normalized, _ = Normalize([.485, .456, .406], [.229, .224, .225])(pil_float, target)
        archive.add(name + "/pil_rgb8", pil_tensor)
        archive.add(name + "/pil", pil_float)
        archive.add(name + "/pil_normalized", normalized)
    return {"archive": archive.write(directory, "resize"), "cases": [list(case) for case in cases],
            "tensor_order": "RGB8 -> float32/255 -> bilinear align_corners=false (named antialias), no output quantization",
            "dataset_order": "RGB8 PIL -> upstream Resize bilinear antialias -> RGB8 quantization -> upstream ToImage/ToDtype(scale) -> Normalize"}


def generate(args):
    import torch
    source = Path(args.upstream).resolve()
    source_hashes = verify_source(source)
    environment = check_environment()
    sys.path.insert(0, str(source / "src"))
    from rfdetr.config import RFDETRNanoConfig, RFDETRSegNanoConfig, TrainConfig, SegmentationTrainConfig
    from rfdetr.models._defaults import ModelDefaults
    from rfdetr.models.lwdetr import build_model_from_config
    from rfdetr.models.weights import load_pretrain_weights
    from rfdetr.assets.model_weights import ModelWeights, validate_pretrain_weights
    from rfdetr.training.param_groups import get_param_dict  # genuine eager dependency closure
    if not torch.cuda.is_available():
        raise RuntimeError("upstream oracle requires CUDA; no hardware skip counts as evidence")
    torch.backends.cuda.matmul.allow_tf32 = False
    torch.backends.cudnn.allow_tf32 = False
    torch.set_float32_matmul_precision("highest")
    torch.backends.cudnn.benchmark = False
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    if (output / "manifest.json").exists():
        raise RuntimeError("refusing to overwrite published oracle evidence")
    directory = Path(tempfile.mkdtemp(prefix=".producing-", dir=output))
    build_config = torch.__config__.show()
    manifest = {"format": FORMAT, "revision": REVISION, "source_hashes": source_hashes,
                "producer_sha256": digest(__file__), "dependency_sha256": digest(ROOT / "requirements.txt"),
                "payload_sha256": digest(ROOT / "nvidia-payload.json"), "environment": environment,
                "fingerprint": args.fingerprint, "cuda_device": torch.cuda.get_device_name(),
                "torch_build_config": build_config[:65536], "torch_build_config_truncated": len(build_config) > 65536,
                "policy": {"matmul_tf32": torch.backends.cuda.matmul.allow_tf32, "cudnn_tf32": torch.backends.cudnn.allow_tf32,
                           "matmul_precision": torch.get_float32_matmul_precision(), "cudnn_benchmark": torch.backends.cudnn.benchmark,
                           "flash_sdp": torch.backends.cuda.flash_sdp_enabled(), "math_sdp": torch.backends.cuda.math_sdp_enabled(),
                           "memory_efficient_sdp": torch.backends.cuda.mem_efficient_sdp_enabled(),
                           "attention": "upstream grid_sample deformable attention; native production CUDA",
                           "matcher": "FP32 cases use untouched stock; AMP main uses fp32_matcher_costs: original HungarianMatcher._focal_classification_cost, matcher.box_cxcywh_to_xyxy, matcher.batch_sigmoid_ce_loss, matcher.batch_dice_loss and matcher-origin torch.cdist receive FP32 operands with autocast disabled; original mask point_sample/einsum and matcher forward/class/batched eligibility are unchanged; stock prefix is untouched AMP",
                           "gradient_scale": 1, "accumulated_microbatches": 1},
                "presets": [], "cases": [], "unsupported": []}
    precisions = ["fp32", "fp16"]
    if torch.cuda.is_bf16_supported():
        precisions.append("bf16")
    else:
        manifest["unsupported"].append("bf16: CUDA device does not support BF16")
    try:
        defaults = ModelDefaults(force_no_pretrain=True)
        for preset, config_type, train_type, weight_name in [
            ("rf-detr-nano", RFDETRNanoConfig, TrainConfig, "rf-detr-nano.pth"),
            ("rf-detr-seg-nano", RFDETRSegNanoConfig, SegmentationTrainConfig, "rf-detr-seg-nano.pt")]:
            weights = Path(args.weights) / weight_name
            if not weights.is_file():
                raise RuntimeError(f"official pretrained artifact missing: {weights}")
            asset = ModelWeights.from_filename(weight_name)
            if asset is None or not asset.md5_hash:
                raise RuntimeError(f"pinned upstream registry has no integrity identity: {weight_name}")
            validate_pretrain_weights(str(weights), strict=True)
            verified_sha256 = digest(weights)
            # Two genuine grouped-query heads bound training memory while retaining
            # the preset's resolution, widths, decoder depth and query count.
            mc = config_type(pretrain_weights=str(weights), group_detr=2, device="cuda")
            tc = train_type(dataset_dir=".", output_dir=".", lr=1e-4, lr_encoder=1e-4,
                            grad_accum_steps=1, clip_max_norm=.1, weight_decay=1e-4)
            torch.manual_seed(731)
            model = build_model_from_config(mc, tc, defaults)
            invariants = constructor_evidence(model)
            load_pretrain_weights(model, mc)
            initial = {name: value.detach().cpu().clone() for name, value in model.state_dict().items()}
            checkpoint = directory / (preset + ".pth")
            # Actual builder/load state, in the production-supported upstream envelope.
            from rfdetr._namespace import _namespace_from_configs
            torch.save({"model": initial, "args": _namespace_from_configs(mc, tc, defaults)}, checkpoint)
            original = directory / ("original-" + weight_name)
            shutil.copyfile(weights, original)
            if digest(original) != verified_sha256:
                raise RuntimeError(f"verified pretrained artifact changed during production: {weights}")
            manifest["presets"].append({"name": preset, "checkpoint": checkpoint.name, "sha256": digest(checkpoint),
                                       "original": original.name, "original_sha256": digest(original),
                                       "verified_asset": {"registry": "rfdetr.assets.model_weights.ModelWeights",
                                                          "revision": REVISION, "filename": asset.filename,
                                                          "url": asset.url, "md5": asset.md5_hash,
                                                          "sha256": verified_sha256},
                                       "model_config": mc.model_dump(mode="json"), "train_config": tc.model_dump(mode="json"),
                                       "constructor": invariants})
            model.cuda()
            for precision in precisions:
                for training in (False, True):
                    for batch in (1, 2):
                        steps = 8 if training and batch == 2 and preset == "rf-detr-nano" and precision == "fp32" else 1
                        manifest["cases"].append(run_case(directory, model, initial, mc, tc, defaults, preset, precision, batch, training, steps))
            del model, initial
        manifest["resize"] = resize_evidence(directory)
        if source_hashes != verify_source(source):
            raise RuntimeError("upstream source changed during production")
        manifest["status"] = "references_complete_native_comparison_pending"
        # Publish a complete immutable generation first; manifest is the last atomic
        # commit point. A failed comparison keeps this generation for focused retries.
        generation = "references"
        directory.rename(output / generation)
        manifest["directory"] = generation
        json_write(output / "manifest.json", manifest)
    except BaseException:
        # Partial tensors remain available for diagnosis, but have no manifest.
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--payload-snapshot")
    parser.add_argument("--check-environment", action="store_true")
    parser.add_argument("--upstream")
    parser.add_argument("--output")
    parser.add_argument("--weights")
    parser.add_argument("--fingerprint")
    args = parser.parse_args()
    if args.payload_snapshot:
        json_write(Path(args.payload_snapshot), payload())
    elif args.check_environment:
        check_environment()
    else:
        if not all((args.upstream, args.output, args.weights, args.fingerprint)):
            parser.error("generation requires --upstream, --output, --weights and --fingerprint")
        generate(args)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"RF-DETR upstream oracle failed: {type(error).__name__}: {error}", file=sys.stderr)
        raise
