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

Use Linux with Docker/Buildx, NVIDIA container GPU access, and Wayland for the
GUI. Run from the repository root; the wrapper supplies container toolchains.
Bootstrap a fresh checkout, then build and launch:

```bash
./tools/bootstrap/bootstrap_mmltk.sh --launch "$PWD" --build-tools
./mmltk --build
./mmltk --gui
```

Rebuild changed tool sources with `./mmltk --build-tools`. The product build
packages the CLI, browser host, Firefox runtime, and Iced WebAssembly interface.
See [build and cache details](docs/build.md) for prerequisites, tool ownership,
generated bindings, and reusable state.

## Training, validation, and prediction

The RF-DETR catalog includes detection, segmentation, and keypoint models.
Train, Validate, Predict, and Export share model preparation, output selection,
and per-workflow GPU choices. Auto Output creates a new run at Start; a manual
Train destination can also load saved charts. Status retains selectable warnings
and errors with Copy and Dismiss.

Train requires training and validation data; final-test data is optional.
Transfer and Resume prepare weights or a complete session before Start. Training
supports shared gradients, independent models, periodic averaging, per-model
recipes, and final model selection. EMA is optional and off by default;
independent/periodic modes and model soups are experimental.

Validate presents metrics and retained samples in the shared viewer. Predict
accepts compiled data, an image, or local video; video supports Pause/Resume/Stop,
sample saving, full annotated H.264/MKV output, and partial media after interruption.
Export writes ONNX, TensorRT, or both. Local failures report their cause and exit
status; restarting is explicit. See the [workflow guide](docs/rfdetr-workflows.md),
[training reference](docs/rfdetr-training.md), and
[merging and Resume reference](docs/model-merging.md) for controls, recipes,
class/joint meaning, artifact admission, and continuation.

Compiled datasets use format 11 and retain format-10 reading. Native checkpoints
use format 5 and retain supported format-4 reading. Dataset compilation defaults
to Stretch; Letterbox and explicit
[RGB resampling methods](docs/datasets.md#rgb-resampling-methods) are available.
Explore thumbnails preserve source proportions. The viewer's Original option
changes displayed geometry; it does not recover source resolution.

For your data, start with the [Directory dataset format](docs/datasets.md).
Built-in Coco custom and Coconut recipes share a persistent source cache;
Coconut's optional dropped-mask recovery is off by default. Compilation reports
acquisition, labels/masks, pixels, and bounded-repair omissions separately; both
output splits require usable data. Directory and benchmark compilation share CPU
capacity and reusable pinned storage. Perceptual GPU augmentation is a separate
training option. See [benchmark datasets](docs/benchmark-datasets.md) and
[compilation execution](docs/compile-execution.md).

## Quick reference

```bash
./mmltk --help
./mmltk --test list
./mmltk --logs --family latest-wayland-test --triage
```

The [command reference](docs/commands.md) covers CLI and GUI operations.
[Validation](docs/validation.md) covers focused checks, numerical gates, and
hardware acceptance; [logging](docs/logging.md) covers diagnostics and transcripts.
Matched-state numerical checks and measured independent trajectories do not
establish AP, convergence, or throughput equivalence.

`./mmltk --test all` runs ordinary native, browser JavaScript/Rust,
cleanup/declaration, log-query, and compositor fixtures. Packaged Wayland
acceptance runs separately. External upstream tensors and historical sessions
require `--with-upstream` or an explicit evidence path; ordinary tests do not
generate them. Dataset profiles and full Coconut measurements are also
[opt-in](docs/validation.md#data-compilation-and-resampling-profiles).

## Codebase

Independent C++ systems own application work; Rust/Iced owns presentation and
interaction. Native rendering publishes completed images with their geometry
and labels through a GPU graphics connection. Firefox owns reusable Vulkan
display storage and Wayland presentation cadence; the interface can redraw the
last completed image while native work continues.

| Start here | Purpose |
| --- | --- |
| [Native driver](tools/mmltk.cpp) and [tools graph](tools/CMakeLists.txt) | Container orchestration and standalone tools |
| [Product graph](CMakeLists.txt) | Native application, browser, and packaged runtime |
| [CLI entrypoint](src/entrypoints/cli/cli.cpp) | Dataset commands and RF-DETR dispatch |
| [Desktop entrypoint](src/entrypoints/desktop/browser_runtime_entry.cpp) | Browser-host startup |
| [Application shell](src/controller/shell/application_shell.h) | System construction and lifetime |
| [Iced app](src/frontend/iced/src/app.rs) | UI composition and routing |

The [technical wiki](docs/README.md) indexes the reference guides.
[CONTRACT.md](CONTRACT.md) owns architecture; [AGENTS.md](AGENTS.md) owns
engineering policy.

## License

Copyright 2026 Ryan Michael Lewkowicz. Repository code is licensed under
[Apache 2.0](LICENSE); see [NOTICE](NOTICE) for attribution. Vendored code,
models, and bundled assets retain their upstream licenses and notices.
