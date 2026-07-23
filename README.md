# Multi Model Loader Toolkit

A real time interface for training, prediction, annotation, augmentation, and composition of popular opensource models.

## Current State 
### OS Compatibility
This is designed to be cuda and linux only. The massive performance gains come from targeted optimizations built around cuda and linux specific systems. You can try mac or windows, but I have not tested it. Windows does have WSL, but you're losing 30% performance and graphics memory to WDM any way. Mac has Rosetta, but that's always had tenuous functionality. It's my opinion if you're going to be in this space, use linux and until everyone catches up, we're on cuda. 

### CLI/RF-DETR
Should be 100% mathematically equivalent to the official python repo. Things like hungarian matching actually use scipys underlying C. Some of the functionality is not. Such as seeded experiments etc. All of the SOTA object detection plays games with that anyway, and I don't feel that raw MAP is an indicator of functional training. I don't think anyone is training on COCO alone and publishing that map. There's libraries that will autotune hyps, they have custom datasets etc. Chasing a peak benchmark is not indicative of general real world performance of training and execution.

### GUI
The GUI is a highly custom firefox app shell with a rust based UI called iced. Most ancillary features such as crash reporting, telemetry, webrtc and various third_party assets have been pruned entirely. There are integrations that allow texture sharing directly from the GPU. Part of compilation publishes an openapi spec. The UI truly is just a shell, it communicates intents to the backend using websockets. The UI was originally an angular app.  

## Third Party and AI Development Workflows
Third party contains aggressively modified upstreams. This project would not exist without AI, but at the same time you can't just say "One high quality C++ plz 🙏". There's a number of work flows this repo leans on to ensure proper class structure, reduced LOC, broadly DRY code, and best practices C++. All code goes through both cross file and interfile de duplication. As well as various cppcheck and clang tidy suites. 

## Build

Build the optimized release runtime:

```bash
./mmltk --build
```

`--build` drives one Release CMake/Ninja graph. Ninja may start core C++/CUDA,
the iced/Trunk bundle, and the coarse `mmltk_firefox_runtime` edge
independently. The Firefox edge invokes local Mach/RecursiveMake/Cargo with all
available processors; Ninja also receives the full processor count. Tests and
browser-test infrastructure have been removed from the owned Firefox tree, so
they are neither built nor run by `--build`.

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

The runtime-base image owns stable TensorRT and ONNX Runtime libraries and
reuses NCCL from the PyTorch runtime. The final image adds native deliverables,
the owned Firefox runtime, and browser assets in separate cacheable layers; its
release label is accepted only when the runtime-base identity and all staged
output fingerprints still match.

Every native C, C++, CUDA, and Rust link uses mold, including Firefox target
and host tools. The iced browser application targets WebAssembly and therefore
does not use a native ELF linker.

Run an isolated cold timing build without deleting or changing normal compiler
caches:

```bash
./utilities/build_time_trace.sh
```

To compare cold, warm, and no-op behavior, reuse one named isolated cache:

