# Multi Model Loader Toolkit

A real-time interface for training, prediction, annotation, augmentation, and composition of popular open-source models.

## Current State

### Questionable License

PyTorch and CUDA take many, many hours to build. NVIDIA publishes a container with PyTorch and CUDA under its own terms. I do not want to build PyTorch and CUDA in this repo. So I pull a pinned version of their container, and I take just the selected binaries this repo needs. Our development and runtime images are built independently on Ubuntu 24.04; NVIDIA's image is the binary donor. I'm not sure what that means for the licensing questions. Things would get weird really fast.

I'm going to implement YOLO26. I still have questions about how the upstream licensing applies to a native implementation. [SAM3](https://github.com/facebookresearch/sam3/blob/main/LICENSE), for example, has its own license to work through. IANAL (UANAL?), so who knows. I'm more of a birdlaw guy. My instinct is still "they can kick rocks" (I love Ultralytics and think they gave a TON to the community; I just mean that colloquially).

I do my best to respect and post licenses. I post my code under the Apache license. So if I claim a thing, and then they claim a thing, can they claim a thing against you if you use my thing?

### OS Compatibility

This is designed for CUDA and Linux only. The optimizations are built around CUDA and Linux-specific systems, and the GUI requires Wayland. Windows, WSL, and macOS are outside the supported and validated platform. It's my opinion that if you're going to be in this space, use Linux. Until everyone catches up, we're on CUDA.

### CLI/RF-DETR

RF-DETR aims for mathematical equivalence with the official Python repo. Hungarian matching uses a native rectangular assignment solver. Some functionality differs, such as seeded experiments. All of the SOTA object detection plays games with that anyway, and I don't feel that raw mAP is an indicator of functional training. I don't think anyone is training on COCO alone and publishing that mAP. There are libraries that autotune hyperparameters, custom datasets, etc. Chasing a peak benchmark is not indicative of general real-world training and execution performance.

### GUI

The GUI is a highly custom Firefox app shell with a Rust UI built on Iced. Ancillary features such as crash reporting, telemetry, WebRTC, and various third-party assets have been pruned from the owned runtime. Compilation publishes the native reflected browser contract, and logical controls communicate typed intents to the native backend over a session-bound WebSocket.

Iced owns the interface, scrolling, layout, and pan/zoom. Native systems own image processing, annotations, augmentation, neural restoration, and final display production. Annotation consumes editing input independently of GPU rendering. Firefox allocates shared Vulkan workspaces that native CUDA fills; Iced samples them directly when the device and layout support it, otherwise Firefox makes one GPU copy into reusable sample storage. Display runs through WebGPU/Vulkan and Wayland. See [GUI interaction and presentation](docs/gui-interaction.md) for input ordering, reusable buffers, image custody, and redraw behavior.

Explore retains individual GPU thumbnails and the gallery while you visit an image. Scrolling prioritizes visible rows, then four rows ahead and four behind. Returning to the gallery reuses ready tiles and resumes unfinished work.

Diagnostics and pixel probes are off during an ordinary `./mmltk --gui` run.
Use the [logging guide](docs/logging.md) to capture a reproduction explicitly.

## Third Party and AI Development Workflows

`third_party` contains aggressively modified upstreams. This project would not exist without AI, but at the same time you can't just say "One high quality C++ plz 🙏". There are a number of workflows this repo leans on to ensure proper class structure, reduced LOC, broadly DRY code, and C++ best practices. First-party code goes through both cross-file and within-file deduplication, as well as the configured static analysis suites.

## Build

Use a Linux host with Docker/Buildx and NVIDIA GPU access configured for
containers. Run commands from the repository root; the wrapper supplies the
container toolchains.

```bash
./mmltk --build
./mmltk --gui
```

The full build packages the native CLI, browser host, owned Firefox runtime,
and Iced WebAssembly interface. The GUI requires an active Wayland session.

See [build and cache details](docs/build.md) for prerequisites, toolchain
policy, generated bindings, and build timing.

## Quick reference

```bash
./mmltk --help                              # Native CLI commands
./mmltk --test list                         # Available test suites and options
./mmltk --logs --family latest-wayland-test --triage
```

Start with the [dataset format and compiler](docs/datasets.md) for your own
data, or the [command reference](docs/commands.md) for CLI and GUI operations.
[Validation](docs/validation.md) covers tidy, cleanup, focused tests, and
hardware acceptance; [logging](docs/logging.md) explains captured evidence.

## Codebase

Independent C++ systems own application work; Rust/Iced owns the interface.
`PresentationSystem` selects and publishes producer-owned display workspaces.
Firefox owns Vulkan allocation/export, sampling resources, and display cadence.

| Start here | Purpose |
| --- | --- |
| [mmltk](mmltk) and [CMakeLists.txt](CMakeLists.txt) | Container orchestration and the build graph |
| [CLI entrypoint](src/entrypoints/cli/cli.cpp) | Dataset commands and RF-DETR dispatch |
| [Desktop entrypoint](src/entrypoints/desktop/browser_runtime_entry.cpp) | Browser-host startup and configuration |
| [Application shell](src/controller/shell/application_shell.h) | Native system construction and lifetime |
| [Iced app](src/frontend/iced/src/app.rs) | UI composition and message routing |

The [technical wiki](docs/README.md) indexes the reference, architecture,
data, and engineering guides. [CONTRACT.md](CONTRACT.md) is the architectural
authority; [AGENTS.md](AGENTS.md) defines development workflows.

## License

Copyright 2026 Ryan Michael Lewkowicz. Repository code is licensed under
[Apache 2.0](LICENSE); see [NOTICE](NOTICE) for attribution. Vendored code and
bundled assets retain their respective upstream licenses and notices.
