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
The [custom recipe](../src/backend/data/benchmark/benchmark_custom_recipe.cpp),
[sampler](../src/backend/data/benchmark/benchmark_sampling.cpp), and
[category mappings](../src/backend/data/benchmark/benchmark_catalog.cpp) own those facts.
The [Open Images acquisition owner](../src/backend/data/benchmark/open_images_acquisition.cpp)
retains its source-specific acquisition and quarantine decisions.
Its adapters retain supplied boxes, area, identities, crowd/raw-ignore facts,
and source order. COCO-style polygon or RLE segmentation becomes source mask
support before resizing. Open Images `IsGroupOf` becomes crowd, with its category
MID retained separately from the mapped class. Equal boxes remain distinct.

COCONut unions B and the two Objects365 extensions, preferring Large over XL
for duplicate physical images. It retains foreground-empty images, validates
validation membership separately and excludes it from training, and uses neither
concatenated nested S/B/L/XL sets nor supplemental sampling. Publication requires
every offered image and
required panoptic mask after bounded repair. [Recovery](#optional-dropped-mask-recovery)
preserves these memberships.

The [pinned release catalog](../src/backend/data/benchmark/coconut/coconut_catalog.cpp) owns exact
URLs, revisions, expected sizes, available SHA-256 identities, and patch lists:

| Release | Native input and membership |
| --- | --- |
| `coconut_b` | Four Parquet shards; 241,602 rows from COCO train2017 and unlabeled2017 |
| `relabeled_coco_val` | One Parquet shard; 5,000 COCO val2017 rows |
| `coconut_large` | JSON plus panoptic tar; Objects365 v2 training patches 32, 35, 40, 50 |
| `coconut_xlarge` | Paired per-image JSON/PNG entries in a tar; additional patches 17, 23, 25, 28, 38, 42, 44 |
| `coconut_val` | COCO-shaped JSON and panoptic tar; separate Objects365 v1 JPEG archive |

Hugging Face URLs pin `resolve/<revision>/<file>`; Objects365 validation JPEGs
use the catalog's public Google Drive endpoint. HTTP identity, response/range,
and structural checks reject HTML masquerading as archives. Admitted records
and overlap reconciliation determine final counts, not release estimates or tar
viewer counts. The manifest retains
offered/admitted counts, duplicate XL coverage, annotation/source identities,
and validation membership.

## Native import and provenance

[coconut_parquet.cpp](../src/backend/data/benchmark/coconut/coconut_parquet.cpp) reads bounded
Arrow record batches for embedded PNG masks, `segments_info`, and `image_info`.
[coconut_annotations.cpp](../src/backend/data/benchmark/coconut/coconut_annotations.cpp) owns
external JSON/archive admission and component construction;
[coconut_physical.cpp](../src/backend/data/benchmark/coconut/coconut_physical.cpp)
resolves requested physical images, and
[coconut_native_image.cpp](../src/backend/data/benchmark/coconut/coconut_native_image.cpp)
owns per-image normalization and immutable native labels.
[coconut_recipe.cpp](../src/backend/data/benchmark/coconut/coconut_recipe.cpp)
selects components and validation policy. Arrow/Parquet is a
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

B's train/unlabeled membership comes from requested JPEG members in the
eligible physical archives, bound to their admitted generations. Recognized
`coco_url` namespaces guide the search; unknown or contradicted hints fall
through to other eligible sources. A hint, row position, numeric range, or
foreground-filtered stock annotation index alone cannot establish membership.
The resolver retains encountered locations and stops when its requests settle;
compilation does not require a complete archive census first.
Large's heterogeneous rows join through declared IDs, filenames, or full
Objects365 stems. Image rows without `id` join through `object365_name` or
`object365_file_name`; an annotation's supplied release ID remains intact.
XL pairs full names in `panseg/` and `panseg_info/`; its PNG supplies missing
dimensions. Validation joins use the declared physical stem, independently of
archive order. Import, inventory, and extraction share canonical member spelling:
leading `./` is normalized. Consumed members reject absolute paths, traversal,
backslashes, NUL, unsupported entry types, and invalid extents. Requested or
already-consumed joins reject encountered identity conflicts. Unused archive
tails are not a prerequisite for admitting a requested member.

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
does not interrupt compilation. One compile-scoped report owner lazily opens
the append stream, batches records up to 64 KiB, and flushes at image/release
settlement and during destruction, including failure unwinding.

Each COCONut label conversion compares its image's admitted header geometry
with the annotation canvas. If dimensions differ, all objects on that image
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
[CoconutImportLimits](../src/backend/data/benchmark/coconut/detail/coconut_native_image.h).
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

[CoconutRecoveryOriginals and CoconutMaskRecovery](../src/backend/data/benchmark/coconut/detail/coconut_mask_recovery.h)
share one immutable physical-image lookup per admitted original generation and
reuse per-lane RLE scratch. Recovered support retains its original backing
through the last consumer. A dropped slot is a declared thing with zero panoptic
support and no authoritative box. A boxed, present-empty mask is already a valid
object and is not a dropped slot. Recovery requires matching physical namespace, image
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

The [compiler](../src/backend/data/benchmark/benchmark_compiler.cpp) constructs
[BenchmarkCompilePipeline](../src/backend/data/benchmark/detail/benchmark_pipeline.h)
before source preparation. Ready metadata, headers, pixel decode/resize,
normalization, recovery, labels, archive work, and cache writes share its CPU
lanes. The configured worker count is clamped to the process's assigned CPUs.
Source controllers own I/O, lifecycle locks, and dependency waits outside those
lanes. Jobs blocked on resources do not prevent another feasible job in the
same stage from running. One CPU uses cooperative ready work between bounded
parser/consumer steps; a library call is not interrupted midway.

Acquisition has independent connection admission through
[BenchmarkCurl](../src/backend/data/benchmark/detail/benchmark_curl.h).
Artifacts enter independently when their own lock and resource allowance are
available. Eligible segmented transfers share transport with whole files;
they can run alongside other artifacts in the batch. Fresh segmentation
requires an expected size of at least 512 MiB and multiple admitted connections,
with nominal ranges of at least 64 MiB. Valid stored ranges can resume on one
connection. Response ranges and validators still govern admission and ordinary
fallback. Open Images retains one acquisition session across its bounded groups,
so a retrying group does not stop later eligible groups.

The product dependencies are per source or image:

| Product | Required facts |
| --- | --- |
| Membership | Canonical source metadata, requested physical joins, recipe reconciliation, and sampling where applicable |
| Geometry | An admitted image header bound to its encoded file generation; it can precede successful body decoding |
| Native labels | Parsed records and exact mask support; eligible recovery additionally needs that physical namespace's original split to settle |
| Compiled labels | Owned native/normalized labels plus the same image generation's geometry |
| Pixels | Admitted encoded image and a canonical destination slot |
| Final label placement | Canonical image order, settled provenance bases, and label/run prefix offsets |
| Publication | Both splits' pixels, placed labels, persisted sections, sync, and manifest completion |

Coco custom prepares independent annotation sources alongside image acquisition.
Its sampling and permitted quarantine still determine final membership.
COCONut metadata and physical resolution progress while optional originals or
another release's masks remain pending. Train and validation originals publish
independently; pending train recovery leaves B's unlabeled images runnable.
Large-over-XL reconciliation remains a real membership dependency.

Image publications capture source generation, attempt, and mutation custody.
Before placement is known, the pipeline retains compact header/file facts or
bounded encoded input. After registration it reuses stable per-image slots;
duplicate readiness cannot create duplicate pixel work. Each image joins its
own geometry and owned label input, so label conversion need not wait for a
pixel drain or release-wide normalization. A failed body can retain useful
header geometry while its image is repaired. Old publications cannot attach
to replacement generations or attempts.

[BenchmarkSplitAssembly](../src/backend/data/benchmark/detail/benchmark_writer.h)
retains immutable image chunks until each split's counts settle, fixes disjoint
offsets, and copies labels/runs once into final split storage while checking the
fields it consumes. Validation placement can finish while training preparation
continues. [BenchmarkSplitWriter](../src/backend/data/benchmark/benchmark_writer.cpp)
writes pixels directly into staged mapped slots and retains compatible completed
pixels across repair. Its final seal reuses admitted metadata and confirms
persisted header/layout through the existing descriptor after sync.
Failure/cancellation settles borrowed jobs before their owners or staged files
retire. The seal is compile-local ownership, not an on-disk trust flag.

### Archive and parser continuation

[BenchmarkArchive](../src/backend/data/benchmark/detail/benchmark_archive.h)
retains one opened inode, encountered member positions, and decoder state.
Safe raw extents use positional reads; gzip reuses a bounded compile-local
dictionary/index while retained. Known requests are visited in physical order
under one archive-format context. Pausing releases decoder capacity; evicted
gzip state or context-dependent ZIP/extended/sparse tar reads may replay the
required prefix. Control/chunk limits can fall back to bounded streaming.
There is no persisted seek sidecar or mandatory EOF/index-export pass.
Gzip decoder and caller capacity participate in the shared CPU allowance;
nonpreemptible grants last through that reader's active stream and leave
consumer capacity available. Returning decoder capacity does not release
encoded bytes still held by another consumer.

[JSON discovery](../src/backend/data/benchmark/benchmark_json.cpp) walks the
document structure once and yields bounded record ranges. Semantic parsing
retains canonical rows, source offsets, and reusable parser/mask scratch.
Open Images parses CSV rows once, with image ordering checked across chunks.
COCONut JSON retains parsed rows and physical joins across subsequent mask
work and compatible repair. Sparse mask operations use intervals/RLE; RGB ID
PNG normalization still reads the decoded pixels needed to establish support.

Parquet binds external fields from the
[canonical declarations](../src/backend/data/benchmark/coconut/detail/coconut_parquet.h).
Independent row-group ranges share CPU admission with Arrow internal threading
and prebuffering disabled. The image-column pass retains typed metadata;
later projections read missing segments/PNGs, skip reused fields, and omit
payload reads for completely reusable groups. Each range keeps a forward
Arrow reader and physical continuation across compatible groups. Resource
pressure can close/reacquire readers without reparsing retained rows; physical
archive prefixes may need replay. Borrowed PNG spans retain their record batch
and its complete allocation pool after a reader closes.

Completion order never supplies semantic order:

| Input | Canonical source order |
| --- | --- |
| Stock COCO / Objects365 JSON | Annotation-object byte offset |
| Open Images CSV | Original row byte offset |
| COCONut JSON | Annotation-array position, with segment prefixes including all declared segments |
| COCONut Parquet | Catalog shard, row group, then row; segment prefixes include stuff and omitted things |
| COCONut XL | Lexicographic physical-stem order, independently of tar order |
| Final COCONut components | Physical image IDs within canonical component order, preserving namespaces and Large-over-XL reconciliation |

### Memory, descriptors, and storage

[Resource allowances](../src/backend/data/benchmark/detail/benchmark_resources.h)
admit input, working storage, and completion needs together. Encoded capacity,
retained Arrow allocations, archive workspace, bounded handoffs, and reusable
decoder/normalizer/recovery/label scratch stay charged until their physical
backing is released. Shared backing is charged once. A stopped input reader
can lend unused promised workspace to ready pixel/label consumers only within
a stable allocation window; borrowers and their scratch settle before that
reader allocates again. Idle oversized scratch yields under pressure.

The private transient scheduling target is
`min(2 GiB, max(256 MiB, 64 MiB × CPU count))`. It is neither a process-memory
cap nor a public tuning option. A legal oversized operation can run without
competing transient users and finish its dependent consumers. Canonical
metadata, originals, normalized/native products, and final labels remain
dataset-sized retained products; their overlap can exceed the transient target.
Whole-image JPEG/PNG decode and resizing retain their existing storage needs
and allocation-failure behavior.

Descriptor admission measures process headroom and reserves dependent reader,
writer, and transport needs before retaining their source. Copied allowances
share the same commitment; nested consumers cannot spend a promise twice.
Closing a reader returns its own descriptors and unused commitments while
published descendants retain theirs. Small retained file/lease controls have
separate accounting from transient decoder workspace.

[StorageReservationPool](../src/backend/data/benchmark/benchmark_storage.cpp)
shares one compile-scoped ledger keyed by destination filesystem. Downloads,
image writes, caches, and split staging reserve outstanding physical growth;
already allocated blocks are not counted again. Sparse logical file length is
not allocated capacity. Allocation withdrawal precedes truncation/removal, and
[BenchmarkStagedArtifact](../src/backend/data/benchmark/detail/benchmark_staging.h)
keeps the file, growth reservation, and unpublished-file cleanup together.
These reservations coordinate this compilation, not unrelated filesystem users.

## Persistent cache and publication

Source-image validation and decoding recognize JPEG or PNG from the encoded
content, including PNG payloads stored under `.jpg` archive members. The shared
[image decoder](../src/backend/data/benchmark/benchmark_image_decoder.cpp) retains TurboJPEG
for JPEG and uses the existing PNG decoder without recompressing the source.
Cached image paths keep their stable `.jpg` spelling and original encoded bytes;
PNG pixels, dimensions, and annotation joins remain intact.

The persistent source cache survives compilation failure/cancellation, output
replacement, and recipe changes. [Cache selection](commands.md#benchmark-cache-selection)
owns the default location and wrapper/CLI precedence.

[benchmark_compiler.cpp](../src/backend/data/benchmark/benchmark_compiler.cpp) resolves
and checks both the compiler's staging output and the final publication
destination against the cache. Equal paths, a cache inside either output, or
an output inside the cache are rejected before publication can replace data.
The GUI's outer staging transaction supplies its final destination separately.
Compilation validates and syncs staged outputs before atomic publication;
failure or cancellation preserves the previous published output.

The [cache layout](../src/backend/data/benchmark/benchmark_cache.cpp) separates
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

Actual consumers still admit image headers and decode the required bytes.
[Encoded image custody](../src/backend/data/benchmark/detail/benchmark_image_input.h)
keeps mapped or pooled bytes with their header; a compact header-only fact can
be reused after reopening only if the file's inode identity still matches.
Deferred warm reads keep the inspected file open while waiting for byte
admission, so replacing a path cannot substitute another generation.

COCONut reuses admitted component locators and otherwise resolves requested
physical members during consumption. A retained physical-inventory format
remains available to explicit inventory callers, but production preparation
does not require that full census. Source archives remain retained, and healthy
reuse does not hash whole payloads.

Normalized indexes map immutable backing. Header/layout and image extents are
checked before membership use; boxes and runs are admitted once when full
annotation consumption is required. Derived read selections retain the full
source identity and backing instead of copying payloads. Completed builder
checks and inventory serialization seals are reused by storage/publication.
The mapping retains the opened inode after its descriptor closes, so atomic
replacement/unlink can proceed after the path-based readers settle without
holding an annotation lifecycle lock through downstream label work. Published
normalized inodes are never truncated or modified in place.

After COCONut annotation-import failure, a file matching its pinned SHA-256 is retained.
Recovery replaces only inputs without a matching pinned checksum. If every
input already matches, the import error is returned without re-downloading the
same release. That required checksum decision runs even with tracing disabled,
and failure to read its input remains fatal. Hashes used only for optional
failure diagnostics are lazy and trace-gated. A failed artifact's digest is
retained with its download generation rather than reread by each observer.

Repairs execute under the shared physical cache lease. They invalidate affected
proofs and corrupt artifacts/JPEGs, retain unrelated valid JPEGs, and rebuild
dependent labels at the compiler's preparation boundary after affected readers
settle. Parsed rows, compatible native products, and unaffected completed pixels
remain reusable. Changed physical or original generations withdraw only their
dependent label/pixel facts and progress; unrelated work continues. COCONut's
physical archive owner permits at most three structural admissions per archive
across that recovery. COCONut never converts exhausted recovery into silent
image loss; Coco custom retains its
established quarantine behavior. Exhausted recovery includes the underlying
failure and available image ID. Opt-in `benchmark.images.validation_failed`
records include the archive member, source/shard, image ID, encoded byte count,
and rejection reason.

[CocoAnnotationCache](../src/backend/data/benchmark/detail/benchmark_annotation_cache.h)
is the shared stock-annotation admission owner for Coco custom, Coconut's
Stock validation, and optional recovery originals. It discovers valid normalized
indexes before requesting raw annotations, so a valid stock index remains usable
with the raw archive/JSON absent. A missing split can be rebuilt while an already
settled split is retained. Original split publications carry their own generation;
withdrawal invalidates dependent recovery without discarding independent
physical work. Typed storage/resource-capacity failures and cancellation
propagate directly; they do not trigger corruption repair or needless
re-downloads. [Resource and storage admission](#memory-descriptors-and-storage)
owns the capacity rules.

COCONut annotation identity binds the release revision, normalization revision,
and annotation artifact identities. Each component additionally binds its
physical namespace and the physical shards used by its complete backing.
Large/XL components therefore do not inherit unrelated physical-archive
dependencies. Selection views retain the full component's dependency summary,
including images outside that selection. Older completion identities can
require rebuilding a component without changing normalized-index version 3.

### Recovery-derived annotation caches

Recovery-off components keep their existing normalized indexes. An eligible
component with usable originals uses a separate
`indexes/coconut-<release>/<physical-source>.recovery-<identity>.normalized.bin`
with its own `.inventory` and `.complete.json`. The identity binds the base
component inputs, physical namespace, recovery policy version (currently 1),
and admitted original annotation identity. Changes to those facts require a
different derived product. Original indexes, base components, physical
inventories, downloads, image caches, and image-group proofs remain reusable.

The canonical [inventory declarations](../src/backend/data/benchmark/coconut/detail/coconut_inventory.h)
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

Counts describe selected normalization recovery, excluding historical `failed.txt`
entries and later geometry checks. Cached products retain counts and replay
unresolved omissions to that report. Recovery off records zeros; on reports a
recovered/unresolved summary. These product/cache facts need no diagnostic log.

## Reading compilation progress

[DatasetCompileTracks](../src/backend/data/compiler/dataset_compile_progress.h) declares
three independent tracks shared by benchmark and Directory compilation. Each
has completed work, a known/unknown total, activity, active/completed state, and
explicit invalidated work. The native declaration also supplies generated Rust;
the GUI and CLI format the same typed facts for their respective displays.

| Track | Benchmark units and completion |
| --- | --- |
| Acquisition | Artifact bytes transferred or admitted from cache, aggregated once per artifact; completion additionally requires the owning acquisition work to settle |
| Labels/masks | Normalization work plus completed source/component label plans; COCONut normalization counts full-import rows, while Coco custom uses index/sampling milestones |
| Pixels | Successfully compiled image slots across train and validation, retaining compatible completed work through preparation retries |

Label totals can grow when final plans become known; they are work units, not
image counts or a time estimate. COCONut metadata-only preparation does not
count full-import rows a second time. Directory compilation marks Acquisition
as unnecessary, with known zero completed and total work; its labels and pixels
each count completed images. A completed acquisition track can coexist with
active labels and pixels. All three completing still leaves validation, sync,
and atomic publication to settle before the operation succeeds.

[ProgressReporter](../src/backend/data/benchmark/benchmark_progress.cpp) owns benchmark
observations and compile-scoped artifact/indexing ledgers. An observed artifact
with an unknown size makes its source and aggregate byte totals unknown; the
GUI shows `?` and omits a determinate bar for that track. Totals can change as
new artifacts are observed. The overall phase and current activity describe
the latest observed work, not the only running lane.

Per-source rows retain actual bytes, resolved/selected images, retry counts,
cache reuse, resume, and invalidated-image facts. Resolved images include
permitted quarantine outcomes; successful cache-write observations follow
physical write settlement. Projected output bytes are a storage-admission
bound, not bytes already written.

Optional `BenchmarkSourceProgress.transfer` uses canonical
`BenchmarkTransferProgress` for the latest artifact's completed/total/retained
bytes, attempt, reuse, and resume state; source counters aggregate artifacts.
Zero transfer total means unknown. Retained ≤ completed ≤ known total. Source
activity changes/completion clear the latest transfer, which does not enumerate
all concurrent in-flight artifacts.

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

The [Dataset progress component](../src/frontend/iced/src/view/train/dataset/progress.rs)
formats byte quantities as KiB, MiB, GiB, or TiB, including byte throughput and
projected storage. Values at least one KiB use one decimal place; smaller nonzero
values use three so that one byte remains visible. Zero is `0 KiB`.
Counts use comma grouping without converting the integer text through floating
point. Completed quantities whose total matches appear once; unknown totals
remain `?`. Directory acquisition consequently reads **No acquisition needed ·
0 KiB** and has no determinate bar.

Source headings distinguish **Cached**, **Complete**, **Active**, and **Waiting**.
For example, a completed cached source can read **Objects365 v2 · Cached** above
**85.0 GiB · 345,491 images**. Active sources retain their activity and current
transfer, with attempt, retained-byte, retry, resume, and repair details when
present. Completed sources drop obsolete transfer activity while retaining
aggregate retry/resume/repair facts. Neutral planning rows do not claim a known
empty total before size or completion establishes it. The current-work caption
uses bytes for Downloading/Extracting and grouped work counts for other phases;
it remains separate from the three tracks. Known positive track totals support
bars; unknown, zero, and unnecessary work preserve the bar's space without
inventing a percentage.

Native activity and terminal results control the
[progress area's cancellation and settlement](gui-interaction.md#dataset-compilation-controls).
Its animation never decides completion. The CLI retains exact byte counts,
unknown-total text, transfer attempts, retained bytes, resume, and retry details
through [its source-status formatter](../src/backend/data/benchmark/benchmark_compiler.cpp);
native activity strings carry the operation description rather than a second
encoded copy of the transfer quantities.

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

[benchmark_annotations.cpp](../src/backend/data/benchmark/benchmark_annotations.cpp)
owns normalized-index layout and staged publication. Its six
[AnnotationRejectCounts](../src/backend/data/benchmark/detail/benchmark_annotations.h)
fields generate both binary and named JSON projections from one declaration.
Compile-time format guards pin their names, `uint64` types, and declaration
order. [Cache admission](#persistent-cache-and-publication) covers parsed and
mapped products and the lifetime of their checked backing.

[coconut_inventory.h](../src/backend/data/benchmark/coconut/detail/coconut_inventory.h) is the
canonical inventory declaration. The explicit physical-inventory format binds
records to archive identity, namespace, shard, and canonical members. Component
inventories are admitted together with their normalized index, input identity,
edition, namespace, member joins, and normalization revision. Inventory strings are
bounded to 4,096 bytes. Their canonical encoding is sealed once for identity,
size, and publication. Incompatible or invalid cache records require rebuilding
from admitted source inputs rather than reinterpretation.

[Packed format limits](datasets.md#instances-and-masks) permit split RLE blocks
beyond 4 GiB through 64-bit offsets but reject unrepresentable offsets, counts,
coordinates, and extents before publication. Exact COCONut masks are never
simplified/sampled to fit. Recompiling older bins preserves reusable downloads,
images, and normalized annotations. [Bounded fixtures](validation.md#benchmark-compilation-evidence)
prove conversion/reuse, not full-release capacity at every resolution or live
download duration/liveness.
