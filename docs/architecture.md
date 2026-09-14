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
| [src/frontend/iced/src/main.rs](../src/frontend/iced/src/main.rs) | Browser application entrypoint |
| [src/frontend/iced/src/app.rs](../src/frontend/iced/src/app.rs) | Iced app composition and mapped messages |

The root [CMakeLists.txt](../CMakeLists.txt) registers component directories;
each component's `CMakeLists.txt` declares its sources, ordinary headers,
retained modules, links, and tests. Native executable targets install as
`mmltk` and `mmltk-browser-host`.

## Native domain work

`src/controller/subsystems/` contains product systems. Dataset/model and
compute systems are grouped under `system/`; training, annotation, Explore,
Live, and Upscale have their own implementation directories. Shared settings,
file dialogs, external-provider access, Firefox process ownership, and
diagnostics live under `src/controller/services/`.

Annotation's [input executor](../src/controller/subsystems/annotation/annotation_system.cpp)
owns the mutable document and ordered history through
[AnnotationDocument](../src/controller/subsystems/annotation/detail/annotation_document.h).
Mouse records and document commands share its native ordered queue. Its
separate GPU execution owner receives
[immutable render descriptions](../src/controller/subsystems/annotation/detail/annotation_render_state.h).
The [interaction guide](gui-interaction.md#ordered-annotation-input-and-retained-storage)
describes their retained storage and command continuations.

The implementation layers below those systems are:

| Directory | Contents |
| --- | --- |
| [src/backend/data](../src/backend/data) | Source compilation, compiled formats, loaders, and dataset artifacts |
| [src/backend/models/rfdetr](../src/backend/models/rfdetr) | RF-DETR contract, architecture, augmentation, export, inference, training |
| [src/backend/ml](../src/backend/ml) | Torch/CUDA layers and model-runtime integration |
| [src/backend/imaging](../src/backend/imaging) | Annotation, raster, Explore rendering, and upscaling algorithms |
| [src/backend/media](../src/backend/media) | Capture and Live media implementations |
| [src/frameworks](../src/frameworks) | GPU, process, transport, serialization, and reflection facilities |
| [src/common](../src/common) | Shared types, math, I/O, concurrency, logging, and Linux system support |

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

The separate
[application_workspace_abi_emitter.h](../src/controller/browser/application_workspace_abi_emitter.h)
projects the native graphics records into a data-only Rust artifact consumed
by Firefox. It derives field types, enum values, and size/alignment/offset
assertions from the canonical native declarations. This graphics ABI is
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
Annotation, Predict, Live, and Upscale. Predict's implementation is in
[compute_systems.cpp](../src/controller/subsystems/system/compute_systems.cpp).

[VisualRuntimeOwner](../src/controller/presentation/detail/visual_runtime_owner.h)
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

## Tests and vendor changes

Tests are generally colocated with their owning component; shared acceptance
support and the packaged Wayland integration suite live under
[src/acceptance](../src/acceptance). Wrapper suite selection is defined in
`mmltk`, with native target registration in component CMake files.

`third_party` is maintained as part of this codebase, including the owned
Firefox and Iced changes. It retains upstream notices and per-project build
policies. Development and review procedures are in [AGENTS.md](../AGENTS.md);
available checks are described in [validation](validation.md).
