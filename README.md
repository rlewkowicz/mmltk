# fastloader

Zero-copy C++/CUDA dataset loader for segmentation datasets. The compiled `.bin` file **is** the tensor: `float32 NCHW [0,1]` on disk, mmap'd directly into your process, handed to Python as numpy views with no copies.

## How it works

```text
PNG dataset + JSON annotations + categories.json
        |
        v  (compile - one time)
   .bin file on disk
   +--------------------------------------------+
   | FileHeader | Index | Labels | RLE | Pixels |
   |            |       |        |     | NCHW   |
   +--------------------------------------------+
        |                               ^
        v  (load)                       |
   mmap(MAP_SHARED | MAP_POPULATE)------+
        |
        +--> numpy array view
        |
        +--> pinned gather buffer --> async DMA --> GPU
```

Sequential batches can read directly from the mmap span. Shuffled batches gather once into pinned host memory and submit a single async copy to device memory.

## Source Dataset Format

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

## Build

This repo is local-only now. The supported build trees are:

- `build/release`: optimized runtime build, no tests, no profiling helpers
- `build/dev`: development build with debug symbols, assertions, tests, and profiling runners

Install the pinned native RF-DETR dependency bundle first:

```bash
./utilities/install_local_rfdetr_deps.sh
```

Configure and build with CMake presets:

```bash
rm -rf build/release build/dev

cmake --preset release --fresh
cmake --build --preset release -j"$(nproc)"

cmake --preset dev --fresh
cmake --build --preset dev -j"$(nproc)"
ctest --preset dev --output-on-failure
```

`./build.sh` is the local shortcut:

```bash
# Build both trees
./build.sh

# Build only one tree
./build.sh release
./build.sh dev

# Optional knobs
FASTLOADER_BUILD_FRESH=1 FASTLOADER_BUILD_RUN_TESTS=1 ./build.sh dev
```

Release uses LTO/IPO when the toolchain supports it, `-O3`, section splitting, section GC, native CPU tuning, and linker optimization flags. Dev keeps the same native tuning, turns on profiling hooks, adds debug info and assertions, and builds the full test/profile surface.
Dev also carries the aggressive host-side warning profile. Optional host-only sanitizers are available via `-DFASTLOADER_DEV_ENABLE_ASAN=ON` and `-DFASTLOADER_DEV_ENABLE_UBSAN=ON` when you explicitly want an instrumented local pass.

The pinned local dependency layout is:

- ONNX Runtime GPU under `/opt/onnxruntime`
- CUDA 13.2 LibTorch under `/opt/pytorch-2.10.0-cu132`
- CPU-only checkpoint parser venv under `/opt/fastloader-pth-parse`

## Test And Analysis

Run the native suite from the dev tree:

```bash
ctest --preset dev --output-on-failure
ctest --test-dir build/dev --output-on-failure -R rfdetr_native_integration
```

Run static analysis locally:

```bash
./utilities/run_static_analysis.sh
./utilities/run_static_analysis.sh build/dev
```

`utilities/run_static_analysis.sh` runs `clang-tidy`, `cppcheck`, `valgrind` against `build/dev/test_roundtrip`, and `vulture` across the repo Python utilities/tests.
The default profile is intentionally aggressive: `clang-tidy` runs `clang-analyzer`, `bugprone`, `performance`, and `misc-use-internal-linkage`, with analyzer/bugprone/performance findings treated as errors; `cppcheck` runs `--enable=all --inconclusive --check-level=exhaustive` with Linux-focused suppressions. Clang analyzer alpha checks stay available, but they are opt-in because the current Clang 21 alpha bundle is noisy and internally inconsistent on some check combinations. Useful knobs:

- `FASTLOADER_STATIC_ENABLE_ANALYZER_ALPHA=1` to enable experimental clang analyzer alpha checks
- `FASTLOADER_STATIC_INCLUDE_TESTS=0` to analyze only `src/`
- `FASTLOADER_STATIC_ENABLE_CHECK_PROFILE=1` to emit clang-tidy timing profiles under `build/dev/`

## Runtime Surface

There is no public `fastloader` Python runtime module. The supported surfaces are:

