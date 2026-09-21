# Multi Model Loader Toolkit

A real time interface for training, prediction, annotation, augmentation, and composition of popular open source models.

## Current State

### Questionable License

Pytorch and Cuda take many many hours to build. Nvidia publishes a container with the latest pytorch and cuda, but it is licensed, so I cannot redistribute it. I do not want to build pytorch and cuda in this repo. So I pull their container, and I take just the selected binaries this repo needs. Our development and runtime images are built independently on Ubuntu 24.04; Nvidia's image is the binary donor. I'm not sure what that means. You can't claim the binaries are licensed if they are not licensed in the pytorch container. Things would get weird really fast.

I'm going to implement yolo26. Glenn is very protective and broadly interprets that GPLv3. This project is open source so it does not affect me in a sense because my modifications are public. [SAM3](https://github.com/facebookresearch/sam3/blob/main/LICENSE) for example, licenses their algorithms. It's explicit. IANAL (UANAL?) so who knows. I'm more of a birdlaw guy. But ultralytics license covers the code. This is C++. So I'm pretty sure they can kick rocks (I love ultralytics, and think they gave a TON to the community, I just mean that colloquially).

I do my best to respect and post licenses. I just post my code as apache license. So if I claim a thing, and then they claim a thing, can they claim a thing against you if you use my thing?   

### OS Compatibility

This is designed to be cuda and linux only. The massive performance gains come from targeted optimizations built around cuda and linux specific systems. You can try mac or windows, but I have not tested it. Windows does have WSL, but you're losing 30% performance and graphics memory to WDM anyway. Mac has Rosetta, but that's always had tenuous functionality. It's my opinion if you're going to be in this space, use linux and until everyone catches up, we're on cuda.

### CLI/RF-DETR

Should be 100% mathematically equivalent to the official python repo. Things like hungarian matching actually use scipys underlying C. Some of the functionality is not. Such as seeded experiments etc. All of the SOTA object detection plays games with that anyway, and I don't feel that raw MAP is an indicator of functional training. I don't think anyone is training on COCO alone and publishing that map. There's libraries that will autotune hyps, they have custom datasets etc. Chasing a peak benchmark is not indicative of general real world performance of training and execution.

### GUI

The GUI is a highly custom Firefox app shell with a Rust UI built on iced. Most ancillary features such as crash reporting, telemetry, WebRTC, and various third-party assets have been pruned entirely. Part of compilation publishes the audited native reflected browser contract, and logical controls communicate typed intents to the native backend over a session-bound WebSocket.

The graphics responsibility is deliberately split. iced/WebGPU owns latency-sensitive presentation work that does not modify the native product: scrolling, responsive layout, hit testing, atlas UV offsets, and detail pan/zoom. The native CUDA systems own compiled-tensor access, pinned gathers, augmentation, resampling, image and annotation products, and neural restoration. Iced draws text labels from the native annotation facts. Each producer keeps its working images private. `PresentationSystem` borrows a typed read view, performs one receiver-owned GPU copy and final composition, waits for that work to complete, and publishes only its own persistent exported backbuffer. Firefox imports that backbuffer for WebGPU/Vulkan composition and Wayland presentation. Browser image delivery stays on the GPU; typed CBOR/WebSocket messages carry application control and state. Iced view transforms reuse the completed browser image.

The independent C++ systems architecture and its failure, shutdown, and GPU resource rules are documented in [CONTRACT.md](CONTRACT.md).

## Third Party and AI Development Workflows

Third party contains aggressively modified upstreams. This project would not exist without AI, but at the same time you can't just say "One high quality C++ plz 🙏". There's a number of workflows this repo leans on to ensure proper class structure, reduced LOC, broadly DRY code, and best practices C++. All code goes through both cross file and within file deduplication, as well as the configured static analysis suites.

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

## Training, validation, and prediction

Train prepares the selected model and starts the run from its primary action.
Its center shows a selectable chart dashboard with a separate live progress
card. Train and validation inputs are required; the final-test split is optional.
Choose Transfer or Resume in the weights card. Auto Output creates a fresh run
directory at Start; Browse Output selects a manual destination and loads its
saved charts. EMA is optional and off by default.
Validate shows twelve COCO summaries beside six sample tiles and uses Explore's
viewer, including Upscale and Open in Annotation. Its GT labels and Det labels
switch the two annotation layers independently. Predict is a quick visual check
for compiled data, an image, or a local video, with Pause/Resume/Stop for video.

The [RF-DETR workflow guide](docs/rfdetr-workflows.md) covers class identity,
metrics, output files, and continuation. Current native checkpoints use version
3 and saved plots require run format 2; older application checkpoints
and old output-directory history are unsupported. Upstream weights keep their
own supported input routes. Compiled datasets use format 8; recompile older
bins. Dataset compilation defaults to Stretch, with Letterbox available
explicitly. The shared viewer's Original option restores source aspect from
compiled pixels; it does not recover source resolution.

The Dataset card's benchmark override offers Coco custom and Coconut.
Coconut compiles the full COCONut training recipe with a choice of validation
annotations and membership, reusing the persistent source cache. See the
[built-in dataset guide](docs/benchmark-datasets.md) for those choices and for
reading download and extraction progress.

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
`PresentationSystem` routes the foreground source and its graphics handoff.
Firefox owns two reusable Vulkan display buffers, sampling resources, and
display cadence. Completed images carry their own geometry and labels over
the graphics channel; the browser can keep redrawing them while native work
continues. See [GUI interaction and presentation](docs/gui-interaction.md).

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
