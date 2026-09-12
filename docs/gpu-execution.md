# GPU-local execution and image loading

[Wiki index](README.md) · [Commands](commands.md) · [Datasets](datasets.md) · [Validation](validation.md)

The architectural resource rules are in
[CONTRACT.md](../CONTRACT.md#execution-failure-and-shutdown).
The settings below select concrete devices, placement, and loading transport.
The [GUI interaction guide](gui-interaction.md#native-gpu-custody-and-completion)
owns producer/raw/display inventories, completion, and redraw mechanics.

## Device and NUMA placement

```bash
./mmltk --gui --device-id 0 --numa-node -1
```

`--device-id` uses the CUDA-visible device index. `--numa-node -1` requests
automatic GPU-local placement. Resolution uses the GPU's PCI identity,
permitted local CPUs, and memory node. A single-node machine can resolve
unknown reported locality; on a multi-node machine, unknown locality needs an
explicit override. A node that conflicts with known GPU locality fails.

Single-device training exposes `--numa-node`. Distributed training uses
`--device-ids 0,1 --numa-nodes 0,3`, with one override per selected rank;
`-1` entries resolve automatically. A scalar node override is rejected for
multiple devices. These are fragments for the training command; use
`./mmltk rfdetr train --help` for its required dataset/model arguments.

Existing CPU eligibility constrains placement within the node. Independent
systems retain their own worker pools and may use overlapping eligible CPUs.
Workers verify fixed affinity, strict `MPOL_BIND`, high normal-scheduler
priority, and required storage I/O priority before admission. The wrapper
supplies execution capabilities and nice/memlock limits. Denied required
placement or policy is an operation failure.

Owned local host storage is page-rounded, prefaulted, residency-checked, and
registered for CUDA with reusable capacity. Foreign and file-cache pages keep
their actual ownership. CUDA stream priority does not establish DMA-engine
priority. The implementation starts in
[src/common/system](../src/common/system) and
[src/frameworks/gpu](../src/frameworks/gpu).

## H2D and GDRCopy

Compiled-image loading defaults to H2D with persistent local pinned staging
and asynchronous DMA. Use `--gdrcopy` explicitly where the command exposes
that option:

```bash
./mmltk --gui --gdrcopy
./mmltk bench --compiled ./compiled/train.bin --gdrcopy
```

Desktop startup applies its selected transport to Train, Validate, Predict,
and Explore for the session without rewriting saved settings. Explore
reconstructs its runtime when the relevant saved device, placement, or
transport configuration changes. GDRCopy selection has no automatic transport
fallback.

The GPU framework builds its private static GDRCopy library from
`third_party/gdrcopy`. Runtime notices install under
`/opt/mmltk/share/mmltk/licenses/gdrcopy`. It can use an available `/dev/gdrdrv`
or the supported CUDA DMA-BUF mmap path. The wrapper exposes a present driver
node; it does not install a kernel module. Set `GDRCOPY_USE_DMABUF_MMAP=1` to
select DMA-BUF explicitly. Backend selection is immutable for an existing
mapped allocation.

`MMLTK_GDR_TRACE_FILE` enables mapped-buffer JSONL diagnostics and is forwarded
with host-path rewriting. `MMLTK_NUMA_TRANSFER_TRACE_FILE` provides the separate
NUMA transfer trace. See [logging](logging.md) for joining captured identities.

## Shared-workspace interoperability

Firefox allocates a linear Vulkan `R8G8B8A8_UNORM` image and one independent
device-memory allocation per physical workspace slot. It exports opaque
memory and timeline-semaphore file descriptors over the existing `SCM_RIGHTS`
channel. Native CUDA imports them on the matching device and writes a pitched
linear image view. This is separate from CUDA DMA-BUF CPU mapping for GDRCopy
and from the compositor's DMA-BUF interfaces.

Firefox's [workspace integration](../third_party/firefox/gfx/wgpu_bindings/src/server.rs)
queries the actual rendering device's image format/type/tiling/usage and
opaque-FD export support. Direct sampling additionally requires linear
`SAMPLED_IMAGE` and linear-filter support with the exact external image usage.
The result supplies the physical device UUID, device incarnation, capacity,
row pitch, subresource byte offset, allocation size, alignment, memory-type
bits, and dedicated-allocation requirement. Firefox rechecks these facts
against the image it actually allocates and binds. Direct images retain legal
tracked `GENERAL` layout; unsupported sampling uses one GPU copy into the
reusable sample arena described in [presentation custody](gui-interaction.md#native-gpu-custody-and-completion).

Source admission follows this ownership order:

```text
Native producer requests capacity on the browser's rendering device
    → Firefox negotiates layout, allocates and binds independent Vulkan storage
    → Firefox completes initial external ownership and exports memory/timeline FDs
    → native producer execution owner imports the full allocation into CUDA
    → producer fills/finalizes it from authoritative raw data
    → Presentation offers the exact completed workspace for browser acquisition
```

The initial `UNDEFINED` transition occurs before producer filling. It cannot
preserve an already produced image. First admission, browser/device replacement,
or capacity growth therefore prepares a new allocation while retaining the
previous completed product. Subsequent source reads acquire/release the
external image in `GENERAL`; copied sample storage returns to its shared
shader-read layout.

[ImageWorkspace](../src/frameworks/gpu/image_workspace.h) coordinates native
admission and physical access.
[ImportedImageBuffer](../src/frameworks/gpu/imported_image_buffer.cpp) validates
the CUDA device UUID, retains the received backing FD, and gives CUDA a separate
consuming duplicate. It imports and maps the full reported allocation at offset
zero, then exposes `mapped_base + image_offset` with the negotiated row pitch.
Release frees the mapped base, destroys external memory, and only then releases
backing and context custody. Allocation size, image offset, dedicated flags,
and Vulkan alignment are distinct facts; CUDA VMM granularity is not mapping
metadata for this path.

The independent backing reference survives browser resource retirement,
replacement, and exporter process exit while native aliases or GPU work remain.
Firefox's physical image and semaphore owners likewise retain their Vulkan
device through partial construction and final destruction, even after registry
or IPC removal. Exporter exit alone proves neither GPU completion nor safe
native reuse. [Presentation lifetime](gui-interaction.md#native-gpu-custody-and-completion)
owns source-read, callback, draw, and terminal-settlement rules.

When the raw producer device differs from the display device, workspace
finalization owns the existing peer or reusable pinned transfer route and any
required receiver storage. Matching display UUIDs are required for the imported
allocation even in this case. A clean-only same-device product can write final
storage directly; products with semantic planes use the controller-bound fused
raster finalizer. Late layout readiness completes retained raw work without
repeating its domain operation. Same-GPU display performs no
GPU-to-CPU-to-GPU pixel transfer. A cross-device route without peer access may
use pinned host staging; explicitly enabled pixel probes also read back small
GPU samples. These are separate from the same-GPU display path.

The [standalone CUDA/Vulkan diagnostic](validation.md#standalone-cudavulkan-diagnostic)
exercises allocation, descriptor, timeline, pixel, and exporter-exit behavior
without a browser. It complements packaged Wayland acceptance.

## ONNX capture and verification storage

The packaged ONNX CUDA provider captures on its calling thread and stream.
Independent domain workers can allocate while another worker captures a
graph. Capture-end cleanup owns both an unpublished raw graph and executable
graph until installation succeeds, retaining the original error on failure.
Graph replay remains enabled; domain workers keep their independent resource
and execution ownership. The pinned source patch and its build fingerprint
are described in [build inputs](build.md#toolchain-and-image-inputs).

ShiftLUT operators retain their physical storage across replay and provider
fallback. Ordinary operators have no allocation-counter state or updates.
Verification callers may explicitly supply a scoped allocation counter that
outlives the operator. These internals are declared in
[shiftlut_onnx_ops.h](../src/backend/imaging/upscale/detail/shiftlut_onnx_ops.h);
the existing Upscale executable owns the real-provider capture, replay,
counter-isolation, and independent raster-oracle cases.

The capture/counter cases do not require the external raster-oracle files.
Running them establishes those specific behaviors, not the numerical coverage
of the full oracle suite. See [validation evidence](validation.md#gui-behavior-and-evidence-ownership)
and [ShiftLUT tooling](commands.md#shiftlut-model-tooling) for the respective
commands.

## Read-only capability inspection

```bash
./mmltk --diagnose-io ./compiled/train.bin > io-capabilities.json
```

This uses the existing `MMLTK_BUILD_IMAGE` (default `mmltk-build:latest`) and a
read-only dataset mount. It does not build or pull images, read file contents,
or run a benchmark. JSON includes GPU/driver and kernel details, filesystem
and block devices, PCIe paths, memory-lock limits, visible cuFile/GDRCopy
components, and `O_DIRECT` open / `STATX_DIOALIGN` observations.

Unavailable tools or inaccessible metadata are reported as unavailable.
Docker/image/GPU startup failures reach stderr before JSON can be produced.
An accepted open or installed library does not prove storage-to-GPU DMA,
functional NUMA binding, pinned allocation, or available mapping capacity.
DMA-BUF export and CPU-mmap support are reported separately per visible GPU.

For a bounded container/driver/library inventory without a dataset:

```bash
./mmltk --diagnose-gpu-environment runtime
./mmltk --diagnose-gpu-environment wayland-validation
./mmltk --diagnose-gpu-environment development
```

The default is `runtime`. Each selection requires its existing wrapper image
and a running Docker daemon; it neither builds nor pulls an image and uses no
network. The report includes image identity, kernel, GPU/driver details,
library locations and version queries, Vulkan ICD manifests, and observed
virtualization evidence. Version queries load a library in an isolated
diagnostic process without creating a CUDA context or Vulkan instance.
Available library files and that process's loader resolution do not prove
which libraries the application loaded; use the [runtime provenance records](logging.md#vulkan-diagnostics-and-descriptor-provenance)
for that evidence. The source is
[diagnose_gpu_environment.py](../tools/diagnose_gpu_environment.py).

## Functional GDR checks

During the permitted testing stage, choose each available backend/device:

```bash
./mmltk --test application-systems --executable mmltk_frameworks_gpu_tests -- '[gdr]~[hardware]'
./mmltk --test application-systems --executable mmltk_frameworks_gpu_tests \
  --env GDRCOPY_USE_DMABUF_MMAP=0 --env MMLTK_GDR_TEST_BACKEND=gdrdrv \
  --env MMLTK_GDR_TEST_DEVICE=0 -- '[gdr][hardware]'
./mmltk --test application-systems --executable mmltk_frameworks_gpu_tests \
  --env GDRCOPY_USE_DMABUF_MMAP=1 --env MMLTK_GDR_TEST_BACKEND=dmabuf \
  --env MMLTK_GDR_TEST_DEVICE=0 -- '[gdr][hardware]'
```

These check functionality without throughput timing. An unavailable
capability produces an explicit skip; it does not establish a successful
transfer. Common-system and GPU tests provide the functional policy/allocation
checks that the restricted capability report cannot prove.