- the native library
- `fastloader_cli`
- the native RF-DETR runtime

Python remains only as an embedded dependency for upstream `.pth` checkpoint parsing.

Most direct runtime commands need the pinned local runtime libraries on `LD_LIBRARY_PATH`:

```bash
export FASTLOADER_RUNTIME_LD_LIBRARY_PATH=/usr/local/cuda-13.2/lib64:/opt/pytorch-2.10.0-cu132/lib:/opt/onnxruntime/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
```

## Usage

Compile a dataset:

```bash
env LD_LIBRARY_PATH="$FASTLOADER_RUNTIME_LD_LIBRARY_PATH" \
./build/release/fastloader_cli compile /path/to/dataset /path/to/output train 432

env LD_LIBRARY_PATH="$FASTLOADER_RUNTIME_LD_LIBRARY_PATH" \
./build/release/fastloader_cli compile /path/to/dataset /path/to/output val 432
```

Inspect a compiled file:

```bash
env LD_LIBRARY_PATH="$FASTLOADER_RUNTIME_LD_LIBRARY_PATH" \
./build/release/fastloader_cli info /path/to/compiled/train.bin
```

Benchmark a compiled file:

```bash
env LD_LIBRARY_PATH="$FASTLOADER_RUNTIME_LD_LIBRARY_PATH" \
./build/release/fastloader_cli bench /path/to/compiled/train.bin 32 3
```

RF-DETR native predict:

```bash
env LD_LIBRARY_PATH="$FASTLOADER_RUNTIME_LD_LIBRARY_PATH" \
./build/release/fastloader_cli rfdetr predict \
  --compiled ./compiled/train.bin \
  --weights ./weights/rf-detr-nano.pth \
  --output ./predictions.json \
  --batch-size 4 \
  --threshold 0.5 \
  --workers 8 \
  --lanes 2 \
  --cpu-affinity 0-15
```

RF-DETR native train:

```bash
env LD_LIBRARY_PATH="$FASTLOADER_RUNTIME_LD_LIBRARY_PATH" \
./build/release/fastloader_cli rfdetr train \
  --train-compiled ./compiled/train.bin \
  --val-compiled ./compiled/val.bin \
  --output-dir ./output-rfdetr \
  --weights ./weights/rf-detr-nano.pth \
  --epochs 12 \
  --batch-size 2 \
  --grad-accum-steps 1 \
  --workers 8 \
  --cpu-affinity 0-15
```

Seg-medium validation helper:

```bash
./utilities/run_seg_medium_validation.sh
./utilities/run_seg_medium_validation.sh --profile
```

For `rfdetr validate`, the public `batch_size` knob means:

- ONNX/TensorRT: synthetic width backed by internal lane parallelism
- checkpoint evaluation: real PyTorch batch size

## Profiling

Profiling is built only in `dev`. The profiling hooks compile to no-ops in `release`.

Canonical host-side entrypoint:

```bash
./utilities/profile_baseline.sh
./utilities/profile_baseline.sh "$PWD/profiles/$(date +%s).log" 10
```

Low-level local runner:

```bash
./utilities/run_profile.sh
./utilities/run_profile.sh "$PWD/profiles/$(date +%s).log" "$PWD/build/dev"
```

Key env vars:

- `FASTLOADER_PROFILE_REPETITIONS`, `FASTLOADER_PROFILE_WARMUP_RUNS`
- `FASTLOADER_PROFILE_ENABLE_RFDETR_TRAIN`
- `FASTLOADER_PROFILE_RFDETR_TRAIN_WEIGHTS`
- `FASTLOADER_PROFILE_RFDETR_TRAIN_NUM_IMAGES`
- `FASTLOADER_PROFILE_RFDETR_TRAIN_BATCH_SIZE`
- `FASTLOADER_PROFILE_RFDETR_TRAIN_WORKERS`
- `FASTLOADER_PROFILE_RFDETR_TRAIN_PREFETCH_FACTOR`
- `FASTLOADER_PROFILE_ENABLE_RFDETR_VALIDATE`
- `FASTLOADER_PROFILE_RFDETR_VALIDATE_COMPILED`
- `FASTLOADER_PROFILE_RFDETR_VALIDATE_CHECKPOINT`
- `FASTLOADER_PROFILE_RFDETR_VALIDATE_LIMIT_IMAGES`
- `FASTLOADER_PROFILE_RFDETR_VALIDATE_WORKERS`
- `FASTLOADER_PROFILE_RFDETR_VALIDATE_BATCH_SIZE`
- `FASTLOADER_PROFILE_RFDETR_VALIDATE_CPU_AFFINITY`

