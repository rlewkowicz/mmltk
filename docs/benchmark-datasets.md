# Built-in benchmark datasets

[Wiki index](README.md) · [Source formats and loading](datasets.md) · [Dataset controls](gui-interaction.md#dataset-compilation-controls) · [Commands](commands.md#benchmark-cache-selection) · [Diagnostics](logging.md#benchmark-compilation-traces)

The native benchmark compiler prepares `train.bin`, `val.bin`, and
`benchmark_manifest.json` from retained source data. Both recipes use the
existing COCO80 foreground catalog, [resize geometry](datasets.md#resize-geometry),
and [compiled format 9](datasets.md#compiled-binary-format).

## Recipes and validation membership

| Selection | Training images and annotations | Validation images and annotations |
| --- | --- | --- |
| Coco custom | Existing COCO 2017 training recipe plus sampled Objects365 v2 and Open Images v7, using their existing category mappings | COCO val2017 with stock instance annotations |
| Coconut / Coconut validation | All B images plus Large and nonduplicate XL extension images, with COCONut annotations | Relabeled COCO val2017 plus the COCONut Objects365 validation extension |
| Coconut / Stock validation | Same full COCONut training set | COCO val2017 with stock instance annotations |
| Coconut / Coconut stock | Same full COCONut training set | COCO val2017 with COCONut enhanced annotations |

Coco custom preserves its existing source catalogs, deterministic supplemental
sampling, split membership, annotation order, and permitted quarantine policy.
The [custom recipe](../src/backend/data/benchmark_custom_recipe.cpp),
[sampler](../src/backend/data/benchmark_sampling.cpp), and
[category mappings](../src/backend/data/benchmark_catalog.cpp) own those facts.
The [Open Images acquisition owner](../src/backend/data/open_images_acquisition.cpp)
retains its source-specific acquisition and quarantine decisions.
Its adapters retain supplied boxes, area, identities, crowd/raw-ignore facts,
and source order. COCO-style polygon or RLE segmentation becomes source mask
support before resizing. Open Images `IsGroupOf` becomes crowd, with its category
MID retained separately from the mapped class. Equal boxes remain distinct.

COCONut uses the union of B and the two Objects365 extensions. Large wins when
Large and XL offer the same physical image. Images without selected foreground
instances remain in the dataset. Validation membership is checked separately
and cannot enter training. The nested S/B/L/XL releases are not concatenated as
independent complete datasets, and COCONut does not apply supplemental sampling.
Every offered image and required panoptic mask must be available after bounded
archive/image repair for publication to succeed. Optional
[dropped-mask recovery](#optional-dropped-mask-recovery) preserves these recipe
and validation memberships.

The [pinned release catalog](../src/backend/data/coconut_catalog.cpp) owns exact
URLs, revisions, expected sizes, available SHA-256 identities, and patch lists:

| Release | Native input and membership |
| --- | --- |
| `coconut_b` | Four Parquet shards; 241,602 rows from COCO train2017 and unlabeled2017 |
| `relabeled_coco_val` | One Parquet shard; 5,000 COCO val2017 rows |
| `coconut_large` | JSON plus panoptic tar; Objects365 v2 training patches 32, 35, 40, 50 |
| `coconut_xlarge` | Paired per-image JSON/PNG entries in a tar; additional patches 17, 23, 25, 28, 38, 42, 44 |
| `coconut_val` | COCO-shaped JSON and panoptic tar; separate Objects365 v1 JPEG archive |

Hugging Face inputs use pinned `resolve/<revision>/<file>` URLs. The separate
Objects365 validation JPEG archive uses the catalog's public Google Drive
download endpoint. HTTP identity, response/range validation, and structural
admission still govern downloads; an HTML response cannot become an admitted
image archive. Final counts come from admitted records and overlap
reconciliation, rather than rounded release descriptions or tar viewer counts.
The manifest records offered/admitted component counts, duplicate XL coverage,
selected annotation identities, source artifacts, and validation membership.

## Native import and provenance

[coconut_parquet.cpp](../src/backend/data/coconut_parquet.cpp) reads bounded
Arrow record batches for embedded PNG masks, `segments_info`, and `image_info`.
[coconut_annotations.cpp](../src/backend/data/coconut_annotations.cpp) owns
external JSON/archive admission, physical joins, and exact annotation
normalization. [coconut_recipe.cpp](../src/backend/data/coconut_recipe.cpp)
selects those components and the validation policy. Arrow/Parquet is a
[private native format dependency](build.md#native-parquet-dependency);
compilation has no Python or Hugging Face utility execution dependency.

Physical provenance consists of the source namespace, numeric image ID,
archive/shard identity, and canonical archive member. The component inventory
also retains the declared release-row ID and source ordinal. These can differ:
the validation row ID `691105` joins through its declared
`object365_file_name` to `objects365_v1_00091105`, with a JPEG under `image/`
and a mask under `panoptic_o365val_v3/`. The compiled image ID is the physical
ID; [source-kind values](datasets.md#per-image-index) retain the annotation
provenance. Objects365 v1 and v2 remain distinct namespaces.

B's train/unlabeled membership comes from complete JPEG archive inventories
bound to archive identity. Its `coco_url`, row position, numeric ranges, or a
foreground-filtered stock annotation index cannot establish that subset.
Large's heterogeneous rows join through declared IDs, filenames, or full
Objects365 stems. Image rows without `id` join through `object365_name` or
`object365_file_name`; an annotation's supplied release ID remains intact.
XL pairs full names in `panseg/` and `panseg_info/`; its PNG supplies missing
dimensions. Validation joins use the declared physical stem, independently of
archive order. Import, inventory, and extraction share canonical member spelling:
leading `./` is normalized, while absolute paths, traversal, backslashes, NUL,
ambiguous joins, and unsupported archive entries are rejected.

Panoptic PNGs must contain bounded 8-bit RGB data. Segment IDs use the exact
24-bit value `R | (G << 8) | (B << 16)`, without color conversion. Record-level
`isthing` determines foreground admission, even when a stuff record uses a
numeric thing-category ID. Admitted things use the existing COCO80 mapping.
Masks retain exact pixel support, including holes and single pixels, as
row-major runs. Supplied boxes stay authoritative; otherwise support supplies
exclusive upper bounds. Supplied valid area, crowd, ignore, category, segment ID,
and ordinal remain intact; absent/null area comes from mask support. Optional
[recovery](#optional-dropped-mask-recovery) supplies original COCO geometry for
recovered objects and updates visible area for masks it carves.
An image with no admitted things still has an image record.

A declared thing still lacking both mask pixels and an authoritative box after
any selected recovery is omitted from normalized object metadata and counted
as a dropped instance; its image and other objects remain. Compilation appends
the image/member, physical and release image IDs, object/category IDs, release,
and reason to `failed.txt` in
the nearest `.cache` ancestor of the benchmark cache (normally
`.cache/failed.txt`). A custom cache outside `.cache` keeps the report at its
own root. The report uses one JSON object per line and retains earlier entries.
It is append-only history: a later successful recovery does not remove an older
rejection. Compiler decisions never read this report; current selected recovery
counts come from the [annotation products and manifest](#recovery-derived-annotation-caches).
The first rejection produces a concise progress warning; report-write failure
does not interrupt compilation.

Before preparing COCONut labels, compilation checks each retained image's header
against its annotation canvas. If dimensions differ, all objects on that image
are omitted from compiled metadata and reported through the same `failed.txt`
path, with both dimensions in the reason. The image remains, using its actual
dimensions for resizing and Original view. A size mismatch does not establish
that the mask belongs to the image, so the compiler does not rescale those
annotations or invalidate the archive. Unreadable images still use bounded
physical repair. These checks also apply when normalized annotations are reused
from cache; retained source annotations and image bytes remain reusable.

Current import admission bounds each encoded PNG to 64 MiB, each decoded image
to 64 Mi pixels, each axis to 32,767, and each segment list to 65,535 entries.
The canonical limits are in
[CoconutImportLimits](../src/backend/data/detail/coconut_annotations.h).
These precede the separate compiled-format capacity checks below.

## Optional dropped-mask recovery

For Coconut, **Recover dropped masks from original annotations** is off by
default. Enable it in the [Dataset controls](gui-interaction.md#dataset-compilation-controls)
before compiling. Existing datasets require recompilation to gain recovered
objects; changing the setting does not modify an existing `.bin`, source JPEG,
or panoptic PNG. Recovery runs before the ordinary Stretch/Letterbox projection
and keeps compiled format 9.

The original stock COCO train2017 instance annotations can recover dropped
things in COCONut B's physical COCO train images. Stock val2017 originals serve
the relabeled COCO validation component selected by Coconut validation or
Coconut stock. Stock validation keeps its stock labels. COCO unlabeled and
Objects365 components have no original-mask recovery source and retain their
ordinary normalization.

[CoconutMaskRecovery](../src/backend/data/coconut_mask_recovery.cpp) indexes
admitted originals by physical image once and reuses bounded per-image RLE
scratch. A dropped slot is a declared thing with zero panoptic support and no
authoritative box. A boxed, present-empty mask is already a valid object and
is not a dropped slot. Recovery requires matching physical namespace, image
identity, and canvas dimensions, then admits each category/crowd/raw-ignore
group conservatively:

- Original candidates must have valid nonempty masks, boxes, area, and distinct
  annotation IDs; their count must equal the group's COCONut thing count.
- Every surviving thing must intersect exactly one original candidate, and
  different survivors must identify different candidates.
- The remaining original count must exactly equal the dropped-slot count.
  Ambiguous, invalid, or incomplete groups remain unrecovered.

COCO annotation IDs and COCONut segment IDs are independent. Within an admitted
remaining set, sorted original annotation IDs pair with sorted dropped segment
IDs. The recovered object keeps its COCONut category, flags, segment identity,
and source ordinal while taking the original COCO mask, authoritative box, and
area. [Recovery provenance](#recovery-derived-annotation-caches) records both IDs.

The union of recovered pixels is subtracted from every surviving, unrecovered
thing mask on that image. Its supplied COCONut box remains authoritative;
otherwise its new support determines the box. A carved mask's stored source
area becomes its remaining foreground count. A fully carved object with a box
survives with a present-empty mask; one without a box is omitted. Carving does
not recursively recover that new omission. Stuff segments are unchanged.

Original splits are admitted independently through the shared COCO annotation
cache. Unavailable downloads or unusable source archives/documents can leave
optional originals unavailable after bounded source attempts, with a progress
warning and the affected objects omitted. An admitted split remains usable
when the other optional split fails. Stock validation still requires usable
val2017 annotations. Cancellation, allocation/capacity failures, local file or
I/O failures, and staging/publication failures remain fatal to the operation;
they are not converted into optional-source omissions.

## Overlapping acquisition, labels, and pixels

The [compiler](../src/backend/data/benchmark_compiler.cpp) fixes selected split
membership and reserves pixel storage before final labels are available.
Acquisition, annotation preparation, and pixel compilation can then advance
independently, subject to their actual input dependencies and one compile-wide
CPU budget. Completion order does not change recipe membership, annotation
order, or the final compiled image order.

Coco custom prepares independent annotation sources while COCO image archives
are acquired. An available archive can be inspected and extracted while another
transfer is pending. Each settled source group can prepare its labels while
later sources are still being acquired; admitted cached images can compile
pixels while label work continues. Objects365's sampled shards still settle
before that source's label plan chooses its available membership.

COCONut acquires annotation inputs alongside physical archive inventories.
Each managed release lane retains its own lifecycle lease through metadata and
mask import. The metadata receiver can consume a ready release while another
lane is acquiring inputs, waiting for its lease, or importing masks. Complete
physical inventories still establish image joins, and the reconciled metadata
establishes split membership before pixel slots are reserved. Full mask
normalization can overlap selected-image extraction and pixel compilation.
Warm components retain their admitted normalized data across the metadata and
full-label handoff instead of reopening the same product.

The [download boundary](../src/backend/data/detail/benchmark_download.h) returns
typed durable artifact results; [image readiness](../src/backend/data/detail/benchmark_images.h)
is emitted only after admitted cache reuse or an atomic image write. Queued and
active pixel work retain the source generation's lease until their reads settle.
[BenchmarkCompilePipeline](../src/backend/data/benchmark_pipeline.cpp) registers
one slot per selected physical image before admission, ignores duplicate
readiness, and queues those retained slots without per-image task allocations.
Its consumers block on notifications. A single-worker compilation handles pixel
readiness inline.

Acquisition, parsing, decompression, cache writers, and pixel lanes receive
bounded portions of the eligible CPUs. Each pixel lane owns reusable decoder
and [resizer scratch](datasets.md#optional-perceptual-downscaling), with no nested
resizer thread pool. [BenchmarkSplitWriter](../src/backend/data/benchmark_writer.cpp)
retains successful pixel slots through final label preparation and compatible
repair. It validates final source dimensions, metadata, masks, and persisted
sections before publication. Failure or cancellation retires queued custody
and joins active readers before their source generation or staged output can
be replaced or destroyed.

## Persistent cache and publication

Source-image validation and decoding recognize JPEG or PNG from the encoded
content, including PNG payloads stored under `.jpg` archive members. The shared
[image decoder](../src/backend/data/benchmark_image_decoder.cpp) retains TurboJPEG
for JPEG and uses the existing PNG decoder without recompressing the source.
Cached image paths keep their stable `.jpg` spelling and original encoded bytes;
PNG pixels, dimensions, and annotation joins remain intact.

The benchmark cache is persistent source data. Its default wrapper location is
`.cache/benchmark-dataset/v1`; it remains reusable across compilation failures,
cancellation, output replacement, and recipe changes. Cache-root precedence and
the wrapper/CLI configuration are documented in
[benchmark cache selection](commands.md#benchmark-cache-selection).

[benchmark_compiler.cpp](../src/backend/data/benchmark_compiler.cpp) resolves
and checks both the compiler's staging output and the final publication
destination against the cache. Equal paths, a cache inside either output, or
an output inside the cache are rejected before publication can replace data.
The GUI's outer staging transaction supplies its final destination separately.
Compilation validates and syncs staged outputs before atomic publication;
failure or cancellation preserves the previous published output.

The [cache layout](../src/backend/data/benchmark_cache.cpp) separates
`downloads/`, `images/`, `indexes/`, and `locks/`. Downloaded archives and COCO/
Objects365 JPEGs are shared physical data. COCONut label indexes are separated
by annotation edition and physical namespace. Image completion proofs additionally
bind the selected recipe/validation choice and exact image selection:
Coco custom uses the group's `.complete.json`, while COCONut uses
`.recipe-proofs/coconut-<validation-value>-<selection-digest>.json` under the
shared image group. Reusing pixels never substitutes stock labels for enhanced
labels.

Reuse has distinct levels:

1. An admitted image-group completion proof resolves that selection without
   another extraction or per-JPEG validation pass.
2. An admitted archive is reused without another network transfer. Matching
   completion metadata carries its HTTP identity; permitted preseeded files
   remain subject to structural admission.
3. An incomplete image group scans and retains individually valid JPEGs before
   extracting the missing selection. The initial reuse count is published even
   when all selected images already exist and only the proof is missing.

COCONut additionally admits full physical archive inventories before importing
labels, even when selected image-group proofs exist. Those inventories have
their own identity-bound reuse path. Source archives remain retained; ordinary
reuse does not add routine whole-file hashing. Failure diagnosis retains the
existing SHA-256 path and bounded repair policy.

After annotation-import failure, a file matching its pinned SHA-256 is retained.
Recovery replaces only inputs without a matching pinned checksum. If every
input already matches, the import error is returned without re-downloading the
same release. Ordinary successful cache reuse still performs no routine hash.

Repairs execute under the shared physical cache lease. They invalidate affected
proofs and corrupt artifacts/JPEGs, retain unrelated valid JPEGs, and rebuild
dependent inventories/labels at the compiler's preparation boundary after
affected readers settle. Completed pixels from unaffected source generations
remain reusable; repaired sources explicitly withdraw their completed pixel
and annotation contributions. COCONut's physical archive owner permits at most
three structural admissions per archive across that recovery. COCONut never
converts exhausted recovery into silent image loss; Coco custom retains its
established quarantine behavior. Exhausted recovery includes the underlying
failure and available image ID. Opt-in `benchmark.images.validation_failed`
records include the archive member, source/shard, image ID, encoded byte count,
and rejection reason.

[CocoAnnotationCache](../src/backend/data/detail/benchmark_annotation_cache.h)
is the shared stock-annotation admission owner for Coco custom, Coconut's
Stock validation, and optional recovery originals. It discovers valid normalized
indexes before requesting raw annotations, so a valid stock index remains usable
with the raw archive/JSON absent. A missing split can be rebuilt while an already
settled split is retained. Typed storage-capacity failures and cancellation propagate directly;
they do not trigger corruption repair or needless re-downloads.
[benchmark_storage.cpp](../src/backend/data/benchmark_storage.cpp) owns capacity
checks and concurrent reservations.

### Recovery-derived annotation caches

Recovery-off components keep their existing normalized indexes. An eligible
component with usable originals uses a separate
`indexes/coconut-<release>/<physical-source>.recovery-<identity>.normalized.bin`
with its own `.inventory` and `.complete.json`. The identity binds the base
component inputs, physical namespace, recovery policy version (currently 1),
and admitted original annotation identity. Changes to those facts require a
different derived product. Original indexes, base components, physical
inventories, downloads, image caches, and image-group proofs remain reusable.

The canonical [inventory declarations](../src/backend/data/detail/coconut_inventory.h)
derive the recovery trailer and its checked encoding. Each image retains
recovered COCONut IDs, source ordinals/categories, original COCO IDs, and
unresolved omissions. Joint index/inventory/completion admission verifies these
facts; invalid or interrupted products rebuild. Unavailable originals select
the base component without publishing a successful derived recovery cache.
A later compilation attempts original admission again.

`benchmark_manifest.json` keeps these current facts under `recipe`:

| Field | Meaning |
| --- | --- |
| `recover_dropped_masks` | Selected compile option |
| `original_annotations.train`, `.validation` | Present when recovery is enabled: admitted original annotation identities, or `null` when unavailable |
| `recovered_objects`, `unresolved_objects` | Counts from the selected recovery-eligible COCO components |
| `components[].recovery_policy`, `.original_annotation_identity` | Policy and original identity governing that component; zero/empty for base products |
| `components[].recovered_objects`, `.unresolved_objects` | That component's current counts after membership reconciliation |

These counts concern normalization's recovery outcome, not every historical
`failed.txt` line or the later image/header geometry checks. Cached derived
products retain the same counts and replay their unresolved omissions to the
append-only report. Recovery-off compilation records zero recovery counts.
When enabled, compilation exposes a concise recovered/unresolved progress
summary. No diagnostic log is needed to establish cache validity or these
product facts.

## Reading compilation progress

[DatasetCompileTracks](../src/backend/data/dataset_compile_progress.h) declares
three independent tracks shared by benchmark and Directory compilation. Each
has completed work, a known/unknown total, activity, active/completed state, and
explicit invalidated work. The native declaration also supplies generated Rust;
the GUI and CLI display the same facts.

| Track | Benchmark units and completion |
| --- | --- |
| Acquisition | Artifact bytes transferred or admitted from cache, aggregated once per artifact; completion additionally requires the owning acquisition work to settle |
| Labels/masks | Normalization work plus completed source/component label plans; COCONut normalization counts full-import rows, while Coco custom uses index/sampling milestones |
| Pixels | Successfully compiled image slots across train and validation, retaining compatible completed work through preparation retries |

Label totals can grow when final plans become known; they are work units, not
image counts or a time estimate. COCONut metadata-only preparation does not
count full-import rows a second time. Directory compilation shows Acquisition
as **unnecessary**, with a known `0 / 0`; its labels and pixels each count
completed images. A completed acquisition track can coexist with active labels
and pixels. All three completing still leaves validation, sync, and atomic
publication to settle before the operation succeeds.

[ProgressReporter](../src/backend/data/benchmark_progress.cpp) owns benchmark
observations and compile-scoped artifact/indexing ledgers. An observed artifact
with an unknown size makes its source and aggregate byte totals unknown; the
GUI shows `?` and omits a determinate bar for that track. Totals can change as
new artifacts are observed. The overall phase and current activity describe
the latest observed work, not the only running lane.

Per-source rows retain actual bytes, resolved/selected images, retry counts,
cache reuse, resume, and invalidated-image facts. Resolved images include
permitted quarantine outcomes; successful cache-write observations follow
physical write settlement. Projected output bytes are a storage-admission
bound, not bytes already written. After failure or cancellation, unfinished GUI
tracks remain **incomplete** rather than showing successful completion.

Fresh, resumed, retried, and re-downloaded transfers report bytes actually
written. A resumed transfer distinguishes retained bytes from its new work.
Segmented downloads add in-flight written bytes to durable completed ranges;
the preallocated `.part` file length is not a transfer counter. `.part.json`
retains resume identity and range progress. Range rejection or discarded
attempt work can deliberately roll progress back. The compile-owned ledgers
replace an artifact or release contribution rather than adding retries twice.
Repair withdraws affected completed work and records the invalidation; unrelated
completed work stays counted. Unknown totals remain open-ended until the
downloader establishes an exact size.

Transfer and scan observations use existing bounded observation points.
Enabled [benchmark traces](logging.md#benchmark-compilation-traces) expose cache,
archive, transfer, and retry context independently of pixel probes. Diagnostic
records do not authorize cache reuse, readiness, completion, or publication.
An unchanged source-image count can coexist with advancing download bytes,
labels, or pixels; it alone establishes neither a deadlock nor network liveness.

## Cache formats and capacity

These formats have independent versions:

| Artifact | Current representation |
| --- | --- |
| Compiled `train.bin` / `val.bin` | [Format 9](datasets.md#compiled-binary-format), with 64-bit mask byte offsets and 60-byte instance records |
| Normalized annotation index | Version 3, 256-byte header, 32-byte image records, 64-byte annotation records, and source-mask RLE |
| Download, extraction, and image-group metadata | Cache schema 2; the cache directory name remains `v1` |
| COCONut physical/component inventory | Version 1 (`CNUTIVN1`), declaration-order little-endian encoding with checked strings/counts and a SHA-256 trailer; derived components additionally bind the recovery-policy trailer |
| `benchmark_manifest.json` | Schema 3 compilation facts, including recipe, source identities, recovery selection/counts, mappings, resolution, resize/resampling policy, and cache root |

[benchmark_annotations.cpp](../src/backend/data/benchmark_annotations.cpp)
owns normalized-index layout and staged publication. Its six
[AnnotationRejectCounts](../src/backend/data/detail/benchmark_annotations.h)
fields generate both binary and named JSON projections from one declaration.
Compile-time format guards pin their names, `uint64` types, and declaration
order. Each COCO-style fill worker retains parser, mask, and polygon scratch
across records; box-only Open Images records avoid per-annotation empty-mask
allocations.

[coconut_inventory.h](../src/backend/data/detail/coconut_inventory.h) is the
canonical inventory declaration. Physical inventories bind records to their
archive identity, namespace, shard, and canonical members. Component inventories
are admitted together with their normalized index, input identity, edition,
namespace, member joins, and normalization revision. Inventory strings are
bounded to 4,096 bytes. Incompatible or invalid cache records require rebuilding
from admitted source inputs rather than reinterpretation.

Compiled mask offsets address the complete split's RLE block with 64 bits, so
COCONut label preparation can exceed 4 GiB of mask data. Older compiled files
require recompilation; source downloads, extracted images, and normalized
annotation caches remain reusable.

The [packed format limits](datasets.md#instances-and-masks) still apply. The
compiler rejects unrepresentable offsets, counts, coordinates, or extents
before publishing output. Exact COCONut masks
are not simplified or sampled to fit those limits. Bounded local fixtures
establish the documented conversion/reuse behavior; they do not establish
full production-release capacity at every resolution or the duration/liveness
of live downloads. [Validation ownership](validation.md#benchmark-compilation-evidence)
locates those cases.
