# Built-in benchmark datasets

[Wiki index](README.md) · [Source formats and loading](datasets.md) · [Dataset controls](gui-interaction.md#dataset-compilation-controls) · [Commands](commands.md#benchmark-cache-selection) · [Diagnostics](logging.md#benchmark-compilation-traces)

The native benchmark compiler prepares `train.bin`, `val.bin`, and
`benchmark_manifest.json` from retained source data. Both recipes use the
existing COCO80 foreground catalog, [resize geometry](datasets.md#resize-geometry),
and [compiled format 10](datasets.md#compiled-binary-format).

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
concatenated nested S/B/L/XL sets nor supplemental sampling. Release inventory
defines eligible membership; independently encountered archive images may be
compiled speculatively but are excluded unless listed. After bounded repair,
publication retains the healthy subset, including partially valid labels and
valid zero-label backgrounds. Unreadable pixels, contradictory geometry, and
wholly corrupt required labels exclude that image. Missing labels do not prove
background. Both required splits must remain nonempty. [Recovery](#optional-dropped-mask-recovery)
preserves the recipe's intended membership and annotation meaning.

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
[coconut_import.cpp](../src/backend/data/benchmark/coconut/coconut_import.cpp) owns
external JSON/archive admission, retained rows, and ready normalization/recovery;
[coconut_annotations.cpp](../src/backend/data/benchmark/coconut/coconut_annotations.cpp)
owns persisted component admission, assembly, and publication;
[coconut_physical.cpp](../src/backend/data/benchmark/coconut/coconut_physical.cpp)
consumes physical archives independently and joins image identities, and
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
and a mask under `panoptic_o365val_v3/`. Compiled `source_image_id` retains that
physical source ID; its compiled slot ID is a separate identity.
[Source-kind values](datasets.md#per-image-index) retain the annotation
provenance. Objects365 v1 and v2 remain distinct namespaces.

B's train/unlabeled membership comes from actual JPEG members in the
eligible physical archives, bound to their admitted generations. Recognized
`coco_url` namespaces are hints; a contradicted hint cannot override admitted
physical identity. A hint, row position, numeric range, or foreground-filtered
stock annotation index alone cannot establish membership. Independent image
consumers publish encountered members, while annotation consumers publish
their own labels without initiating a JPEG-archive search. Their per-image join
checks namespace, generation, and ambiguity before publication. Useful image
and label work needs no complete archive census or release inventory first.
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
as a dropped instance. Other valid objects remain; an image whose required
foreground labels are all rejected is excluded, while a proven background
image remains eligible. Compilation appends
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

Labels can normalize and project using their annotation/PNG canvas before the
JPEG arrives. The eventual admitted image header must agree before that sample
becomes eligible. Contradictory dimensions exclude the sample and produce an
omission report; the compiler never silently rescales labels to force a match.
Unreadable images use bounded physical repair. These checks also apply when
normalized annotations are reused from cache; unrelated retained source
annotations and image bytes remain reusable.

Canonical source ordinals settle independently of pixel placement. A late known
prefix can backpatch only the used instance records, preserving pixels and mask
runs. If an irrecoverably unreadable annotation artifact has an unknown extent,
healthy later artifacts retain their known identities and local ordinals with
the [incomplete-provenance flag](datasets.md#instances-and-masks). An unknown
prefix is never assumed to be zero. Component completion facts retain the
known source extent, its completeness, and affected image IDs; normalized-v3
records, cache reuse, `recipe.components[].incomplete_ordinal_images`, progress
warnings, and `failed.txt` preserve that distinction. A later compilation clears
it only when authoritative source extents prove the exact release-global
ordinals. No unrelated discovery scan delays healthy work to obtain that proof.

Current import admission bounds each encoded PNG to 64 MiB, each decoded image
to 64 Mi pixels, each axis to 32,767, and each segment list to 65,535 entries.
The canonical limits are in
[CoconutImportLimits](../src/backend/data/benchmark/coconut/detail/coconut_input.h).
These precede the separate compiled-format capacity checks below.

## Optional dropped-mask recovery

For Coconut, **Recover dropped masks from original annotations** is off by
default. Enable it in the [Dataset controls](gui-interaction.md#dataset-compilation-controls)
before compiling. Existing datasets require recompilation to gain recovered
objects; changing the setting does not modify an existing `.bin`, source JPEG,
or panoptic PNG. Recovery runs before the ordinary Stretch/Letterbox projection
and uses the same compiled format as ordinary compilation.

The original stock COCO train2017 instance annotations can recover dropped
things in COCONut B's physical COCO train images. Stock val2017 originals serve
the relabeled COCO validation component selected by Coconut validation or
Coconut stock. Stock validation keeps its stock labels. COCO unlabeled and
Objects365 components have no original-mask recovery source and retain their
ordinary normalization.

[CoconutRecoveryOriginals and CoconutMaskRecovery](../src/backend/data/benchmark/coconut/detail/coconut_mask_recovery.h)
share one immutable physical-image lookup per admitted original generation and
reuse context-owned RLE scratch. Recovered support retains its original backing
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
before source preparation. The pipeline owns source/image joins and selected
label claims; ready metadata, headers, pixel decode/resize, normalization,
recovery, labels, gathering, archive work, and cache writes use the data-owned
[shared execution kernel](compile-execution.md#owners-and-ready-work).
Its live-class floors, elastic lanes, and bounded file service also serve
Directory compilation. The configured worker count respects assigned CPUs;
source policy and dependency custody stay with the recipe/import owners.

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
| Membership | Release inventory, admitted physical joins, recipe reconciliation, and sampling where applicable |
| Geometry | An admitted image header bound to its encoded file generation; it can precede successful body decoding |
| Native labels | Parsed records and exact mask support; eligible recovery additionally needs that physical namespace's original split to settle |
| Compiled labels | Owned native/normalized labels and their declared canvas; eventual image geometry must agree |
| Pixels | Admitted encoded image and a stable arrival-order physical slot |
| Label placement | That image's arithmetic label slot, independent of other images' label sizes or pixel completion |
| Publication | Final eligible populations, selected label claims and provenance status, completed pixels/labels, in-place compaction, sync, and manifest completion |

Coco custom prepares independent annotation sources alongside image acquisition.
Its sampling and permitted quarantine still determine final membership.
COCONut metadata and physical resolution progress while optional originals or
another release's masks remain pending. Train and validation originals publish
independently; pending train recovery leaves B's unlabeled images runnable.
Native products unrelated to deferred original-mask recovery remain runnable.
Large-over-XL reconciliation settles the selected labels for duplicate physical
images; it does not prevent independent pixel conversion.

Each row owns its physical-readiness registration. Observing the snapshot,
generation, and registration is atomic; retiring the last copy withdraws only
that observer. Original-mask waits share one subscription per physical namespace
and original generation, retaining weak row references and a delivery receipt.
Rows joining a delivered generation can proceed immediately; retiring a row
removes its wait without disturbing another row or namespace.

Image publications capture source generation, attempt, and mutation custody.
The compile-only source-family/image-ID join retains one compact per-image
state and stable slot; duplicate readiness cannot create duplicate pixel work.
Pixels and labels write independently. The pinned label-output lease moves to
the bounded file operation and remains alive through physical completion, then
its workspace becomes reusable. Independent image/label publication uses bounded
completion windows refilled by completion order; a slow earlier image does not
serialize ready peers. No complete-image queue or global label/run prefix
inventory is needed.
A failed body can retain useful header geometry while its image is repaired.
Old publications cannot attach to replacement generations or attempts.

[BenchmarkSplitWriter](../src/backend/data/benchmark/benchmark_writer.cpp) writes
pixels directly into stable mapped blocks and complete labels into arithmetic
slots. Late Large labels replace XL labels in the same physical slot without
reconverting healthy pixels. Final selection verifies that the selected claim
occupies that slot; a surviving claim can be replayed from canonical component
custody after a preferred claim is withdrawn. Validation work can finish while
training preparation continues. The writer seals only eligible slots, performs
the shared [same-inode compaction](datasets.md#staging-and-in-place-compaction),
then checks the persisted header/layout through the existing descriptor after
sync. Resident metadata retains inactive holes rather than renumbering images.
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

Ordinary compilation selects forward streaming extraction. A parallel gzip
backend requires its complete fixed workspace and helper-CPU grant to be
admitted; it is not multiplied by the number of source controllers. Suspending
an archive retires its decoder without allocating replacement state while
consumers retain the opened generation and their encoded bytes.

[JSON discovery](../src/backend/data/benchmark/benchmark_json.cpp) walks the
document structure once and yields bounded record ranges. Semantic parsing
retains canonical rows, source offsets, and reusable parser/mask scratch.
Open Images parses CSV rows once, with image ordering checked across chunks.
COCONut JSON retains parsed rows and physical joins across subsequent mask
work and compatible repair. Sparse mask operations use intervals/RLE; RGB ID
PNG normalization still reads the decoded pixels needed to establish support.
Production imports discover records and normalize ready masks within each
source consumer. JSON chunks and Parquet row groups can normalize before later
records are parsed; XL consumes ready record/mask pairs while discovering its
archive. PNG headers supply actual dimensions for archive normalization scratch.
Uncompressed USTAR members support direct revisits; out-of-order compressed
or context-dependent members share one ordered remainder traversal instead of
replaying their archive prefix per annotation.

Parquet binds external fields from the
[canonical declarations](../src/backend/data/benchmark/coconut/detail/coconut_parquet.h).
Independent row-group ranges share CPU admission with Arrow internal threading
and prebuffering disabled. The image-column pass retains typed metadata;
later projections read missing segments/PNGs, skip reused fields, and omit
payload reads for completely reusable groups. Each range keeps a forward
Arrow reader and physical continuation across compatible groups. Resource
pressure can close/reacquire readers without reparsing retained rows; physical
archive prefixes may need replay. A contended physical-source lock also lets
settled metadata retire its Arrow projection before waiting. Borrowed PNG spans
retain their record batch and its complete allocation pool after a reader closes.

[CoconutParquetPool](../src/backend/data/benchmark/coconut/detail/coconut_parquet_pool.h)
allocates payload from pinned windows and keeps live batch addresses stable.
The reader quiesces its allocation window before semantic consumers borrow
capacity, then reacquires a complete window before its next library call.
Recovery waits are per physical namespace/original generation; they park the
affected rows while other parsing and normalization remain ready.

[CoconutStaging](../src/backend/data/benchmark/coconut/detail/coconut_staging.h)
appends immutable segments, mask support, native objects, and deferred input
into generation-owned blocks, normally 4 MiB. A short aligned claim is separate
from file-service preparation and payload fill. Published ranges never move;
their block survives through its last borrower even after the append owner
retires. Normalized component assembly plans disjoint record/run destinations
and gathers bounded chunks on shared lanes, retaining canonical source order
and backing through completion.

Ready-input arrival determines physical slot placement. Source annotation order
has its own authority:

| Input | Canonical source order |
| --- | --- |
| Stock COCO / Objects365 JSON | Annotation-object byte offset |
| Open Images CSV | Original row byte offset |
| COCONut JSON | Annotation-array position, with segment prefixes including all declared segments |
| COCONut Parquet | Catalog shard, row group, then row; segment prefixes include stuff and omitted things |
| COCONut XL | Lexicographic physical-stem order, independently of tar order |
| COCONut component provenance | Physical image IDs within canonical component order, preserving namespaces and Large-over-XL reconciliation; incomplete extents remain explicitly marked |

### Memory, descriptors, and storage

The [shared preparation and admission rules](compile-execution.md#fixed-pinned-arenas)
own the checked normal budget, fixed pinned contexts, separate library charges,
descriptor commitments, and exclusive oversized lineage. Source readers, Arrow
batches, encoded images, and output leases retain their physical custody across
parked work. The normal bank stays pinned while borrowers change.

Benchmark source windows are bounded by actual admitted headers/rows and
independent completion windows. Reader future growth is distinct from immutable
live backing and ready-row scratch. Optional originals, component planning, and
physical output preparation share admission without one controller per worker.
Mapped/staged annotation products retain their file, mapping, and charged
custody through the last consumer. Canonical metadata and immutable retained
products can outlive transient work and exceed its normal-arena bound.

Downloads retain benchmark-specific storage estimates; the neutral
[filesystem ledger and staged-file owner](compile-execution.md#file-preparation-and-settlement)
coordinate downloads, images, caches, and split output on the same destination
filesystem. Sparse logical length, occupied page allocation, and outstanding
growth are separate facts. Completed compiled labels live in their sample slots.

## Persistent cache and publication

Source-image validation and decoding recognize JPEG or PNG from the encoded
content, including PNG payloads stored under `.jpg` archive members. The shared
[image decoder](../src/backend/data/compile_image_decoder.cpp) retains TurboJPEG
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
by annotation edition and physical namespace. Image completion proofs retain
their source identities: Coco custom uses the selected group's `.complete.json`.
COCONut uses `indexes/<source>/<shard>.images.inventory` with
`images/<source>/<shard>/.recipe-proofs/physical.inventory.complete.json`.
The latter binds the catalog artifact, admitted archive identity/size, and
known unreadable image IDs; recipe membership is selected separately from
these physical facts. Reusing pixels never substitutes stock labels for enhanced
labels.

Reuse has distinct levels:

1. An admitted Coco custom image-group completion proof resolves that selection
   without another extraction or preliminary per-JPEG validation pass. COCONut
   replays its retained physical inventory and opens/publishes healthy cached
   images independently of annotation readiness, without traversing the archive.
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

COCONut image consumers build physical inventories while publishing ready
images; a completed census is not a prerequisite for the first pixel or label
work. Annotation controllers first attempt an existing version-3 release cache
and otherwise admit independent `.part-<shard>.normalized.bin` products, then
merge component provenance when those inputs settle. Release and part caches
remain reusable across cold, warm, and repaired acquisitions. Source archives
remain reusable on disk, and healthy reuse does not hash whole payloads.

At input EOF, controllers close archive descriptors, return unused continuation
commitments, and release their strong source lease before joining downstream
work. Cache preparation first reuses a weakly retained `ArtifactLease` if that
exact live lease is still held by a controller or sibling. It reacquires the
physical cache lease only after the previous lease expires, avoiding a wait on
its own still-live ownership. The gated
[input-custody events](logging.md#benchmark-compilation-traces) distinguish reuse
from a new acquisition wait.

Normalized indexes map immutable backing. Header/layout and image extents are
checked before membership use; boxes and runs are admitted once when full
annotation consumption is required. Derived read selections retain the full
source identity and backing instead of copying payloads. Completed builder
checks and inventory serialization seals are reused by storage/publication.
An immutable row-admission proof binds the source domain, complete normalized
image record, exact box/run spans, and their backing. Native staging and cache
gather reuse that proof; changed geometry, domain, or extents cannot inherit it.
Persisted untrusted rows still pass their ordinary admission checks.
The mapping retains the opened inode after its descriptor closes, so atomic
replacement/unlink can proceed after the path-based readers settle without
holding an annotation lifecycle lock through downstream label work. Published
normalized inodes are never truncated or modified in place.

Validation, recovery, and cache-part preparation advance incrementally for ready
rows. Final membership, provenance, sync, and publication retain their required
settlement barriers. An eligible unpublished mapped staging inode can be sealed,
synced, and renamed into its first destination on the same filesystem while
existing borrowers retain the mapping. An independent destination receives its
own staged copy; publication never repurposes an already published inode.

After COCONut annotation-import failure, a file matching its pinned SHA-256 is retained.
Recovery replaces only inputs without a matching pinned checksum. If every
input already matches, bounded import admission reports the source-local
rejection and retains its healthy subset without re-downloading identical bytes.
That required checksum decision runs even with tracing disabled,
and failure to read its input remains fatal. Hashes used only for optional
failure diagnostics are lazy and trace-gated. A failed artifact's digest is
retained with its download generation rather than reread by each observer.

Repairs execute under the shared physical cache lease. They invalidate affected
proofs and corrupt artifacts/JPEGs, retain unrelated valid JPEGs, and rebuild
dependent labels at the compiler's preparation boundary after affected readers
settle. Parsed rows, compatible native products, and unaffected completed pixels
remain reusable. Changed physical or original generations withdraw only their
dependent label/pixel facts and progress; unrelated work continues. COCONut's
physical archive owner permits at most three admissions per archive across
that recovery. Exhausted source-local recovery reports unavailable or rejected
images and preserves readable peers; Coco custom retains its established
quarantine behavior. Required splits must retain healthy samples. Capacity
overflow, allocation/storage/internal failures, and cancellation remain fatal
and cannot become omissions. Reports retain the underlying failure and available
image identity. Opt-in `benchmark.images.validation_failed`
records include the archive member, source/shard, image ID, encoded byte count,
and rejection reason.

Coco custom can publish early labels while physical acquisition is still active.
If acquisition retires that image generation for repair, required final assembly
replays missing labels from the retained annotation product after the source
settles. Cancellation and failures belonging to the unchanged image generation
still propagate; retired early work cannot attach to replacement pixels.

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
explicit invalidated work. Its canonical `DatasetCompileUnit` is `Items`,
`Bytes`, `Images`, or `Rows`. The native declaration also supplies generated Rust;
the GUI and CLI format the same typed facts for their respective displays.

| Track | Benchmark units and completion |
| --- | --- |
| Acquisition | Artifact bytes transferred or admitted from cache, aggregated once per artifact; completion additionally requires the owning acquisition work to settle |
| Labels/masks | COCONut full-import rows (`Rows`), or Coco custom index/sampling milestones (`Items`); completion also requires every selected source/component label plan to settle |
| Pixels | Successfully compiled physical image slots across train and validation, retaining compatible completed work through preparation retries; final selection settles the eligible population |

COCONut's row ledger and component jobs are separate measures. Completing a
component never adds a job to the row count; the current Labels phase can report
those jobs separately. A known B release does not make the aggregate row total
known while Large or XL remains unknown. Settling a release establishes its
actual extent; invalidating an undeclared settled extent makes the aggregate
unknown again. Explicit metadata-only imports and concurrent original-index
jobs do not add rows to this ledger. Native progress validation continues to
reject completed work above a known total and incompatible active/complete
states; totals are not enlarged to conceal invalid accounting.
Source activity reports parsed image/annotation rows and resolved source images.
XL also reports annotation-archive entries before its first JSON record arrives.
Independent physical-archive consumers report encountered images and their
artifact; parser/archive observations retain their own work cursor. These
updates can advance before the first normalized row;
they do not increment the Labels/masks completion count. Normalization reports
its first completed row, then bounded batches and the exact final count.
Directory compilation marks Acquisition as unnecessary, with known zero
completed and total work; its labels and pixels each count completed images.
A completed acquisition track can coexist with
active labels and pixels. Speculative pixel counts and unknown totals need not
equal the final eligible population. Rejected annotation/image counters report
the settled omissions. All three tracks completing still leaves compaction,
validation, sync, and atomic publication to settle before the operation succeeds.

The **Compacting** phase reports eligible pixel and used-label bytes processed
by [in-place finalization](datasets.md#staging-and-in-place-compaction), including
live ranges already at their final location. It excludes unused slot padding
and is not an extra download count or a promise that every reported byte was
rewritten. Benchmark progress aggregates both splits; zero-label splits still
process their eligible pixels. Phase completion follows actual range work.

[ProgressReporter](../src/backend/data/benchmark/benchmark_progress.cpp) owns benchmark
observations and compile-scoped artifact/indexing ledgers. An observed artifact
with an unknown size makes its source and aggregate byte totals unknown; the
GUI shows `?` and omits a determinate bar for that track. Totals can change as
new artifacts are observed. At snapshot materialization, unfinished indexing,
including an unknown row total, takes priority; afterward the caption selects
active Pixels, unfinished
admitted Labels, or outstanding acquisition. A settled Indexing count therefore
does not leave that caption over ongoing pixel work. Planning, Compacting,
Syncing, and Publishing retain their lifecycle boundaries. The selected phase
describes one observed kind of work while other tracks can advance concurrently.

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
Track formatting follows the native unit: row and image counts carry `rows`
and `images`, bytes use IEC quantities, and generic items remain grouped counts.
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
uses bytes for Downloading, Extracting, and Compacting, and grouped work counts
for other phases; it remains separate from the three tracks. Known positive track totals support
bars; unknown, zero, and unnecessary work preserve the bar's space without
inventing a percentage.

Native activity and terminal results control the
[progress area's cancellation and settlement](gui-interaction.md#dataset-compilation-controls).
Its animation never decides completion. The CLI retains exact byte counts,
unknown-total text, transfer attempts, retained bytes, resume, and retry details
through [its source-status formatter](../src/backend/data/benchmark/benchmark_compiler.cpp);
native activity strings carry the operation description rather than a second
encoded copy of the transfer quantities.

Exact native counters feed the shared
[100 ms dirty-state delivery](compile-execution.md#progress-delivery). Ordinary
snapshots coalesce before copying and serialization; lifecycle/repair boundaries
and ordered publication flushes preserve their required observations. Intermediate
source activity or throttle decisions may therefore be absent from delivered
snapshots. Enabled source-activity traces retain those decisions separately.
Enabled [benchmark traces](logging.md#benchmark-compilation-traces) expose cache,
archive, transfer, and retry context independently of pixel probes. Diagnostic
records do not authorize cache reuse, readiness, completion, or publication.
An unchanged source-image count can coexist with advancing download bytes,
labels, or pixels; it alone establishes neither a deadlock nor network liveness.

## Cache formats and capacity

These formats have independent versions:

| Artifact | Current representation |
| --- | --- |
| Compiled `train.bin` / `val.bin` | [Format 10](datasets.md#compiled-binary-format): fixed 64-slot blocks, resident metadata, and measured per-sample label capacity |
| Normalized annotation index | Version 3, 256-byte header, 32-byte image records, 64-byte annotation records, and source-mask RLE |
| Download, extraction, and image-group metadata | Cache schema 2; the cache directory name remains `v1` |
| COCONut physical/component inventory | Version 1 (`CNUTIVN1`), declaration-order little-endian encoding with checked strings/counts and a SHA-256 trailer; derived components additionally bind the recovery-policy trailer |
| `benchmark_manifest.json` | Schema 3 compilation facts, including recipe, source identities, recovery selection/counts, mappings, resolution, resize/resampling policy, and cache root |

[benchmark_annotations.cpp](../src/backend/data/benchmark/benchmark_annotations.cpp)
owns normalized-index admission and staged publication. Its private
`NormalizedIndexLayout` computes the checked version-3 extents used for mapped
capacity, loading, admission, and publication. Its six
[AnnotationRejectCounts](../src/backend/data/benchmark/detail/benchmark_annotations.h)
fields generate both binary and named JSON projections from one declaration.
Compile-time format guards pin their names, `uint64` types, and declaration
order. [Cache admission](#persistent-cache-and-publication) covers parsed and
mapped products and the lifetime of their checked backing.

[coconut_annotation_identity.h](../src/backend/data/benchmark/coconut/detail/coconut_annotation_identity.h)
and its [implementation](../src/backend/data/benchmark/coconut/coconut_annotation_identity.cpp)
own the shared annotation/recovery policy, input identities, component metadata,
and physical-source split mapping. Runtime import, recovery, and cache owners
consume those facts while retaining their own resource lifetimes.

[coconut_inventory.h](../src/backend/data/benchmark/coconut/detail/coconut_inventory.h) is the
canonical inventory declaration. The explicit physical-inventory format binds
records to archive identity, namespace, shard, and canonical members. Component
inventories are admitted together with their normalized index, input identity,
edition, namespace, member joins, and normalization revision. Inventory strings are
bounded to 4,096 bytes. Their canonical encoding is sealed once for identity,
size, and publication. Incompatible or invalid cache records require rebuilding
from admitted source inputs rather than reinterpretation.

[Packed format limits](datasets.md#instances-and-masks) bound each complete
sample label payload to 500,000 usable bytes and reject unrepresentable counts,
coordinates, and extents before publication. The final header records the
largest eligible payload, including internal alignment; zero-label splits use
zero. Exact COCONut masks are never simplified/sampled to fit. Recompiling older
bins preserves reusable downloads,
images, and normalized annotations. [Bounded fixtures](validation.md#benchmark-compilation-evidence)
prove conversion/reuse, not full-release capacity at every resolution or live
download duration/liveness.
