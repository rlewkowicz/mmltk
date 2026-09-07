#!/usr/bin/env python3
"""Read-only Linux capability inspection, invoked in a container by ./mmltk."""

import argparse
import ctypes
from datetime import datetime, timezone
import errno
import glob
import json
import os
from pathlib import Path
import platform
import resource
import shutil
import stat
import struct
import subprocess


def unavailable(reason):
    return {"status": "unavailable", "reason": str(reason)}


def read_text(path):
    try:
        return {"status": "observed", "value": Path(path).read_text().strip()}
    except OSError as error:
        return unavailable(error)


def command(*arguments):
    executable = shutil.which(arguments[0])
    if executable is None:
        return unavailable(f"{arguments[0]} is not installed in this container")
    try:
        result = subprocess.run(
            [executable, *arguments[1:]],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
        return {
            "status": "observed" if result.returncode == 0 else "unavailable",
            "argv": list(arguments),
            "returncode": result.returncode,
            "stdout": result.stdout[:65536],
            "stderr": result.stderr[:65536],
        }
    except (OSError, subprocess.TimeoutExpired) as error:
        return unavailable(error)


def direct_io(path):
    result = {"read": "not attempted; open and alignment metadata do not prove a DMA route"}
    if not hasattr(os, "O_DIRECT"):
        result["open"] = unavailable("Python does not expose Linux O_DIRECT")
    else:
        try:
            descriptor = os.open(path, os.O_RDONLY | os.O_DIRECT | os.O_CLOEXEC)
            os.close(descriptor)
            result["open"] = {"status": "observed", "accepted": True}
        except OSError as error:
            result["open"] = {
                "status": "unsupported" if error.errno in (errno.EINVAL, errno.EOPNOTSUPP) else "unavailable",
                "errno": error.errno,
                "reason": str(error),
            }
    libc = ctypes.CDLL(None, use_errno=True)
    statx = getattr(libc, "statx", None)
    if statx is None:
        result["alignment"] = unavailable("container libc has no statx entrypoint")
        return result
    statx.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_uint, ctypes.c_void_p]
    statx.restype = ctypes.c_int
    # Linux UAPI statx is 256 bytes. stx_mask is at 0; the two DIOALIGN
    # uint32 fields are at 152 and 156. Unrequested fields remain opaque.
    buffer = ctypes.create_string_buffer(256)
    dioalign = 0x2000
    if statx(-100, os.fsencode(path), 0, dioalign, buffer) != 0:
        result["alignment"] = unavailable(os.strerror(ctypes.get_errno()))
    elif not struct.unpack_from("=I", buffer, 0)[0] & dioalign:
        result["alignment"] = unavailable("kernel/filesystem did not return STATX_DIOALIGN")
    else:
        memory, offset = struct.unpack_from("=II", buffer, 152)
        result["alignment"] = {
            "status": "observed" if memory and offset else "unsupported",
            "memory_bytes": memory,
            "offset_and_length_bytes": offset,
            "reason": "STATX_DIOALIGN returned by kernel for this file",
        }
    return result


def pci_devices():
    root = Path("/sys/bus/pci/devices")
    if not root.is_dir():
        return unavailable("PCI sysfs is not visible in this container")
    try:
        visible_devices = sorted(root.iterdir())
    except OSError as error:
        return unavailable(error)
    devices = []
    for device in visible_devices:
        classification = read_text(device / "class")
        value = classification.get("value", "")
        if not (value.startswith("0x03") or value == "0x010802"):
            continue
        devices.append({
            "address": device.name,
            "sysfs_topology": str(device.resolve()),
            "class": classification,
            "driver": str((device / "driver").resolve()) if (device / "driver").exists() else None,
            "iommu_group": str((device / "iommu_group").resolve()) if (device / "iommu_group").exists() else None,
            "attributes": {
                name: read_text(device / name)
                for name in ("vendor", "device", "numa_node", "current_link_speed", "current_link_width",
                             "max_link_speed", "max_link_width")
            },
        })
    return {"status": "observed", "devices": devices, "scope": "container-visible display and NVMe PCI devices"}


