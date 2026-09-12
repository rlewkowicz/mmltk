#!/usr/bin/env python3
"""Bounded GPU environment inventory, invoked read-only through ./mmltk."""

import csv
import ctypes
from datetime import datetime, timezone
import glob
from itertools import islice
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


TEXT_LIMIT = 8192
ITEM_LIMIT = 64
COMMAND_TIMEOUT = 5
LIBRARY_NAME = re.compile(r"^lib(?:cuda(?:rt)?|vulkan|GLX_nvidia|nvidia-[\w-]+)\.so(?:\.|$)")
ENVIRONMENT_NAMES = (
    "LD_LIBRARY_PATH", "LD_PRELOAD", "CUDA_HOME", "CUDA_PATH", "CUDA_VISIBLE_DEVICES",
    "NVIDIA_VISIBLE_DEVICES", "NVIDIA_DRIVER_CAPABILITIES", "VK_DRIVER_FILES",
    "VK_ICD_FILENAMES", "VK_ADD_DRIVER_FILES", "VK_LAYER_PATH",
    "VK_LOADER_DRIVERS_SELECT", "VK_LOADER_DRIVERS_DISABLE",
    "XDG_CONFIG_HOME", "XDG_CONFIG_DIRS", "XDG_DATA_HOME", "XDG_DATA_DIRS",
)
VERSION_QUERIES = {
    "cuda_driver": ("libcuda.so.1", "cuDriverGetVersion"),
    "cuda_runtime": ("libcudart.so", "cudaRuntimeGetVersion"),
    "vulkan_loader": ("libvulkan.so.1", "vkEnumerateInstanceVersion"),
}


def unavailable(reason):
    return {"status": "unavailable", "reason": str(reason)[:TEXT_LIMIT]}


def read_text(path, limit=TEXT_LIMIT):
    try:
        with open(path, "rb") as source:
            data = source.read(limit + 1)
        return {"status": "observed", "path": str(path),
                "value": data[:limit].decode("utf-8", errors="replace").strip(),
                "truncated": len(data) > limit}
    except OSError as error:
        return {"path": str(path), **unavailable(error)}


def command(*arguments, limit=TEXT_LIMIT):
    executable = shutil.which(arguments[0])
    if executable is None:
        return unavailable(f"{arguments[0]} is not available in this image")
    try:
        result = subprocess.run(
            [executable, *arguments[1:]], stdin=subprocess.DEVNULL,
            capture_output=True, timeout=COMMAND_TIMEOUT, check=False,
            env={**os.environ, "LC_ALL": "C"},
        )
        return {
            "status": "observed" if result.returncode == 0 else "unavailable",
            "executable": str(Path(executable).resolve()), "argv": list(arguments),
            "returncode": result.returncode,
            **{name: getattr(result, name)[:limit].decode("utf-8", errors="replace").strip()
               for name in ("stdout", "stderr")},
            "truncated": len(result.stdout) > limit or len(result.stderr) > limit,
        }
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"argv": list(arguments), **unavailable(error)}


def path_fact(path):
    try:
        resolved = Path(path).resolve(strict=True)
        return {"status": "observed", "path": str(path), "resolved": str(resolved),
                "file_name": resolved.name}
    except (OSError, RuntimeError) as error:
        return {"path": str(path), **unavailable(error)}


def library_inventory():
    cache = command("ldconfig", "-p", limit=256 * 1024)
    paths = {}
    for line in cache.get("stdout", "").splitlines():
        fields = line.strip().split()
        if len(fields) >= 4 and LIBRARY_NAME.match(fields[0]) and fields[-2] == "=>":
            paths.setdefault(fields[-1], fields[0])
            if len(paths) == ITEM_LIMIT:
                break
    cache["stdout"] = "\n".join(f"{name} => {path}" for path, name in paths.items())
    roots = list(dict.fromkeys(filter(None, os.environ.get("LD_LIBRARY_PATH", "").split(":"))))[:16]
    roots += ["/usr/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu",
              "/usr/local/nvidia/lib64", "/usr/local/nvidia/lib", "/usr/lib/wsl/lib"]
    roots += list(islice(glob.iglob("/usr/local/cuda*/lib64"), 8))
    roots = list(dict.fromkeys(roots))
    limited = len(paths) == ITEM_LIMIT
    for root in roots:
        if len(paths) == ITEM_LIMIT:
            break
        for path in glob.iglob(str(Path(root) / "lib*.so*")):
            if not LIBRARY_NAME.match(Path(path).name):
                continue
            paths.setdefault(path, Path(path).name)
            if len(paths) == ITEM_LIMIT:
                limited = True
                break
    return {
        "scope": "available linker-cache entries and conventional/LD_LIBRARY_PATH locations; availability is not loading",
        "linker_cache": cache, "searched_directories": roots, "limit_reached": limited,
        "files": [{"soname": name, **path_fact(path)} for path, name in paths.items()],
    }


