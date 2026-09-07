#!/usr/bin/env python3
"""Extract selected vendor binaries; never compile or execute application code."""
import argparse
import glob
import importlib.metadata as metadata
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

from packaging.requirements import Requirement

from nvidia_payload import elf_environment, excluded_components, library_directories, load_manifest, payload_file, require


class Discovery:
    """Resolve donor content once for both deterministic output modes."""
    def __init__(self, manifest):
        self.manifest = manifest
        self.bytes = {}
        self.elf = {}
        self.dependencies = {}
        self.ownership = {}
        self.package_notices = {}
        self.directories = {}
        self.notice_files = {}
        self.components = []
        self.python = []
        roots = [Path(p) for pattern in manifest["library_roots"] for p in sorted(glob.glob(pattern))]
        headers = [Path(p) for pattern in manifest["header_roots"] for p in sorted(glob.glob(pattern))]
        for component in manifest["components"]:
            selected = self.named_files(roots, component["libraries"], f"{component['name']} library")
            require(selected or not component["required"], f"missing component: {component['name']}")
            if not selected:
                continue
            selected_headers = self.named_files(
                headers, component["headers"], f"{component['name']} header", require_each_pattern=True)
            self.components.append((component["name"], selected, selected_headers))
        queue = list(manifest["python_packages"])
        seen = set()
        while queue:
            dist = metadata.distribution(queue.pop())
            name = dist.metadata["Name"].lower().replace("-", "_")
            if name in seen:
                continue
            seen.add(name)
            require(not excluded_components(manifest, name),
                    f"excluded Python dependency requires explicit audit: {name}")
            require(dist.files is not None, f"Python distribution lacks file inventory: {name}")
            roots = set()
            base = Path(dist.locate_file("")).resolve()
            for item in dist.files:
                if ".." not in item.parts:
                    depth = 2 if item.parts[0] == "nvidia" and len(item.parts) > 1 else 1
                    roots.add(str(Path(*item.parts[:depth])))
            for root in sorted(roots):
                source = base / root
                if not source.exists():
                    continue
                destination = Path(manifest["python_root"]) / root
                if name == "torch" and root == "torch":
                    require(str(source) == manifest["torch_source"], f"unexpected Torch installation: {source}")
                    destination = Path(manifest["torch_root"])
                self.python.append((source, destination))
            for text in dist.requires or []:
                requirement = Requirement(text)
                if requirement.marker is None or requirement.marker.evaluate({"extra": ""}):
                    dependency = metadata.distribution(requirement.name)
                    require(not requirement.specifier or dependency.version in requirement.specifier,
                            f"unsatisfied Python dependency: {text}; found {dependency.version}")
                    queue.append(requirement.name)
        self.python_names = sorted(seen)

    @staticmethod
    def named_files(roots, patterns, label, *, require_each_pattern=False):
        selected = {}
        for pattern in patterns:
            matched = False
            for root in roots:
                for path in sorted(root.glob(pattern)):
                    matched = True
                    require(path.name not in selected or selected[path.name].resolve() == path.resolve(),
                            f"ambiguous {label}: {path}")
                    # Distinct public aliases can resolve to the same file.
                    # Payload.copy preserves their local symlink relationships.
                    selected[path.name] = path
            require(matched or not require_each_pattern, f"missing {label}: {pattern}")
        return list(selected.values())

    def children(self, directory):
        if directory not in self.directories:
            self.directories[directory] = sorted(directory.iterdir())
        return self.directories[directory]

    def is_elf(self, source):
        if source not in self.elf:
            with source.open("rb") as stream:
                self.elf[source] = stream.read(4) == b"\x7fELF"
        return self.elf[source]

    def component_notices(self, directory):
        if directory not in self.notice_files:
            self.notice_files[directory] = sorted({
                path for pattern in self.manifest["notice_patterns"]
                for path in directory.glob(pattern)})
        return self.notice_files[directory]

    def notices(self, source):
        if source not in self.ownership:
            result = subprocess.run(["dpkg-query", "-S", str(source)], capture_output=True, text=True)
            require(result.returncode in (0, 1), f"package ownership discovery failed: {source}: {result.stderr}")
            self.ownership[source] = sorted({line.split(": ", 1)[0] for line in result.stdout.splitlines()})
        for package in self.ownership[source]:
            if package not in self.package_notices:
                directory = Path("/usr/share/doc") / package.split(":")[0]
                self.package_notices[package] = directory if directory.exists() else None
        return [(package, self.package_notices[package]) for package in self.ownership[source]]

    def closure(self, source):
        if source not in self.dependencies:
            result = subprocess.run(["ldd", str(source)], capture_output=True, text=True,
                                    env=elf_environment(self.manifest, source, os.environ))
            diagnostic = result.stdout + result.stderr
            require(not result.returncode or any(message in diagnostic for message in ("not a dynamic executable", "statically linked")),
                    f"cannot inspect donor ELF dependency closure: {source}: {diagnostic.strip()}")
            dependencies = []
            for line in result.stdout.splitlines():
                if "=> not found" in line:
                    name = line.split()[0]
                    require(name in self.manifest["external_driver_libraries"], f"unresolved donor dependency {name}: {source}")
                match = re.search(r"=> (/\S+)", line)
                if match:
                    dependencies.append(Path(match[1]))
            self.dependencies[source] = dependencies
        return self.dependencies[source]