```bash
./utilities/build_time_trace.sh --cache-key comparison --label cold
./utilities/build_time_trace.sh --cache-key comparison --warm-rebuild --label warm
./utilities/build_time_trace.sh --cache-key comparison --no-op --label noop
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
the iced WebAssembly app from `src/browser/app`. The browser host is the only GUI
build path.

Build only the live worktree's iced WebAssembly GUI with the cached container
toolchain:

```bash
./mmltk --build-gui
```

The bundle is written to `build/browser-app/dist`. Both `--build` and
`--build-gui` atomically refresh this canonical bundle. Browser formatting,
metadata, and duplication checks belong to `./mmltk --test gui` and
`./mmltk --test all`; they are not prerequisites of a release build. Every
`--gui` launch serves the canonical bundle. Combine both flags to rebuild
immediately before launching:

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
tracked C/C++/CUDA files using the repo's Google-based `.clang-format`, refreshes
the cached Ninja compile database at `.cache/cmake/analysis`, then runs
`clang-tidy` followed by `cppcheck`. Compiler results share `.cache/ccache`.

If `clang-tidy` stops on a file you are fixing, restart from that translation unit:

```bash
./mmltk --tidy --start-at src/cpu_affinity.cpp
```

Run all three analysis stages for one tracked C/C++ translation unit:

```bash
./mmltk --tidy --file src/cpu_affinity.cpp
```

### Tests

List the test suites exposed by the Docker-backed wrapper:

```bash
./mmltk --test list
```

`--test` reuses `.cache/cmake/release`, explicitly builds only the selected
`EXCLUDE_FROM_ALL` targets with all available processors, and then runs them.
It never creates a separate tests tree and does not select the independent
Firefox `ALL` edge. An already-current target is a Ninja no-op. Run every suite
with:

```bash
./mmltk --test all
```

RF-DETR integration assets are cached under `.cache/tests/rfdetr` when
you run tests through `./mmltk`. The RF-DETR test bundle downloads the nano
checkpoint from the built-in catalog, normalizes it to the native checkpoint
format, exports ONNX, and builds a TensorRT engine from that cache on first use.

The `gui` and `all` selections also build the browser formatting, metadata, and
duplication-check target. Test binaries are neither built by `--build` nor
installed into the runtime image.

Forward Catch2 selectors or flags after `--`:

```bash
./mmltk --test core -- --list-tests
./mmltk --test rfdetr -- "~[optin]"
```

### GUI

Open the browser-host GUI with the existing repo-root `gui.json`:

```bash
./mmltk --gui
```

The launch requires the canonical GUI bundle produced by either `./mmltk
--build` or `./mmltk --build-gui`; it never falls back to a different packaged
bundle.

Seed the GUI from CLI-compatible RF-DETR arguments before it opens:

```bash
./mmltk --gui rfdetr validate --compiled ./compiled-seg-medium-synth/val.bin --onnx ./models/rf-detr-seg-medium.onnx
./mmltk --gui rfdetr predict --compiled ./compiled-seg-medium-synth/val.bin --output ./predictions.json --weights ./engines/output-seg-medium/train-local/checkpoint_best_regular.pt
./mmltk --gui rfdetr train --train-compiled ./compiled-seg-medium-synth/train.bin --val-compiled ./compiled-seg-medium-synth/val.bin --output-dir ./engines/output-seg-medium/train-local --weights ./engines/output-seg-medium/train-local/checkpoint_best_regular.pt --device-id 0
```

Supported GUI-seeded commands:

- `mmltk rfdetr train`
- `mmltk rfdetr predict`
- `mmltk rfdetr validate`
- `mmltk rfdetr build-engine`
- `mmltk rfdetr export-onnx`

Commands or flags without matching GUI fields are rejected instead of being ignored.

Wrapper-managed containers run with `--privileged`. When `--gui` is set, the
wrapper bind-mounts the active Wayland runtime paths, keeps the container
process on your host UID/GID for display authentication, and recreates the
cached container if that runtime shape changed. Missing Wayland state is a hard
launch failure.

The host serves assets and a session-bound loopback WebSocket, then starts
Firefox with an ephemeral profile. iced requests WebGPU directly. Capture-card
frame presentation is intentionally unavailable in this cutover; controls,
workflow state, diagnostics, and capture ownership remain active.

Set `MMLTK_GUI_TRACE_FILE` for gated host/server JSONL diagnostics and
`MMLTK_FIREFOX_LOG_FILE` for Firefox stdout/stderr. Neither path is opened and
no trace records are assembled unless its variable is set. `MMLTK_FIREFOX_BINARY`
is a diagnostic override for an equivalent packaged runtime.

For NVIDIA Wayland setups, run the wrapper with the same env you use natively:

```bash
XDG_RUNTIME_DIR=/run/user/1000 \
WAYLAND_DISPLAY=wayland-0 \
__NV_PRIME_RENDER_OFFLOAD=1 \
./mmltk --gui rfdetr train --train-compiled ./compiled-seg-medium-synth/train.bin --val-compiled ./compiled-seg-medium-synth/val.bin --output-dir ./engines/output-seg-medium/train-local --weights ./engines/output-seg-medium/train-local/checkpoint_best_regular.pt --device-id 0
```

The wrapper writes seeded GUI state back into the repo-root `gui.json`, and the GUI loads that file on startup.

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

- a repo-level `categories.json`
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

## License

I'm working on this. The goal was going to be to build a licensed product. But even if the interfaces are clean, I don't think it would generate enough revenue. I get it's all AI, but this is on it's way to being a cool framework. There's a lot of value here. I'm going to add sam3, I can't license that, but I can have a default model with simple training interfaces. Then even for just a couple bucks a month, the ability to use multi gpu, remote training and secondary models, that's licensed. Just to keep my head above water. 

Copyright 2026 Ryan Michael Lewkowicz.

This repository is licensed under the Apache License 2.0. See `LICENSE` and
`NOTICE`.

Vendored third-party code under `third_party/` and bundled font assets under
`src/gui/res/fonts/` retain their respective upstream licenses and notices.
