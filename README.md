# Multi Model Loader Toolkit

A real time interface for training, prediction, annotation, augmentation, and composition of popular open source models.

## Current State

### Questionable License

Pytorch and Cuda take many many hours to build. Nvidia publishes a container with the latest pytorch and cuda, but it is licensed, so I cannot redistribute it. I do not want to build pytorch and cuda in this repo. So I pull their container, and I take just the selected binaries this repo needs. Our development and runtime images are built independently on Ubuntu 24.04; Nvidia's image is the binary donor. I'm not sure what that means. You can't claim the binaries are licensed if they are not licensed in the pytorch container. Things would get weird really fast.

I'm going to implement yolo26. Glenn is very protective and broadly interprets that GPLv3. This project is open source so it does not affect me. [SAM3](https://github.com/facebookresearch/sam3/blob/main/LICENSE) for example, licenses their algorithms. It's explicit. IANAL (UANAL?) so who knows. But ultralytics license covers the code. This is C++. So I'm pretty sure they can kick rocks (I love ultralytics, and think they gave a TON to the community, I just mean that colloquially).

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

Build the optimized release runtime:

```bash
./mmltk --build
```

First-party ordinary C++ uses GCC 16.2 and C++26 with reflection. First-party
CUDA uses NVCC 13.4 and CUDA C++23, with GCC 16.2 as its host compiler and an
explicit unsupported-host override. GCC 14 bootstraps GCC 16.2 in its own
container stage. The explicit ONNX, simdjson, and cppcheck container source
builds also use GCC 16.2. Vendored dependencies keep their own build and
language policies; Firefox uses its cached Clang toolchain and bootstrap
sysroot.

`--build` drives one Release CMake/Ninja graph. Ninja may start core C++/CUDA,
the iced/Trunk bundle, and the coarse `mmltk_firefox_runtime` edge
independently. The Firefox edge invokes Mach/RecursiveMake/Cargo inside the
container with all available processors; Ninja also receives the full processor
count. Tests and browser-test infrastructure have been removed from the owned
Firefox tree, so they are neither built nor run by `--build`.

The toolchain image contains the native, CUDA, Rust/iced, and Firefox host
prerequisites. Its Release Ninja branch runs concurrently with the separately
fingerprinted runtime-base image branch; they join only for CMake install and
the final runtime image. Core compilation uses ccache and target-local PCHs,
while owned Firefox uses its canonical objdir, Cargo dep-info, and sccache.
Firefox source is always `third_party/firefox`: the build never clones,
fetches, or bootstraps a remote Firefox checkout.

All wrapper-owned reusable filesystem state is repository-local:

- `.cache/cmake/release` is the shared Release graph used by builds and tests;
  the GUI and analysis trees also remain beneath `.cache/cmake`.
- `.cache/cargo/{home,target/browser-app}` contains Cargo downloads and the
  browser target artifacts.
- `.cache/ccache` contains core C++/CUDA compiler results.
- `.cache/firefox/{obj-minimal-opt,mozbuild,sccache}` contains all reusable
  owned-Firefox state.
- `.cache/browser-app` contains shared Trunk fingerprints and publication
  state for both Release and GUI-only builds.
- `.cache/buildkit` contains exported image-build caches.
- `.cache/image-fingerprints` and `.cache/locks` contain verified image
  identities and checkout/cache-root mutation locks.
- `build/release` and `build/browser-app` contain staged, disposable outputs.

The development toolchain and runtime-base are independent owned Ubuntu 24.04
stages. A digest-pinned NGC donor supplies the selected CUDA, Torch, TensorRT,
NumPy/Python, and runtime dependency payload defined by
`docker/nvidia-payload.json`. That includes NCCL and Torch's CPU MKL runtime;
SYCL is excluded. ONNX Runtime is packaged separately. The final image adds
native deliverables, the owned Firefox runtime, and browser assets in separate
cacheable layers; its release label is accepted only when the runtime-base
identity and all staged output fingerprints still match.

