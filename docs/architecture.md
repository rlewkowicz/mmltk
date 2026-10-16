# Architecture and source guide

[Wiki index](README.md) · [Quick start](../README.md#codebase) · [Build graph](build.md)

[CONTRACT.md](../CONTRACT.md) defines architecture and product guarantees. This
page locates implementation owners; linked references define their behavior.

## Entrypoints and application construction

| Owner | Responsibility |
| --- | --- |
| [Native wrapper](../tools/mmltk.cpp), [tools CMake](../tools/CMakeLists.txt) | Container operations and the independent [tools graph](build.md#standalone-tools-module-graph) |
| [Root CLI](../src/entrypoints/cli/cli.cpp), [RF-DETR CLI](../src/entrypoints/cli/rfdetr_cli.cpp) | Command admission and execution; reflected options in the adjacent `cli_options.h` and `rfdetr_cli_options.h` |
| [Desktop entrypoint](../src/entrypoints/desktop/browser_runtime_entry.cpp) | Packaged `mmltk-browser-host`; options in `browser_runtime_options.h` |
| [Tool entrypoints](../src/entrypoints/tools) | ONNX inspection/simplification and benchmark diagnostics |
| [Application shell](../src/controller/shell/application_shell.h), [system storage](../src/controller/shell/application_system_storage.h) | Construction, connections, browser loop, system lifetime and visual-source readers |
| [Iced main](../src/frontend/iced/src/main.rs), [app](../src/frontend/iced/src/app.rs) | Browser entrypoint, composition and mapped messages |

Root CMake registers component directories. Each component declares its own
sources, headers, retained modules, links and tests. Product executables install
as `/opt/mmltk/bin/mmltk` and `/opt/mmltk/bin/mmltk-browser-host`, independently
of the wrapper. [Build declarations](build.md#target-declarations-and-precompiled-headers)
own declaration-only dependencies, header isolation and target-local PCHs.
CLI startup captures logging overrides once, including sibling ONNX-tool policy;
validation and backend preflight precede device work. Prediction retains each
source name and ordinal when moving paths into the admitted request.

## Native domain work

`src/controller/subsystems/` contains product systems. Dataset, model and
prediction live under `system/`; training, validation, export, annotation,
Explore, Live and Upscale have dedicated directories. Shared services live in
`src/controller/services/{file_dialog,settings,training,firefox,vast,diagnostics}`.
The [local runtime](../src/controller/runtime/local_run.h) owns worker startup,
stop and join; systems retain admission, state, failures and events.

| System | Mutable responsibility |
| --- | --- |
| [Settings](../src/controller/services/settings/settings_system.h) | Live settings and mutation admission; immutable shell-supplied CUDA inventory remains outside persisted preferences |
| [Settings store](../src/controller/services/settings/settings_store.h) | Parsing, repair, revision admission and durable writes; `SettingsLocation` owns path bytes across worker calls |
| [Dataset](../src/controller/subsystems/system/dataset_system.cpp) | Captured compile/inspect requests, cancellation and retained `ArtifactStore` runtime |
| [Model](../src/controller/subsystems/system/model_system.cpp) | Complete selection identity, cancellable preparation, and admitted detector or training-only encoder facts |
| [Training](../src/controller/subsystems/train/training_system.cpp) | Sibling CLI via `TrainProcessClient`; history/Resume via `TrainRunStore` |
| [Validation](../src/controller/subsystems/validate/validation_system.cpp) | Evaluation, metrics, retained samples and captured saving |
| [Prediction](../src/controller/subsystems/system/predict_system.cpp) | Incremental inference, playback, previews and source-specific output |
| [Export](../src/controller/subsystems/export/export_system.cpp) | Requested ONNX/TensorRT chain and publication |
| [Annotation](../src/controller/subsystems/annotation/annotation_system.cpp) | Ordered input, document and history; a separate GPU owner consumes immutable render descriptions |

[Workflows](rfdetr-workflows.md) defines admission, artifacts and metrics;
[GUI interaction](gui-interaction.md) defines presentation. Annotation's
[document](../src/controller/subsystems/annotation/detail/annotation_document.h)
owns mutation/history; [persistence](../src/controller/subsystems/annotation/annotation_persistence.cpp)
publishes its named CBOR representation. [GPU selection](gpu-execution.md#workflow-device-selection)
defines device inventory and runtime replacement.

| Backend directory | Responsibility |
| --- | --- |
| [data](../src/backend/data) | Benchmark recipes/acquisition, compiled storage, compilation and loading |
| [models/rfdetr](../src/backend/models/rfdetr) | Model contract, architecture, augmentation, training, inference and export |
| [ml](../src/backend/ml) | Torch/CUDA layers and runtime integration |
| [imaging](../src/backend/imaging) | Sampling, resize, annotation, raster, Explore and upscale primitives |
| [media](../src/backend/media) | Capture, Live, video decode, timing/audio, encoding and muxing |
| [frameworks](../src/frameworks) | GPU, processes, transport, serialization and reflection |
| [common](../src/common) | Types, math, I/O, concurrency, logging and Linux support |

Shared imaging stays below dataset/model policy: [sampling](../src/backend/imaging/sampling.h)
owns affine points, normalized pixel centers, pixel indices and RLE sampling;
[raster](../src/backend/imaging/raster) owns class colors and reusable painting;
[resample](../src/backend/imaging/resample) owns resize geometry and RGB methods.
Compilers transform pixels, continuous boxes and categorical masks; ordinary
file-image prediction also consumes RGB resize. Readers expose stored geometry,
annotation meaning and provenance without depending on model policy.

### Dataset owners

[Datasets](datasets.md), [benchmark recipes](benchmark-datasets.md) and
[compile execution](compile-execution.md) define formats, policy, resource
admission and publication. Source ownership is:

| Owner | Responsibility |
| --- | --- |
| [compiled_file.h](../src/backend/data/compiled_file.h), [FileLayout](../src/backend/data/compiled/compiled_file_layout.h) | Shared format/validation surface and checked block/payload arithmetic |
| [CompiledSlotStorage](../src/backend/data/detail/compiled_slot_storage.h), [compactor](../src/backend/data/compiled/compiled_file_compaction.cpp) | Borrowed writes and occupied ranges in the staged inode; common finalization for Directory and benchmark output |
| [CompiledDataset](../src/backend/data/compiled/compiled_dataset.h), [DatasetLoader](../src/backend/data/loading/dataset_loader.h) | Bounded resident metadata, mapped image views, eligible-ID schedules and leased prepared stream slots |
| [CompileExecution](../src/backend/data/detail/compile_execution.h), [FileWorkQueue](../src/backend/data/detail/file_work_queue.h) | Fixed pinned arenas, allowances, bounded CPU/file work and physical completion |
| [CompilePixelWorkspace](../src/backend/data/detail/compile_pixel_workspace.h) | Checked prepared CPU decode/resize with front-end-specific codec policy |
| [BenchmarkCompilePipeline](../src/backend/data/benchmark/detail/benchmark_pipeline.h) | Source/image joins, claims, withdrawal, drain and separate bounded image/output file services sharing the CPU budget |
| [BenchmarkImagePublication](../src/backend/data/benchmark/detail/benchmark_image_publication.h) | Encoded-input/cache-write custody for generic extraction and Coconut |
| [BenchmarkDatasetSelection](../src/backend/data/benchmark/benchmark_dataset_options.h) | Canonical recipe, validation and recovery defaults; generated settings project it, Rust owns controls |
| [Compile progress](../src/backend/data/compiler/dataset_compile_progress.h) | Acquisition, labels/masks, pixels and source observations; revisioned [application projection](application-wire.md#dataset-operation-snapshots) |

Recipes own membership and recovery; Curl owns transport, archives retain
opened generations, split writers own placement/sealing, and recipe/artifact
services choose final publication. Pixels and labels advance independently.
Coconut's `benchmark/coconut/detail/` separates parsed records/import limits
(`coconut_input.h`), import continuations (`coconut_import.h`), component products
(`coconut_annotations.h`), shared identities (`coconut_annotation_identity.h`),
reflected inventory and checked I/O (`coconut_inventory*.h`), and immutable
range/block custody (`coconut_staging.h`). `CoconutRecipeInputs` owns original
providers and final source reconciliation. `BenchmarkMappedArtifact` combines
immutable mappings with neutral `StagedArtifact` ownership.

The shell supplies `RuntimeDiagnosticTarget` by value to the artifact runtime;
a borrowed observer exists only during synchronous Compile. Data code consumes
trace callbacks without a controller dependency. [CompileTraceSelection](../src/backend/data/compile_trace_selection.h)
and [execution observation](../src/backend/data/detail/compile_execution_observer.h)
carry demand separately from scheduling/resources. [Logging](logging.md#benchmark-compilation-traces)
owns disablement, delivery and shutdown.

### Model and media owners

RF-DETR's [model](../src/backend/models/rfdetr/core/model.h),
[decoded state](../src/backend/models/rfdetr/core/model_state.h) and
[state admission](../src/backend/models/rfdetr/core/model_state_load.h) are ordinary
public boundaries. The [model registry](../src/backend/models/catalog/model_registry.cpp)
projects canonical descriptors. [Workflow source ownership](rfdetr-workflows.md#backend-ownership)
locates training lanes, metrics, snapshots, optimizer and checkpoints.
[Training](rfdetr-training.md) owns equations/execution; [merging](model-merging.md)
owns synchronization, selection and Resume.

[Encoder asset declarations](../src/backend/models/rfdetr/contract/encoder_pretraining.h)
own immutable source and provenance facts; the preset catalog selects the family.
[ArtifactStore](../src/controller/services/artifact_store.cpp) owns shared
[acquisition/cache publication](build.md#model-asset-cache), while the native
[encoder importer](../src/backend/models/rfdetr/core/encoder_pretraining.cpp)
owns complete tensor admission and staged adaptation into the model-owned encoder.
Training owns initialization order and distributed agreement. These boundaries
keep acquisition out of the backend and resource/execution policy out of reflected
provenance. [Training initialization](rfdetr-training.md#pretrained-encoder-initialization)
defines exact behavior.

[GpuBatchPreprocessor](../src/backend/models/rfdetr/core/gpu_batch_preprocessor.h)
owns reusable normalized inference input; [postprocess](../src/backend/models/rfdetr/core/postprocess.h)
owns physical ranking, class projection and query-aligned outputs;
[evaluator](../src/backend/models/rfdetr/core/evaluator.h) owns box/mask/OKS
accumulation. [Input and selection](rfdetr-workflows.md#model-input-and-detection-selection)
distinguishes model candidate defaults from evaluation limits. Immutable class
and joint meaning comes from the [data catalog](../src/backend/data/catalog/class_catalog.h).

[CaptureSession](../src/backend/media/capture/capture_session.h) depends on the
GPU framework. [LiveMediaDataPlane](../src/backend/media/live/live_session_controller.h)
connects capture, analysis, overlays, fanout and compositing through private
`media/live/detail/` owners. `LiveSystem` captures Predict's prepared model and
settings at Start; its native adapter supplies a model-neutral analysis factory
on the media owner's stream. Raster composition remains private.

## Workflow output and media handoffs

[WorkflowOutputSelection/Facts](../src/controller/contracts/workflow_output.h)
separate configured destinations from admitted directories, committed artifacts,
samples and partial-video facts. [Run reservation](../src/controller/services/training/run_output.h)
is a shared filesystem utility; systems retain admission/failure policy.
`TrainRunStore` additionally owns history/Resume. [ExportRunRequest](../src/controller/subsystems/export/export_run.h)
retains independent format choices through one weights→ONNX→engine chain.

Rust's [PendingStart](../src/frontend/iced/src/app/workflows.rs) captures typed
validation/prediction saving options before preparation; accepted runs retain
them independently of later viewer edits. Shared [model](../src/frontend/iced/src/view/workflow/model_card.rs),
[Output](../src/frontend/iced/src/view/workflow/output.rs) and
[GPU](../src/frontend/iced/src/view/workflow/gpu.rs) cards own selection interaction;
Train history and Predict saving remain local components.

[ValidationSampleOutput](../src/controller/subsystems/validate/detail/validation_sample_output.h)
copies borrowed captures into immutable receiver-owned products; interactive
samples can adopt them. [PredictionOutput](../src/controller/subsystems/system/detail/prediction_output.h)
owns samples/full video and publication, with bounded subset/reservoir selection.
Both reuse `PredictionPreviewComposition`; display admission is optional.
[PredictionDelivery](../src/backend/models/rfdetr/inference/prediction_delivery.h)
separates semantic/JSON, preview and saving demand, retaining borrowed reads
until copies finish. Timing/audio travels through the same inference pass.

[VideoFileSource](../src/backend/media/video/video_file_source.h) owns demux,
decode and timestamps/audio. [VideoFileSink](../src/backend/media/video/video_file_sink.h)
owns encoder admission, conversion, bounded packet custody, audio remux and MKV
publication. Media depends on image/GPU facilities rather than RF-DETR/GUI.
[CaptionRaster](../src/backend/imaging/raster/caption_raster.h) owns native glyph
painting; [RenderedImageWriter](../src/backend/imaging/raster/rendered_image_writer.h)
owns bounded PNG work, pinned transfer, staging and atomic publication. Iced
retains interactive caption layout. [Workflow output](rfdetr-workflows.md#prediction-samples-and-full-video)
owns names, sampling and interrupted-output behavior; [build inputs](build.md#native-caption-and-video-assets)
own fonts and encoder dependencies.

## Shared native facilities

| Facility | Contract |
| --- | --- |
| [ScopedFd](../src/common/io/scoped_fd.h), [eventfd](../src/common/io/event_fd.h) | Descriptor custody; EINTR-safe counter reads report count, byte count and captured error; callers choose short/empty/failure policy |
| [ScopedEventCancellation](../src/common/concurrency/event_cancellation.h) | Bridges stop tokens to borrowed/consumed domain cancellation; settles and destroys its callback before owned token storage |
| [CancellationObservation](../src/common/types/cancellation_observation.h) | Borrowed observation, without execution-source ownership |
| [File memory](../src/common/io/file_memory.h) | Handles, mappings, checked interrupted/short positional/vectored I/O and parent creation (`.` for bare filenames); staged owners choose allocation/reclaim policy |
| [WorkspaceBuffer](../src/common/types/workspace_buffer.h), [NumaMemory](../src/common/system/numa_memory.h) | Bounded caller-owned scratch views; NUMA allocation and optional page-lock custody |
| [JSON files](../src/common/io/json_file.h), [staging](../src/common/io/staging_directory.h), [publication](../src/common/io/path_publication.h) | Append/atomic JSON and temporary cleanup; domains retain content sync, cancellation, destination and format policy |
| [File digest](../src/common/io/file_digest.cpp) | Snapshot identity/cancellation checks and overwrite scratch bounded to 16 MiB; hashes only completed reads |
| [Runtime paths](../src/common/system/runtime_paths.h), [subprocess support](../src/frameworks/process/subprocess_utils.h) | Repository/install/assets resolution and process execution |
| [Test support](../src/test_support) | Neutral filesystem, process, CUDA, asynchronous wait, console and CLI helpers; domain fixtures stay colocated |

[UTF-8 recognition](../src/common/types/utf8.h) shares scalar rules and bounded
AVX2 ASCII scanning. Visual diagnostics replace malformed bytes with U+FFFD and
truncate between scalars; benchmark diagnostic paths use bounded flagged repair.
Native paths/failures remain intact. JSON rejects invalid text; CBOR reports
`InvalidUtf8`, including segmented/borrowed input. Repair and wire rejection are
separate policies. [Logging](logging.md#fatal-stderr-reports) owns optional native
logging and initialization-independent fatal stderr; entrypoints choose exit
policy. The opt-in profiling registry allocates a metric-name key only on first
insertion.

## Reflected value and persistence boundaries

[Reflection authoring](reflection.md) owns registration, annotations and CLI
syntax. [Reflected descriptors](../src/frameworks/reflection/reflected_descriptors.h)
parse/validate scalar, optional and repeatable values before mutation; repeatable
admission checks declared item limits before physical `inplace_vector` capacity.
Rejected input preserves the destination. Shared field traits serve compile-time
and runtime checks.

[CBOR](../src/frameworks/serialization/cbor_wire.h) supports non-consuming
lookahead and direct copies/borrows across at most two segments. Owned leaves
move into containers. [Reflected decoding](../src/frameworks/serialization/reflected_cbor_detail.h)
uses compile-time named-key indexes; validation follows base/member declaration
order, then the first unknown key in wire order. Opaque relations preserve wire
order. [Application records](application-wire.md#record-shapes) distinguish
positional transport from named persistence. Encoding measures before reserving,
checks borrowed map keys as UTF-8, and preserves depth/item/error order. Browser
records and Rust map keys move owned values rather than copying them.

[Torch archive CBOR](../src/backend/ml/torch/archive_cbor.h) provides bounded
reflected records inside byte-tensor archive entries. Its ordinary owner checks
CPU byte type, rank and capacity, retains tensor storage through decoding, and
normalizes contiguous storage unless the caller requires it. Model state,
continuation and checkpoint owners retain entry names, domain limits, optionality,
migrations and errors. The separate [build target](build.md#target-declarations-and-precompiled-headers)
keeps this shared storage mechanism out of unrelated Torch dependencies.

[Settings persistence](../src/controller/contracts/gui_settings.cpp) structurally
projects same-name source/UI/training/loading fields and complete
`ExploreViewState`, including inherited loading and `show_labels`; renamed keys,
repair and conversions remain explicit. Benchmark rejection counters derive
binary/JSON projections from one declaration under their [format guards](benchmark-datasets.md#cache-formats-and-capacity).
Coconut's external JSON/Parquet declarations retain separate format conversion;
parsing, joins and resource policy remain ordinary data owners.
[Preset lookup](../src/backend/models/rfdetr/contract/preset_catalog.h) accepts
exact case-sensitive canonical/upstream names and declared 2XLarge aliases;
[model configuration](../src/backend/models/rfdetr/contract/model_config.cpp)
separately owns weight-filename/path inference.

## Native/Rust boundary

[Application wire](application-wire.md) owns schema inputs, generators, typed
records and compatibility. [Reduction](../src/frontend/iced/src/view_model/reduction.rs)
calls generated dispatch; owning `view_model/` modules implement presentation
traits. [Visual projection](../src/controller/presentation/visual_source_projection.h)
derives source readers and observations from canonical frame/revision facts.
[Visual frames](../src/controller/presentation/visual_system_types.h) pair geometry
and optional resize provenance with each product. Upscale separately projects
native input, prepared input and output; Rust owns Original view and transforms.

Native dispatch/snapshots live in `src/controller/browser/`; [BrowserServer](../src/frameworks/transport/browser_server.h)
owns the listener/physical transport. Rust `application_codec.rs`,
`transport_connection.rs` and `view_model/` own records, connection and reduction;
`app/`, `view/` and widgets own interaction/layout. The chart catalog selects
generated scalar identities with checked nested/array access, optional values
and nonnumeric metadata. [Build generation](build.md#generated-bindings-and-dependency-maintenance)
owns separate application/Firefox artifacts and invalidation.

Shared frontend owners are page/workflow layout (`view/mod.rs`, `view/workflow/`),
visual order (`view/navigation.rs`), typed numeric fields (`workflow/fields.rs`),
retained disclosures (`shared/transition.rs`), fitted text (`shared/status_text.rs`)
and Dataset progress (`train/dataset/progress.rs`). `view/metrics/` separates
catalog, bounded history and retained plots; `train/progress.rs` displays native
live facts. `view_model/notices.rs` owns session notices; `view/status.rs` owns
focus, clipboard and display. [GUI interaction](gui-interaction.md) defines their
behavior. Mouse records come from [canonical workspace input](../src/controller/contracts/workspace_input.h),
with Rust capture in `workspace_input.rs`, ordered retention in
`transport_connection.rs`, and native queues in `presentation/workspace_input.h`.

## Presentation and browser integration

`src/controller/presentation/` owns routing, admission/publication, visual workers
and diagnostics. Explore, Annotation, Predict, Validate, Live and Upscale own
final products; Train charts are ordinary Iced drawing. [VisualDocument](../src/controller/presentation/visual_document.h)
preserves continuous coordinates and independent mask support during import.
[VisualRuntimeOwner](../src/controller/presentation/visual_runtime_owner.h) runs
event-driven dirty work and continuations on each producer worker.

The GPU framework groups CUDA/context/retirement in `cuda/`, pinned/GDRCopy
storage in `memory/`, images/interop in `image/`, and runtime/model/workers in
`runtime/`. [SystemImageRuntime](../src/frameworks/gpu/runtime/system_image_runtime.h)
owns retained products/countable reads; [ImageWorkspace](../src/frameworks/gpu/image/image_workspace.h)
owns native final-allocation custody/layout; [ImportedImageBuffer](../src/frameworks/gpu/image/imported_image_buffer.h)
retains CUDA mapping, context and backing descriptor. Controller finalization
connects them to raster code without a reverse GPU-framework dependency.
Explore's thumbnail cache, stream, atlas and read scheduler retain distinct
identity, coordination, allocation-local meaning and disk/GPU admission;
[dataset residency](datasets.md#explore-thumbnails-and-atlas-residency) defines their handoffs.

Rust's [presentation surface](../src/frontend/iced/src/presentation_surface.rs)
and [metadata decoder](../src/frontend/iced/src/presentation_surface/metadata.rs)
retain paired images/facts. Owned Firefox integration lives in
`third_party/firefox`: `WebGPUParent.cpp` serializes binding changes,
`gfx/wgpu_bindings/src/server.rs` owns generations/dispatcher custody, and
`mmltk_workspace_channel.rs` owns exclusive channel claims. Only an explicitly
bound actor claims a workspace channel. Ordinary WebGPU remains independent.
Physical Vulkan images/semaphores retain device custody beyond IPC removal.
The completion worker retires semaphores at the actual submission fence;
Iced's [primitive resource batch](../third_party/iced/wgpu/src/primitive.rs)
retains encoded resources through submission or abandonment.

[GUI input/presentation](gui-interaction.md) owns ordering, retained images and
transforms; [GPU execution](gpu-execution.md) owns allocation/transfers;
[logging](logging.md) owns fatal process classification and diagnostics.
`integration_control.rs` is frontend acceptance control; its `reporting.rs`
collects effect-only evidence. Train uses owned `third_party/iced_plot`;
Validate's results/samples remain local UI components.

## Tests and vendor changes

Tests are colocated with owners; [acceptance](../src/acceptance) contains shared
scenarios and packaged Wayland integration. [Validation](validation.md#gui-behavior-and-evidence-ownership)
maps suites, fixtures and evidence. Component CMake registers native tests;
`tools/mmltk.cpp` selects wrapper suites. [Tool fixtures](validation.md#native-tool-fixtures)
use the independent tools runner. Owned Firefox/Iced changes retain upstream
notices and per-project build policies; [AGENTS.md](../AGENTS.md) defines working rules.
