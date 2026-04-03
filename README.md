# Multi Model Loader Toolkit

A real time interface for training, prediction, annotation, augmentation, and composition of popular opensource models.

## Current State 
The training/validation etc is all great, if you're coming across this, you won't have my dataset format, I'm sure your llm will get it, but I'll still publish one soon. Should be mathematically equivalent to python. But that is tenuously verified. Training 2-10x faster than python. 

Just did a big cleanup pass, haven't done a review yet. Getting ready to push the docker image. The gui works, has some good value, but is not fully functional.

## Build

Build the runtime image once:

```bash
docker build -t mmltk .
```

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
- builds a cached Docker build-stage image for `--tidy`
- streams stdout/stderr through `docker exec`
- rewrites absolute host paths into the container's `/host/...` bind

### Static Analysis

Run the full Docker-backed static-analysis pass:

```bash
./mmltk --tidy
```

The wrapper rebuilds a cached analysis image from the Docker build stage, regenerates
`build/docker-dev-tidy/compile_commands.json` inside the container, then runs
`clang-tidy` followed by `cppcheck`.

If `clang-tidy` stops on a file you are fixing, restart from that translation unit:

```bash
./mmltk --tidy --start-at src/cpu_affinity.cpp
```

### Tests

List the bundled test bundles exposed by the Docker-backed wrapper:

```bash
./mmltk --test list
```

Run every bundled suite from the wrapper-managed Docker container:

```bash
./mmltk --test all
```

RF-DETR integration assets are cached under `.mmltk-data/test-cache/rfdetr` when
you run tests through `./mmltk`. The RF-DETR test bundle downloads the nano
checkpoint from the built-in catalog, normalizes it to the native checkpoint
format, exports ONNX, and builds a TensorRT engine from that cache on first use.

Forward Catch2 selectors or flags after `--`:

```bash
./mmltk --test core -- --list-tests
./mmltk --test rfdetr -- "~[optin]"
```

### GUI

Open the GUI with the existing repo-root `gui.json`:

```bash
./mmltk --gui
```

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

Wrapper-managed containers run with `--privileged`. When `--gui` is set, the wrapper also bind-mounts the active UI runtime paths, keeps the container process on your host UID/GID for display auth, prefers `MMLTK_GUI_PLATFORM=x11` when `DISPLAY` is available, and recreates the cached container if that GUI runtime shape changed.

For NVIDIA/Xwayland setups, run the wrapper with the same env you use natively:

```bash
XDG_RUNTIME_DIR=/run/user/1000 \
DISPLAY=:0 \
XAUTHORITY=/run/user/1000/.mutter-Xwaylandauth.G5IRL3 \
WAYLAND_DISPLAY=wayland-0 \
__NV_PRIME_RENDER_OFFLOAD=1 \
__GLX_VENDOR_LIBRARY_NAME=nvidia \
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
