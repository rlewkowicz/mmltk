# Source datasets and compilation

[Wiki index](README.md) · [Quick start](../README.md#build) · [Commands](commands.md) · [GPU loading](gpu-execution.md)

The native compiler consumes a split directory of PNG images and JSONL
instance annotations, with a category table at the dataset root:

```text
dataset/
  categories.json
  train/
    000001.png
    000001.jsonl
    000002.png
    000002.jsonl
  val/
    000001.png
    000001.jsonl
```

Use matching six-digit, contiguous filenames starting at `000001`.
Every image needs its annotation file, including background-only images,
whose file contains no instance lines. Each selected split must be nonempty.

## Category table

`categories.json` needs a nonempty `classes` array with at most 256 entries.
IDs must be unique, dense, and start at zero or one; names must be unique,
nonempty, NUL-free, and at most 31 bytes. The compiler rejects names that would
require truncation in format 7. It normalizes source IDs into the immutable
ordered foreground [class catalog](rfdetr-workflows.md#class-identity-and-model-admission).
For example:

```json
{
  "classes": [
    { "id": 1, "name": "category_1" },
    { "id": 2, "name": "category_2" }
  ]
}
```

An optional `splits.<name>` entry must provide a positive `total` image count.
When the selected split has no such entry, the compiler scans for matching
contiguous PNG/JSONL sequences. With an entry, it uses the declared count and
opens those numbered assets. Additional metadata may describe the dataset, but
the category names, IDs, split counts, and actual images/annotations drive
compilation.

## Instance records

Each nonempty JSONL line contains one JSON object:

| Field | Meaning |
| --- | --- |
| `class` | Required category name from `categories.json` |
| `mask_rle_encoding` | Required literal `row_major_start_length` |
| `mask_rle` | Required nonempty foreground runs, encoded as whitespace-separated `start:length` pairs |
| `image_size_wh` | Optional `[width, height]`; if present, must match the PNG |
| `bbox_xyxy` | Optional declared integer bounds used for mismatch diagnostics; compiled bounds derive from the mask |

RLE offsets index the row-major source image: `y * width + x`. Runs must have
positive lengths, stay inside the image, be sorted, and not overlap. This is
start/length encoding, not alternating foreground/background counts.
Declared bounds use `[x1, y1, x2, y2]` in absolute pixels with exclusive upper
edges, matching the bounds computed from the mask.

For a 432×432 image, this example covers a 24-pixel run on each of three rows
starting at `(10, 20)`:

```json
{"class":"category_1","mask_rle_encoding":"row_major_start_length","mask_rle":"8650:24 9082:24 9514:24","bbox_xyxy":[10,20,34,23],"image_size_wh":[432,432]}
```

Malformed JSON, an unknown class, unsupported encoding, empty foreground, or
invalid runs fail compilation. The compiler letterboxes images to the target
dimensions and transforms masks consistently. Instances that lose all
foreground during resizing are dropped and counted in compilation progress.

The format and validation implementation is in
[dataset_compiler_scan_labels.cpp](../src/backend/data/dataset_compiler_scan_labels.cpp);
compiled metadata is defined in
[compiled_format.h](../src/backend/data/compiled_format.h).

## Compiled binary format

The compiler writes one self-contained, versioned `.bin` file per split. It
stores the expensive source transformations—not PNG or JSON—so runtime loading
does not decode images, parse annotations, resize masks, letterbox inputs, or
convert pixel layouts.

The current format is version 7. Its authoritative definitions and validation
rules are
[compiled_format.h](../src/backend/data/compiled_format.h) and
[compiled_file_utils.h](../src/backend/data/compiled_file_utils.h). The file is
a native little-endian Linux format written from packed fixed-width
structures; consumers must reject an unknown magic or version instead of
guessing a compatible layout.

```text
byte 0
┌──────────────────────────────────────────┐
│ FileHeader (8,328 bytes)                 │
├──────────────────────────────────────────┤ index_offset = 8,328
│ ImageEntry[num_images] (32 bytes each)   │
├──────────────────────────────────────────┤
│ zero padding to a 2 MiB boundary         │
├──────────────────────────────────────────┤ pixel_offset
│ image 0: planar RGB float32              │
│ image 1: planar RGB float32              │ fixed image_stride
│ ...                                      │
├──────────────────────────────────────────┤ label_offset
│ PackedInstance[sum(num_instances)]       │ 16 bytes each
├──────────────────────────────────────────┤ mask_rle_offset
│ RLEPair[sum(mask_rle_pairs)]             │ 8 bytes each
└──────────────────────────────────────────┘ total_file_size
```

All offsets in `FileHeader` are absolute file offsets. `ImageEntry.pixel_offset`
is also absolute. `ImageEntry.label_offset` is relative to the start of the
label block, and `PackedInstance.mask_rle_offset` is relative to the start of
the RLE block. Images, labels, and RLE pairs appear in compiled image order
without holes inside their respective blocks.

### File header

`FileHeader` is packed and exactly 8,328 bytes:

| Byte | Type | Field | Meaning |
| ---: | --- | --- | --- |
| 0 | `uint64` | `magic` | `0x464153544c445232` |
| 8 | `uint32` | `version` | Current value: `7` |
| 12 | `uint32` | `num_images` | Number of images in this split |
| 16 | `uint32` | `image_width` | Compiled image width |
| 20 | `uint32` | `image_height` | Compiled image height |
| 24 | `uint32` | `channels` | Channel count; compiled RGB data uses 3 |
| 28 | `uint32` | `num_classes` | Used entries in `class_names`, 1–256 |
| 32 | `uint64` | `index_offset` | Start of the `ImageEntry` array |
| 40 | `uint64` | `label_offset` | Start of the packed instance block |
| 48 | `uint64` | `pixel_offset` | Start of the image blob |
| 56 | `uint64` | `mask_rle_offset` | Start of the RLE-pair block |
| 64 | `uint64` | `total_file_size` | Exact expected file length |
| 72 | `uint64` | `image_stride` | Bytes per image: `width × height × channels × 4` |
| 80 | `char[256][32]` | `class_names` | NUL-terminated class names indexed by normalized class ID |
| 8,272 | `uint32` | `max_instances_per_image` | Maximum instance count in any image |
| 8,276 | `uint8[52]` | reserved | Zeroed space retained for format evolution |

The pixel block begins at
`align_up(sizeof(FileHeader) + num_images * 32, 2 MiB)`. Its size must be
exactly `num_images * image_stride`; the label block follows it immediately,
then the RLE block, then end of file.

Each image is already letterboxed to the compiled dimensions and stored as
three contiguous planes in `NCHW` order: all red pixels, then green, then blue.
Each sample is a native IEEE-754 `float32` normalized from 8-bit RGB to
`[0, 1]`. Letterbox padding is zero.

### Per-image index

Each packed `ImageEntry` is 32 bytes:

| Byte | Type | Field | Meaning |
| ---: | --- | --- | --- |
| 0 | `uint64` | `pixel_offset` | Absolute start of this image's fixed-stride pixels |
| 8 | `uint32` | `label_offset` | Byte offset into the label block |
| 12 | `uint16` | `num_instances` | Number of instances for this image |
| 14 | `uint16` | padding | Zero |
| 16 | `uint32` | `label_bytes` | `num_instances * 16` |
| 20 | `uint32` | `original_width` | Source width before compilation |
| 24 | `uint32` | `original_height` | Source height before compilation |
| 28 | `uint32` | reserved | Zero |

The original dimensions allow consumers to reconstruct the exact letterbox
transform without storing redundant per-image scale and padding values.

### Instances and masks

Each packed `PackedInstance` is 16 bytes:

| Byte | Type | Field | Meaning |
| ---: | --- | --- | --- |
| 0 | `uint8` | `class_id` | Dense zero-based index into `class_names` |
| 1 | `uint8` | padding | Zero |
| 2 | `int16` | `bbox_x1` | Inclusive left bound in compiled pixels |
| 4 | `int16` | `bbox_y1` | Inclusive top bound |
| 6 | `int16` | `bbox_x2` | Exclusive right bound |
| 8 | `int16` | `bbox_y2` | Exclusive bottom bound |
| 10 | `uint32` | `mask_rle_offset` | Byte offset into the RLE block |
| 14 | `uint16` | `mask_rle_pairs` | Number of runs owned by this instance |

Every `RLEPair` is `{ uint32 start, uint32 length }` and therefore 8 bytes.
Starts index the compiled `width × height` mask in row-major order. Runs are
positive, sorted, non-overlapping, and contained by the image. Bounding boxes
are recomputed from these transformed runs during compilation, so pixels,
bounds, and masks share one geometry.

The packed limits are intentional: class IDs occupy one byte, coordinates must
fit signed 16-bit values, and an image's instance count and an instance's RLE
pair count must each fit an unsigned 16-bit value. Compilation rejects values
that cannot be represented.

## Compile and inspect

```bash
./mmltk compile --source-dir ./dataset --output-dir ./compiled --split train --width 432 --height 432
./mmltk compile --source-dir ./dataset --output-dir ./compiled --split val --width 432 --height 432
./mmltk info --compiled ./compiled/train.bin
```

`compile` handles one selected split. It also exposes CUDA mask batch size,
CUDA device, and CPU worker options through `./mmltk compile --help`.
Supplying width without height makes the target square. The RF-DETR-specific
`rfdetr compile` command handles its train/validation dataset preparation;
consult its own help for model-specific dimensions and options.

`rfdetr compile` accepts `--perceptual-downscale`. The GUI exposes the same
optional choice with its compilation controls; the root `compile` command
retains its existing option surface.

To measure actual loading rather than inspect metadata:

```bash
./mmltk bench --compiled ./compiled/train.bin --batch-size 32 --epochs 1
```

This benchmark reads data and performs loader work.
`./mmltk --diagnose-io ./compiled/train.bin` is a separate read-only capability
inspection that does not read dataset contents or benchmark transfers.

## Benchmark-source acquisition

The separate [benchmark dataset compiler](../src/backend/data/benchmark_dataset_compiler.h)
prepares COCO 2017, Objects365 v2, and Open Images v7 inputs through the
backend data layer. [benchmark_compiler.cpp](../src/backend/data/benchmark_compiler.cpp)
owns orchestration, normalized annotations, split selection, and compiled
publication. [open_images_acquisition.cpp](../src/backend/data/open_images_acquisition.cpp)
owns Open Images acquisition and quarantine decisions;
[benchmark_storage.cpp](../src/backend/data/benchmark_storage.cpp) owns storage
checks and concurrent reservations; and
[benchmark_progress.cpp](../src/backend/data/benchmark_progress.cpp) owns
source/phase progress projection. Download/cache identity stays with the data
layer while resizing uses the shared imaging owners below.

The normalized annotation-index cache has its own version-2 format and
256-byte header, independent of compiled format 7.
[AnnotationRejectCounts](../src/backend/data/detail/benchmark_annotations.h)
is the canonical declaration for its six rejection counters. Binary
encode/decode follows reflected declaration order; compile-time guards pin
the six names, `uint64` types, and order required by that cache format. Named
JSON projections derive from those same fields. The
[index implementation](../src/backend/data/benchmark_annotations.cpp) owns
the header layout and staged publication.

## Optional perceptual downscaling

Compilation and GPU augmentation each expose an independent
`perceptual_downscale` setting, false by default. RF-DETR CLI compilation uses
`--perceptual-downscale`; training augmentation uses
`--aug-perceptual-downscale` alongside enabled GPU augmentation. The normal
resizing path and its pixels remain unchanged when the option is off.
The option affects shrinking RGB pixels, not categorical masks, boxes, class
identity, letterbox geometry, or compiled format 7.

[RgbImageResizer](../src/backend/imaging/resample/image_resize.h) owns CPU execution and
[GpuPerceptualDownscaler](../src/backend/imaging/resample/image_resize_cuda.h) owns reusable
CUDA workspace. Both implement the SSIM local-moment method attributed in
[perceptual_downscale_math.h](../src/backend/imaging/resample/detail/perceptual_downscale_math.h).
They accept checked RGB8, straight-alpha RGBA8, or planar unit-sRGB float views
with explicit byte strides and capacities. sRGB is decoded once, the filter
works in linear-light Y/Cb/Cr, and the output is encoded once. RGBA filtering
uses premultiplied linear color and area-averaged alpha, with safe
unpremultiplication. Nonfinite/out-of-range float samples clamp deterministically.

The checked downscale operation rejects enlargement and overlapping nonidentity
views; an identity copy is exact. The ordinary CPU resize entrypoint retains
its established enlargement path. CUDA callers supply the execution context,
stream, producer dependencies, and exact source/destination custody. Tables
and tightly sized working buffers are reused; cross-stream reuse follows GPU
completion, and capacity pressure is bounded. The augmentation owner prepares
configuration changes transactionally before admitting the new execution.

Compilation records the selected resampling policy in its benchmark compilation
facts without changing downloaded source-cache identity. Recompile when changing
the policy; an existing `.bin` already contains its transformed pixels.
Perceptual downscaling can emphasize noise and makes no detector-accuracy or
measured performance claim.

## Loading a compiled file

Metadata-only inspection reads and validates the header and section extents:

```bash
./mmltk info --compiled ./compiled/train.bin
```

Product loading opens the file through
[`CompiledDataset`](../src/backend/data/compiled_dataset.h). Opening performs
the complete structural check before exposing any view:

1. map the regular file read-only with `MAP_SHARED`;
2. copy and validate the header magic, version, dimensions, stride, and limits;
3. validate section order, exact sizes, 2 MiB pixel alignment, and end of file;
4. validate every pixel and label index, original dimension, class ID,
   bounding box, RLE span, and RLE run;
5. expose immutable spans over the mapped index, labels, and masks, plus direct
   fixed-stride pixel addresses.

Invalid or truncated files fail at open. The mapping owns the lifetime of every
metadata and pixel view, so callers do not retain pointers beyond the
`CompiledDataset`.

Training and inference use
[`DatasetLoader`](../src/backend/data/dataset_loader.h), which turns those
mapped images into bounded, leased GPU batches. At a high level:

```cpp
mmltk::backend::data::DatasetLoader loader({
    .compiled_path = "./compiled/train.bin",
    .batch_size = 32,
    .prefetch_factor = 6,
});

mmltk::backend::data::Batch batch{};
while (loader.next_batch(batch)) {
    loader.handoff_batch(batch, consumer_cuda_stream);
    // Enqueue GPU work using batch.device_images and its mapped metadata.
    loader.release_batch(batch, consumer_cuda_stream);
}
```

`handoff_batch` makes the consumer stream wait for the slot's transfer event
without synchronizing the whole device. `release_batch` records completion on
that stream; the slot cannot be refilled until the GPU consumer has finished.
Batch pointers are leases, not independently owned allocations. A checked-out
batch must be released, and stale or foreign leases are rejected.

Sequential epochs retain file order. Shuffled epochs use chunks sized to the
larger of eight images or one batch, capped by the dataset size. They shuffle
chunks within locality blocks targeting 256 images and shuffle the blocks,
while retaining image order inside each chunk. A block contains at least one
whole chunk, so its actual extent depends on batch size. Batch sharding selects
complete batch ordinals for a rank, and `drop_last` controls the partial final
batch.

## Why compiled loading is fast

Most of the speed comes from moving variable-cost work out of the hot path:

- PNG decode, RGB conversion, resizing, letterboxing, JSON parsing, mask
  transformation, bounding-box derivation, and validation happen once during
  compilation.
- Runtime images have a fixed `float32` stride and O(1) address calculation.
  Labels and masks are compact packed arrays rather than per-image object
  graphs.
- The 2 MiB-aligned pixel blob is friendly to huge-page-backed file-cache
  mappings. The loader supplies Linux `madvise` hints separately for the
  sequential index/metadata and for normal, sequential, or random pixel access.
- Consecutive source images with consecutive batch destinations are coalesced.
  File-cache population and copies are issued in bounded chunks of at most
  16 MiB, with cancellation checked between chunks.
- The locality-aware shuffle avoids turning every batch into unrelated page
  faults while still changing global and within-block order each epoch.
- Prefetch slots, read workers, read lists, pinned host buffers, device
  buffers, CUDA events, and the copy stream are bounded and reused. Buffers
  grow only to a high-water mark instead of allocating per batch.
- Workers and pinned pages use the resolved GPU-local CPU/NUMA placement.
  Host pages are bound, prefaulted, residency-checked, and then registered with
  CUDA.
- I/O gathering, transfer, and model consumption overlap across slots.
  Completion is event-driven on a dedicated owner worker; the design avoids
  polling and avoids device-wide synchronization during ordinary batches.

### Default H2D path

The default path copies demanded mapped pixels into persistent GPU-local,
NUMA-bound pinned host storage, then submits one asynchronous
`cudaMemcpyAsync` for the packed batch on a nonblocking high-priority copy
stream. A CUDA event hands the finished device slot to the consumer stream.
This path works without GDRCopy:

```bash
./mmltk bench --compiled ./compiled/train.bin --batch-size 32 --epochs 1
```

For a contiguous CPU batch, `host_images` can alias the mapped pixel blob
directly. Other CPU views are materialized only when requested.

### GDRCopy path

Where supported, `--gdrcopy` replaces the pinned-host-plus-H2D path with
persistent GPU allocations mapped into CPU address space. Read workers copy
file-backed bytes directly into that mapping with GDRCopy, publish a device
pointer lease, and track consumer completion with CUDA events:

```bash
./mmltk bench --compiled ./compiled/train.bin --batch-size 32 --epochs 1 --gdrcopy
```

This removes the intermediate pinned staging copy and the explicit
host-to-device DMA submission. It is not storage-to-GPU DMA: the compiled file
still enters through the Linux page cache and a CPU worker performs the copy
into mapped GPU memory. GDRCopy has no automatic fallback; use the default H2D
path if the selected GPU, driver, `/dev/gdrdrv`, or CUDA DMA-BUF mapping path
cannot support it.

Use `--prefetch-factor`, worker, CPU-affinity, and NUMA options only on commands
that expose them; the exact surface is listed by that command's `--help`.
Measure the real workload before increasing prefetch depth: each slot owns
capacity for a full batch, so extra overlap also consumes more pinned or device
memory. See [GPU-local execution](gpu-execution.md) for placement, transport,
capability inspection, and functional GDR checks.

## Explore thumbnails and atlas residency

Explore uses
[CompiledImageStream](../src/backend/data/compiled_image_stream.h) for reusable
disk-read, host-transfer, and device lanes. Explore's read scheduler retains a
separate detail lane, so selecting a full-resolution image can progress
independently of unrelated gallery reads. The default H2D path retains pinned
host pixels and descriptor storage through asynchronous transfers; GDRCopy
uses the explicitly selected mapped-device route described above.

Within Explore, [GalleryReadScheduler](../src/controller/subsystems/explore/detail/gallery_read_scheduler.h)
owns that stream, lane state, read/transfer callbacks, demand priority, and
ingress settlement.
[GalleryDescriptorStorage](../src/controller/subsystems/explore/detail/gallery_descriptor_storage.h)
owns pinned descriptor staging, device descriptor arrays, and augmentation
working storage.
[ExploreHostAllocations](../src/controller/subsystems/explore/detail/gallery_host_allocations.h)
retains the pinned allocation owners. `GalleryStream` coordinates their
transactions with the thumbnail cache and physical atlas; its public surface
does not expose their mutable storage.

The [gallery stream](../src/controller/subsystems/explore/gallery_stream.cpp)
first places completed cached visible thumbnails. Disk admission and ready
GPU work then prioritize:

1. Every visible row, including partially visible rows, in row order.
2. Up to four rows forward in the last nonzero scroll direction, nearest first.
3. Up to four rows behind, nearest first.

Initial direction is increasing row order. The two neighbor ranges clip
independently at dataset boundaries; missing rows on one side do not enlarge
the other side. Priority governs admission, not asynchronous completion.
In-flight work retains its custody, useful settled inputs can be rebound to
new demand, and speculative admission preserves foreground lane capacity.
Scroll position and direction supply demand; mouse hover and selection do not
change loading priority. The frontend can submit its latest measured viewport
while native work is busy.

All disk/augmentation/raster misses produce completed individual GPU cache
tiles before atlas placement. Pixel identity includes the retained dataset
incarnation, compiled image, augmentation configuration/seed, and card raster
extent. Filtered position and viewport row count are demand, not pixel identity.
Overlay validity is separate: box/mask/class changes reuse clean tiles and
retained annotation meaning; label visibility remains presentation state.
Per-image augmentation and donor choices remain independent of batch order.

Enabling augmentation, rerolling its seed, or disabling it keeps completed
tiles when dataset identity, the retained compiled-file incarnation, and card
raster extent still match. `GalleryThumbnailCache::Retained` exposes those
drawable pixels together with their existing `GalleryTileMeaning`.
`Find` additionally requires the current preview identity: retained entries
marked `refresh_pending` remain drawable but still need replacement work.
An incompatible dataset/incarnation or raster extent invalidates the entries.

The first atlas result restores these completed visible tiles before admitting
new misses. Each refreshed tile replaces both pixels and annotation meaning
after completion. Semantic overlay changes can redraw boxes or masks from the
retained meaning during a pending preview refresh; they do not clear
`refresh_pending` or claim that old clean pixels satisfy the new preview.
Thus labels and semantic geometry continue to describe the displayed pixels
through enable, reroll, disable, cancellation, and partial progress.

[GalleryThumbnailCache](../src/controller/subsystems/explore/detail/gallery_thumbnail_cache.h)
retains bounded high-water capacity derived from the admitted maximum viewport
plus eight neighboring rows, capped by dataset size. Its global ceiling derives
from `kExploreVisibleItemCapacity`. Demand pins are installed before eviction;
physical readers/writers and rollback banks separately protect in-flight data.
Smaller demand does not shrink storage or clear useful cached entries. Cache
pixels, clean/semantic banks, lanes, descriptors, and retained metadata all
consume storage; diagnostic byte categories include their existing subsets
and must not be added twice.

`GalleryThumbnailCache::PhysicalRow` owns the physical slot/bank row
calculation used by rendering, atlas copies, and probes. Callers combine that
row with the actual buffer pitch; demand size and vector capacity do not
define physical tile addresses.

Host-page pinning and device-cache residency describe different resources.
Host pages remain registered through H2D/D2H completion. Cached GPU pixels stay
resident by retaining device allocations and read/write custody; GPU storage
is not registered as host memory.

[GalleryAtlas](../src/controller/subsystems/explore/detail/gallery_atlas.h)
keeps a separate directory for each physical output owner.
Each directory records its allocation identity, layout generation, initialized
cells, and clean/semantic meaning. A logical row maps to the allocation's
circular row position. A writable candidate reconciles against its own
contents, which may be older than the preceding publication; it does not copy
a complete atlas to establish a patch baseline.

First-use or invalid cells are initialized, missing visible tiles get explicit
placeholders, and removed/unused cells—including padding in the partial final
row—are cleared. Matching overlapping cells survive. Every touched entry is
invalidated before a GPU write; new meaning is installed only after successful
settlement. A failed or cancelled partial write leaves those cells invalid and
preserves the last committed product. Detail output invalidates the acquired
owner's atlas directory before any copy, growth, or clearing.

Gallery/detail retention and the exact displayed layout, crop, labels, and hit
identity are described in [GUI interaction](gui-interaction.md#explore-gallery-and-displayed-geometry).