class Payload:
    def __init__(self, discovery, output, mode):
        self.discovery = discovery
        self.manifest = discovery.manifest
        self.output = output / mode
        self.mode = mode
        self.sources = {}
        self.copied_sources = {}
        self.copied_directories = set()
        self.noticed_sources = set()
        self.pending = []
        self.inspected = set()
        self.packages = set()
        self.components = []

    def copy(self, source, destination):
        source = Path(source)
        destination = Path(destination)
        if (not payload_file(self.manifest, source.resolve(), self.mode)
                or not payload_file(self.manifest, destination, self.mode)):
            return
        require(source.exists(), f"missing payload input: {source}")
        if source.is_symlink() and source.is_dir():
            resolved = source.resolve(strict=True)
            cuda = Path(self.manifest["cuda_source"])
            if resolved.is_relative_to(cuda):
                real_destination = Path(self.manifest["cuda_root"]) / resolved.relative_to(cuda)
            else:
                require(any(resolved.is_relative_to(Path(root).resolve())
                            for root in self.manifest["audited_native_roots"]),
                        f"unaudited external directory symlink: {source} -> {resolved}")
                real_destination = resolved
            self.copy(resolved, real_destination)
            target = self.output / destination.relative_to("/")
            if not target.is_symlink():
                target.parent.mkdir(parents=True, exist_ok=True)
                target.symlink_to(os.path.relpath(real_destination, destination.parent))
            return
        if source.is_dir():
            directory_key = (source.resolve(), destination)
            if directory_key in self.copied_directories:
                return
            self.copied_directories.add(directory_key)
            for child in self.discovery.children(source):
                self.copy(child, destination / child.name)
            return
        target = self.output / destination.relative_to("/")
        if target in self.sources:
            require(self.sources[target] == source.resolve(), f"duplicate payload destination: {destination}")
            return
        target.parent.mkdir(parents=True, exist_ok=True)
        self.sources[target] = source.resolve()
        if source.is_symlink():
            resolved = source.resolve(strict=True)
            # Keep each SONAME chain local to its selected component even when
            # the donor uses absolute symlinks across installation prefixes.
            real_destination = destination.parent / resolved.name
            self.copy(resolved, real_destination)
            if real_destination != destination:
                target.symlink_to(resolved.name)
            else:
                self.copy_bytes(resolved, target)
        else:
            self.copy_bytes(source.resolve(), target)
        if not target.is_symlink():
            self.copied_sources.setdefault(source.resolve(), target)
            if self.discovery.is_elf(source.resolve()):
                self.pending.append(source.resolve())
                self.notices(source)

    def copy_bytes(self, source, target):
        if source in self.discovery.bytes:
            os.link(self.discovery.bytes[source], target)
        else:
            shutil.copy2(source, target)
            self.discovery.bytes[source] = target

    def notices(self, source):
        source = Path(source).resolve()
        if source in self.noticed_sources:
            return
        self.noticed_sources.add(source)
        for package, directory in self.discovery.notices(source):
            if package in self.packages:
                continue
            self.packages.add(package)
            if directory:
                notices = self.output / self.manifest["notice_root"].lstrip("/") / "packages"
                self.copy_notices(directory, notices / package)

    def copy_notices(self, source, target, active=()):
        # Notices retain their complete dereferenced trees independently of the
        # native runtime filter, sharing identical bytes across both outputs.
        source = source.resolve(strict=True)
        require(source not in active, f"cyclic package notice directory: {source}")
        if source.is_dir():
            target.mkdir(parents=True, exist_ok=True)
            for child in self.discovery.children(source):
                self.copy_notices(child, target / child.name, (*active, source))
            shutil.copystat(source, target)
        elif not target.exists():
            self.copy_bytes(source, target)

    def libraries(self):
        # Native libraries distributed in wheels keep their licenses and
        # notices in distribution metadata rather than Ubuntu package docs.
        for name in self.manifest["notice_distributions"]:
            dist = metadata.distribution(name)
            require(dist.files is not None, f"missing notice inventory: {name}")
            roots = {item.parts[0] for item in dist.files if item.parts[0].endswith(".dist-info")}
            require(len(roots) == 1, f"missing/ambiguous notice metadata: {name}")
            for root in roots:
                self.copy_notices(Path(dist.locate_file(root)),
                                  self.output / self.manifest["notice_root"].lstrip("/") / "python" / root)
        for name, selected, headers in self.discovery.components:
            self.components.append(name)
            prefix = self.manifest["tensorrt_root"] if name == "tensorrt" else self.manifest["native_root"]
            for path in selected:
                self.copy(path, Path(prefix) / "lib" / path.name)
                # Vendor-owned roots can carry plugins and notices beside the
                # principal DSOs. Preserve the selected component's whole lib
                # directory, never the system-wide lib directory.
                if any(path.parent.is_relative_to(source_prefix)
                       for source_prefix in self.manifest["component_library_prefixes"]):
                    self.copy(path.parent, Path(prefix) / "lib")
                    for notice in self.discovery.component_notices(path.parent.parent):
                        self.copy(notice, Path(prefix) / "share" / name / notice.name)
            if self.mode == "development":
                for path in headers:
                    self.copy(path, Path(prefix) / "include" / path.name)

    def python_packages(self):
        for source, destination in self.discovery.python:
            self.copy(source, destination)
        link = self.output / self.manifest["python_root"].lstrip("/") / "torch"
        link.parent.mkdir(parents=True, exist_ok=True)
        link.symlink_to(self.manifest["torch_root"])
        # The checkpoint bridge clears PYTHONPATH and LD_LIBRARY_PATH before
        # exec. Register the owned payload with the Ubuntu interpreter/loader.
        site = self.output / f"usr/local/lib/python{self.manifest['python_abi']}/dist-packages"
        site.mkdir(parents=True, exist_ok=True)
        (site / "mmltk-nvidia.pth").write_text(self.manifest["python_root"] + "\n")
        loader = self.output / "etc/ld.so.conf.d/mmltk-nvidia.conf"
        loader.parent.mkdir(parents=True, exist_ok=True)
        loader.write_text("\n".join(library_directories(self.manifest)) + "\n")
        return self.discovery.python_names

    def audited_native_libraries(self):
        # Torch and NVSHMEM require these MPI/UCX/UCC runtime trees. Preserve
        # their vendor-relative plugin paths and expose DSOs in the owned root.
        for root in map(Path, self.manifest["audited_native_roots"]):
            require(root.is_dir(), f"missing audited native dependency directory: {root}")
            for directory in (root, root.parent / "share"):
                if not directory.is_dir():
                    continue
                resolved = directory.resolve(strict=True)
                self.copy(resolved, resolved)
                if directory != resolved:
                    alias = self.output / directory.relative_to("/")
                    alias.parent.mkdir(parents=True, exist_ok=True)
                    alias.symlink_to(os.path.relpath(resolved, directory.parent))
            for library in self.discovery.children(root):
                if ".so" in library.name and not library.is_dir():
                    self.copy(library, Path(self.manifest["native_root"]) / "lib" / library.name)
            for ancestor in (root.parent, root.parent.parent):
                for notice in self.discovery.component_notices(ancestor):
                    self.copy_notices(notice, self.output / notice.relative_to("/"))

    def closure(self):
        errors = []
        while self.pending:
            source = self.pending.pop()
            if source in self.inspected:
                continue
            self.inspected.add(source)
            try:
                dependencies = self.discovery.closure(source)
            except RuntimeError as error:
                errors.append(str(error))
                continue
            for dependency in dependencies:
                if dependency.name in self.manifest["external_driver_libraries"]:
                    continue
                if dependency.resolve() in self.copied_sources:
                    continue
                # Keep the owned Ubuntu glibc/loader; retain other transitive
                # DSOs and their package notices at an explicit search prefix.
                if dependency.name.startswith(("libc.so.", "libm.so.", "libpthread.so.", "libdl.so.", "librt.so.", "ld-linux")):
                    continue
                if not payload_file(self.manifest, dependency.resolve(), self.mode):
                    errors.append(f"native dependency requires explicit excluded-component audit: "
                                  f"{source} -> {dependency} (resolved: {dependency.resolve()})")
                    continue
                self.copy(dependency, Path(self.manifest["native_root"]) / "lib" / dependency.name)
        require(not errors, "native dependency closure failed:\n" + "\n".join(sorted(set(errors))))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--donor", required=True)
    args = parser.parse_args()
    manifest = load_manifest(args.manifest)
    require(args.donor == manifest["donor"], "donor override disagrees with canonical immutable manifest")
    require(os.uname().machine == "x86_64", "payload requires Linux/amd64")
    require(f'VERSION_ID="{manifest["ubuntu"]}"' in Path("/etc/os-release").read_text(), "donor Ubuntu mismatch")
    cuda = Path(manifest["cuda_source"])
    expected = manifest["donor_metadata"]
    # NGC records the toolkit release in its immutable environment; this donor
    # omits version.json. Independently verify the compiler and runtime package.
    version = os.environ.get("CUDA_VERSION")
    require(version == expected["cuda_version"], f"CUDA release metadata mismatch: {version}")
    nvcc = subprocess.run([str(cuda / "bin/nvcc"), "--version"],
                          capture_output=True, text=True, check=True)
    compiler_version = re.search(r"\bV(\d+\.\d+\.\d+)\b", nvcc.stdout)
    require(compiler_version is not None and compiler_version[1] == expected["nvcc_version"],
            f"NVCC version mismatch: {nvcc.stdout.strip()}")
    runtime_package = subprocess.run(
        ["dpkg-query", "-W", "-f=${Status}\t${Version}", expected["cudart_package"]],
        capture_output=True, text=True, check=True)
    require(runtime_package.stdout == "install ok installed\t" + expected["cudart_version"],
            f"CUDA runtime package mismatch: {runtime_package.stdout}")
    torch_distribution = metadata.version("torch")
    require(torch_distribution == expected["torch_distribution_version"],
            f"Torch distribution version mismatch: {torch_distribution}")
    # TensorRT's canonical native header records all four ABI version fields.
    version_headers = {p.resolve() for root in manifest["header_roots"] for p in map(Path, glob.glob(root + "/NvInferVersion.h"))}
    require(len(version_headers) == 1, f"missing/ambiguous TensorRT version header: {version_headers}")
    text = version_headers.pop().read_text()
    # TensorRT 11's public NV_TENSORRT_* macros alias TRT_*_ENTERPRISE.
    trt_parts = []
    for part in ("MAJOR", "MINOR", "PATCH", "BUILD"):
        alias = re.search(r"#define\s+NV_TENSORRT_" + part + r"\s+TRT_" + part + r"_ENTERPRISE\b", text)
        value = re.search(r"#define\s+TRT_" + part + r"_ENTERPRISE\s+(\d+)", text)
        require(alias is not None and value is not None, f"missing TensorRT 11 {part} version declaration")
        trt_parts.append(value[1])
    trt = ".".join(trt_parts)
    require(trt == manifest["versions"]["tensorrt"], f"TensorRT version mismatch: {trt}")
    cccl = cuda / "targets/x86_64-linux/include/cccl/thrust/system/detail/sequential/execution_policy.h"
    require(cccl.is_file(), "missing canonical CCCL sequential policy header")
    require("_CCCL_GLOBAL_CONSTANT tag seq;" not in cccl.read_text(),
            "selected donor still has CCCL detail::sequential::seq defect; inspect exact header before patching")
    discovery = Discovery(manifest)
    for mode in manifest["modes"]:
        payload = Payload(discovery, args.output, mode)
        if mode == "development":
            payload.copy(cuda, Path(manifest["cuda_root"]))
        else:
            payload.copy(cuda / "targets/x86_64-linux/lib", Path(manifest["cuda_root"]) / "targets/x86_64-linux/lib")
            lib64 = payload.output / manifest["cuda_root"].lstrip("/") / "lib64"
            lib64.symlink_to("targets/x86_64-linux/lib")
            for notice in discovery.component_notices(cuda):
                payload.copy(notice, Path(manifest["cuda_root"]) / notice.name)
        payload.libraries()
        packages = payload.python_packages()
        payload.audited_native_libraries()
        payload.closure()
        record = payload.output / "usr/share/mmltk/nvidia-payload.json"
        record.parent.mkdir(parents=True, exist_ok=True)
        record.write_text(json.dumps({"manifest": manifest, "mode": mode, "components": payload.components,
                                      "python_packages": packages,
                                      "deb_packages": sorted(payload.packages)}, indent=2) + "\n")


if __name__ == "__main__":
    main()