Short RF-DETR native train profile pass:

```bash
FASTLOADER_PROFILE_ENABLE_RFDETR_TRAIN=1 \
FASTLOADER_PROFILE_RFDETR_TRAIN_WEIGHTS="$PWD/engines/output-seg-medium/rf-detr-seg-medium.pt" \
FASTLOADER_PROFILE_RFDETR_TRAIN_NUM_IMAGES=24 \
FASTLOADER_PROFILE_RFDETR_TRAIN_BATCH_SIZE=6 \
FASTLOADER_PROFILE_RFDETR_TRAIN_WORKERS=16 \
FASTLOADER_PROFILE_RFDETR_TRAIN_PREFETCH_FACTOR=2 \
FASTLOADER_BASELINE_WARMUP_RUNS=0 \
./utilities/profile_baseline.sh profiles/rfdetr-train.log 1
```

Direct train-profile runner call:

```bash
env LD_LIBRARY_PATH="$FASTLOADER_RUNTIME_LD_LIBRARY_PATH" \
FASTLOADER_PROFILE_LOG="$PWD/profiles/rfdetr-train.local.log" \
FASTLOADER_PROFILE_APPEND=0 \
./build/dev/fastloader_rfdetr_train_profile_runner \
  --test-dir /tmp/fastloader_profile_fixture/rfdetr-train \
  --weights-path "$PWD/engines/output-seg-medium/rf-detr-seg-medium.pt" \
  --width 432 \
  --height 432 \
  --num-images 24 \
  --batch-size 6 \
  --epochs 1 \
  --device-id 0 \
  --workers 16 \
  --prefetch-factor 2 \
  --compile-workers -1 \
  --repetitions 1 \
  --warmup-runs 0
```

Full native checkpoint evaluation profile pass:

```bash
FASTLOADER_PROFILE_ENABLE_RFDETR_VALIDATE=1 \
FASTLOADER_PROFILE_RFDETR_VALIDATE_LIMIT_IMAGES=0 \
FASTLOADER_PROFILE_RFDETR_VALIDATE_WORKERS=16 \
FASTLOADER_PROFILE_RFDETR_VALIDATE_BATCH_SIZE=5 \
FASTLOADER_BASELINE_WARMUP_RUNS=0 \
./utilities/profile_baseline.sh profiles/rfdetr-validate.log 1
```

## Compiled Binary Format

```text
[FileHeader]       - magic, dimensions, class names, section offsets
[ImageEntry[N]]    - per-image: pixel offset, label offset, instance count
[PackedInstance[]] - flat labels: class_id, bbox_xyxy, mask RLE pointer
[RLEPair[]]        - mask RLE pairs (start, length)
[float32 blob]     - NCHW [0,1] pixel data, 2MB-aligned for huge pages
```

- `PackedInstance`: 16 bytes
- `ImageEntry`: 24 bytes
- Pixel blob: `N * C * H * W * 4` bytes
- 432x432 train (200k images): about 435 GB on disk

## Linux Optimizations

The loader is Linux-only and uses:

- `mmap(MAP_SHARED | MAP_POPULATE)`
- `madvise(MADV_HUGEPAGE)`
- `readahead()`
- `madvise(MADV_WILLNEED)`
- pinned host buffers via `cudaMallocHost`
- async transfers on non-blocking CUDA streams

## Project Structure

```text
include/
src/
tests/
third_party/
utilities/
```

Notable utility entrypoints:

- `utilities/install_local_rfdetr_deps.sh`
- `utilities/run_static_analysis.sh`
- `utilities/run_profile.sh`
- `utilities/profile_baseline.sh`
- `utilities/run_seg_medium_validation.sh`
