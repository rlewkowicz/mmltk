# Source datasets and compilation

[Wiki index](README.md) · [Commands](commands.md) · [Benchmark sources](benchmark-datasets.md) · [GPU execution](gpu-execution.md)

The Directory compiler consumes `categories.json` and nonempty split directories
of matching, contiguous six-digit PNG/JSONL pairs beginning at `000001`:

```text
dataset/
  categories.json
  train/000001.png
  train/000001.jsonl
  val/000001.png
  val/000001.jsonl
```

Every image needs a JSONL file, including empty files for backgrounds.

## Category table

`categories.json` requires 1–256 `classes`, with unique dense IDs beginning at
zero or one and unique, nonempty, NUL-free names of at most 31 bytes. Names
cannot be truncated. IDs normalize into an immutable ordered foreground
[class catalog](rfdetr-workflows.md#class-identity-and-model-admission).

```json
{"classes":[{"id":1,"name":"category_1"}]}
```

Optional `splits.<name>.total` declares a positive image count; compilation opens
those numbered assets. Otherwise it scans for matching contiguous pairs.
Other descriptive metadata does not determine membership.

A class may declare an ordered keypoint schema:

```json
{"id":1,"name":"animal","keypoints":["left_eye","right_eye"],
 "skeleton":[[0,1]],"horizontal_flip":[1,0],"oks_sigmas":[0.025,0.025]}
```

The [schema authority](../src/backend/data/catalog/keypoint_schema.h) permits
at most 256 unique nonempty NUL-free joint names per class, each at most
256 bytes, and 512 unique undirected skeleton edges. Edges use zero-based
indexes, cannot self-connect, and must reference declared joints.
`horizontal_flip` and `oks_sigmas` must each match the joint count: flip is an
involutive permutation and sigmas are finite/positive. A nonempty keypoint list
therefore requires both arrays. Skeleton/flip/sigmas without `keypoints` fail.
Joint meaning follows names, named flip partners, sigmas, and skeleton, separately
from class IDs and model output slots.

## Instance records

Each nonempty JSONL line is one object:

| Field | Meaning |
| --- | --- |
| `class` | Required category name |
| `bbox_xyxy` | Continuous source-pixel `[x1,y1,x2,y2]`; authoritative when supplied |
| `mask_rle` | Optional whitespace-separated `start:length` foreground runs; `""` is a present empty mask |
| `mask_rle_encoding` | Required literal `row_major_start_length` with `mask_rle` |
| `keypoints` | Flattened `[x,y,visibility,...]` in the class's declared joint order; required for a nonempty schema |
| `image_size_wh` | Optional `[width,height]`, matching the PNG |
| `area` | Optional finite nonnegative original source-pixel area |
| `iscrowd`, `ignore` | Optional boolean or integer `0`/`1`; independent facts |
| `id`, `image_id`, `category_id` | Optional nonnegative integer source identities |

RLE indexes `y × width + x`; positive runs must be sorted, nonoverlapping, and
inside the source image. These are start/length pairs, not alternating counts.
A box needs finite strictly ordered corners. Fractional/out-of-image coordinates
survive transformation. Without a box, nonempty mask support supplies bounds
with exclusive upper edges. Masks may extend beyond an authoritative box.

Absent `area` uses present-mask foreground count, including zero, else box area.
Within one JSONL file, supplied `image_id` values must agree. Omitted category
identity uses that class's original category ID. Source line ordinals preserve
annotation order; equal class/box records remain distinct.

For a 432×432 source, three 24-pixel runs beginning at `(10,20)` are:

```json
{"class":"category_1","mask_rle_encoding":"row_major_start_length","mask_rle":"8650:24 9082:24 9514:24","bbox_xyxy":[10,20,34,23],"image_size_wh":[432,432]}
```

Keypoints require exactly three scalars per declared joint. Coordinates must be
finite; visibility is integer `0` Unannotated, `1` Occluded, or `2` Visible.
Occluded/visible coordinates use the box transform; unannotated coordinates
persist as zero. Source occlusion remains distinct from missing annotation.

Malformed JSON, unknown classes, invalid runs/encoding/metadata/joints, or no
valid box/nonempty mask fail compilation. A detection survives an empty resized
mask. Missing and present-empty masks differ; segmentation requires explicit
mask presence while detection can use box-only labels. Source admission is in
[dataset_compiler_scan_labels.cpp](../src/backend/data/compiler/dataset_compiler_scan_labels.cpp).

## Resize geometry

Directory/benchmark default to **Stretch**, independently scaling both axes.
Explicit **Letterbox** (`--resize-mode Letterbox`) preserves source aspect,
centers content, and uses zero RGB padding. Changing geometry or resampling
requires recompilation.

[ImageResizeGeometry](../src/backend/imaging/resample/image_resize.h) fits one
Letterbox axis exactly, rounds the other to the nearest positive integer, and
floors half-padding offsets. Source coordinates become
`x × resized_width / original_width + offset_x` and likewise for y. Pixels,
continuous boxes/keypoints, and nearest categorical masks share this geometry.
Stored mode/original dimensions reconstruct the content rectangle.

The [Original viewer](gui-interaction.md#original-view-and-annotation-import)
restores source aspect, not resolution. Explore/Validation detail metadata
retains resize provenance/source/content extents. [Upscale](gpu-execution.md#upscale-input-preparation)
uses that provenance independently of viewer preference.

[CompilePixelWorkspace](../src/backend/data/detail/compile_pixel_workspace.h)
binds admitted header, decode/resize storage, geometry, and destination layout.
Execution rechecks the encoded header before writing. Its shared
`RgbImageResizer::resize_to_planar` writes packed RGB8 to planar floats. Native
perceptual shrinking writes directly to planes with exact RGB8 quantization then
`float(byte) × (1.0F / 255.0F)`; AVIR/enlargement reuse byte scratch. Identity
conversion and zero padding retain those values.

### RGB resampling methods

[RgbResizeMethod](../src/backend/imaging/resample/image_resize.h) serves both
compilers and ordinary-image prediction:

| `--resize-method` | Operation |
| --- | --- |
| `Native` (default) | AVIR RGB8 or optional perceptual shrinking, then planar unit-sRGB float |
| `TorchBilinear` | RGB8 → planar float32 `[0,1]` → ATen bilinear, `align_corners=false`, no antialias/byte quantization |
| `TorchBilinearAntialias` | Same float path with antialiased bilinear |

[Torch resize](../src/backend/imaging/resample/torch_resize.cpp) uses CPU execution
with a restored task-local one-thread OpenMP limit. Preparation charges supplied
float planes and bounded opaque ATen temporaries; views/temporaries retire per
image. Missing pinned Torch/OpenMP support fails admission without fallback.
Identity uses exact RGB-to-float conversion.

The perceptual flag applies only to Native. The GUI's one Resize method group
maps **AVIR** to Native/false and **Perceptual downscaling** to Native/true;
AVIR/Torch choices clear the flag. Perceptual uses AVIR if either axis grows.
All choices preserve geometry, zero padding, boxes, joints, and categorical
masks. Manifests record resampling independently of source-cache identity;
existing compiled pixels are never reinterpreted.

Float-tensor resizing differs from upstream PIL resizing, which antialiases and
quantizes to RGB8 before float conversion. Upstream file prediction instead
uses float bilinear without antialiasing. Independent checks compare both routes;
a resize choice alone establishes no accuracy gain.

### Categorical mask sampling

The [RLE sampler](../src/backend/data/mask_rle_utils.cpp) validates source runs,
maps interval boundaries with exact nearest-center inverse coordinates, and
reuses row scratch. It emits/coalesces target runs without a target bitmap;
source bounds/counts still come from source support. Dense masks share the
emitter, with 32-byte AVX2 scans and bounded scalar tails. RGB-to-planar conversion
processes sixteen pixels per SIMD step without changing normalization/support.

## Compiled binary format

Writers produce **format 11**. Readers accept formats **10 and 11**; older
versions require recompilation. [compiled_format.h](../src/backend/data/compiled/compiled_format.h),
[layout](../src/backend/data/compiled/compiled_file_layout.cpp), and
[admission](../src/backend/data/compiled/compiled_file_utils.h) define this native
little-endian Linux representation. Format 10 has no extended catalog/joints;
its ordered names construct a detection/segmentation catalog without inventing
keypoint meaning. Reading preserves original metadata bytes for identity while
the in-memory header reports current format.

```text
FileHeader (8,320 bytes)
optional encoded ordered class catalog
padding to 2 MiB
64 pixel slots + 64 label slots, padded to 2 MiB   ┐ repeated blocks
...                                              ┘
ImageEntry[num_slots] (32 bytes each)
```

For logical pixel bytes `P = width × height × 3 × sizeof(float)`, complete label
capacity `L`, and catalog length `C`, checked [FileLayout](../src/backend/data/compiled/compiled_file_layout.cpp)
uses integer division:

```text
pixel_pitch     = align_up(P, 4096)
label_pitch     = align_up(L, 4096)
block_bytes     = align_up(64 × (pixel_pitch + label_pitch), 2 MiB)
data_offset     = align_up(sizeof(FileHeader) + C, 2 MiB)
block_base(i)   = data_offset + (i / 64) × block_bytes
pixel_offset(i) = block_base(i) + (i % 64) × pixel_pitch
label_offset(i) = block_base(i) + 64 × pixel_pitch + (i % 64) × label_pitch
block_count     = (num_slots + 63) / 64
index_offset    = data_offset + block_count × block_bytes
total_file_size = index_offset + num_slots × sizeof(ImageEntry)
```

All extents/native file offsets are checked before use. Partial final blocks
retain 64-slot extent; inactive slots remain holes. `num_images` counts eligible
samples while `num_slots` bounds physical IDs. Benchmark placement follows
readiness, so recompilation promises neither physical order nor identical bytes.

### Staging and in-place compaction

Both compilers default to **121,824 usable label bytes/sample**, including
instances, alignment, runs, and joints. Page pitch is 122,880 bytes; padding adds
no usable capacity. There is no sizing scan. Directory `--max-label-bytes` admits
multiples of eight up to **500,000**. Persisted admission retains that independent
limit, including larger existing labels. Overflow is fatal with image/required/
capacity context; masks/joints are never simplified or truncated to fit.

After writers/mappings settle, finalization records the largest complete eligible
`label_bytes` as header `max_label_bytes` and derives final pitch. Background-only
splits use zero; rejected/speculative slots do not affect maxima.

[Compaction](../src/backend/data/compiled/compiled_file_compaction.cpp) moves live
pixel/label prefixes forward in the same inode with at most **4 MiB** reusable
pinned scratch. Per block, pixels then labels follow increasing source order;
a complete chunk is read before overlapping writes. Adjacent pixels fill the
admitted window. Label windows capture at most one block's live prefixes before
batched page allocation/reclamation, keeping nonadjacent writes separate.
Rejected capacity batches split into smaller safe operations with source bytes
retained; a larger window does not raise minimum scratch/free-space needs.

Physical IDs/sample-local run offsets survive. Supported hole punching reclaims
unused extents; truncation sets exact final length. Unchanged label pitch needs
no move buffer. Header/catalog/index writes sync before atomic publication.
Failure/cancellation retires staging and preserves prior output.

Writers reserve/preallocate page-covered pixels and used labels, not the sparse
prefix. Stable block mappings survive growth while borrowed. Logical file length
includes holes/padding; it differs from allocated/read bytes. The
[filesystem ledger](compile-execution.md#file-preparation-and-settlement)
coordinates outstanding growth.

### File header

Packed `FileHeader` is 8,320 bytes; offsets are absolute:

| Byte | Type | Field | Meaning |
| ---: | --- | --- | --- |
| 0 | `uint64` | `magic` | `0x464153544c445232` |
| 8 | `uint32` | `version` | Current value: `11` |
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
| 8,269 | `uint32` | `catalog_version` | `0`: absent; `1`: encoded ordered class catalog |
| 8,273 | `uint32` | `catalog_bytes` | Catalog bytes immediately after the header; at most 32 MiB |
| 8,277 | `uint8[43]` | reserved | Zero |

The optional catalog immediately follows the header, is at most 32 MiB, and
uses reflected CBOR encoding of the ordered class catalog (catalog version 1).
Header/catalog names must agree. It carries each class's keypoint schema.
Catalog version/length are both zero when absent; format 10 requires both zero.

Images are three contiguous float32 planes (R, G, B), unit RGB from source bytes,
with zero Letterbox padding. ImageNet normalization belongs to
[model preprocessing](rfdetr-workflows.md#model-input-and-detection-selection).

### Per-image index

The resident index has packed 32-byte entries and a checked **1,000,000,000-byte**
allocation/read limit:

| Byte | Type | Field | Meaning |
| ---: | --- | --- | --- |
| 0 | `uint16` | `num_instances` | Number of instances in this slot |
| 2 | `uint8` | `status` | `0`: inactive; `1`: eligible |
| 3 | `uint8` | `has_source_image_id` | Whether the source image ID is present |
| 4 | `uint32` | `label_bytes` | Complete used payload: Instances, alignment, runs, and optional joint section; see below |
| 8 | `uint32` | `num_runs` | Total mask runs in this sample |
| 12 | `uint32` | `original_width` | Source width before compilation |
| 16 | `uint32` | `original_height` | Source height before compilation |
| 20 | `uint8` | `source` | Annotation provenance, using the values below |
| 21 | `uint8[3]` | reserved | Zero |
| 24 | `uint64` | `source_image_id` | Original source identity; zero when absent |

Inactive entries publish no labels/runs/payload. Background entries retain valid
dimensions/source facts with zero labels. Physical IDs, filtered positions,
and source IDs are separate. Original dimensions plus resize mode reconstruct
geometry without redundant per-image scale/padding.

| `source` | Annotation provenance |
| ---: | --- |
| 0 | Generic |
| 1 | Stock COCO |
| 2 | Stock Objects365 |
| 3 | Open Images |
| 4 | COCONut annotations on COCO images |
| 5 | COCONut annotations on Objects365 v1 images |
| 6 | COCONut annotations on Objects365 v2 images |

Annotation provenance is independent of mask presence. COCONut stores physical
image ID here; its [inventory](benchmark-datasets.md#native-import-and-provenance)
retains differing release-row IDs and exact member joins.

### Instances and masks

Packed instances remain 60 bytes:

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

Flag bits: `1` mask present, `2` crowd, `4` raw ignore, `8` annotation ID present,
`16` category ID present, `32` incomplete source ordinal; all other bits zero.
Presence distinguishes zero IDs from absence and empty masks from missing masks.
Absent identities have zero payload. Bit 32 is COCONut-only: an unknown earlier
extent leaves local, not proven release-global, ordinals. Cache settlement clears
it only with authoritative prefix extents; other identities remain valid.

`RLEPair = {uint32 start, uint32 length}` is 8 bytes. Positive sorted,
nonoverlapping runs index the compiled row-major mask and stay inside its canvas.
`mask_rle_offset` is relative to that sample's run region, not the file. Masks
and continuous boxes retain independent extents.

Sample payload begins with instances, padding to eight bytes, then runs:
`base_bytes = align_up(num_instances × 60, 8) + num_runs × 8`.
When joints exist, append `PackedJointHeader {uint32 version=1,count}`, one
8-byte `PackedJointSpan {uint32 offset,uint16 count,uint16 reserved=0}` per
instance, then 12-byte `PackedKeypoint {float32 x,y,uint8 visibility,uint8[3]
reserved=0}` records; round final payload to eight bytes. Span offsets count
joint records, in contiguous instance order, not bytes. Counts must match each
class's schema; every joint is referenced exactly once. Coordinates are finite,
visibility is 0/1/2, and unannotated coordinates/reserved bytes are zero.
No joint section is emitted when total joint count is zero.

COCO/Objects365 category IDs remain numeric. Open Images encodes up to eight
identifier bytes after `/m/` little-endian in `uint64`, zero-padded, reversibly;
source kind selects interpretation. Category mapping does not erase identity.

Limits: 256 classes; axes ≤32,767; instances/image and runs/instance fit `uint16`;
complete labels ≤500,000 bytes; run offsets stay sample-local `uint64`.
Split storage may exceed 4 GiB. Boxes must remain finite/strictly ordered after
float32 conversion. Producers reject unrepresentable values before publication.

## Compile and inspect

```bash
./mmltk compile --source-dir ./dataset --output-dir ./compiled --split train --width 432 --height 432
./mmltk compile --source-dir ./dataset --output-dir ./compiled --split val --width 432 --height 432
./mmltk info --compiled ./compiled/train.bin
./mmltk bench --compiled ./compiled/train.bin --batch-size 32 --epochs 1
```

`compile` handles one split; width alone makes a square. `compile --help` owns
worker/CUDA-mask/device options. `rfdetr compile` prepares train/validation with
model-specific dimensions/options. Both expose `--resize-mode`, `--resize-method`,
and `--perceptual-downscale`, matching [Dataset controls](gui-interaction.md#dataset-compilation-controls).
`rfdetr validate` resize options affect source compilation, never existing bins.

Compilation uses shared [execution/pinned storage](compile-execution.md): Directory
retains strict numbered inputs, reusable worker storage, actual-header plans,
and explicit exceptional handling for legal oversized work without requiring
CUDA. [Progress](benchmark-datasets.md#reading-compilation-progress) has three
independent tracks: Directory labels/pixels count images. `info` inspects metadata;
`bench` performs real loader work. [Capability inspection](gpu-execution.md#read-only-capability-inspection)
reads no payload and benchmarks no transfers.

## Benchmark-source acquisition

[Benchmark datasets](benchmark-datasets.md) owns Coco custom/Coconut recipes,
import/provenance, recovery, cache repair, and capacity. They share the format,
resizer, slot storage, execution, and loader here. [Cache selection](commands.md#benchmark-cache-selection)
owns location/precedence.

## Optional perceptual downscaling

Compile `--perceptual-downscale` and training augmentation
`--aug-perceptual-downscale` are independent, default-false settings. Compile's
flag affects Native shrinking only; off uses AVIR. Masks, boxes, joints, classes,
geometry, and record layout remain unchanged.

[CPU RgbImageResizer](../src/backend/imaging/resample/image_resize.h) serves both
compilers with image-level parallelism; [GpuPerceptualDownscaler](../src/backend/imaging/resample/image_resize_cuda.h)
serves augmentation. The SSIM local-moment authority is
[perceptual_downscale_math.h](../src/backend/imaging/resample/detail/perceptual_downscale_math.h).
Checked RGB8, straight-alpha RGBA8, and planar unit-sRGB float views provide
explicit strides/capacities. Decode sRGB once, filter linear-light Y/Cb/Cr,
encode once. RGBA uses premultiplied linear color, area-averaged alpha, and safe
unpremultiplication. Invalid/out-of-range floats clamp deterministically.

CPU AVX2/FMA handles eight neighboring cells for integer/fractional shrink,
retaining per-lane footprint weights, scalar row-major order, compensated sums,
and completed-lane sum/compensation. Tails/short rows use the same scalar rules;
RGB reads consume exactly three bytes without padding overread. Coefficients,
reconstruction, transfer quantization, and planar stores share scalar arithmetic,
low-variance, and edge rules. Byte/compiled outputs preserve exact RGB8
quantization; continuous float uses scalar `powf` encoding.

One immutable 4,097-entry base-code lookup derives from canonical Float32
thresholds. Power-of-two buckets need at most one verified correction; scalar
threshold search remains authoritative. Full vectors use unaligned stores,
tails masked stores. Fully assigned axis/state scratch skips clearing;
accumulators initialize sums/compensation explicitly.

Ordinary shrink/enlarge uses [AVIR float4 SSE](../third_party/avir/avir_float4_sse.h),
with scalar-policy signed half-integer rounding and no thread rounding-mode
change. Fixtures allow one RGB8 level versus scalar AVIR; identity is exact.
Native plans select Identity, PerceptualInteger, PerceptualFractional, or Avir
once; explicit Torch uses its separate float route. AVIR sizing counts allocator
requests without creating coefficients; execution coefficients/queues stay within
the borrowed arena. Byte output prepares for overwrite. Worker resizer controls
persist; execution binds current scratch without a nested pool.

Checked downscale rejects enlargement and overlapping nonidentity views;
ordinary resize retains enlargement. CUDA callers supply context/stream,
dependencies, and source/destination custody; reusable buffers/tables follow
physical completion with bounded pressure. Configuration changes prepare
transactionally. Byte-input transfer tables retain backing and completion;
float-only inputs need none. Geometry axis tables are separate; growth/failed
preparation invalidates relevant reuse. Small footprints preserve direct row
sample/compensated order; large ones preserve strided traversal/reduction tree.
Alpha, thresholds, filtering, and rounding remain shared policy.

Resampling is recorded separately from source-cache identity and requires
recompilation when changed. Perceptual filtering can emphasize noise; it proves
no accuracy gain. [Performance interpretation](benchmark-performance.md)
separates numerical behavior from measurements.

## Loading a compiled file

[CompiledDataset](../src/backend/data/compiled/compiled_dataset.h) trusts
compiler-published sample facts for Explore, Train, validation, and diagnostics:

1. Map the regular file read-only with `MAP_SHARED`; admit header/format,
   dimensions, stride, limits, catalog, checked layout, exact offsets/length.
2. Bound/copy the resident physical index; sum recorded eligible label/run counts.
3. Expose resident facts and arithmetic pixel/payload addresses without scanning
   samples, recomputing population/maxima, or validating all annotation payloads.

There is one opening route, no full-payload verification option or pixel hash.
Unsupported/truncated layouts fail. On-demand annotation views check requested
payload/joint extents and schema. Explicit mask availability reads flags,
including present-empty; Train requests no unused summary. Explore reads needed
class/mask metadata, never all runs for validation. `eligible_slots()` visits
status-bearing physical IDs without a dense-offset remap. Borrowed views cannot
outlive the dataset mapping/resident index.

Open accepts cancellation and an optional existing pool. Index copies cancel
between bounded chunks; aggregation checks every slot and joins partitions
before publication. Explore uses gallery workers, registers stop before open,
and records non-crowd donor summaries in its parallel summary pass. Producer
[CompiledRecordChecks](../src/backend/data/compiled/compiled_file_utils.h)
admit encoding/copying before publication; readers do not repeat a full audit.

[DatasetLoader](../src/backend/data/loading/dataset_loader.h) produces leased GPU
batches:

```cpp
mmltk::backend::data::DatasetLoader loader({
    .compiled_path = "./compiled/train.bin", .batch_size = 32, .prefetch_factor = 6,
});
mmltk::backend::data::Batch batch{};
while (loader.next_batch(batch)) {
    loader.handoff_batch(batch, consumer_cuda_stream);
    // Enqueue work using batch.device_images and per-image annotations.
    loader.release_batch(batch, consumer_cuda_stream);
}
```

Handoff waits for transfer on the consumer stream, without device-wide sync.
Release records consumer completion; refill waits for it. Every checked-out
lease must release; stale/foreign leases fail.

Sequential epochs visit eligible physical IDs. Ordinary shuffled epochs use
chunks `max(8,batch_size)` capped by population, shuffling chunks within roughly
256-image locality blocks and shuffling blocks while keeping each chunk ordered.
Blocks hold at least one full chunk. Rank sharding selects batch ordinals;
`drop_last` controls the final partial batch. Training instead supplies its
[session schedule](rfdetr-training.md#sparse-data-planning-and-stochastic-identity):
explicit draw order/rank slices/Resume offset are never reshuffled or reassigned.
Empty rank slices retain collectives without empty forward allocation.

Per slot, sorted physical descriptors improve read locality while destination
offsets preserve logical order, repeats, annotation pointers, and draw keys.
A cursor covers used labels/logical pixels, deduplicates repeated source advice,
and coalesces only contiguous source+destination copies. Training uses random
mapping advice plus explicit local ranges. Annotation-only `describe_batch`
supports [next-batch planning](rfdetr-training.md#bounded-next-batch-preparation)
without consuming pixels/cursor.

`MADV_WILLNEED` is advice. Optional bounded `MADV_POPULATE_READ` uses
`FileWorkQueue` on the existing reader pool; physical completion/capacity wakes
continuations. Gather turns copy at most 16 MiB with disjoint destinations and
yield to peers. H2D shares reader capacity; GDR serializes a slot through its
mapped owner. Each stream caps tasks by its configured workers even on a larger
borrowed pool; compiler class floors do not apply. Cancellation settles calls,
gathers, callbacks, and GPU owners before reuse.

### Prepared batch storage

Local slot count is `min(prefetch_depth,local_batch_count)`, retaining one slot
for zero batches. Workers respect useful chunks/slots/CPUs. H2D may use several
workers per large slot; `gather_workers=0` chooses prefetch factor, otherwise it
sets the ceiling. Evaluation/inference retain their configured reader ceiling
without clamping to slots.

Every slot prepares maximum batch `B`, even for an empty shard: host/device
pixels `B × image_stride`, logical labels `B × header.max_label_bytes`, bounded
descriptors, and per-image views. Backing may round up, but RAM label pitch is
the exact header maximum, not file pitch. Zero-label splits need no label arena.
Reads copy only used label prefixes/logical pixels and update active views;
unused capacity remains untouched. Buffers/containers never grow during reads
or epoch changes. Replacement requires settled users. Delivered
`image_capacity_bytes` is active count × stride, independent of retained capacity;
annotation-only planning still needs no pixels.

## Why compiled loading is fast

Compilation removes decode, conversion, resize, parsing, and transforms. Fixed
block arithmetic gives O(1) addresses and sample-local packed labels. Reusable
prefetch storage, pinned pages, workers, CUDA events, and copy streams overlap
I/O, transfer, and consumption. Physical adjacency coalesces only across actual
logical extents; page/block padding breaks runs. Read/copy turns stay ≤16 MiB.
Mapping advice can be normal, sequential, or random.

Workers/pages follow [GPU-local CPU/NUMA placement](gpu-execution.md#device-and-numa-placement).
Independent bounded upload-completion and consumer-retirement queues have
local owner workers; delayed consumers cannot block unrelated uploads. Each
slot's retirement follows its transfer callback. GDR needs only consumer
retirement. Completion is event-driven without ordinary device-wide sync.

### Default H2D path

Mapped pixels gather into persistent GPU-local NUMA-bound pinned host storage,
then one asynchronous packed-batch copy uses a nonblocking high-priority stream.
An event hands off the device slot. `host_images` may alias mapped pixels only
when arithmetic extents are exactly contiguous; otherwise it exposes prepared
host storage. Padding/block boundaries can prevent aliasing. No GDRCopy needed.

### GDRCopy path

Explicit `--gdrcopy` uses persistent GPU allocations CPU-mapped through GDRCopy:

```bash
./mmltk bench --compiled ./compiled/train.bin --batch-size 32 --epochs 1 --gdrcopy
```

Workers copy page-cache bytes into mapped GPU memory; CUDA events protect
consumer leases. This removes intermediate pinned staging/H2D DMA, but is not
storage-to-GPU DMA. Unsupported device/driver/gdrdrv/DMA-BUF mapping fails without
fallback. Prepared host capacity remains for optional CPU access: nonaliasing
access gathers on the caller under slot execution policy, without allocation or
a callback wait requiring that caller's lease lock.

Use only options exposed by each command's help. Additional useful prefetch slots
cost full maximum-batch host/device/label storage; measure before increasing.
[GPU execution](gpu-execution.md#functional-gdr-checks) owns transport admission
and functional checks.

## Explore thumbnails and atlas residency

[CompiledImageStream](../src/backend/data/compiled/compiled_image_stream.h)
provides reusable read/host/device lanes, including an independent detail lane.
[GalleryReadScheduler](../src/controller/subsystems/explore/detail/gallery_read_scheduler.h)
owns stream/lane/callback/demand/ingress state;
[GalleryDescriptorStorage](../src/controller/subsystems/explore/detail/gallery_descriptor_storage.h)
owns pinned/device descriptors and augmentation scratch;
[ExploreHostAllocations](../src/controller/subsystems/explore/detail/gallery_host_allocations.h)
retains registered owners. `GalleryStream` coordinates them without exposing
mutable storage. Host pages survive H2D/D2H completion; GPU tiles instead retain
device allocation/read-write custody.

Place completed cached visible tiles first, then admit:

1. All visible rows, including partial rows, in row order.
2. Up to four neighboring rows forward in last nonzero scroll direction.
3. Up to four behind, nearest first; initial direction is increasing rows.

Each side clips independently. Priority controls admission, not completion;
in-flight work retains custody, reusable settled inputs can rebind, speculation
preserves foreground capacity. Scroll/viewport drives demand, never hover or
selection; latest measured viewport can arrive while busy.

Misses produce completed individual GPU cache tiles before atlas placement.
Pixel identity includes dataset incarnation, compiled image, augmentation
config/seed, and raster extent; filtered position/viewport rows are demand only.
Overlay validity is separate, preserving clean tiles/meaning across box/mask/class
changes. Label visibility is presentation state. Augmentation/donor choices do
not depend on batch order.

Cards fit original aspect inside square atlas cells/padding. Stretch samples the
full compiled canvas; Letterbox samples its reconstructed content rectangle;
missing source dimensions use canvas aspect. The [tile pass](../src/backend/imaging/explore/explore_render_core.cu)
samples that crop directly, clamping RGB and using the same mapping for boxes,
masks, joints, and native caption placement. Augmentation precedes presentation in
compiled-canvas coordinates.

Enable/reroll/disable retains completed tiles when dataset/incarnation/raster
match. `GalleryThumbnailCache::Retained` supplies pixels with their existing
`GalleryTileMeaning`; `Find` also requires current preview identity.
`refresh_pending` tiles stay drawable but need replacement. Incompatible identity
invalidates them. First atlas output restores retained visible tiles before
misses; each refreshed completion replaces pixels and meaning together. Semantic
overlay redraw during refresh uses retained meaning without claiming current
pixels, preserving displayed-label pairing through cancellation/partial progress.

[Thumbnail cache](../src/controller/subsystems/explore/detail/gallery_thumbnail_cache.h)
has bounded high-water capacity: admitted maximum viewport plus eight neighbor
rows, capped by dataset population and `kExploreVisibleItemCapacity`. Demand pins
precede eviction; physical readers/writers/rollback banks independently protect
in-flight data. Smaller demand retains capacity/useful entries. Pixel, clean/
semantic banks, lanes, descriptors, and metadata all cost storage; diagnostic
subset byte categories must not be double-counted. `PhysicalRow` plus actual
pitch defines tile addresses, never demand/vector capacity.

[GalleryAtlas](../src/controller/subsystems/explore/detail/gallery_atlas.h)
keeps allocation-specific directories of layout generation, initialized cells,
and clean/semantic meaning, with circular physical rows. Each candidate
reconciles its own potentially older contents; no full-atlas baseline copy.
Initialize invalid cells, add placeholders for missing visible tiles, clear
removed/unused cells and partial-row padding, preserve matching overlaps.
Invalidate touched entries before GPU writes; install meaning only after
settlement. Failed/cancelled writes leave cells invalid and preserve the last
committed product. Detail output invalidates its acquired atlas directory before
copy/growth/clear. [GUI interaction](gui-interaction.md#explore-gallery-and-displayed-geometry)
owns displayed layout/crop/labels/hit identity.
