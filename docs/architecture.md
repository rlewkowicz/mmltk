# Architecture and source guide

[Wiki index](README.md) · [Quick start](../README.md#codebase) · [Build graph](build.md)

[CONTRACT.md](../CONTRACT.md) defines desired ownership, product outcomes,
resource lifetime, and shutdown behavior. This guide locates the implementation;
the contract's ownership matrix remains the authority.

## Entrypoints and application construction

| Location | Responsibility |
| --- | --- |
| [src/entrypoints/cli/cli.cpp](../src/entrypoints/cli/cli.cpp) | Dataset compile/info/benchmark commands and root dispatch |
| [src/entrypoints/cli/rfdetr_cli.cpp](../src/entrypoints/cli/rfdetr_cli.cpp) | RF-DETR command parsing and execution |
| [src/entrypoints/desktop/browser_runtime_entry.cpp](../src/entrypoints/desktop/browser_runtime_entry.cpp) | Packaged `mmltk-browser-host` startup |
| [src/entrypoints/desktop/browser_runtime_options.h](../src/entrypoints/desktop/browser_runtime_options.h) | Reflected desktop execution options |
| [src/entrypoints/tools](../src/entrypoints/tools) | ONNX inspection and simplification executables |
| [src/controller/shell/application_shell.h](../src/controller/shell/application_shell.h) | Application construction, browser loop, and system lifetime |
| [src/controller/shell/application_system_storage.h](../src/controller/shell/application_system_storage.h) | Owns the connected system instances and materialized visual-source readers |
| [src/frontend/iced/src/main.rs](../src/frontend/iced/src/main.rs) | Browser application entrypoint |
| [src/frontend/iced/src/app.rs](../src/frontend/iced/src/app.rs) | Iced app composition and mapped messages |

The root [CMakeLists.txt](../CMakeLists.txt) registers component directories;
each component's `CMakeLists.txt` declares its sources, ordinary headers,
retained modules, links, and tests. Native executable targets install as
`mmltk` and `mmltk-browser-host`. The [build reference](build.md#target-declarations-and-precompiled-headers)
owns declaration-only dependencies, header isolation, and target-local PCHs.

## Native domain work

`src/controller/subsystems/` contains product systems. Dataset/model and
prediction systems are grouped under `system/`; training, validation, export,
annotation, Explore, Live, and Upscale have their own implementation directories. Shared settings,
file dialogs, external-provider access, Firefox process ownership, and
diagnostics live under `src/controller/services/`.

The neutral [controller runtime](../src/controller/runtime/local_run.h) owns
one local job's worker, startup settlement, stop request, and join. Product
systems using it retain their own admission, mutable state, failure
translation, and event policy. Visual workers use the separate presentation
runtime described below.

[SettingsSystem](../src/controller/services/settings_system.h) owns live
settings and mutation admission.
[SettingsStore](../src/controller/services/settings_store.h) owns parsing,
repair, revision admission, durable writes, and persistence failures;
[SettingsLocation](../src/controller/services/settings_location.h) carries the
owned path bytes across worker calls.

| Workflow owner | Implementation and handoff |
| --- | --- |
| [TrainingSystem](../src/controller/subsystems/train/training_system.cpp) | Starts the sibling CLI through `TrainProcessClient`, owns run inspection/history through `TrainRunStore`, and admits checkpoint resume |
| [ValidationSystem](../src/controller/subsystems/validate/validation_system.cpp) | Owns the selected evaluation session, detailed result pages, and retained samples through `ValidationSamples` |
| [PredictSystem](../src/controller/subsystems/system/predict_system.cpp) | Owns incremental prediction, video playback control, and latest preview products |
| [ExportSystem](../src/controller/subsystems/export/export_system.cpp) | Owns model export and engine preparation |

[RF-DETR workflows](rfdetr-workflows.md) owns model/class admission, metrics,
checkpoint and history formats, and prediction behavior. Canonical
[training](../src/backend/models/rfdetr/contract/training_metrics.h) and
[evaluation](../src/backend/models/rfdetr/contract/evaluation_metrics.h)
declarations generate the browser payloads. The backend's
[catalog](../src/backend/data/catalog/class_catalog.h) supplies immutable class
identity independently of model execution.

Annotation's [input executor](../src/controller/subsystems/annotation/annotation_system.cpp)
owns the mutable document and ordered history through
[AnnotationDocument](../src/controller/subsystems/annotation/detail/annotation_document.h).
Mouse records and document commands share its native ordered queue. Its
separate GPU execution owner receives
[immutable render descriptions](../src/controller/subsystems/annotation/detail/annotation_render_state.h).
The [interaction guide](gui-interaction.md#ordered-annotation-input-and-retained-storage)
describes their retained storage and command continuations.
[annotation_persistence.cpp](../src/controller/subsystems/annotation/annotation_persistence.cpp)
owns file publication for the document owner's named CBOR representation;
document/history mutation stays with `AnnotationDocument`.

The implementation layers below those systems are:

| Directory | Contents |
| --- | --- |
| [src/backend/data](../src/backend/data) | Source compilation, compiled formats, loaders, and dataset artifacts |
| [src/backend/models/rfdetr](../src/backend/models/rfdetr) | RF-DETR contract, architecture, augmentation, export, inference, training |
| [src/backend/ml](../src/backend/ml) | Torch/CUDA layers and model-runtime integration |
| [src/backend/imaging](../src/backend/imaging) | Shared image primitives, resampling, annotation, raster, Explore rendering, and upscaling algorithms |
| [src/backend/media](../src/backend/media) | Capture and Live media implementations |
| [src/frameworks](../src/frameworks) | GPU, process, transport, serialization, and reflection facilities |
| [src/common](../src/common) | Shared types, math, I/O, concurrency, logging, and Linux system support |

Shared image declarations live below dataset and model policy:
[sampling.h](../src/backend/imaging/sampling.h) owns shared CPU/CUDA pixel-index
and RLE sampling,
[class_palette.h](../src/backend/imaging/raster/class_palette.h) owns class
colors, [image_operations.h](../src/backend/imaging/raster/image_operations.h)
owns reusable raster operations, and
[resample](../src/backend/imaging/resample) owns CPU/CUDA resizing.
[Dataset compilation](datasets.md) owns acquisition, annotations, cache
identity, progress, and format-7 output while consuming those operations.

RF-DETR's ordinary [model.h](../src/backend/models/rfdetr/core/model.h),
[model_state.h](../src/backend/models/rfdetr/core/model_state.h), and
[model_state_load.h](../src/backend/models/rfdetr/core/model_state_load.h)
expose typed model execution, decoded state ownership, and staged state
admission. The [RF-DETR source map](rfdetr-workflows.md#backend-ownership)
locates training lanes, metric handoff, snapshots, optimizer, and checkpoint
declarations. The model [registry](../src/backend/models/catalog/model_registry.cpp)
projects descriptors from canonical model-contract contributions.

Capture's ordinary [capture_session.h](../src/backend/media/capture/capture_session.h)
owns device/session access and depends on the GPU framework.
Live's [live_session_controller.h](../src/backend/media/live/live_session_controller.h)
connects capture, analysis, overlays, fanout, and compositing through private
owners under `media/live/detail/`. Its public declarations directly name
capture, annotation, ML-runtime, and GPU dependencies; raster composition
remains private to the implementation.

## Shared Linux facilities

[ScopedFd](../src/common/io/scoped_fd.h) owns descriptors, and
[event_fd.h](../src/common/io/event_fd.h) centralizes signal, drain, and
blocking worker-wait behavior. Callers choose the failure policy.
[file_memory.h](../src/common/io/file_memory.h) owns file handles and mappings;
[json_file.h](../src/common/io/json_file.h) supplies append and atomic JSON
publication; [StagingDirectory](../src/common/io/staging_directory.h) owns
temporary-directory cleanup until publication. These facilities do not own
domain persistence formats.

[runtime_paths.h](../src/common/system/runtime_paths.h) resolves repository,
executable, installation, and packaged asset paths for native consumers.
[subprocess_utils.h](../src/frameworks/process/subprocess_utils.h) owns shared
process execution support. Test-only filesystem, process, CUDA, asynchronous
wait, console, and CLI-option helpers live under
[src/test_support](../src/test_support); domain fixtures remain with their
components.

## Native/Rust boundary

Start at
[application_schema.h](../src/controller/browser/application_schema.h),
[application_materializer.h](../src/controller/browser/application_materializer.h),
and the generator targets in
[src/controller/browser/CMakeLists.txt](../src/controller/browser/CMakeLists.txt).
They connect canonical native facts to the browser protocol and generated
typed Rust. Generated definitions are consumed through
[generated.rs](../src/frontend/iced/src/generated.rs); the generated files
themselves remain build output.

The [outer-routing emitter](../src/controller/browser/application_outer_routing_emitter.h)
derives system/endpoint identities, snapshot/event/reply variants, decoding,
bootstrap completeness, and exhaustive dispatch to Rust projection traits.
[view_model/reduction.rs](../src/frontend/iced/src/view_model/reduction.rs)
calls that generated dispatch; the owning modules under `view_model/`
implement the traits and their presentation-state reductions.
The [visual projection](../src/controller/presentation/visual_source_projection.h)
declares each producer's frame/revision relation and image projection;
schema materialization derives native readers and
[generated Rust observations](../src/controller/browser/application_visual_projection_emitter.h)
from the same facts.

The separate
[application_workspace_abi_emitter.h](../src/controller/browser/application_workspace_abi_emitter.h)
projects the native graphics records into a data-only Rust artifact consumed
by Firefox. It derives field types, enum values, and size/alignment/offset
assertions from the canonical native declarations in
[presentation/abi](../src/controller/presentation/abi). This graphics ABI is
independent of the application's CBOR package protocol; [build outputs](build.md#generated-bindings-and-dependency-maintenance)
locates both artifacts and their invalidation rules.

The physical listener and transport owner is
[BrowserServer](../src/frameworks/transport/browser_server.h).
Application-level dispatch and snapshots live under
`src/controller/browser/`. On the Rust side, `application_codec.rs`,
`transport_connection.rs`, and `view_model/` handle typed records, connection
behavior, and state reduction. UI layout, styling, navigation, and component
messages live in `app/`, `view/`, and the owning widgets. Use
[generation commands](build.md#generated-bindings-and-dependency-maintenance)
after changing the native schema.

The shared [page canvas](../src/frontend/iced/src/view/mod.rs) and
[workflow compositor](../src/frontend/iced/src/view/workflow/mod.rs) own page
width, scrolling, and ordinary column composition, including Annotate.
[navigation.rs](../src/frontend/iced/src/view/navigation.rs) owns visual order.
[workflow/fields.rs](../src/frontend/iced/src/view/workflow/fields.rs) supplies
typed numeric widgets; Explore's local controls retain their domain-specific
filter editing. The [interaction guide](gui-interaction.md#workflow-layout-and-navigation)
owns the layout and input policies.

The [GUI interaction guide](gui-interaction.md#typed-application-boundary)
owns the current protocol, compact input representation, native command
ordering, and state-publication details. The physical transport ring lives in
[browser_record_ring.h](../src/frameworks/transport/browser_record_ring.h);
shared mouse capture and immediate submission live in
[workspace_input.rs](../src/frontend/iced/src/workspace_input.rs), with ordered
outbound retention in
[transport_connection.rs](../src/frontend/iced/src/transport_connection.rs).
Canonical records come from
[contracts/workspace_input.h](../src/controller/contracts/workspace_input.h);
the shared native queue is
[presentation/workspace_input.h](../src/controller/presentation/workspace_input.h).

## Presentation and browser integration

[src/controller/presentation](../src/controller/presentation) contains native
source selection, workspace admission/publication, shared visual-worker
support, and visual diagnostics. Final display products belong to Explore,
Annotation, Predict, Validate, Live, and Upscale. Predict's implementation is in
[predict_system.cpp](../src/controller/subsystems/system/predict_system.cpp).

Validation adds its own retained atlas/detail producer through
[validation_samples.cpp](../src/controller/subsystems/validate/detail/validation_samples.cpp).
Train's charts are ordinary Iced drawing and use no native image workspace.

[VisualRuntimeOwner](../src/controller/presentation/visual_runtime_owner.h)
runs dirty work and completion continuations on each producer's worker.
[SystemImageRuntime](../src/frameworks/gpu/system_image_runtime.h) owns retained
product storage and counted reads;
[ImageWorkspace](../src/frameworks/gpu/image_workspace.h) owns native custody
of a final Vulkan allocation and its immutable display layout;
[ImportedImageBuffer](../src/frameworks/gpu/imported_image_buffer.h) retains
its CUDA mapping, context, and independent backing descriptor. The controller's
[finalization policy](../src/controller/presentation/visual_runtime.cpp)
connects these GPU owners to the raster backend. The GPU framework has no
reverse dependency on that backend.

Explore's
[GalleryThumbnailCache](../src/controller/subsystems/explore/detail/gallery_thumbnail_cache.h),
[GalleryStream](../src/controller/subsystems/explore/detail/gallery_stream.h),
and [GalleryAtlas](../src/controller/subsystems/explore/detail/gallery_atlas.h)
own retained thumbnail identity, disk/GPU admission, and allocation-local cell
meaning respectively. [Dataset loading and Explore residency](datasets.md#explore-thumbnails-and-atlas-residency)
explains their handoffs.

The Rust browser-image boundary is
[presentation_surface.rs](../src/frontend/iced/src/presentation_surface.rs)
and its child modules. Firefox allocation/export and WebGPU/Vulkan integration
live in the owned [third_party/firefox](../third_party/firefox) tree, including
`gfx/wgpu_bindings/src/server.rs`. Iced owns view transforms and rendering;
its stable graphics binding acquires the latest completed workspace with paired
image metadata, independently of application snapshot delivery.
[metadata.rs](../src/frontend/iced/src/presentation_surface/metadata.rs)
validates that opaque payload against generated native types.
Physical Vulkan image and semaphore owners retain device custody beyond
registry and IPC removal.
The vendored Iced [primitive resource batch](../third_party/iced/wgpu/src/primitive.rs)
retains sampled resources through the actual encoder's submission or
abandonment; it carries no application schema or selection policy.
[workspace_fps.rs](../src/frontend/iced/src/workspace_fps.rs) owns the optional
component meter, observing actual queue submissions separately from settlement.

For lifetime and synchronization invariants, follow the contract's
[GPU buffer flow](../CONTRACT.md#gpu-buffer-flow) and
[shutdown rules](../CONTRACT.md#execution-failure-and-shutdown).
Detailed [input, buffer, and presentation mechanics](gui-interaction.md) have
one home. The [logging guide](logging.md) explains diagnostic activation and
how captures connect these boundaries. Frontend acceptance control lives in
`integration_control.rs`; its private `integration_control/reporting.rs`
owns effect-only collection and reporting.

The retained Train plotting component is
[view/metrics.rs](../src/frontend/iced/src/view/metrics.rs), using the owned
[iced_plot](../third_party/iced_plot) crate. Validation's
[results](../src/frontend/iced/src/view/validate/results.rs) and
[samples](../src/frontend/iced/src/view/validate/samples.rs) components own their
local UI interactions. The primary-action preparation state belongs to
[app/workflows.rs](../src/frontend/iced/src/app/workflows.rs); native runtime
execution remains with each system. The real-model acceptance scenario has its
own [workflow driver](../src/frontend/iced/src/integration_control/workflows.rs).

## Tests and vendor changes

Tests are generally colocated with their owning component; shared acceptance
scenarios and the packaged Wayland integration suite live under
[src/acceptance](../src/acceptance). Neutral support lives in
`src/test_support`; Explore, Upscale, Live, Annotation, and presentation each
register their own tests and domain fixtures. The
[validation ownership map](validation.md#gui-behavior-and-evidence-ownership)
locates those suites; [packaged acceptance](validation.md#packaged-wayland-acceptance)
locates the Wayland session/audit owners. Wrapper suite
selection is defined in `mmltk`, with native target registration in component
CMake files.

`third_party` is maintained as part of this codebase, including the owned
Firefox and Iced changes. It retains upstream notices and per-project build
policies. Development and review procedures are in [AGENTS.md](../AGENTS.md);
available checks are described in [validation](validation.md).