def components():
    cache = command("ldconfig", "-p")
    if cache["status"] == "observed":
        cache["stdout"] = "\n".join(
            line for line in cache["stdout"].splitlines()
            if any(name in line.lower() for name in ("cufile", "gdrapi"))
        )
    roots = ("/usr/lib/x86_64-linux-gnu", "/usr/local/lib", "/opt/nvidia/lib",
             "/usr/local/cuda*/lib64",
             "/usr/local/cuda*/targets/x86_64-linux/lib", "/opt/nvidia/*/lib*")
    libraries = sorted({
        match for root in roots for name in ("libcufile*.so*", "libgdrapi*.so*")
        for match in glob.glob(f"{root}/{name}")
    })
    modules = read_text("/proc/modules")
    if modules["status"] == "observed":
        modules["value"] = "\n".join(
            line for line in modules["value"].splitlines()
            if line.split()[0] in ("nvidia", "nvidia_fs", "nvidia_peermem", "gdrdrv", "nvme", "nvme_core")
        )
    return {
        "scope": "container linker cache and conventional library paths; absence is not proof of host absence",
        "linker_cache": cache,
        "library_paths": libraries,
        "kernel_modules": modules,
        "gdr_device_visible": Path("/dev/gdrdrv").exists(),
        "gdscheck_paths": sorted(glob.glob("/usr/local/cuda*/gds/tools/gdscheck*")),
        "gdscheck_execution": "not run; component discovery only",
        "native_storage_dma": "not established",
        "gdrcopy": "Application-private static userspace library; donor libgdrapi discovery does not identify the application implementation",
    }


def gdrcopy_device_capabilities():
    """Observe each CUDA-visible device; export support alone is not mmap support."""
    try:
        cuda = ctypes.CDLL("libcuda.so.1")
        cuda.cuInit.argtypes = [ctypes.c_uint]
        cuda.cuDriverGetVersion.argtypes = [ctypes.POINTER(ctypes.c_int)]
        cuda.cuDeviceGetCount.argtypes = [ctypes.POINTER(ctypes.c_int)]
        cuda.cuDeviceGet.argtypes = [ctypes.POINTER(ctypes.c_int), ctypes.c_int]
        cuda.cuDeviceGetAttribute.argtypes = [ctypes.POINTER(ctypes.c_int), ctypes.c_int, ctypes.c_int]
        for name in ("cuInit", "cuDriverGetVersion", "cuDeviceGetCount", "cuDeviceGet", "cuDeviceGetAttribute"):
            getattr(cuda, name).restype = ctypes.c_int
        status = cuda.cuInit(0)
        if status:
            return unavailable(f"cuInit returned CUDA status {status}")
        version, count = ctypes.c_int(), ctypes.c_int()
        for operation, output in ((cuda.cuDriverGetVersion, version), (cuda.cuDeviceGetCount, count)):
            status = operation(ctypes.byref(output))
            if status:
                return unavailable(f"CUDA capability query returned status {status}")
        devices = []
        for ordinal in range(count.value):
            device = ctypes.c_int()
            status = cuda.cuDeviceGet(ctypes.byref(device), ordinal)
            if status:
                devices.append({"ordinal": ordinal, "query": unavailable(f"CUDA status {status}")})
                continue
            attributes = {}
            # CUDA Driver ABI: export=124, CPU mmap=152 (introduced in 13.3).
            for name, attribute in (("dma_buf_export", 124), ("dma_buf_mmap", 152)):
                value = ctypes.c_int()
                status = cuda.cuDeviceGetAttribute(ctypes.byref(value), attribute, device.value)
                attributes[name] = unavailable(f"CUDA status {status}") if status else {
                    "status": "observed", "supported": bool(value.value)}
            devices.append({"ordinal": ordinal, **attributes})
        return {"status": "observed", "driver_api_version": version.value,
                "minimum_dmabuf_mmap_version": 13030, "devices": devices,
                "startup_dmabuf_override": os.environ.get("GDRCOPY_USE_DMABUF_MMAP"),
                "descriptor_limit": dict(zip(("soft", "hard"), resource.getrlimit(resource.RLIMIT_NOFILE))),
                "scope": "capabilities only; allocation, BAR availability, registration and mmap remain unverified"}
    except (OSError, AttributeError) as error:
        return unavailable(error)