Inspect public NVIDIA headers and their payload selection through the wrapper:

```bash
./mmltk --diagnose-nvidia-payload donor cudalibxt.h cufftXt.h
```

The JSON report includes header locations, package ownership, direct includes,
and the development selection result. `donor` inspects the pinned donor through
BuildKit; `development` inspects the existing development image without rebuilding
it. Both inspect the image without modifying its files.

Every native C, C++, CUDA, and Rust link uses mold, including Firefox target
and host tools. The iced browser application targets WebAssembly and therefore
does not use a native ELF linker.

Run an isolated cold timing build without deleting or changing normal compiler
caches:

```bash
./tools/build_time_trace.sh
```

To compare cold, warm, and no-op behavior, reuse one named isolated cache:

```bash
./tools/build_time_trace.sh --cache-key comparison --label cold
./tools/build_time_trace.sh --cache-key comparison --warm-rebuild --label warm
./tools/build_time_trace.sh --cache-key comparison --no-op --label noop
```

Reports are written under `build/time-trace/<cache-key>/runs`, while every
isolated reusable cache remains under `.cache/build-time-trace/<cache-key>`.
Reports include gated JSONL phase and Firefox-stage events, configure
executed/skipped state, assigned jobs, Ninja/Firefox overlap, Cargo/Rust work,
ccache/sccache results, PCH artifacts, BuildKit/package cache outcomes, and
runtime layer sizes. `--warm-rebuild` removes only that trace's CMake tree,
Firefox objdir, and staged outputs while preserving ccache, sccache, Cargo,
mozbuild, browser state, and BuildKit exports. Reusing the key again without
either mode records a general reuse run; `--no-op` explicitly requests and
reports the unchanged case. The timing utility uses key-specific build/runtime
image tags and an ephemeral Buildx builder, leaving only exported cache state
beneath the key. A per-key lock rejects overlapping runs.

The packaged `mmltk-browser-host` launches the owned Firefox runtime and serves
the iced WebAssembly app from `src/frontend/iced`. The browser host is the only
GUI build path.

Build only the live worktree's iced WebAssembly GUI with the cached container
toolchain:

```bash
./mmltk --build-gui
```

The bundle is written to `build/browser-app/dist`. Both `--build` and
`--build-gui` atomically refresh this canonical bundle. The GUI and packaged
release build graphs run the canonical Iced formatting, metadata, and compile
check before producing the bundle. Every `--gui` launch serves the canonical
bundle. Combine both flags to rebuild immediately before launching:

```bash
./mmltk --build-gui --gui
```

The canonical host bundle is mounted read-only at the runtime's packaged
browser-app path, so GUI launches do not use an asset-directory override.

The container CMake presets and build toggles are:

- `release` configures the full package and test graph; its test targets are
  `EXCLUDE_FROM_ALL`.
- `gui` builds only the iced WebAssembly bundle.
- `dev` and `analysis` provide non-package development graphs.
- `BUILD_MMLTK_FIREFOX_RUNTIME` controls the coarse owned-Firefox `ALL` target
  and is enabled only by the Release preset.
- `BUILD_MMLTK_BROWSER_HOST` is the GUI CMake toggle.
- `BUILD_MMLTK_BROWSER_APP` controls the iced WebAssembly bundle.
- GUI builds require Rust's `wasm32-unknown-unknown` target, Trunk 0.21.14,
  `wasm-bindgen`, `wasm-opt`, and cargo-dupes 0.2.1.
- `MMLTK_FIREFOX_RUNTIME_ROOT` identifies the owned Linux Firefox package; its
  default is `.cache/firefox/obj-minimal-opt/dist/firefox`.

## Usage

Use the repo-root wrapper:

```bash
./mmltk --help
./mmltk --tidy
./mmltk --test list
./mmltk --test all
./mmltk rfdetr predict --compiled ./compiled-seg-medium-synth/val.bin --output ./predictions.json --weights ./engines/output-seg-medium/train-local/checkpoint_best_regular.pt
```

