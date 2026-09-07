# GDRCopy source provenance

Upstream: https://github.com/NVIDIA/gdrcopy
Commit: `fcec3ce0bb40a97a6cc45dd4afeec4bccb509712`

Imported with `git archive` from that commit in the local `../gdrcopy`
checkout. The import contains every tracked source, license, and upstream
packaging/test file, without Git metadata or generated build products. The
sibling checkout is not a build or runtime input. `LICENSE` retains the
upstream license; individual upstream files retain their copyright notices.

Local changes:

- `src/Makefile`: private static archive target, preserving C and per-ISA flags.
- `src/gdrapi.c`: once-only logging/CPU dispatch; serialized control operations
  and handle lists; conservative atomic mapping-type history for fences;
  atomic exported descriptor close-on-exec verification and failure cleanup;
  safe failed unmap/unpin; DMA-BUF START/END CPU-access protocol and correct
  default cacheability classification; compiler barriers on ARM/POWER fences.
- `src/gdrapi_internal.h`: atomic bounded debug-once counters and per-mapping
  backend identity for DMA-BUF CPU-access synchronization.
- `src/cuda_wrapper.c`: synchronized reference-counted CUDA function-table
  lifetime, runtime `libcuda.so.1`, correct mmap capability attribute (152),
  and null table after failed initialization.
- `src/cuda_wrapper.h`: declare the CUDA 13.3 mmap capability attribute.

Independent established mappings copy concurrently through upstream's dispatch,
alignment handling, and fences. A mapping's owner must exclude its own retirement
from copies. A process that has used multiple cacheability types conservatively
retains the strong mixed-mapping fence thereafter, even if one type retires.

Only the userspace archive is built by the application, in build-directory
staging with hidden symbols and archive symbol exclusion at final linkage. The
upstream driver sources and benchmark programs are retained as provenance, not
built or installed. The application does not load donor `libgdrapi.so`.
Backend selection remains gdrdrv first or CUDA DMA-BUF, respecting the startup
`GDRCOPY_USE_DMABUF_MMAP=1` override. No kernel-module installation is required
when the selected GPU and driver support DMA-BUF mmap.

Backend corrections follow NVIDIA's [CUDA memory API](https://docs.nvidia.com/cuda/cuda-driver-api/group__CUDA__MEM.html),
[CUDA 13.3 release notes](https://docs.nvidia.com/cuda/cuda-toolkit-release-notes/index.html),
and [open kernel DMA-BUF export](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/main/kernel-open/nvidia/nv-dmabuf.c).
CUDA mmap support is distinct from generic DMA-BUF export support. Exported
NVIDIA descriptors are created atomically with `O_CLOEXEC`; the local check
rejects a descriptor that does not retain that property.
