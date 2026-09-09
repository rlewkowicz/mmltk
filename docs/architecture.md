# Architecture and source guide

[Quick start](../README.md#codebase) · [Build graph](build.md) · [GPU execution](gpu-execution.md)

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

The physical listener and transport owner is
[BrowserServer](../src/frameworks/transport/browser_server.h).
Application-level dispatch and snapshots live under
`src/controller/browser/`. On the Rust side, `application_codec.rs`,
`transport_connection.rs`, and `view_model/` handle typed records, connection
behavior, and state reduction. UI layout, styling, navigation, and component
messages live in `app/`, `view/`, and the owning widgets. Use
[generation commands](build.md#generated-bindings-and-dependency-maintenance)
after changing the native schema.

## Presentation and browser integration

[src/controller/presentation](../src/controller/presentation) contains native
source selection, final composition, exported-backbuffer publication, and
visual diagnostics. Producer products originate in their domain systems.

The Rust browser-image boundary is
[presentation_surface.rs](../src/frontend/iced/src/presentation_surface.rs)
and its child modules. Firefox import and WebGPU/Vulkan integration live in
the owned [third_party/firefox](../third_party/firefox) tree, including
`gfx/wgpu_bindings/src/server.rs`. Iced remains responsible for displaying and
transforming completed browser images.

For lifetime and synchronization invariants, follow the contract's
[GPU buffer flow](../CONTRACT.md#gpu-buffer-flow) and
[shutdown rules](../CONTRACT.md#execution-failure-and-shutdown).
The [logging guide](logging.md) explains how captures connect these boundaries.

## Tests and vendor changes

Tests are generally colocated with their owning component; shared acceptance
support and the packaged Wayland integration suite live under
[src/acceptance](../src/acceptance). Wrapper suite selection is defined in
`mmltk`, with native target registration in component CMake files.

`third_party` is maintained as part of this codebase, including the owned
Firefox and Iced changes. It retains upstream notices and per-project build
policies. Development and review procedures are in [AGENTS.md](../AGENTS.md);
available checks are described in [validation](validation.md).
