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
require truncation in the compiled format. It normalizes source IDs into the
immutable ordered foreground [class catalog](rfdetr-workflows.md#class-identity-and-model-admission).
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
| `bbox_xyxy` | Continuous `[x1, y1, x2, y2]` source-pixel corners; authoritative when supplied |
| `mask_rle` | Optional foreground runs as whitespace-separated `start:length` pairs; an empty string means a present, empty mask |
| `mask_rle_encoding` | Required literal `row_major_start_length` when `mask_rle` is present |
| `image_size_wh` | Optional `[width, height]`; if present, must match the PNG |
| `area` | Optional finite, nonnegative original annotation area in source-pixel units |
| `iscrowd`, `ignore` | Optional boolean or integer `0`/`1`; crowd and raw ignore remain separate facts |
| `id`, `image_id`, `category_id` | Optional nonnegative integer source annotation, image, and category identities |

RLE offsets index the row-major source image: `y * width + x`. Runs must have
positive lengths, stay inside the image, be sorted, and not overlap. This is
start/length encoding, not alternating foreground/background counts.
Boxes require finite coordinates and strictly ordered corners. Fractional and
out-of-image coordinates are retained through the compile transform. If the
box is absent, a nonempty source mask supplies its bounds before resizing;
mask-derived upper edges are exclusive. A supplied box is never replaced by
mask bounds, even when the two disagree. Masks may extend beyond their boxes.

If `area` is absent, a present mask supplies its source foreground count,
including zero for an empty mask; otherwise the source box supplies its area.
`image_id` values within one JSONL file must agree. An omitted `category_id`
uses that class's source ID from `categories.json`. Annotation IDs are optional;
source line ordinals preserve order. Equal class/box records remain separate
annotations.

For a 432×432 image, this example covers a 24-pixel run on each of three rows
starting at `(10, 20)`:

```json
{"class":"category_1","mask_rle_encoding":"row_major_start_length","mask_rle":"8650:24 9082:24 9514:24","bbox_xyxy":[10,20,34,23],"image_size_wh":[432,432]}
```

Malformed JSON, an unknown class, unsupported mask encoding, invalid runs,
invalid metadata, or the absence of both a valid box and a nonempty source
mask fail compilation. A valid detection annotation survives when its resized
mask becomes empty. Omitting `mask_rle` and supplying `"mask_rle":""` therefore
have different meanings. Segmentation consumers require explicit mask presence;
box-only data remains usable for detection.

The format and validation implementation is in
[dataset_compiler_scan_labels.cpp](../src/backend/data/compiler/dataset_compiler_scan_labels.cpp);
compiled metadata is defined in
[compiled_format.h](../src/backend/data/compiled/compiled_format.h).

## Resize geometry

Both generic and benchmark compilation default to **Stretch**. It fills the
target canvas using independent horizontal and vertical scales. **Letterbox**
preserves source aspect, centers the resized content, and fills padding with
zero RGB. Select it explicitly with `--resize-mode Letterbox` or the Dataset
card's radio. Geometry and optional perceptual resampling are independent
choices; changing either requires recompiling the affected splits.

[ImageResizeMode and ImageResizeGeometry](../src/backend/imaging/resample/image_resize.h)
are the shared authority. Letterbox fits one target axis exactly, rounds the
other resized extent to the nearest positive integer, and floors each half
padding offset. A source coordinate becomes
`x * resized_width / original_width + offset_x`, with the corresponding
vertical formula. Pixels, continuous boxes, and nearest-sampled categorical
masks use this geometry. The stored mode and original dimensions let readers
reconstruct the content rectangle exactly.

The [Original viewer](gui-interaction.md#original-view-and-annotation-import)
restores source aspect from these compiled pixels and geometry; it does not
recover source resolution. Explore and retained Validation detail frames carry
the stored resize mode as explicit provenance, alongside source and content
extents. [Upscale preparation](gpu-execution.md#upscale-input-preparation) uses
that provenance independently of the viewer preference.

Both compilers use the same
[CompilePixelWorkspace](../src/backend/data/detail/compile_pixel_workspace.h)
decode/resize operation and `RgbImageResizer::resize_to_planar` to project
packed RGB8 into the compiled planar float canvas. Its immutable prepared plan
binds the admitted header, decode storage, resize algorithm, geometry, and
destination layout. Execution checks the actual encoded header against that
plan before writing pixels. Perceptual shrinking writes directly
to those planes while preserving RGB8 quantization followed by
`float(byte) * (1.0F / 255.0F)`. Ordinary AVIR resizing and enlargement reuse
the resizer's byte scratch. Identity conversion and zero Letterbox padding
retain the same pixel values.

Categorical mask resizing uses the
[shared RLE sampler](../src/backend/data/mask_rle_utils.cpp). It validates sorted
source runs, maps interval boundaries with exact nearest-center inverse
coordinates, and reuses row-interval scratch. The compiler emits transformed
runs directly, coalescing adjacent support without a full target bitmap.
Source bounds and foreground counts still
come from the source runs. Dense-mask callers share the run emitter; their AVX2
scan compares 32 bytes at a time with bounded scalar tails. The RGB-to-planar
conversion processes sixteen pixels per SIMD step. These paths preserve exact
mask support and the existing RGB normalization; [fixture evidence](validation.md#fixed-storage-measurements)
does not establish an isolated SIMD speedup.

## Compiled binary format

Each split becomes a self-contained `.bin` containing transformed pixels and
annotations; runtime loading needs no decode, parsing, resizing, or pixel-layout
conversion. **Version 10** is a native little-endian Linux format of packed
fixed-width structures, defined and validated by
[compiled_format.h](../src/backend/data/compiled/compiled_format.h) and
[compiled_file_utils.h](../src/backend/data/compiled/compiled_file_utils.h).
Readers reject unknown magic/version. Format 9 and all older files require
recompilation; there is no legacy reader or migration.

```text
byte 0
┌────────────────────────────────────────────┐
│ FileHeader (8,320 bytes)                    │
│ padding to a 2 MiB boundary                 │
├────────────────────────────────────────────┤ data_offset
│ Block 0: 64 page-aligned pixel slots        │
│          64 page-aligned label slots        │
│          padding to a 2 MiB block extent    │
├────────────────────────────────────────────┤
│ Block 1: same fixed layout                  │
│ ...                                        │
├────────────────────────────────────────────┤ index_offset
│ ImageEntry[num_slots] (32 bytes each)       │
└────────────────────────────────────────────┘ total_file_size
```

Each physical slot has arithmetic pixel and label locations. Labels in one
slot contain its 60-byte instance records, padding to eight bytes, then its
8-byte mask runs. `PackedInstance.mask_rle_offset` is relative to that sample's
run region. There are no global label/run arrays or per-image pixel offsets.

For logical pixel bytes `P = width × height × 3 × sizeof(float)` and label
capacity `L`, the canonical
[FileLayout](../src/backend/data/compiled/compiled_file_layout.cpp) computes:

```text
pixel_pitch     = align_up(P, 4096)
label_pitch     = align_up(L, 4096)
block_bytes     = align_up(64 × (pixel_pitch + label_pitch), 2 MiB)
data_offset     = align_up(sizeof(FileHeader), 2 MiB)
block_base(i)   = data_offset + (i / 64) × block_bytes
pixel_offset(i) = block_base(i) + (i % 64) × pixel_pitch
label_offset(i) = block_base(i) + 64 × pixel_pitch + (i % 64) × label_pitch
block_count     = (num_slots + 63) / 64
index_offset    = data_offset + block_count × block_bytes
total_file_size = index_offset + num_slots × sizeof(ImageEntry)
```

Integer division is used. Arithmetic and native file-offset representation are
checked before allocation, mapping, or access. Final partial blocks retain the
same 64-slot extent. Inactive slots remain holes; physical slot IDs do not change
during finalization. `num_images` counts eligible samples, while `num_slots`
bounds physical IDs. Benchmark slots follow readiness arrival order, so
recompilation does not promise the same physical order or byte-identical files.

### Staging and in-place compaction

Both compilers initially allow **500,000 usable bytes per sample** for the
complete label payload, including instances, runs, and internal alignment.
The page-aligned staging pitch is 503,808 bytes; its extra page padding does
not enlarge the usable capacity. There is no annotation sizing scan. Overflow
is fatal and identifies the image, required bytes, and capacity; exact masks
are never truncated or simplified to fit.

After writers and borrowed mappings settle, finalization measures the largest
complete eligible `ImageEntry.label_bytes`, records that exact value as
`FileHeader.max_label_bytes`, and derives the final layout from it. A split
containing only background images has a maximum and label pitch of zero.
Rejected or speculative slots do not influence the maximum.

[Compaction](../src/backend/data/compiled/compiled_file_compaction.cpp) moves only
live pixel and label prefixes forward within the same staged inode, using at
most 4 MiB of reusable pinned scratch. Within each block it visits pixels then
labels in increasing source order, reading a complete chunk before any
overlapping destination write. Adjacent live pixel extents use the full admitted
scratch window. Label windows capture up to one block's live prefixes before
batched page-covered allocation and consumed-source reclamation; nonadjacent
prefixes retain separate writes. A capacity rejection falls back to smaller safe
operations while captured source bytes remain available. A larger supplied
window is a maximum, not a new minimum storage or scratch requirement.
Compaction preserves physical slot IDs and sample-local run offsets, reclaims
unused extents where hole punching is supported, and
truncates to the computed final length. No move buffer is needed when the label
pitch is unchanged. Header and resident-metadata records are then written and synced
before atomic publication. Failure or cancellation preserves the prior published
output; the unpublished staging file is retired.

Sparse file length and allocated storage are separate facts. Writers reserve
and preallocate page-covered pixel and used-label writes, not the entire sparse
prefix. Stable block mappings survive growth while borrowed. Final file length
still includes arithmetic holes and block/page padding; it is not the total
number of bytes physically allocated or read by the loader. The
[shared storage ledger](compile-execution.md#file-preparation-and-settlement)
coordinates outstanding growth on each destination filesystem.

### File header

`FileHeader` is packed and exactly 8,320 bytes. Its file offsets are absolute:

| Byte | Type | Field | Meaning |
| ---: | --- | --- | --- |
| 0 | `uint64` | `magic` | `0x464153544c445232` |
| 8 | `uint32` | `version` | Current value: `10` |
| 12 | `uint32` | `num_images` | Eligible image count |
| 16 | `uint32` | `num_slots` | Physical slot extent, including inactive holes |
| 20 | `uint32` | `image_width` | Compiled image width |
| 24 | `uint32` | `image_height` | Compiled image height |
| 28 | `uint32` | `channels` | Channel count; compiled RGB data uses 3 |
| 32 | `uint32` | `num_classes` | Used entries in `class_names`, 1–256 |
| 36 | `uint64` | `index_offset` | Start of `ImageEntry[num_slots]` after the fixed blocks |
| 44 | `uint64` | `data_offset` | Start of the fixed blocks, 2 MiB aligned |
| 52 | `uint64` | `total_file_size` | Exact expected file length |
| 60 | `uint64` | `image_stride` | Logical bytes per image: `width × height × channels × 4` |
| 68 | `char[256][32]` | `class_names` | NUL-terminated class names indexed by normalized class ID |
| 8,260 | `uint32` | `max_instances_per_image` | Maximum instance count in an eligible image |
| 8,264 | `uint8` | `resize_mode` | `0`: Stretch; `1`: Letterbox |
| 8,265 | `uint32` | `max_label_bytes` | Exact maximum eligible payload bytes, including internal alignment; at most 500,000 |
| 8,269 | `uint8[51]` | reserved | Zeroed space retained for format evolution |

Each image is already resized to the compiled dimensions and stored as
three contiguous planes in `NCHW` order: all red pixels, then green, then blue.
Each sample is a native IEEE-754 `float32` normalized from 8-bit RGB to
`[0, 1]`. Letterbox padding is zero. These are raw unit-RGB values, without
ImageNet normalization; [model preprocessing](rfdetr-workflows.md#model-input-and-detection-selection)
owns that later GPU step.

### Per-image index

Each packed `ImageEntry` is 32 bytes. The array is copied into resident
application memory when the file opens, with a 1,000,000,000-byte bound checked
before using untrusted slot counts to allocate or read metadata:

| Byte | Type | Field | Meaning |
| ---: | --- | --- | --- |
| 0 | `uint16` | `num_instances` | Number of instances in this slot |
| 2 | `uint8` | `status` | `0`: inactive; `1`: eligible |
| 3 | `uint8` | `has_source_image_id` | Whether the source image ID is present |
| 4 | `uint32` | `label_bytes` | Complete used payload: `align_up(num_instances * 60, 8) + num_runs * 8` |
| 8 | `uint32` | `num_runs` | Total mask runs in this sample |
| 12 | `uint32` | `original_width` | Source width before compilation |
| 16 | `uint32` | `original_height` | Source height before compilation |
| 20 | `uint8` | `source` | Annotation provenance, using the values below |
| 21 | `uint8[3]` | reserved | Zero |
| 24 | `uint64` | `source_image_id` | Original source identity; zero when absent |

Inactive entries publish no instances, runs, or label bytes. Eligible background
entries retain valid dimensions and source facts with zero label bytes. Compiled
physical slot IDs remain separate from source IDs and filtered positions. The
original dimensions and header resize mode reconstruct the transform without
redundant per-image scale and padding fields.

| `source` | Annotation provenance |
| ---: | --- |
| 0 | Generic |
| 1 | Stock COCO |
| 2 | Stock Objects365 |
| 3 | Open Images |
| 4 | COCONut annotations on COCO images |
| 5 | COCONut annotations on Objects365 v1 images |
| 6 | COCONut annotations on Objects365 v2 images |

These values identify the annotation source independently of mask storage. COCONut
stores the physical image ID in `source_image_id`; its separate
[component inventory](benchmark-datasets.md#native-import-and-provenance)
retains any differing release-row ID and the exact archive/member join.

### Instances and masks

Each packed `PackedInstance` is 60 bytes:

| Byte | Type | Field | Meaning |
| ---: | --- | --- | --- |
| 0 | `uint8` | `class_id` | Dense zero-based index into `class_names` |
| 1 | `uint8` | `flags` | Mask presence, crowd, raw ignore, identity presence, and incomplete source ordinal |
| 2 | `float32` | `bbox_x1` | Continuous left corner in compiled pixels |
| 6 | `float32` | `bbox_y1` | Continuous top corner |
| 10 | `float32` | `bbox_x2` | Continuous right corner |
| 14 | `float32` | `bbox_y2` | Continuous bottom corner |
| 18 | `uint64` | `mask_rle_offset` | Byte offset into this sample's run region |
| 26 | `uint16` | `mask_rle_pairs` | Number of runs owned by this instance |
| 28 | `float64` | `original_area` | Supplied or fallback area before compilation |
| 36 | `uint64` | `annotation_id` | Source annotation identity, when present |
| 44 | `uint64` | `source_category_id` | Source category identity, when present |
| 52 | `uint64` | `source_ordinal` | Source annotation order; consult the incomplete-provenance flag |

Flag bits are `1` for mask presence, `2` for crowd, `4` for raw ignore, `8` for
annotation-ID presence, `16` for category-ID presence, and `32` for incomplete
source-ordinal provenance. Unused bits must be zero. Presence bits distinguish
an ID of zero from an absent identity, and a present empty mask from a missing
mask. Absent identities have zero payloads.

Bit `32` is admitted only for COCONut. It means the retained ordinal is local to
its known source extent because an earlier annotation extent is unavailable;
it must not be presented as a proven release-global ordinal. Other known
identities remain intact. [Provenance settlement](benchmark-datasets.md#native-import-and-provenance)
preserves the flag through cache reuse and clears it only when authoritative
source extents establish the exact ordinal.

Every `RLEPair` is `{ uint32 start, uint32 length }` and therefore 8 bytes.
Starts index the compiled `width × height` mask in row-major order. Runs are
positive, sorted, non-overlapping, and contained by the image. Mask support and
continuous box extent remain independent after their common transform.

COCO and Objects365 category identities retain their numeric values. Open
Images stores the identifier bytes after `/m/` in a little-endian `uint64`,
zero-padded to eight bytes; this is reversible encoding, not a hash or dense
class ID. The source-kind field selects the interpretation. Source identities
stay embedded in the same file even when an adapter maps several categories
to one foreground class.

The packed limits are intentional: class IDs occupy one byte, compiled image
axes remain bounded to 32,767, and an image's instance count and an instance's
RLE pair count must each fit an unsigned 16-bit value. Every complete sample
payload must also fit 500,000 bytes; 64-bit run offsets remain sample-local.
Total split storage is not constrained to 4 GiB. Boxes must remain
finite and strictly ordered after conversion to float32. Compilation rejects
values that cannot be represented before publication.

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

Both compilers expose [resize geometry](#resize-geometry) through `--resize-mode`
and [perceptual shrinking](#optional-perceptual-downscaling) through
`--perceptual-downscale`; the [Dataset card](gui-interaction.md#dataset-compilation-controls)
provides matching controls. `rfdetr validate --resize-mode` affects source
compilation only; existing bins retain their stored mode.

Directory and benchmark compilation use one
[execution and pinned-workspace boundary](compile-execution.md). Directory
keeps numbered source slots and strict failures while ready image/label work
shares CPU admission and bounded file preparation. Worker reassignment retains
prepared buffers. Image headers determine actual decode/resize requirements;
legal oversized work uses the explicit exceptional arena policy. CPU compiler
arenas use Linux page locking, without a CUDA context requirement.

Their three progress tracks carry canonical native units independently of the
selected work caption. Directory labels/pixels count images; benchmark row and
component accounting follows the
[compilation progress reference](benchmark-datasets.md#reading-compilation-progress).

To measure actual loading rather than inspect metadata:

```bash
./mmltk bench --compiled ./compiled/train.bin --batch-size 32 --epochs 1
```

This benchmark reads data and performs loader work.
`./mmltk --diagnose-io ./compiled/train.bin` is a separate read-only capability
inspection that does not read dataset contents or benchmark transfers.

## Benchmark-source acquisition

The [benchmark compiler](../src/backend/data/benchmark/benchmark_dataset_compiler.h)
supports **Coco custom**, the existing COCO/Objects365/Open Images recipe, and
**Coconut**, the full COCONut training recipe with three validation choices.
It shares the execution kernel, pinned workspaces, slot storage, compiled
layout, resizer, and loader described here.

[Benchmark datasets](benchmark-datasets.md) owns recipes, import/provenance,
mask recovery, acquisition/cache/repair, and capacity. Its
[progress reference](benchmark-datasets.md#reading-compilation-progress) also
covers Directory compilation. See [Dataset controls](gui-interaction.md#dataset-compilation-controls)
and [cache configuration](commands.md#benchmark-cache-selection).

## Optional perceptual downscaling

Compilation and GPU augmentation have independent, default-false
`perceptual_downscale` settings: root/RF-DETR compile use
`--perceptual-downscale`; enabled training augmentation uses
`--aug-perceptual-downscale`. Off retains ordinary resizing. The option changes
shrinking RGB only, preserving masks, boxes, classes, geometry, and record layout.

[RgbImageResizer](../src/backend/imaging/resample/image_resize.h) owns CPU execution
for both Directory and benchmark compilation.
[GpuPerceptualDownscaler](../src/backend/imaging/resample/image_resize_cuda.h)
owns the reusable CUDA workspace used by RF-DETR augmentation. Compilation uses
CPU image-level parallelism. Both implementations use the SSIM local-moment
method attributed in
[perceptual_downscale_math.h](../src/backend/imaging/resample/detail/perceptual_downscale_math.h).
They accept checked RGB8, straight-alpha RGBA8, or planar unit-sRGB float views
with explicit byte strides and capacities. sRGB is decoded once, the filter
works in linear-light Y/Cb/Cr, and the output is encoded once. RGBA filtering
uses premultiplied linear color and area-averaged alpha, with safe
unpremultiplication. Nonfinite/out-of-range float samples clamp deterministically.

CPU moment accumulation uses AVX2/FMA across eight neighboring output cells for
both integer and fractional shrink ratios. Fractional lanes retain their own
footprint weights, scalar row-major traversal, and compensated sums; completed
lanes retain both sum and compensation unchanged. Short rows and tails use the
same scalar method. Packed RGB reads stay within their three-byte pixels and
never consume row padding. Checked RGB8, RGBA8, and planar float layouts share
these rules.

CPU coefficient calculation, reconstruction, transfer-table quantization, and
compiler planar stores also operate in eight-lane blocks. Scalar and SIMD
helpers share the same arithmetic rules, low-variance handling, and edge/tail
behavior. Byte outputs and compiled planar projection retain exact RGB8
quantization; continuous float output keeps scalar `powf` encoding.

CPU byte quantization uses one immutable 4,097-entry base-code lookup derived
from the canonical Float32 transfer thresholds. Power-of-two buckets and one
threshold correction replace the dependent eight-step SIMD search; table
preparation verifies that each bucket needs at most one correction. The scalar
threshold search remains the numerical authority. Full vectors use ordinary
unaligned stores and tails retain masked stores. Fully assigned axis/state
scratch is prepared for overwrite; mathematical accumulators still initialize
their sums and compensation explicitly.

Ordinary CPU shrinking and enlargement use the existing
[AVIR float4 SSE backend](../third_party/avir/avir_float4_sse.h). Its signed
half-integer rounding follows the scalar float policy without changing the
thread's rounding mode. Resampling fixtures compare the selected SIMD path
against scalar AVIR output within one RGB8 level; identity copies remain exact.
The checked resize plan selects Identity, PerceptualInteger,
PerceptualFractional, or Avir once and supplies the required storage to execution.
AVIR's sizing pass counts allocator requests without constructing filter
coefficients; actual coefficients and queues live only inside that execution's
allocator scope. They cannot outlive the borrowed arena. AVIR byte output is
prepared for overwrite without first clearing those bytes. Each compiler worker
retains its resizer controls and binds admitted scratch for the current image;
resizing creates no nested worker pool. These choices apply to both compilers
without changing mask geometry or the compiled format.

The checked downscale operation rejects enlargement and overlapping nonidentity
views; an identity copy is exact. The ordinary CPU resize entrypoint retains
its established enlargement path. CUDA callers supply the execution context,
stream, producer dependencies, and exact source/destination custody. Tables
and tightly sized working buffers are reused; cross-stream reuse follows GPU
completion, and capacity pressure is bounded. The augmentation owner prepares
configuration changes transactionally before admitting the new execution.

CUDA retains its prepared byte-input transfer table with the backing allocation
and establishes reuse through existing completion submissions. Float-only input
does not prepare that table. Geometry-dependent axis tables remain separate;
growth or failed preparation invalidates the affected reuse. Small-footprint
accumulation traverses rows directly with the same sample and compensated-sum
order. Large-footprint accumulation retains its strided traversal and reduction
tree; filtering, thresholds, alpha handling, and rounding are unchanged.

Benchmark compilation facts record resampling policy independently of source-cache
identity. Changed policy requires recompilation. Perceptual downscaling can
emphasize noise; no detector-accuracy benefit is claimed. The
[compiler performance report](benchmark-performance.md)
states the measured performance and its limits.

## Loading a compiled file

[Metadata-only `info`](#compile-and-inspect) checks the header and section
extents. Product loading through [CompiledDataset](../src/backend/data/compiled/compiled_dataset.h)
admits the resident metadata and each eligible sample before exposing views:

1. map the regular file read-only with `MAP_SHARED`;
2. copy and validate the header magic, version, dimensions, stride, and limits;
3. compute the checked fixed layout, enforce the resident-metadata bound, and
   validate exact section offsets and end of file;
4. copy `ImageEntry[num_slots]` into resident memory, validate slot status and
   every eligible sample's dimensions, counts, classes, boxes, areas, flags,
   provenance, local RLE spans, and runs, and derive mask availability;
5. require the eligible count and observed maximum label bytes to equal the
   header, then expose resident image facts and mapped per-image annotation
   spans and arithmetic pixel addresses.

Invalid or truncated files fail at open. `eligible_slots()` visits physical IDs
directly from status metadata; it does not construct a dense-index-to-offset
remapping. Resident metadata and the mapping belong to `CompiledDataset`, so
borrowed views cannot outlive it.

[CompiledRecordChecks](../src/backend/data/compiled/compiled_file_utils.h)
supplies the same field rules to producers during encoding/copying. Benchmark
split writing retains its checked product through staging; a later untrusted
file open still performs the admission above. Metadata-only `info` does not
read every label or mask, and neither route hashes the pixel payload.

Training and inference use
[`DatasetLoader`](../src/backend/data/loading/dataset_loader.h), which turns those
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
    // Enqueue GPU work using batch.device_images and its per-image annotations.
    loader.release_batch(batch, consumer_cuda_stream);
}
```

`handoff_batch` makes the consumer stream wait for the slot's transfer event
without synchronizing the whole device. `release_batch` records completion on
that stream; the slot cannot be refilled until the GPU consumer has finished.
Batch pointers are leases, not independently owned allocations. A checked-out
batch must be released, and stale or foreign leases are rejected.

Sequential epochs visit eligible physical slots in file order, skipping holes.
Shuffled epochs use chunks sized to the larger of eight images or one batch,
capped by the dataset size. They shuffle chunks within locality blocks targeting
256 images and shuffle the blocks,
while retaining image order inside each chunk. A block contains at least one
whole chunk, so its actual extent depends on batch size. Batch sharding selects
complete batch ordinals for a rank, and `drop_last` controls the partial final
batch.

Training uses the loader's explicit schedule path instead of this ordinary
shuffle/shard policy. Its session-owned
[sparse data plan](rfdetr-training.md#sparse-data-planning-and-stochastic-identity)
supplies draw order, rank slices, and Resume offset; the loader does not
reshuffle them or choose model membership. Empty rank slices retain collective
participation without allocating an empty forward batch.

The stream sorts each slot's physical read descriptors before touching labels,
retaining destination offsets. Image order, repeated draws, annotation pointers,
draw keys and delivered batch identity therefore still follow the logical
schedule. A slot-owned cursor covers used label prefixes and logical pixel
ranges together. It deduplicates repeated source advice while retaining every
logical destination copy; only source-and-destination-contiguous runs coalesce
for gathering. Training opens the shared pixel source with random access
advice, then supplies these explicit local read ranges. Annotation-only
`describe_batch` views support [next-batch preparation](rfdetr-training.md#bounded-next-batch-preparation)
without consuming the loader cursor or obtaining pixel custody.

`MADV_WILLNEED` is advice, not completed I/O. When prefault is enabled, bounded
`MADV_POPULATE_READ` operations run through `FileWorkQueue` on the stream's
existing reader pool. Completion and actual capacity return wake the next
continuation. Gather records issue at most 16 MiB per turn, preserve disjoint
destinations, and yield to peer slots. H2D gathers can share available reader
capacity; GDR gathers serialize a slot's writes through its mapped owner.
The stream caps tasks by its configured worker count even when it borrows a
larger pool. Compilation class floors do not enter runtime reader ownership.
Cancellation settles prefaults, gathers, callbacks, and the existing GPU
completion owners before source or slot reuse.

### Prepared batch storage

The local schedule bounds slot count to the smaller of prefetch depth and
locally scheduled batch count, retaining one slot when that count is zero.
Worker count respects useful bounded chunks across those slots and eligible
CPUs. H2D can use multiple workers for a large slot; `gather_workers` is the
ceiling, with zero selecting `prefetch_factor`. Inference and evaluation pass
their configured reader ceiling through without clamping it to slot count.
Every retained slot prepares the full configured maximum batch size before
reads, including an
empty shard's retained slot; later epochs can use repeated draws without
growing its buffers.

For maximum batch size `B`, each slot owns `B × image_stride` host and device
pixel capacity, `B × header.max_label_bytes` logical label capacity, bounded
read descriptors, and per-image annotation views. A batch of 30 therefore
reserves `30 × header.max_label_bytes` label bytes per prefetch slot. Allocator
alignment can round backing capacity up; RAM label pitch remains the exact
header maximum rather than the file's page-aligned pitch. A zero-label split
needs no label arena.

Reads copy only each image's used label prefix and logical pixel bytes, update
active views/counts, and leave unused capacity untouched. Pixel buffers, label
storage, and read containers do not grow during reads or epoch changes. Explicit
capacity/configuration changes require settled users before replacement.
`image_capacity_bytes` on a delivered batch remains its active image count
times `image_stride`, independently of retained backing capacity. Annotation-only
planning remains available without pixel reads.

## Why compiled loading is fast

The [compiled format](#compiled-binary-format) moves decoding, RGB conversion,
resizing/padding, and source annotation parsing/transforms out of runtime.
Compiled-record admission runs once when the dataset opens.
Fixed block arithmetic gives O(1) image and label addresses; labels/masks are
packed within each sample. Loading additionally uses:

- Fixed blocks start on 2 MiB boundaries and pixel/label slots on 4 KiB
  boundaries. The loader supplies normal, sequential, or random Linux
  `madvise` hints for the mapped data region.
- Physically adjacent logical pixel extents with consecutive batch destinations
  are coalesced. Page padding and block label partitions break adjacency.
  File-cache population and copies are issued in bounded chunks of at most
  16 MiB, with cancellation checked between chunks.
- The ordinary locality-aware shuffle changes global and within-block order
  each epoch. Explicit training schedules keep their own order while physical
  read sorting improves source locality within each slot.
- Prefetch slots, read workers, read lists, pinned host/label buffers, device
  buffers, CUDA events, and the copy stream are bounded and reused. Batch
  storage is allocated at preparation, before steady reads.
- Workers and pinned pages use the resolved GPU-local CPU/NUMA placement.
  Host pages are bound, prefaulted, residency-checked, and then registered with
  CUDA.
- I/O gathering, transfer, and model consumption overlap across slots. Separate
  bounded upload-completion and consumer-retirement queues have device/NUMA-local
  owner workers. A delayed model consumer cannot hold unrelated upload callbacks
  behind its event wait. A slot's consumer retirement still follows its own
  transfer callback. GDRCopy needs only the consumer worker. Completion is
  event-driven, without polling or ordinary device-wide synchronization.

### Default H2D path

The default path copies demanded mapped pixels into persistent GPU-local,
NUMA-bound pinned host storage, then submits one asynchronous
`cudaMemcpyAsync` for the packed batch on a nonblocking high-priority copy
stream. A CUDA event hands the finished device slot to the consumer stream.
This is the default [benchmark path](#compile-and-inspect) and needs no GDRCopy.
The loader's `host_images` can alias mapped pixels when the selected arithmetic
pixel extents are exactly contiguous. Otherwise it exposes the slot's prepared
host buffer. Page padding and block boundaries can prevent that alias.

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

GDR slots also prepare host pixel capacity before reading. Optional CPU access
that cannot alias contiguous mapped pixels gathers into that existing buffer
on the caller thread under the slot's prepared execution policy; it does not
allocate storage or wait behind a loader callback that needs the caller's lease
lock.

Use `--prefetch-factor`, worker, CPU-affinity, and NUMA options only on commands
that expose them; the exact surface is listed by that command's `--help`.
Measure the real workload before increasing prefetch depth: each slot owns
capacity for the configured maximum batch, so additional useful slots
consume more pinned or device memory. See [GPU-local execution](gpu-execution.md)
for placement, transport, capability inspection, and functional GDR checks.
The [recorded bounded measurements](validation.md#fixed-storage-measurements)
compare used-byte copies with the preceding packed algorithm; neither layout
was faster in every measured order.

## Explore thumbnails and atlas residency

Explore uses
[CompiledImageStream](../src/backend/data/compiled/compiled_image_stream.h) for reusable
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

Each square card contains an image rectangle fitted to that image's original
width and height, with the shared atlas padding around it. The
[read scheduler](../src/controller/subsystems/explore/gallery_read_scheduler.cpp)
derives a separate source crop from the stored resize mode: the full canvas for
Stretch, or the stored content rectangle for Letterbox. Missing original
dimensions use the compiled canvas and its aspect. The existing
[GPU tile pass](../src/backend/imaging/explore/explore_render_core.cu) samples
that crop directly into the contained rectangle; it does not create an
intermediate aspect-restored image. RGB sampling clamps to the crop, and boxes
and masks use the same mapping. Native caption projection uses that card geometry
before Iced draws the text. Augmentation still produces pixels and annotation
meaning in the compiled canvas before this presentation transform.

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

Host pages stay registered through H2D/D2H completion. GPU cache residency
instead retains device allocations and read/write custody; it is not host
memory registration.

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
