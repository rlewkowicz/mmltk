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

Producer display workspaces use the existing
[ExportedImageBuffer](../src/frameworks/gpu/exported_image_buffer.cpp) CUDA VMM
allocation with a POSIX opaque file descriptor, imported by Firefox as memory
for a linear Vulkan `R8G8B8A8_UNORM` image. Native writers receive a pitched
linear device view. This route is distinct from CUDA DMA-BUF CPU mapping for
GDRCopy and from the compositor's DMA-BUF interfaces. It does not import an
arbitrary CUDA pointer or use a Vulkan-exported optimal image.
The CUDA VMM `PINNED` allocation property here has a device location; it is
device backing, separate from [host-page pinning](datasets.md#explore-thumbnails-and-atlas-residency).

Firefox's [workspace integration](../third_party/firefox/gfx/wgpu_bindings/src/server.rs)
queries the actual device's image format/type/tiling/usage and opaque-FD import
support. Its result supplies the physical device UUID, device incarnation,
capacity, row pitch, subresource byte offset, required allocation size,
alignment, memory-type requirements, and dedicated-allocation requirement.
The native [ImageWorkspace](../src/frameworks/gpu/image_workspace.h) validates
the device and allocation layout and creates its exportable allocation on the
matching CUDA device. Capacity and byte arithmetic must fit both APIs; Firefox
rechecks the actual image's pitch, offset, alignment, memory-type and dedicated
requirements before import.

Source admission follows this ownership order:

```text
Firefox queries layout for the sample arena's device and capacity
    → producer creates an unpublished exportable workspace
    → Firefox imports it and completes initial Vulkan ownership
    → producer fills/finalizes it from authoritative raw data
    → Presentation borrows and publishes the exact completed workspace
```

The initial `UNDEFINED` transition occurs before producer filling. It cannot
preserve an already produced image. First admission, browser/device replacement,
or capacity growth therefore prepares a new allocation while retaining the
previous completed product. Subsequent source reads acquire/release the
external image in `GENERAL`; the browser sample returns to its shared
shader-read layout after copying. Source and sample storage retain separate
lifetimes as described in [presentation custody](gui-interaction.md#native-gpu-custody-and-completion).

When the raw producer device differs from the display device, workspace
finalization owns the existing peer or reusable pinned transfer route and any
required receiver storage. Matching display UUIDs are required for the imported
allocation even in this case. A clean-only same-device product can write final
storage directly; products with semantic planes use the controller-bound fused
raster finalizer. Late layout readiness completes retained raw work without
repeating its domain operation.

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