The wrapper:

- checks Docker with an instant `docker version` probe
- does one best-effort non-interactive daemon start if Docker is down
- reuses a repo-scoped long-running container
- reuses a fingerprinted source-free toolchain image for builds and `--tidy`
- streams stdout/stderr through `docker exec`
- rewrites absolute host paths into the container's `/host/...` bind

### Static Analysis

Run the full Docker-backed static-analysis pass:

```bash
./mmltk --tidy
```

The wrapper reuses the source-free toolchain image, runs `clang-format` over
tracked first-party C/C++/CUDA files using the repo's Google-based
`.clang-format`, and refreshes the cached Ninja compile database at
`.cache/cmake/analysis`. LLVM 22 `clang-tidy` checks supported translation
units; reflection translation units are validated through their GCC compiler
objects. CUDA analysis covers host and device code at `sm_86`, including
transitively included `.cuh` files. Cppcheck is currently disabled because its
parser cannot handle the repository's C++26 reflection syntax. Compiler results
share `.cache/ccache`; Firefox retains its separate build tooling.

If `clang-tidy` stops on a file you are fixing, restart from that translation unit:

```bash
./mmltk --tidy --start-at src/common/system/cpu_affinity.cpp
```

Run the configured analysis stages for one tracked C/C++ translation unit:

```bash
./mmltk --tidy --file src/common/system/cpu_affinity.cpp
```

Run the focused analysis stages for several translation units in parallel with one flag:

```bash
./mmltk --tidy --file src/common/system/cpu_affinity.cpp src/common/system/execution_policy.cpp
```

Run format and CUDA clang-tidy checks for one translation unit:

```bash
./mmltk --tidy --file src/backend/imaging/explore/explore_render_core.cu
```

### Tests

List the test suites exposed by the Docker-backed wrapper:

```bash
./mmltk --test list
```

Native Catch2 selections reuse `.cache/cmake/release`, explicitly build only
the selected `EXCLUDE_FROM_ALL` targets with all available processors, and then
run those executables. `browser-app` instead configures `.cache/cmake/gui` and
invokes the `mmltk_frontend_iced_tests` target. Neither graph selects the
independent Firefox `ALL` edge. An already-current target is a Ninja no-op. Run
every native suite with:

```bash
./mmltk --test all
```

RF-DETR integration assets are cached under `.cache/tests/rfdetr` when
you run tests through `./mmltk`. The RF-DETR test bundle downloads the nano
checkpoint from the built-in catalog, normalizes it to the native checkpoint
format, exports ONNX, and builds a TensorRT engine from that cache on first use.

The native `all` selection builds and runs every configured Catch2 target. Its
desktop-host fixture also reaches the canonical Iced formatting, metadata, and
compile check. Run `./mmltk --test browser-app` to execute the Iced tests; their
artifacts are not installed into the runtime image.

Forward Catch2 selectors or flags after `--`:

```bash
./mmltk --test core -- --list-tests
./mmltk --test rfdetr -- "~[optin]"
```

### GUI

Open the browser-host GUI:

```bash
./mmltk --gui
```

The launch requires the canonical GUI bundle produced by either `./mmltk
--build` or `./mmltk --build-gui`; it never falls back to a different packaged
bundle.

Submit RF-DETR work through the browser action surface after the page opens.
The action controls provide typed submissions for training, prediction,
validation, engine building, and ONNX export; enter the action fields in the
browser and submit from the matching workflow panel.

Wrapper-managed containers run with `--privileged`. When `--gui` is set, the
wrapper bind-mounts the active Wayland runtime paths, keeps the container
process on your host UID/GID for display authentication, and recreates the
cached container if that runtime shape changed. Missing Wayland state is a hard
launch failure.

### Control-Plane Architecture

