# GPU-local execution and image loading

[Wiki index](README.md) · [Commands](commands.md) · [Datasets](datasets.md) · [Validation](validation.md)

[CONTRACT.md](../CONTRACT.md#execution-failure-and-shutdown) owns architectural
resource rules. This reference owns device/placement/transport and physical
allocation details; [GUI interaction](gui-interaction.md#native-gpu-custody-and-completion)
owns producer/raw/display inventories, redraw, and presentation settlement.

## Workflow device selection

Train, Validate, Export, and Predict share the [GPU selector](gui-interaction.md#gpu-selection)
and retain independent persisted choices:

| Workflow | Setting |
| --- | --- |
| Train | `workflows.train.request.device_ids`, ordered ranks |
| Validate | `workflows.validate.request.device_id` |
| Predict | `workflows.predict.request.device_id` |
| Export | `workflows.export_state.device_id` |

[CudaDeviceFact](../src/frameworks/gpu/cuda/device_inventory.h) supplies CUDA
ordinal/name/total VRAM. Shell discovers process-visible inventory once;
`SettingsSystem` retains immutable `SettingsUiState.cuda_devices` outside saved
settings, projected to Rust. CUDA ordinals may differ from NVML/`nvidia-smi`;
compare physical identities across tools. Missing saved devices fail admission.

[Request materialization](../src/controller/subsystems/system/compute_intent_materializer.cpp)
captures workflow selection: Validate/Predict use saved NUMA overrides; Export
uses automatic locality. Validation source compilation uses its selected device.
`DirectComputeConfiguration` retains admitted placement/worker policy. Model
preparation/inference/ONNX export/TensorRT building use that compute device.
Device-dependent TensorRT inspection requires reinspection after device change;
weight/ONNX inspection remains reusable.

Train preserves rank/device/NUMA order in child commands, including one remaining
rank. Remote-provider requests retain their own policy. Desktop `--gpu`
(`--device-id`) selects native visual execution, independently of saved workflows
and Firefox's Wayland graphics device. [Workspace interoperability](#shared-workspace-interoperability)
handles differing producer/receiver/graphics devices. [Training](rfdetr-training.md#logical-lanes-and-global-batch)
owns DDP replication, per-GPU memory, logical lanes/batches, and physical capacity.

## Device and NUMA placement

```bash
./mmltk --gui --gpu 0 --numa-node -1
```

`-1` resolves PCI identity, eligible local CPUs, and memory node. Single-node
hosts may resolve unknown locality; multi-node unknown locality needs an explicit
override. Conflicting known GPU/node locality fails. Single-device train exposes
`--numa-node`; distributed uses `--device-ids 0,1 --numa-nodes 0,3`, one override
per rank (`-1` automatic). Scalar overrides fail for multiple devices. Consult
`./mmltk rfdetr train --help` for required training arguments.

Existing CPU eligibility constrains node placement. Independent systems own
pools and may overlap eligible CPUs. Workers verify fixed affinity, strict
`MPOL_BIND`, high normal-scheduler priority, and required storage I/O priority.
Wrapper capabilities/nice/memlock limits support admission; denied policy fails.
Transfer host pages are page-rounded, prefaulted, residency-checked, then CUDA
registered with reusable capacity. [Compiler arenas](compile-execution.md#fixed-pinned-arenas)
use `NumaMemory`/`mlock` without CUDA. Foreign/file-cache pages retain actual
ownership. Stream priority does not establish DMA-engine priority. Sources:
[common/system](../src/common/system), [frameworks/gpu](../src/frameworks/gpu).

Each training rank resolves/preloads independently. [Loader ownership](rfdetr-training.md#logical-lanes-and-global-batch)
owns sharing/cancellation; [compiled loading](datasets.md#loading-a-compiled-file)
owns physical reads/upload/consumer retirement. Runtime prefault/gathers use
existing reader pools with per-stream task ceilings, never compiler class floors.

## H2D and GDRCopy

[H2D](datasets.md#default-h2d-path) is default. Select [GDRCopy](datasets.md#gdrcopy-path)
explicitly where exposed:

```bash
./mmltk --gui --gdrcopy
./mmltk bench --compiled ./compiled/train.bin --gdrcopy
```

Desktop transport applies to Train/Validate/Predict/Explore for the session
without rewriting settings. Explore recreates runtime on relevant saved device/
placement/transport changes. Unsupported GDRCopy fails without fallback.
[Prepared batches](datasets.md#prepared-batch-storage) retain maximum host/device
pixels, used-prefix labels with exact header RAM pitch, and metadata before reads;
GDR retains optional CPU-access host space. Hidden GUI controls do not remove
saved policy or change Predict's fixed batch.

The private static library builds from `third_party/gdrcopy`; notices install at
`/opt/mmltk/share/mmltk/licenses/gdrcopy`. Mapping can use `/dev/gdrdrv` or CUDA
DMA-BUF mmap. Wrapper exposes an existing node without installing a module.
`GDRCOPY_USE_DMABUF_MMAP=1` explicitly selects DMA-BUF; an allocation's backend
cannot change. `MMLTK_GDR_TRACE_FILE` enables mapped-buffer JSONL with wrapper
path rewriting; `MMLTK_NUMA_TRANSFER_TRACE_FILE` is separate. [Logging](logging.md)
owns correlation.

## Workflow runtime retirement

[Training lifetimes](rfdetr-training.md#attempt-execution-and-target-lifetime)
separate pinned metadata/mask DMA from targets consumed through backward.
Count/gradient work joins metric completion before optimizer mutation.
Augmentation parameter upload and final image/donor consumption also retire
separately; submission alone proves no physical completion.

Validate/Export/Predict reuse only identical captured configurations.
[CudaRuntimeResources](../src/controller/subsystems/system/detail/cuda_runtime_resources.cpp)
retires actual sessions/streams: bind owner device → prove completion → close
session → destroy stream → restore caller device. Successful retirement is
idempotent and seals that session; ordinary settled failures allow safe retry.
Unproved completion/release/context restoration retains the exact aggregate in
bounded terminal custody and seals workflow admission, including partial
construction. Terminal leases reserve before work; destruction/diagnostics do
not prove safety.

Session close retires count/readback/annotation storage.
[PinnedHostBuffer](../src/frameworks/gpu/memory/pinned_host_buffer.cpp) shares
terminal authority across owner/output/tensor views; explicit or last-borrower
unregister/context failures retain storage at that authority.
[PredictionCountStorage](../src/backend/models/rfdetr/inference/prediction_count.cpp)
shares one registered batch allocation; compatible unborrowed storage retains
capacity, while borrowers/device changes require independent writable storage.

[ValidationSampleOutput](../src/controller/subsystems/validate/detail/validation_sample_output.cpp)
shares retirement authority across receivers, flushes saved-image writes before
changed headless replacement, and keeps valid graphics receivers fixed. Frames
retain context/storage independently of pools; [sample lifecycle](rfdetr-workflows.md#evaluation-metrics-and-retained-samples)
progresses without rendering.

## Inference lanes and borrowed inputs

[InferenceLanes](../src/backend/models/rfdetr/core/inference_lanes.h) bounds streams,
lane-local storage, and ordered completion. Validate/Predict/training validation
own independent settings. Admission respects source population/backend batch
capacity; single-image prediction uses one lane. Physical capacity does not
multiply logical batch.

Work can finish out of order, but callbacks/saves deliver in source order;
pressure waits for oldest result. Native weights may share tensor storage while
execution caches stay lane-local. ONNX bindings are per lane; TensorRT shares
engine with independent contexts/streams. Reuse requires artifact, weight kind,
shape, precision, batch, and capacity match. Training models remain independent.

Release loader borrows after the last source read/copy physically completes,
before unrelated inference where possible; producer observes that event to avoid
prefetch-credit/lane cycles. Video frames borrowed until decoder advance copy
into lane input; decoder stream waits before advancing. Pause stops admission;
Stop/errors/partial setup/callback failure drain work before release. Training
validation drains before weight restoration/mutation/merge/checkpoint.
[EvaluationRunOwner](../src/backend/models/rfdetr/training/evaluation_run_owner.cpp)
destroys matcher/runtime tensors before lane pool because allocator retirement
can still reference its streams.

## Prediction preview storage

[PredictionPreviewPool](../src/controller/subsystems/system/detail/prediction_preview.cpp)
retains source representation: RGB8 uses three bytes/pixel in pinned/device
storage and [converts directly](../src/backend/imaging/raster/chw_image.cu) to
pitched opaque RGBA; CHW float retains its conversion. Box/run alignment survives
either layout. Admission checks the float-sized aggregate limit before compact
allocation. Reuse retains representation after decoded release; all owners
survive their copy/conversion completion. This is separate from model inputs and
[prediction-mask readback](rfdetr-workflows.md#incremental-prediction).

Replacement resolves execution before free-slot mutation; pre-submission refusal
preserves receiver allocation/source. Frames expose actual receiver device.
[Raster](../src/backend/imaging/raster/raster_cuda.cu) scans overwrite-only box/digit/
analysis-mask writers backward to the last matching writer; blended masks and
Validation additive GT/Det preserve forward composition.

## Checkpoint and export readbacks

[TensorReadbackBuffers](../src/backend/ml/cuda/tensor_readback.h) reserves a complete
ordinal tensor inventory for each immutable snapshot. CUDA copies use pinned
GPU-local host storage/per-device streams and finish before CPU archive/export
reads; views release before reuse. Session drain precedes checkpoint staging;
readers finish before mutation. [Session/candidate identities](model-merging.md#artifact-layout-and-formats)
remain distinct and EMA updates stay on GPU.

[Torch stream scope](../src/backend/ml/cuda/detail/torch_cuda_scope.cpp) establishes
a driver context on new threads before pinned/tensor work, including reused
Torch stream tables. [Image context](../src/frameworks/gpu/image/image_buffer.cpp)
balances isolated-context creation's stack entry before normal binding/retirement.
Actual context/completion governs use, not device index alone.
[Perceptual resizing](datasets.md#optional-perceptual-downscaling) owns separate
CUDA scratch/custody.

## Shared-workspace interoperability

Firefox owns two persistent foreground slots, each a linear Vulkan
`R8G8B8A8_UNORM` image with independent memory, separate from raw-product pools.
Opaque memory/timeline FDs cross `SCM_RIGHTS`; matching-device CUDA imports a
pitched view. This differs from GDR CPU DMA-BUF mapping and compositor DMA-BUF.

[Firefox integration](../third_party/firefox/gfx/wgpu_bindings/src/server.rs)
checks actual device format/type/tiling/usage/export support. Direct sampling
also needs linear sampled-image/filter capability for exact external usage.
Negotiation supplies UUID/incarnation, capacity, pitch, image offset, allocation
size/alignment/type bits/dedicated requirement, rechecked on actual image binding.
Direct images retain tracked `GENERAL`; unsupported sampling uses a bounded
reusable GPU [sample arena](gui-interaction.md#native-gpu-custody-and-completion).

```text
Native capacity request on rendering device
→ Firefox layout negotiation / independent allocation / binding
→ initial external transition and exported memory/timeline FDs
→ producer imports full allocation / fills from authoritative raw data
→ Presentation publishes completed image with paired opaque metadata
→ browser binding acquires latest completed image locally
```

Initial `UNDEFINED` transition precedes filling and cannot preserve prior pixels.
First admission/replacement/growth prepares new storage while retaining last
completed product. Later source ownership exchanges use `GENERAL`; copied sample
storage returns to shader-read layout.

[ImageWorkspace](../src/frameworks/gpu/image/image_workspace.h) admits physical
access. [ImportedImageBuffer](../src/frameworks/gpu/image/imported_image_buffer.cpp)
checks CUDA UUID, retains backing FD, and passes a consuming duplicate to CUDA.
Full allocation maps at zero; image view is `base + image_offset` with negotiated
pitch. Release frees mapped base, destroys external memory, then backing/context.
Allocation size/offset/dedicated flags/Vulkan alignment differ from CUDA VMM
granularity, which is not mapping metadata here.

Owner-thread completion/settlement/cancelled writes wake display availability
after unlocking only when physical storage is available. Held reads retain custody;
next dirty render/admission resumes on real availability. Product/display wake
callbacks are separate; detach emits its wake independently. Exceptions are
contained and wakes grant no access. Backing survives browser retirement/exit
while aliases/work remain. Firefox partial-construction/registry/IPC/destruction
owners retain Vulkan devices. Exit proves neither completion nor reuse safety;
[presentation custody](gui-interaction.md#native-gpu-custody-and-completion)
owns final source/callback/draw/terminal settlement.

[SystemImageRuntime](../src/frameworks/gpu/runtime/system_image_runtime.cpp) binds
before model release and destruction; failed binding/incomplete release retains
model/context/stream with cause. Live [slot claims](../src/backend/media/live/detail/live_slot_state.h)
make at most one compare/exchange; callers still own waits/publication and must
prove CUDA completion.

Different raw/display devices use workspace-owned peer transfer or reusable
pinned staging/receiver storage; imported UUID still matches display device.
Clean-only same-device work may fill final storage directly; semantic planes use
controller fused raster. Late layout readiness finalizes retained raw work without
repeating domain operations. Same-GPU display stays on GPU. Host staging for
non-peer cross-device routes and explicit small pixel probes are separate paths.

### Browser canvas descriptors and submission custody

The compositor canvas uses separate DMA-BUF storage. Firefox retains the ordinary
wgpu-validated descriptor and HAL projection, including usage, zero initialization,
and view formats, without another handwritten mapping. Recycle requires same
Vulkan device and complete descriptor.

Canvas accepts `BGRA8Unorm`, 2D, one layer/mip/sample. Modifier selection intersects
compositor/driver support for exact extent, HAL usage, `ALIAS` creation flags,
and view formats; it requires DMA-BUF import/export and **1–3 planes**. Unsupported
capability returns a controlled API error; allocation retains Vulkan status.

Each handle retains signal semaphore/exported FD. Unsent signals may leave the
pending queue; accepted submissions retain a separate reference through their
exact fence. Completion worker services accepted work while idle/after later API
errors. Handle drop/recycle releases only its custody, without an empty submission
or dropping-thread wait. Timeline completion uses a second `VkSubmitInfo` after
canvas signals in the same `vkQueueSubmit`, not a second queue call. Fence
completion releases the submission reference; final owner destroys semaphore/
device custody without a reference cycle. [Signal diagnostics](logging.md#physical-presentation-evidence)
observe those boundaries.

### Display transfer coverage

`ImageWorkspaceDamage` retains **64 transitions**, each/query at most **8
rectangles**. Overlaps merge conservatively; touching rectangles merge if union
is rectangular. Overflow coarsens to enclosing rectangle, then extends it.
Empty partial changes retain an empty rectangle for finalizer validation.
Missing chains/owner changes/full damage require full coverage.

Workspace validates exact allocation, initialized extent, prior owner/revision;
missing/incompatible baseline forces full fill. Cross-device partial transfer
clips each plane, retaining coordinates/pitches. Pinned staging packs combined
region bytes. Raw borrows survive transfer; scratch survives final display.
[Raster finalizer](../src/controller/presentation/visual_runtime.cpp) uses the same
span with ≤8 submissions. These savings concern native transfer/finalization:
Firefox copy fallback still copies full retained arena capacity; direct sampling
needs no copy. Source-read/draw settlement is unchanged. Public raw
`ImageProductBuffer::CopyFrom` still makes complete receiver-owned products.
Sources: [image_buffer.cpp](../src/frameworks/gpu/image/image_buffer.cpp),
[image_workspace.cpp](../src/frameworks/gpu/image/image_workspace.cpp).

The [standalone diagnostic](validation.md#standalone-cudavulkan-diagnostic)
covers allocation/descriptors/timelines/pixels/exporter exit without browser;
packaged Wayland acceptance remains separate.

## Upscale input preparation

[UpscaleSystem](../src/controller/subsystems/upscale/upscale_system.cpp) copies
clean/semantic source planes into receiver-owned storage before releasing borrows.
All methods use `restored_upscale_input_extent`:

| Native facts | Prepared geometry |
| --- | --- |
| Explicit Stretch, known source dimensions, no partial crop | Keep matching aspect; otherwise expand one axis using ceiling division, shrinking neither |
| Letterbox, including rounded content filling canvas | Keep canvas/content/padding |
| Missing provenance/source size, or separate partial content | Keep native geometry |

Methods scale prepared extent **4×**. A 432×432 Stretch frame from 16:9 prepares
768×432 then outputs 3072×1728, without allocating source resolution. Complete
extent/content/device bounds admit before work.

`UpscaleAlgorithm` reuses prepared clean storage on its context; changed extent
uses GPU bilinear, unchanged uses copied input. Identity includes clean content,
source extent/provenance/prepared extent. Methods share preparation; semantic-only
changes preserve clean work. Semantics use nearest sampling; continuous document
geometry maps native-to-actual output. Cancellation/completion retain all custody.

[UpscaleImageMetadata](../src/controller/subsystems/upscale/upscale_system.h)
pairs `input` native provenance, `prepared_extent`/`prepared_content`, and `frame`
completed 4× geometry retaining original source extent. Generated Rust
[decoding](../src/frontend/iced/src/presentation_surface/metadata.rs) validates
pairing/prepared/scaled bounds; [Original display/import](gui-interaction.md#original-view-and-annotation-import)
uses paired geometry independently of processing.

## Upscale warm execution

Controller admits one proactive warm per method with explicit
`ImageUpscalerPurpose::Warm`; it computes a complete image. Only
[ONNX](../src/backend/imaging/upscale/image_upscaler_onnx.cpp) adds executions to
reach three-inference graph readiness after tiling, reusing the last valid fixed
tile binding without preparation/stitch. Ordinary/graph-disabled work has no tail.
Basic/ONNX/TensorRT retain arithmetic/storage/cancellation rules. TensorRT records
final completion/consumer wait per touched lane, even preparation-only cancellation;
stream order protects repeated fixed-buffer use.

## Model backend precision

Tensor dtype, autocast, multiplication precision, and compiler options differ.
Native RF-DETR disables cuBLAS/cuDNN TF32; FP16 permission may choose capable
hardware's BF16, so evidence records actual precision. Default SDPA remains
accelerated; [selective compilation](rfdetr-training.md#native-selective-compilation)
owns explicit local math/replay guards.

[RuntimeBackendOptions](../src/backend/ml/runtime/backend_factory.h)
defaults `allow_fp16=true`, `allow_tf32=true`. [ONNX CUDA](../src/backend/ml/runtime/backend_onnx.cpp)
enables graph optimization and explicitly forwards TF32 permission as provider
`use_tf32`. [TensorRT](../src/backend/ml/runtime/tensorrt_runtime.cpp) forwards
permission to its builder, clearing TF32 when disabled; TensorRT 11 uses
model-defined precision, so FP16 permission alone does not lower FP32 graphs.
Nominal FP32 output does not establish equal arithmetic. The
[independent oracle](validation.md#independent-upstream-diagnostic) controls and
reports comparison precision separately.

## ONNX capture and verification storage

Packaged ONNX CUDA captures on calling thread/stream, allowing independent workers
to allocate concurrently. Failed capture-end installation cleans unpublished raw/
executable graphs while retaining original cause. Replay remains enabled with
independent domain ownership. [Build inputs](build.md#toolchain-and-image-inputs)
own the pinned patch/fingerprint.

The [runtime backend](../src/backend/ml/runtime/backend_onnx.cpp) retains the
session's actual `CudaPinned` allocator. Close clears bindings and output values,
destroys the session, then settles its canonical CUDA stream before releasing
that allocator on the ordinary owner thread. Session destruction can enqueue
deferred CPU frees; their callbacks must finish before final pinned-host release.
Backend release failures propagate through `RuntimeBackend::Close` before lane
closure. [Release diagnostics](logging.md#onnx-session-release) observe this
sequence only when native trace logging is enabled.

ShiftLUT storage persists across replay/fallback. Ordinary operators allocate no
counter state; verification can supply a scoped counter outliving the operator.
[shiftlut_onnx_ops.h](../src/backend/imaging/upscale/detail/shiftlut_onnx_ops.h)
declares these internals. Upscale tests own real-provider capture/replay,
counter-isolation, and independent raster-oracle cases. Capture/counter cases need
no external oracle and prove no full-oracle coverage. See [tooling](commands.md#shiftlut-model-tooling)
and [evidence ownership](validation.md#gui-behavior-and-evidence-ownership).

## Read-only capability inspection

```bash
./mmltk --diagnose-io ./compiled/train.bin > io-capabilities.json
```

[I/O diagnostic](../tools/diagnostics/diagnose_dataset_io.cpp) uses existing
`MMLTK_BUILD_IMAGE` (default `mmltk-build:latest`) and read-only dataset mount,
without build/pull/payload read/benchmark. JSON covers CPU/GPU/driver/kernel/NUMA,
UUID/VRAM, DRM-to-PCI nodes, filesystems/block/PCIe paths, memlock, cuFile/GDRCopy,
`O_DIRECT` open, and `STATX_DIOALIGN`.

`cpu_topology` includes eligible CPU/package/core, SMT, available core-type/capacity,
frequency/governor/energy policy, and hybrid `cpu_core`/`cpu_atom` membership.
Missing fields mean unavailable, not homogeneous hardware. `filesystem_capacity`
is an instantaneous `statvfs` total/free/available/fragment sample, separate from
[compiler transactions](compile-execution.md#file-preparation-and-settlement).
Unrelated writers may immediately consume space.

Unavailable tools/metadata are explicit; Docker/image/GPU startup failures reach
stderr before JSON. Successful open/library discovery proves no direct storage
DMA, functional binding/pinning, or mapping capacity. Per-GPU DMA-BUF export and
CPU-mmap capabilities are separate.

For a bounded inventory without a dataset:

```bash
./mmltk --diagnose-gpu-environment runtime
./mmltk --diagnose-gpu-environment wayland-validation
./mmltk --diagnose-gpu-environment development
```

Default `runtime` requires its existing wrapper image/running daemon; no build,
pull, or network. Report contains image/kernel/GPU/driver/library/version/Vulkan
ICD/virtualization facts. Isolated library queries create neither CUDA context nor
Vulkan instance. Files/query loader resolution do not prove application-loaded
libraries; [runtime provenance](logging.md#vulkan-diagnostics-and-descriptor-provenance)
does. [Source](../tools/diagnostics/diagnose_gpu_environment.cpp) uses the
[tools closure](build.md#native-tools-and-checkout-bootstrap) mounted read-only;
only query children dynamically load libraries, not ordinary host-driver startup.

## Functional GDR checks

Choose available backend/device for functional, untimed checks:

```bash
./mmltk --test application-systems --executable mmltk_frameworks_gpu_tests -- '[gdr]~[hardware]'
./mmltk --test application-systems --executable mmltk_frameworks_gpu_tests \
  --env GDRCOPY_USE_DMABUF_MMAP=0 --env MMLTK_GDR_TEST_BACKEND=gdrdrv \
  --env MMLTK_GDR_TEST_DEVICE=0 -- '[gdr][hardware]'
./mmltk --test application-systems --executable mmltk_frameworks_gpu_tests \
  --env GDRCOPY_USE_DMABUF_MMAP=1 --env MMLTK_GDR_TEST_BACKEND=dmabuf \
  --env MMLTK_GDR_TEST_DEVICE=0 -- '[gdr][hardware]'
```

Unavailable capability explicitly skips, never implies successful transfer.
Common/GPU functional suites prove policy/allocation behavior that inspection
alone cannot establish.
