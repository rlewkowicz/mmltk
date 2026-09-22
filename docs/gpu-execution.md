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

Train, Validate, and Predict hide the H2D/NUMA widgets; their saved settings,
desktop startup overrides, and native execution remain supported. Explore
retains its controls. GUI prediction's fixed batch size 1 is independent of
transport selection.

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

## Prediction preview storage

[PredictionPreviewPool](../src/controller/subsystems/system/detail/prediction_preview.cpp)
captures the source representation with each retained preview. Decoded RGB8
uses three bytes per pixel in pinned upload staging and device storage; the
[raster converter](../src/backend/imaging/raster/chw_image.cu) copies those RGB
bytes directly into pitched RGBA with opaque alpha. Captured CHW float sources
retain their existing conversion. Boxes and run storage keep their required
alignment after either pixel layout.

Admission still checks the established float-sized aggregate limit before
choosing compact physical storage. Reuse retains the representation after the
decoded source is released; decoded ownership, pinned staging, and device
storage remain protected until their respective copies and conversions settle.
This preview path is independent of model-input normalization and
[encoded prediction-mask readback](rfdetr-workflows.md#incremental-prediction).

The [raster backend](../src/backend/imaging/raster/raster_cuda.cu) scans
overwrite-only box/digit and analysis-mask colors from the last object backward,
stopping at the final matching writer for each pixel. Blended mask passes and
Validation's additive GT/Det composition retain their forward composition work.

## Checkpoint and export readbacks

[TensorReadbackBuffers](../src/backend/ml/cuda/tensor_readback.h) owns reusable
ordinal-addressed storage for one immutable serialization snapshot. It reserves
the next complete tensor inventory before staging. CUDA sources use pinned
GPU-local host storage and per-device streams; copies complete before CPU
archive or export readers consume them. Those readers release their views
before storage is reused. Checkpoint saves share the ordinary/EMA snapshot
across their artifacts; enabled EMA updates themselves stay on the GPU.

The shared [Torch stream boundary](../src/backend/ml/cuda/detail/torch_cuda_scope.cpp)
establishes a driver context before pinned-host or tensor work on a newly
started thread, including reuse after Torch's stream tables already exist.
The [image context owner](../src/frameworks/gpu/image_buffer.cpp) balances
isolated-context creation's stack entry before ordinary binding and retirement.
Context identity and physical completion, rather than a reported device index
alone, govern safe resource use.

Optional perceptual resizing has a separate reusable CUDA owner with explicit
source/destination custody and bounded workspace. Its color, geometry, and
CPU/CUDA contracts are documented with
[dataset resizing](datasets.md#optional-perceptual-downscaling).

## Shared-workspace interoperability

Firefox supplies two persistent foreground display slots, each with a linear
Vulkan `R8G8B8A8_UNORM` image and independent device-memory allocation. These
are separate from retained raw-product pools. It exports opaque
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
    → Presentation publishes the completed image and paired opaque metadata
    → the stable browser binding acquires the latest completed image locally
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

Owner-thread completion, settlement, and cancelled writes wake display
availability after releasing the workspace lock, only when physical storage
is available. A held display read keeps its custody through these notifications.
The next dirty render or pending admission resumes on that real availability
edge. Product wake callbacks remain separate from display-availability
callbacks; final detach emits its display wake independently of the ordinary
availability gate. Callback exceptions are contained, and a wake itself grants
no read or write permission.

The independent backing reference survives browser resource retirement,
replacement, and exporter process exit while native aliases or GPU work remain.
Firefox's physical image and semaphore owners likewise retain their Vulkan
device through partial construction and final destruction, even after registry
or IPC removal. Exporter exit alone proves neither GPU completion nor safe
native reuse. [Presentation lifetime](gui-interaction.md#native-gpu-custody-and-completion)
owns source-read, callback, draw, and terminal-settlement rules.

[SystemImageRuntime](../src/frameworks/gpu/system_image_runtime.cpp) binds its
execution context before model release and again before destroying the released
model. Failed binding or incomplete release retains the model, context, and
stream custody with the failure. Live's private
[slot-state operations](../src/backend/media/live/detail/live_slot_state.h)
share a retirement claim that makes at most one compare/exchange attempt.
Callers retain their completion waits and publication policy; the claim itself
does not establish CUDA completion.

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

### Display transfer coverage

`ImageWorkspaceDamage` retains 64 content transitions with at most eight
rectangles per transition and accumulated query. Overlapping rectangles merge
conservatively; touching rectangles merge when their union is rectangular.
Separated changes retain separate coverage. Exceeding the rectangle capacity
coarsens that set to one enclosing rectangle; later additions extend it.
An empty partial change keeps one empty rectangle so finalizer validation still
runs. A missing transition chain, owner change, or full-image change requires
full coverage.

`ImageWorkspace` validates damage against the exact display allocation,
initialized extent, and prior product owner/revision. Missing or incompatible
baseline facts require a full fill. For admitted partial coverage on a different
device, its private transfer storage copies only the clipped regions of each
plane, retaining original coordinates and pitches for final composition.
The pinned route packs those regions into retained per-plane staging sized for their
combined bytes. Raw readers remain held through transfer settlement; transfer
scratch remains held through final display completion.

The controller's [raster finalizer](../src/controller/presentation/visual_runtime.cpp)
consumes the same region span, with at most eight region submissions. These
savings apply to native transfer and finalization. Firefox's capability-copy
fallback still copies the retained arena's full capacity; sparse native damage
does not establish a valid baseline in an alternate browser sample slot.
Direct sampling performs no such copy. Neither route changes source-read or
draw settlement requirements.

This partial storage stays inside the workspace finalization boundary.
Public `ImageProductBuffer::CopyFrom` operations continue to produce complete
receiver-owned raw products. The implementation is in
[image_buffer.cpp](../src/frameworks/gpu/image_buffer.cpp) and
[image_workspace.cpp](../src/frameworks/gpu/image_workspace.cpp).

The [standalone CUDA/Vulkan diagnostic](validation.md#standalone-cudavulkan-diagnostic)
exercises allocation, descriptor, timeline, pixel, and exporter-exit behavior
without a browser. It complements packaged Wayland acceptance.

## Upscale input preparation

The [Upscale controller](../src/controller/subsystems/upscale/upscale_system.cpp)
copies the selected native frame and its clean/semantic planes into receiver-owned
storage before releasing the source borrow. All three methods share the same
preparation policy, defined by `restored_upscale_input_extent`:

| Native input facts | Prepared geometry |
| --- | --- |
| Explicit Stretch provenance, known source dimensions, and no partial crop | Retain matching aspect; otherwise expand one axis with ceiling division to enclose the native extent at source aspect, shrinking neither |
| Letterbox, including rounded content that fills the canvas | Retain the native canvas, content rectangle, and padding |
| Missing resize provenance or source dimensions, or a separate partial content rectangle | Retain native geometry |

The method's four-times scale applies to this prepared extent. For example,
432×432 Stretch input from a 16:9 source prepares 768×432 pixels, then produces
3072×1728. The source image's original resolution is not allocated. Checked
extent/content arithmetic and the device's output bounds admit the complete
result before work begins.

`UpscaleAlgorithm` owns the reusable prepared clean buffer on its execution
context. A changed extent uses the existing GPU bilinear raster operation;
unchanged geometry uses the copied input directly. Reuse follows clean-content
identity, source extent, resize provenance, and prepared extent. Method switches
can share preparation; semantic-only revisions preserve compatible clean work.
Semantic planes scale with nearest sampling, and continuous document geometry
projects from the native input to the actual output. The method's ordinary
completion and cancellation boundaries retain all preparation and consumer
custody.

Canonical [UpscaleImageMetadata](../src/controller/subsystems/upscale/upscale_system.h)
keeps the geometries distinct:

| Field | Meaning |
| --- | --- |
| `input` | Exact native source frame, including compiled content, source extent, and resize provenance |
| `prepared_extent`, `prepared_content` | Geometry submitted to the selected method |
| `frame` | Completed output, at four times the prepared extent/content, with the original source extent retained |

Reflection projects these facts and checked scaling into Rust. The
[graphics metadata decoder](../src/frontend/iced/src/presentation_surface/metadata.rs)
checks the native source pairing, prepared bounds, and scaled output together.
The [Original display and Annotation import rules](gui-interaction.md#original-view-and-annotation-import)
use the paired geometry independently of processing.

## Upscale warm execution

The [Upscale controller](../src/controller/subsystems/upscale/upscale_system.cpp)
admits proactive warming once per method and passes an explicit
`ImageUpscalerPurpose::Warm` through the native execution request. A warm request
computes one complete image. The [ONNX runtime](../src/backend/imaging/upscale/image_upscaler_onnx.cpp) alone
supplies any remaining executions needed to reach its three-inference graph
readiness threshold after tiling, reusing the last valid fixed tile binding
without another image preparation or stitch. Ordinary requests and graph-disabled
fallback have no readiness tail.

Basic, ONNX, and TensorRT keep their existing arithmetic, storage, cancellation,
and completion rules. TensorRT records a final completion event and consumer
wait for each touched lane, including a lane that reached only preparation
before cancellation; its stream orders repeated use of that lane's fixed buffers.

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