`ApplicationShell` starts one `BrowserServer`, serves the iced assets, and
launches Firefox with an ephemeral profile. Native systems and the Rust
presentation model exchange reflected typed CBOR over bidirectional WebSockets.
Rust verifies schema agreement before installing current native snapshots.
The same canonical C++ declarations provide validation, exhaustive intent
dispatch, typed UI events, browser descriptors, and the generated Rust
projection.

Intent handlers call focused application systems through direct typed methods.
Dataset compilation, training, inference, annotation, live media, and other
long-running work execute on workers owned by the system performing that work.
Progress and failures are published directly as typed UI events. Ordinary
synchronous failures propagate through normal C++ exceptions; worker and
external-service boundaries translate failures once for the browser.

Each system owns and reuses the CUDA contexts, streams, events, models, and
private high-water buffers required by its workload. Cross-system receivers
borrow typed image reads, copy into their own reusable storage, and finish the
GPU copy before releasing the read. `PresentationSystem` selects a completed
source product for final composition into one native exported backbuffer.
Producers continue working while backgrounded. Firefox owns native import,
browser samples, the downstream WebGPU/Vulkan queues, swapchain, compositor
cadence, and Wayland presentation. Iced owns view transforms and retained
images for redraws.

Shutdown stops browser ingress, requests each system's workers to stop, returns
from the browser loop, and joins Firefox before retiring its imported native
resources. Systems join their workers, and reverse-order RAII releases physical
resources.

Set `MMLTK_GUI_TRACE_FILE` to opt into detailed JSONL command, transport,
workspace, worker, cleanup, and resource-failure records. When it is unset, the
runtime does not assemble diagnostic records or create a diagnostics file. To
capture the native trace and the separate Firefox log together, launch with:

```bash
MMLTK_GUI_TRACE_FILE=.mmltk-data/logs/gui-trace.jsonl \
MMLTK_FIREFOX_LOG_FILE=.mmltk-data/logs/firefox.log \
./mmltk --gui
```

The native trace starts a new capture; preserve earlier logs before reusing a
path. `MMLTK_GUI_TRACE_FILE` is the supported native trace variable.

Inspect a compiled file's Linux storage and GPU capabilities without reading
its contents or running a benchmark:

```bash
./mmltk --diagnose-io ./compiled/train.bin > io-capabilities.json
```

This standalone command uses the existing `MMLTK_BUILD_IMAGE` (default
`mmltk-build:latest`) with a read-only dataset mount. It never builds or pulls
an image. JSON includes GPU/driver, kernel, filesystem and block devices, PCIe
paths, container memory-lock limits, visible cuFile/GDRCopy components, and
`O_DIRECT` open / `STATX_DIOALIGN` observations. Missing tools or inaccessible
metadata are reported as unavailable. An accepted open or installed library
does not establish native storage-to-GPU DMA; no direct read is attempted.
Docker/image/GPU-container startup failures are reported on stderr before a
JSON report can be produced. See the
[Explore streaming review](docs/explore-streaming-review.md) for captured
evidence, complexity, and remaining validation.

Compiled-image loading for Train, Validate, Predict, and Explore defaults to
H2D, using persistent local pinned staging and asynchronous DMA.
Select `--gdrcopy` explicitly for GDRCopy; there is no automatic fallback.
`--numa-node -1` selects known GPU-local placement; explicit node overrides
must agree with the selected GPU. Explore reconstructs its runtime when saved
device, placement, or transport settings change. The packaged GUI selects H2D
for all four compiled-image workflows at startup; `./mmltk --gui --gdrcopy`
selects GDRCopy for that session. Startup selection does not rewrite saved settings.
For joined atlas diagnostics, launch with
`MMLTK_GUI_TRACE_FILE=.mmltk-data/logs/gui-trace.jsonl MMLTK_FIREFOX_LOG_FILE=.mmltk-data/logs/firefox.log ./mmltk --gui`.
The native trace carries both surface identity halves; Firefox and Iced report
the same identity as 32 hexadecimal digits. Use fresh logs for each reproduction.

