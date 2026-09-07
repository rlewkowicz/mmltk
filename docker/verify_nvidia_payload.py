#!/usr/bin/env python3
"""Verify the selected vendor payload in its owned Ubuntu image."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

from nvidia_payload import elf_environment, library_directories, payload_file


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("development", "runtime"), required=True)
    parser.add_argument("--packaged", action="store_true")
    args = parser.parse_args()
    record = json.loads(Path("/usr/share/mmltk/nvidia-payload.json").read_text())
    manifest = record["manifest"]
    if record["mode"] != args.mode:
        raise RuntimeError("payload mode mismatch")
    if os.uname().machine != "x86_64" or f'VERSION_ID="{manifest["ubuntu"]}"' not in Path("/etc/os-release").read_text():
        raise RuntimeError(f"owned image must be Ubuntu {manifest['ubuntu']} Linux/amd64")
    roots = [Path(manifest[name]) for name in ("cuda_root", "torch_root", "python_root", "native_root", "tensorrt_root")]
    roots.append(Path("/opt/onnxruntime"))
    for root in map(Path, manifest["audited_native_roots"]):
        roots.extend((root, root.parent / "share"))
    if args.packaged:
        roots.append(Path("/opt/mmltk"))
    checked = set()
    errors = []
    for root in roots:
        for path in root.rglob("*"):
            if path in checked:
                continue
            checked.add(path)
            if ((path.is_file() or path.is_symlink())
                    and not payload_file(manifest, path, args.mode)):
                errors.append(f"excluded file in {args.mode} payload: {path}")
            if path.is_symlink():
                if not path.exists():
                    errors.append(f"dangling payload symlink: {path}")
                continue
            if not path.is_file():
                continue
            with path.open("rb") as stream:
                if stream.read(4) != b"\x7fELF":
                    continue
            # ldd reports the image loader's actual resolution, including each
            # object's RUNPATH and transitive dependencies. Driver libraries
            # are injected only when a GPU container starts.
            result = subprocess.run(["ldd", str(path)], capture_output=True, text=True,
                                    env=elf_environment(manifest, path, os.environ))
            diagnostic = result.stdout + result.stderr
            if result.returncode and not any(message in diagnostic for message in ("not a dynamic executable", "statically linked")):
                errors.append(f"payload loader failure: {path}: {diagnostic.strip()}")
            for line in result.stdout.splitlines():
                if "=> not found" in line and line.split()[0] not in manifest["external_driver_libraries"]:
                    errors.append(f"unresolved payload dependency: {path}: {line.strip()}")
    if errors:
        raise RuntimeError("\n".join(errors))
    if args.mode == "development":
        if not (Path(manifest["cuda_root"]) / "bin/nvcc").is_file():
            raise RuntimeError("development payload lacks NVCC")
    helper = Path(manifest["torch_root"]) / "bin/torch_shm_manager"
    if not helper.is_file() or not os.access(helper, os.X_OK):
        raise RuntimeError("payload lacks required Torch shared-memory manager")
    expected_paths = library_directories(manifest)
    actual_paths = os.environ.get("LD_LIBRARY_PATH", "").split(":")
    if [path for path in actual_paths if path in expected_paths] != expected_paths:
        raise RuntimeError("loader search order disagrees with the canonical manifest")
    site = Path(f"/usr/local/lib/python{manifest['python_abi']}/dist-packages/mmltk-nvidia.pth")
    if site.read_text() != manifest["python_root"] + "\n":
        raise RuntimeError("Python site registration disagrees with the canonical manifest")
    if Path("/etc/ld.so.conf.d/mmltk-nvidia.conf").read_text() != "\n".join(expected_paths) + "\n":
        raise RuntimeError("loader registration disagrees with the canonical manifest")
    # Import on CPU during image construction still exercises Torch's native
    # initialization and NumPy ABI without requiring the injected GPU driver.
    # Match the checkpoint bridge's sanitized environment; isolated Python
    # additionally proves imports cannot come from the workspace or user site.
    python_environment = os.environ.copy()
    for name in ("PYTHONPATH", "LD_LIBRARY_PATH"):
        python_environment.pop(name, None)
    subprocess.run([sys.executable, "-I", "-B", "-c", "import torch,numpy,importlib.metadata; "
                    f"assert torch.__version__ == {manifest['donor_metadata']['torch_runtime_version']!r}; "
                    "assert importlib.metadata.version('torch') == "
                    f"{manifest['donor_metadata']['torch_distribution_version']!r}; "
                    "assert torch.from_numpy(numpy.zeros((1,),dtype=numpy.float32)).numel() == 1; "
                    "x = torch.linspace(-1, 1, 1024); "
                    "assert torch.isfinite(torch.nn.functional.gelu(x)).all(); "
                    "assert torch.isfinite(torch.linalg.svdvals(torch.eye(8))).all()"],
                   env=python_environment, check=True)
    print(json.dumps({"payload_verified": args.mode, "donor": manifest["donor"], "versions": manifest["versions"]}))


if __name__ == "__main__":
    main()
