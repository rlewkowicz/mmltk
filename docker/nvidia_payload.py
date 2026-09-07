"""Canonical NVIDIA payload metadata and component-aware mode policy."""
import json
from functools import cache
from pathlib import Path
import re


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


@cache
def _resolved_root(path):
    return Path(path).resolve()


@cache
def _excluded_name_pattern(names):
    return re.compile(r"(?<![a-z0-9])(?:lib)?(" + "|".join(map(re.escape, names)) + ")")


def excluded_components(manifest, name):
    # Component prefixes may follow path/package separators or the shared-library
    # "lib" prefix. The interior "dali" in cudalibxt.h does not name DALI.
    names = tuple(manifest["excluded_names"])
    return set(_excluded_name_pattern(names).findall(str(name).lower())) if names else set()


def load_manifest(path):
    manifest = json.loads(Path(path).read_text())
    metadata = manifest["donor_metadata"]
    require(metadata["cuda_version"].rsplit(".", 1)[0] == manifest["versions"]["cuda"],
            "CUDA donor build does not match the required toolkit release")
    for key in ("torch_distribution_version", "torch_runtime_version"):
        require(metadata[key].split(".nv", 1)[0] == manifest["versions"]["torch"],
                f"{key} does not match the required Torch source version")
    require(manifest["modes"] == ["development", "runtime"], "unexpected payload modes")
    require(len(manifest["library_order"]) == 4
            and set(manifest["library_order"]) == {"cuda_root", "torch_root", "native_root", "tensorrt_root"},
            "missing, duplicate, or unknown NVIDIA library search root")
    for key in ("cuda_root", "torch_root", "python_root", "native_root", "tensorrt_root", "notice_root"):
        root = Path(manifest[key])
        require(root.is_absolute() and ".." not in root.parts, f"invalid normalized root: {root}")
    return manifest


def payload_file(manifest, path, mode):
    """Select product files and retain the Torch executable used at import."""
    path = Path(path)
    excluded = excluded_components(manifest, path)
    if excluded and not (
            excluded <= {"hpcx", "hpc-x"}
            and any(path.is_relative_to(root) or path.is_relative_to(_resolved_root(root))
                    or path.is_relative_to(Path(root).parent / "share")
                    or path.is_relative_to(_resolved_root(root).parent / "share")
                    for root in manifest["audited_native_roots"])):
        return False
    if path.name.startswith("cuda-gdb-python"):
        if path.name.removeprefix("cuda-gdb-python").removesuffix("-tui") != manifest["python_abi"]:
            return False
    # Torch's bundled native test executables require unrelated donor services;
    # Python's torch.testing helpers remain part of the library package.
    torch_root = Path(manifest["torch_root"])
    if path.is_relative_to(manifest["torch_source"]):
        path = torch_root / path.relative_to(manifest["torch_source"])
    if path.is_relative_to(torch_root / "test"):
        return False
    # These three libraries are installed only by PyTorch's INSTALL_TEST rule.
    if path.parent == torch_root / "lib" and (
            path.name.endswith("_test.so") or path.name == "libbackend_with_compiler.so"):
        return False
    helper = Path(manifest["torch_root"]) / "bin/torch_shm_manager"
    if path == helper or path == helper.parent:
        return True
    if path.is_relative_to(helper.parent):
        return False
    if mode == "development":
        return True
    if (set(path.parts) & {"include", "include64", "stubs", "nvvm", "libdevice", "cmake", "pkgconfig", "__pycache__"}
            or path.suffix in {".a", ".la", ".pc", ".mod", ".smod", ".h", ".hpp", ".cuh", ".o",
                               ".pyc", ".pyo", ".cpp", ".cc", ".c", ".cu", ".cmake"}
            or path.name in {"gcc", "g++", "ptxas", "nvcc", "nvlink", "cicc", "fatbinary"}
            or path.name.startswith("libcuda.so")
            or any(path.name.startswith(name.split(".so", 1)[0] + ".so")
                   for name in manifest["external_driver_libraries"])):
        return False
    for key in ("cuda_root", "torch_root", "python_root", "native_root", "tensorrt_root"):
        root = Path(manifest[key])
        if path.is_relative_to(root) and "bin" in path.relative_to(root).parts:
            return False
    return True


def library_directories(manifest):
    return [str(Path(manifest[key]) / ("lib64" if key == "cuda_root" else "lib"))
            for key in manifest["library_order"]]


def elf_environment(manifest, path, environment):
    """Model the local search roots established by vendor/application launchers."""
    environment = environment.copy()
    for root in (Path(manifest["cuda_root"]) / "compute-sanitizer",
                 Path("/opt/mmltk/lib/mmltk/firefox")):
        if Path(path).is_relative_to(root):
            environment["LD_LIBRARY_PATH"] = str(root) + ":" + environment.get("LD_LIBRARY_PATH", "")
            break
    return environment