The GPU framework's GDRCopy implementation is a private static library from
`third_party/gdrcopy`; runtime notices are installed at
`/opt/mmltk/share/mmltk/licenses/gdrcopy`. GDRCopy uses either an available
`/dev/gdrdrv` or CUDA 13.3+ DMA-BUF mmap on the selected GPU. The wrapper exposes
a present driver node, without installing a kernel module. Set
`GDRCOPY_USE_DMABUF_MMAP=1` before startup to select DMA-BUF explicitly.
`MMLTK_GDR_TRACE_FILE` enables mapped-buffer JSONL diagnostics and is forwarded
with host-path rewriting. `--diagnose-io` reports export and CPU-mmap support
separately for each CUDA-visible device; this does not prove available BAR,
descriptor, or allocation capacity.

During Final Validation, run the small functional GDR tests for each supported
backend and visible device. These checks contain no throughput timings:

```bash
./mmltk --test application-systems --executable mmltk_frameworks_gpu_tests -- "[gdr]~[hardware]"
./mmltk --test application-systems --executable mmltk_frameworks_gpu_tests --env GDRCOPY_USE_DMABUF_MMAP=0 --env MMLTK_GDR_TEST_BACKEND=gdrdrv --env MMLTK_GDR_TEST_DEVICE=0 -- "[gdr][hardware]"
./mmltk --test application-systems --executable mmltk_frameworks_gpu_tests --env GDRCOPY_USE_DMABUF_MMAP=1 --env MMLTK_GDR_TEST_BACKEND=dmabuf --env MMLTK_GDR_TEST_DEVICE=0 -- "[gdr][hardware]"
```

An unavailable capability produces an explicit skip, not evidence of a successful
transfer. Backend selection is immutable for an existing mapped allocation.

For NVIDIA Wayland setups, run the wrapper with the same env you use natively:

```bash
XDG_RUNTIME_DIR=/run/user/1000 \
WAYLAND_DISPLAY=wayland-0 \
__NV_PRIME_RENDER_OFFLOAD=1 \
./mmltk --gui
```

## DONE

- [x] **RF-DETR**
- [x] **Muon optimizer**
- [x] **GUI**
- [x] **Remote training**
- [x] **Docker build**

## TODO

- [ ] **SAM3**: For ancillary annotation support
- [ ] **libSGM**: For disparity map and optical flow
- [ ] **Binary classifier**: I made this with an off the shelf timm model. I was trying to avoid pybind. It works great and I don't wanna mess with the C++ conversion. Idk what I'm doing with this.
- [ ] **Keypoints**: This is not a model in a sense if I'm not mistaken. I'm fairly certain most popular keypoint and pose systems are post process. If I recall, openpose uses Dijkstra. I want to take some of these newer pathing algos like tsinghuas and see if we can't find some interesting value.

<p align="center">
  <img alt="SAM3 badge" src="https://img.shields.io/badge/SAM3-interactive_segmentation-orange?style=flat-square">
  <img alt="libSGM badge" src="https://img.shields.io/badge/libSGM-stereo_matching-brightgreen?style=flat-square">
  <img alt="Binary classifier badge" src="https://img.shields.io/badge/Binary_classifier-fast_triage-blue?style=flat-square">
  <img alt="Keypoints badge" src="https://img.shields.io/badge/Keypoints-pose_%26_landmarks-pink?style=flat-square">
</p>

## Source Dataset Format

I thought about supporting traditional datasets. I just don't feel like it. It's about toolchains, it's about products, it's about interfaces. This is a holistic platform. It's about redefining how we interact with vision modeling systems.

The source dataset is organized like a YOLO-style split tree, but annotations are JSON-based instead of `.txt` files:

```text
dataset/
  categories.json
  train/
    000001.png
    000001.jsonl
    000002.png
    000002.jsonl
    ...
  val/
    000001.png
    000001.jsonl
    ...
```

The compiler expects:

- a dataset-root `categories.json`
- one split directory per dataset split, such as `train/` or `val/`
- six-digit sequential filenames starting at `000001`
- one `.png` image and one matching `.jsonl` annotation file per image