def library_version(kind, requested):
    """Child process: load one library and call only its non-context version API."""
    _, symbol = VERSION_QUERIES[kind]
    result = {"requested": requested, "query": symbol,
              "scope": "diagnostic process resolution; no CUDA context or Vulkan instance is created"}
    try:
        library = ctypes.CDLL(requested)
        function = getattr(library, symbol)
        version = ctypes.c_uint32() if kind == "vulkan_loader" else ctypes.c_int()
        function.argtypes = [ctypes.POINTER(type(version))]
        function.restype = ctypes.c_int
        address = ctypes.cast(function, ctypes.c_void_p).value
        mapping = unavailable("version entrypoint was not found in bounded /proc/self/maps")
        maps = read_text("/proc/self/maps", limit=128 * 1024)
        for line in maps.get("value", "").splitlines():
            fields = line.split(maxsplit=5)
            if len(fields) != 6:
                continue
            start, end = (int(part, 16) for part in fields[0].split("-"))
            if start <= address < end:
                mapping = path_fact(fields[5])
                break
        result["loaded_object"] = mapping
        status = function(ctypes.byref(version))
        result["api_status"] = status
        if status != 0:
            return {**result, **unavailable(f"{symbol} returned status {status}")}
        value = version.value
        if kind == "vulkan_loader":
            decoded = {"variant": value >> 29, "major": (value >> 22) & 0x7f,
                       "minor": (value >> 12) & 0x3ff, "patch": value & 0xfff}
        else:
            decoded = {"major": value // 1000, "minor": (value % 1000) // 10}
        return {**result, "status": "observed", "api_version_raw": value, "api_version": decoded}
    except (OSError, AttributeError, ValueError) as error:
        return {**result, **unavailable(error)}


def loaded_versions(inventory):
    versions = {}
    for kind, (soname, _) in VERSION_QUERIES.items():
        requested = soname
        if kind == "cuda_runtime":
            # Runtime images may retain only the versioned SONAME. Prefer the
            # first observed versioned SONAME while retaining loader resolution.
            candidates = [item for item in inventory["files"] if item["soname"].startswith("libcudart.so")]
            exact = next((item for item in candidates if item["soname"] == soname), None)
            if exact is None and candidates:
                requested = candidates[0]["soname"]
        receipt = command(sys.executable, "-I", "-B", __file__, "--library-version", kind, requested)
        if receipt["status"] != "observed" or receipt["truncated"]:
            versions[kind] = receipt
            continue
        try:
            versions[kind] = json.loads(receipt["stdout"])
        except json.JSONDecodeError:
            versions[kind] = {"receipt": receipt, **unavailable("version child returned invalid JSON")}
    return versions


def vulkan_manifests(inventory):
    roots = ["/etc/vulkan/icd.d", "/usr/local/share/vulkan/icd.d", "/usr/share/vulkan/icd.d"]
    for variable, fallback in (("XDG_CONFIG_HOME", ".config"), ("XDG_DATA_HOME", ".local/share")):
        location = os.environ.get(variable)
        if not location and os.environ.get("HOME"):
            location = str(Path(os.environ["HOME"]) / fallback)
        if location:
            roots.append(str(Path(location) / "vulkan/icd.d"))
    for variable in ("XDG_CONFIG_DIRS", "XDG_DATA_DIRS"):
        roots.extend(str(Path(root) / "vulkan/icd.d") for root in
                     list(filter(None, os.environ.get(variable, "").split(":")))[:8])
    filenames = {}
    for variable in ("VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_ADD_DRIVER_FILES"):
        for path in list(filter(None, os.environ.get(variable, "").split(":")))[:ITEM_LIMIT]:
            if len(filenames) == ITEM_LIMIT:
                break
            filenames.setdefault(path, variable)
    for root in dict.fromkeys(roots):
        for path in islice(glob.iglob(str(Path(root) / "*.json")), ITEM_LIMIT):
            if len(filenames) == ITEM_LIMIT:
                break
            filenames.setdefault(path, "directory")
    manifests = []
    for filename, discovered_by in filenames.items():
        entry = {"manifest": path_fact(filename), "discovered_by": discovered_by}
        text = read_text(filename)
        try:
            if text["status"] != "observed" or text["truncated"]:
                entry["declaration"] = text if text["status"] != "observed" else unavailable("manifest exceeds byte limit")
            else:
                document = json.loads(text["value"])
                icd = document["ICD"]
                library = icd["library_path"]
                if not isinstance(library, str) or not library:
                    raise ValueError("ICD library_path must be nonempty text")
                entry["declaration"] = {
                    "status": "observed", "file_format_version": document.get("file_format_version"),
                    "library_path": library, "declared_api_version": icd.get("api_version"),
                }
                if "/" in library:
                    candidate = Path(library) if Path(library).is_absolute() else Path(filename).parent / library
                    candidates = [path_fact(candidate)]
                else:
                    candidates = [item for item in inventory["files"] if item["soname"] == library]
                entry["available_objects"] = candidates or unavailable("declared library not found in bounded inventory")
        except (json.JSONDecodeError, KeyError, TypeError, ValueError) as error:
            entry["declaration"] = unavailable(error)
        manifests.append(entry)
    return {"scope": "available ICD declarations; no ICD selection or Vulkan instance creation was performed",
            "searched_directories": list(dict.fromkeys(roots)), "limit_reached": len(filenames) == ITEM_LIMIT,
            "manifests": manifests}


def gpu_identity():
    fields = ("index", "name", "uuid", "pci.bus_id", "driver_version")
    receipt = command("nvidia-smi", "--query-gpu=" + ",".join(fields), "--format=csv,noheader,nounits")
    if receipt["status"] != "observed":
        return receipt
    rows = list(islice(csv.reader(receipt["stdout"].splitlines()), ITEM_LIMIT + 1))
    devices = []
    for row in rows[:ITEM_LIMIT]:
        if len(row) != len(fields):
            return {"receipt": receipt, **unavailable("unexpected nvidia-smi identity columns")}
        devices.append({
            field: unavailable(f"nvidia-smi returned {value.strip()}") if
            value.strip() in ("N/A", "[N/A]", "[Not Supported]", "[Unknown Error]") else value.strip()
            for field, value in zip(fields, row)
        })
    receipt.pop("stdout")
    return {"status": "observed", "query": receipt, "devices": devices,
            "limit_reached": len(rows) > ITEM_LIMIT}


def virtualization_facts():
    detection = {}
    for scope in ("container", "vm"):
        receipt = command("systemd-detect-virt", "--" + scope)
        if receipt.get("returncode") == 1 and receipt.get("stdout") == "none":
            receipt["status"] = "not_detected"
        detection[scope] = receipt
    return {
        "scope": "container-visible facts and explicit detector results; absence does not establish bare metal or an outer host OS",
        "detection": detection,
        "hypervisor_type": read_text("/sys/hypervisor/type"),
        "container_marker": read_text("/run/systemd/container"),
        "dxg_device": path_fact("/dev/dxg"),
        "dmi": {name: read_text(Path("/sys/class/dmi/id") / name)
                for name in ("sys_vendor", "product_name", "product_version", "bios_vendor")},
    }


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "--library-version" and sys.argv[2] in VERSION_QUERIES:
        print(json.dumps(library_version(sys.argv[2], sys.argv[3]), sort_keys=True))
        return
    if len(sys.argv) != 1:
        raise SystemExit("Use ./mmltk --diagnose-gpu-environment [runtime|wayland-validation|development]")
    inventory = library_inventory()
    modules = read_text("/proc/modules", limit=64 * 1024)
    if modules["status"] == "observed":
        modules["value"] = "\n".join(line for line in modules["value"].splitlines()
                                    if line.split()[0].startswith(("nvidia", "nouveau", "dxgkrnl")))
    kernel = os.uname()
    report = {
        "schema_version": 1, "observed_at": datetime.now(timezone.utc).isoformat(),
        "scope": "read-only metadata and library version APIs; no application, CUDA context/device selection, Vulkan instance, allocation, or GPU workload",
        "limits": {"file_bytes": TEXT_LIMIT, "inventory_items": ITEM_LIMIT, "command_timeout_seconds": COMMAND_TIMEOUT},
        "container": {
            "image": os.environ.get("MMLTK_DIAGNOSTIC_IMAGE"),
            "image_id": os.environ.get("MMLTK_DIAGNOSTIC_IMAGE_ID"),
            "hostname": kernel.nodename, "uid": os.getuid(), "gid": os.getgid(), "groups": os.getgroups(),
            "os_release": read_text("/etc/os-release"), "cgroup": read_text("/proc/self/cgroup"),
            "docker_marker": path_fact("/.dockerenv"),
        },
        "kernel": {name: getattr(kernel, name) for name in ("sysname", "release", "version", "machine")},
        "environment": {name: os.environ[name][:TEXT_LIMIT] if name in os.environ else unavailable("not set")
                        for name in ENVIRONMENT_NAMES},
        "nvidia": {
            "gpus": gpu_identity(), "proc_driver_version": read_text("/proc/driver/nvidia/version"),
            "module_version": read_text("/sys/module/nvidia/version"), "loaded_kernel_modules": modules,
        },
        "libraries": inventory, "loaded_version_queries": loaded_versions(inventory),
        "vulkan": vulkan_manifests(inventory),
        "package_versions": command("dpkg-query", "-W", "-f=${binary:Package}\t${Version}\n",
                                    "libvulkan*", "libnvidia*", "cuda-cudart*", "nvidia-*"),
        "virtualization": virtualization_facts(),
    }
    print(json.dumps(report, sort_keys=True, indent=2))


if __name__ == "__main__":
    main()