def numa_policy_query():
    try:
        library = ctypes.CDLL("libnuma.so.1", use_errno=True)
        query = library.get_mempolicy
        query.argtypes = [ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_ulong),
                          ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong]
        mode = ctypes.c_int()
        mask = (ctypes.c_ulong * 64)()
        if query(ctypes.byref(mode), mask, 64 * ctypes.sizeof(ctypes.c_ulong) * 8, None, 0):
            code = ctypes.get_errno()
            return {"status": "denied" if code in (errno.EPERM, errno.EACCES) else "unavailable",
                    "errno": code, "reason": os.strerror(code)}
        return {"status": "observed", "mode": mode.value, "node_mask_words": list(mask)}
    except (OSError, AttributeError) as error:
        return unavailable(error)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiled_file")
    parser.add_argument("--source-path", required=True)
    arguments = parser.parse_args()
    report = {
        "schema_version": 1,
        "observed_at": datetime.now(timezone.utc).isoformat(),
        "image": os.environ.get("MMLTK_DIAGNOSTIC_IMAGE"),
        "image_id": os.environ.get("MMLTK_DIAGNOSTIC_IMAGE_ID"),
        "scope": "read-only container capability observation; no content read, writes, CUDA allocation, or benchmark",
        "source_path": arguments.source_path,
        "kernel": platform.uname()._asdict(),
        "gpu": command("nvidia-smi", "--query-gpu=name,driver_version,pci.bus_id,memory.total", "--format=csv"),
        "gpu_topology": command("nvidia-smi", "topo", "-m"),
        "numa_hardware": command("numactl", "--hardware"),
        "online_nodes": read_text("/sys/devices/system/node/online"),
        "node_sysfs": {path: {name: read_text(Path(path) / name) for name in ("cpulist", "meminfo")}
                       for path in sorted(glob.glob("/sys/devices/system/node/node[0-9]*"))},
        "numa_policy": command("numactl", "--show"),
        "effective_affinity": sorted(os.sched_getaffinity(0)),
        "process_status": read_text("/proc/self/status"),
        "cpuset_cpus": read_text("/sys/fs/cgroup/cpuset.cpus.effective"),
        "cpuset_memory_nodes": read_text("/sys/fs/cgroup/cpuset.mems.effective"),
        "nice_limits": dict(zip(("soft", "hard"), resource.getrlimit(resource.RLIMIT_NICE))),
        "host_transfer_observation": {
            "inventory": "docs/explore-streaming-review.md, Phase 3D host-boundary inventory",
            "allocation_trace": "MMLTK_NUMA_TRANSFER_TRACE_FILE: context, node, capacity, registration, source/receiver route",
            "matcher_trace": "MMLTK_MATCHER_TRACE_FILE: exact active cost DMA and packed assignment upload counters",
            "gdr_trace": "MMLTK_GDR_TRACE_FILE: actual mapping backend and consumer lifetime",
            "owned_pages": "functional tests verify local anonymous pages before portable CUDA registration",
            "foreign_pages": "shared file-cache, imported graphics and library-owned pages retain their actual owner's policy",
            "evidence": "This command does not execute any allocation, DMA, GDR write, or autograd consumer",
        },
        "numa_syscall_permissions": {
            "get_mempolicy": numa_policy_query(),
            "set_mempolicy_mbind_move_pages": "not probed by this read-only report; functional tests verify binding and page residency",
            "setpriority_ioprio_set_registration": "not attempted; effective policy and registration require the focused functional tests",
            "scope": "diagnostic container drops capabilities; its seccomp and limits differ from runtime/test launches",
        },
        "pci": pci_devices(),
        "memlock_bytes": dict(zip(("soft", "hard"), resource.getrlimit(resource.RLIMIT_MEMLOCK))),
        "memlock_scope": "diagnostic container limits; -1 means unlimited; production process may differ",
        "components": components(),
        "gdrcopy_devices": gdrcopy_device_capabilities(),
    }
    try:
        metadata = os.stat(arguments.compiled_file)
        if not stat.S_ISREG(metadata.st_mode):
            raise ValueError("compiled_file must be a regular file")
        report["file"] = {
            "status": "observed", "size_bytes": metadata.st_size,
            "device_major": os.major(metadata.st_dev), "device_minor": os.minor(metadata.st_dev),
            "inode": metadata.st_ino,
            "format_validation": "not attempted; this command inspects storage, not compiled contents",
        }
        report["filesystem"] = command("findmnt", "--json", "--target", arguments.compiled_file,
                                       "--output", "SOURCE,FSTYPE,TARGET,OPTIONS,MAJ:MIN")
        report["block_devices"] = command("lsblk", "--json", "--output",
                                          "NAME,MAJ:MIN,TYPE,PKNAME,MODEL,TRAN,ROTA,LOG-SEC,PHY-SEC")
        report["direct_io"] = direct_io(arguments.compiled_file)
    except (OSError, ValueError) as error:
        report["file"] = unavailable(error)
    print(json.dumps(report, indent=2))
    return 0 if report["file"]["status"] == "observed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