`categories.json` carries dataset metadata, the category table, and optional split counts. Keep category names agnostic to your problem domain; the compiler only requires unique names and dense ids starting at `0` or `1`.

Example shape:

```json
{
  "meta": {
    "dataset_name": "example-dataset",
    "version": "1.0",
    "image_format": "png",
    "image_size_wh": [432, 432],
    "bbox_format": "xyxy_absolute_pixels",
    "mask_format": "rle_row_major_start_length",
    "background_annotation_policy": "dataset_defined"
  },
  "classes": [
    { "id": 1, "name": "category_1" },
    { "id": 2, "name": "category_2" }
  ],
  "splits": {
    "train": { "total": 1000, "background": 0, "annotated": 1000 },
    "val": { "total": 100, "background": 0, "annotated": 100 }
  }
}
```

Each `.jsonl` file is line-delimited JSON, with one object per instance. The supported fields are:

- `class`: category name matching an entry in `categories.json`
- `bbox_xyxy`: `[x1, y1, x2, y2]` in absolute pixel coordinates
- `mask_rle`: row-major `start:length` runs separated by spaces
- `image_size_wh`: optional `[width, height]` validation field

Example instance line:

```json
{"class":"category_1","bbox_xyxy":[10,20,100,140],"mask_rle":"8650:24 9082:24 9514:24","image_size_wh":[432,432]}
```

Background-only images are supported as long as the matching `.jsonl` file still exists and contains no instance lines.

## GPU-local execution

Train and Explore resolve each CUDA-visible device through its PCI identity and
use that GPU's permitted local memory node and CPUs. A single-node machine can
resolve GPU locality reported as unknown. On a multi-node machine, unknown
locality needs a deliberate `numa_node` override; a conflicting override fails.
Single-device training and desktop startup expose `--numa-node` (`-1` selects automatic locality).
Distributed training uses `--device-ids 0,1 --numa-nodes 0,3` with one override per selected rank;
`-1` entries resolve that device automatically. A scalar override is rejected for multiple devices.
For desktop startup,
`./mmltk --gui --device-id 0 --numa-node 0` selects the visual device and its node. Existing CPU
lists constrain eligibility within that node. Independent systems retain their
own worker pools and can overlap local CPUs. Validation and export retain the shell-resolved
placement across runtime reconstruction and verify worker policy before operation admission.
Training solver workers reuse runtime-owned PMR arrays backed by strictly local anonymous pages.

Boundary workers verify fixed CPU affinity, strict `MPOL_BIND`, normal-scheduler
nice -10 or better, and best-effort I/O priority 0 for storage work before
admission. The wrapper supplies runtime/test nice and memlock limits and the
required capabilities; a denied required policy is an operation failure.
Owned local host storage is page-rounded, prefaulted and residency-checked
before CUDA registration, with stable high-water reuse. File-cache and foreign
pages retain their actual ownership and are not claimed to be relocated.

`./mmltk --diagnose-io compiled/train.bin` reports read-only topology and policy
capabilities. Its deliberately restricted container does not prove functional
NUMA binding or pinned allocation. The common-system and GPU focused tests
provide those functional checks during Final Validation. CUDA stream priority
continues to be supported; it does not establish DMA-engine priority.

## License

I'm working on this. The goal was going to be to build a licensed product. But even if the interfaces are clean, I don't think it would generate enough revenue. I get it's all AI, but this is on it's way to being a cool framework. There's a lot of value here. I'm going to add sam3, I can't license that, but I can have a default model with simple training interfaces. Then even for just a couple bucks a month, the ability to use multi gpu, remote training and secondary models, that's licensed. Just to keep my head above water.

Copyright 2026 Ryan Michael Lewkowicz.

This repository is licensed under the Apache License 2.0. See `LICENSE` and
`NOTICE`.

Vendored third-party code under `third_party/` and bundled font assets under
`src/frontend/iced/assets/fonts/` retain their respective upstream licenses
and notices.
