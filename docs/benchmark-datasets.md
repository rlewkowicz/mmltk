# Built-in benchmark datasets

[Wiki index](README.md) · [Source formats and loading](datasets.md) · [Dataset controls](gui-interaction.md#dataset-compilation-controls) · [Cache selection](commands.md#benchmark-cache-selection) · [Diagnostics](logging.md#benchmark-compilation-traces)

The native compiler publishes `train.bin`, `val.bin`, and
`benchmark_manifest.json` from retained source data. Both recipes use COCO80,
the shared [resize policy](datasets.md#resize-geometry), and the current
[compiled format](datasets.md#compiled-binary-format).

## Recipes and validation membership

| Selection | Training | Validation |
| --- | --- | --- |
| Coco custom | COCO train2017 plus deterministically sampled Objects365 v2/Open Images v7 with category mappings | Stock COCO val2017 instances |
| Coconut / Coconut validation | COCONut B + Large + nonduplicate XL extensions | Relabeled COCO val2017 + COCONut Objects365 validation extension |
| Coconut / Stock validation | Same full COCONut training set | Stock COCO val2017 instances |
| Coconut / Coconut stock | Same full COCONut training set | COCONut-enhanced COCO val2017 |

[Coco custom](../src/backend/data/benchmark/benchmark_custom_recipe.cpp), its
[sampler](../src/backend/data/benchmark/benchmark_sampling.cpp),
[mappings](../src/backend/data/benchmark/benchmark_catalog.cpp), and
[Open Images acquisition](../src/backend/data/benchmark/open_images_acquisition.cpp)
own catalog, deterministic sampling, split/order, acquisition, and quarantine.
Adapters retain authoritative boxes, area, identities, crowd/raw-ignore, and
source order. COCO polygon/RLE becomes source mask support. Open Images
`IsGroupOf` becomes crowd; category MID stays separate from mapped class.
Equal boxes remain separate objects.

Coconut unions B/Large/XL, preferring Large labels for duplicate physical XL
images. It neither concatenates nested S/B/L/XL sets nor samples supplements.
Validation membership is checked separately and excluded from training. Release
inventory determines eligibility; speculative archive pixels are excluded unless
listed. Bounded repair retains healthy subsets, partial labels, and proven
zero-label backgrounds. Unreadable pixels, contradictory geometry, or wholly
corrupt required foreground exclude the sample. Missing labels never prove a
background. Both splits must remain nonempty.

The [pinned catalog](../src/backend/data/benchmark/coconut/coconut_catalog.cpp)
owns URLs, revisions, expected lengths, available SHA-256 identities, and patches:

| Release | Inputs/membership |
| --- | --- |
| `coconut_b` | Four Parquet shards; 241,602 COCO train2017/unlabeled2017 rows |
| `relabeled_coco_val` | One Parquet shard; 5,000 COCO val2017 rows |
| `coconut_large` | JSON + panoptic tar; Objects365 v2 patches 32, 35, 40, 50 |
| `coconut_xlarge` | Paired per-image JSON/PNG tar; additional patches 17, 23, 25, 28, 38, 42, 44 |
| `coconut_val` | COCO-shaped JSON + panoptic tar; separate Objects365 v1 JPEG archive |

Hugging Face addresses pin `resolve/<revision>/<file>`; validation JPEGs use
the catalog's public Google Drive endpoint. HTTP identity, responses/ranges,
and structural admission reject HTML disguised as an archive. Actual admitted
records/reconciliation determine counts, not release estimates/tar viewers.
Manifest facts retain offered/admitted counts, duplicate XL coverage, source/
annotation identities, and validation membership.

## Native import and provenance

| Source owner | Responsibility |
| --- | --- |
| [coconut_parquet.cpp](../src/backend/data/benchmark/coconut/coconut_parquet.cpp) | Bounded Arrow batches: PNG masks, `segments_info`, `image_info` |
| [coconut_import.cpp](../src/backend/data/benchmark/coconut/coconut_import.cpp) | External JSON/archive admission, retained rows, ready normalization/recovery |
| [coconut_annotations.cpp](../src/backend/data/benchmark/coconut/coconut_annotations.cpp) | Persisted components, assembly, publication |
| [coconut_physical.cpp](../src/backend/data/benchmark/coconut/coconut_physical.cpp) | Independent physical archives and identity joins |
| [coconut_native_image.cpp](../src/backend/data/benchmark/coconut/coconut_native_image.cpp) | Per-image normalization and immutable native labels |
| [coconut_recipe.cpp](../src/backend/data/benchmark/coconut/coconut_recipe.cpp) | Component selection and validation policy |

Arrow/Parquet is a [private native dependency](build.md#native-parquet-dependency);
compilation runs no Python/Hugging Face utility.

Physical identity is namespace + numeric image ID + archive/shard identity +
canonical member. Inventories separately retain release-row ID and ordinal.
For example, validation row `691105` declares `object365_file_name` resolving to
physical `objects365_v1_00091105`, JPEG under `image/`, and mask under
`panoptic_o365val_v3/`. Compiled source ID is physical; compiled slot ID is
separate. [Annotation source kinds](datasets.md#per-image-index) distinguish
COCONut provenance; Objects365 v1/v2 remain separate namespaces.

B train/unlabeled membership comes from actual JPEG members in admitted archive
generations. Recognized `coco_url` namespaces are hints, never overrides for
contradicting physical identity. Row position, numeric range, hint, or a
foreground-filtered stock index cannot establish membership. Independent image
and annotation consumers join per image with namespace/generation/ambiguity
checks; labels never initiate a JPEG-archive search. Useful work does not wait
for complete archive census or release inventory.

Large rows join by declared IDs, filenames, or full Objects365 stems; id-less
image rows use `object365_name`/`object365_file_name`. Supplied annotation release
IDs remain intact. XL pairs full stems in `panseg/` and `panseg_info/`; PNGs
supply missing dimensions. Validation joins declared physical stems independent
of tar order. All owners normalize leading `./`; consumed members reject
absolute paths, traversal, backslashes, NUL, unsupported types, and invalid
extents. Requested/already-consumed joins reject encountered identity conflicts;
unused tails need not be visited to admit requested members.

Panoptic PNGs require bounded 8-bit RGB and exact segment IDs
`R | (G << 8) | (B << 16)`, without color conversion. Record `isthing`, not numeric
category alone, determines foreground. Things use COCO80 mapping and exact
row-major support, including holes/single pixels. Supplied boxes are authoritative;
otherwise support gives exclusive upper bounds. Valid area/crowd/ignore/category/
segment identity/ordinal survive; absent/null area uses foreground count.
[Recovery](#optional-dropped-mask-recovery) can supply original COCO geometry
and update carved areas. No admitted things still produces an image record.

After selected recovery, a thing with neither support nor authoritative box is
omitted and counted as dropped. Valid peers survive; rejecting all required
foreground excludes the image, whereas a proven background remains eligible.
Declared PNG canvas may drive normalization/projection before JPEG arrival, but
the admitted JPEG header must agree before eligibility. Contradictions produce
omission, never forced label rescaling. Bounded physical repair handles unreadable
images. These rules also govern cache reuse.

Rejections append JSONL to `failed.txt` at the nearest `.cache` ancestor
(normally `.cache/failed.txt`), or custom cache root when no ancestor exists.
Records include member/image, physical/release IDs, object/category IDs, release,
and reason. The first rejection warns through progress. A compile-scoped owner
opens lazily, batches ≤64 KiB, and flushes at image/release settlement/destruction,
including unwinding. Report-write failure is nonfatal. This append-only history
never drives decisions; later success does not erase old failures. Current
recovery counts belong to [products/manifest](#recovery-derived-annotation-caches).

Canonical ordinals settle separately from pixel placement. Known late prefixes
backpatch used instance records only. An unreadable artifact with unknown extent
leaves healthy later local ordinals explicitly incomplete; never assume zero.
Completion facts retain known extent/completeness/affected IDs; normalized-v3,
inventories, `recipe.components[].incomplete_ordinal_images`, warnings, and
`failed.txt` retain this meaning. Later authoritative extents alone clear the
[compiled flag](datasets.md#instances-and-masks). No unrelated census delays work.
Final selection groups updates in ≤64 images/split per file operation, awaits
physical completion before batch reuse, and flushes partial batches before seal;
no-op changes write nothing. Masks/pixels/identities/order remain intact.

[CoconutImportLimits](../src/backend/data/benchmark/coconut/detail/coconut_input.h)
bounds PNGs to **64 MiB encoded**, **64 Mi pixels decoded**, axes **32,767**,
and **65,535 segments**, before separate compiled-payload admission.

## Optional dropped-mask recovery

**Recover dropped masks from original annotations** defaults off in
[Dataset controls](gui-interaction.md#dataset-compilation-controls). Enabling
requires recompilation; existing bins/JPEGs/PNGs remain unchanged. Recovery runs
before Stretch/Letterbox with the ordinary compiled format.

| Component | Optional original annotations |
| --- | --- |
| B physical COCO train images | Stock train2017 instances |
| Relabeled COCO validation in Coconut validation/stock | Stock val2017 instances |
| COCO unlabeled / Objects365 | None; ordinary normalization |
| Stock validation | Already uses required stock labels |

[CoconutRecoveryOriginals / CoconutMaskRecovery](../src/backend/data/benchmark/coconut/detail/coconut_mask_recovery.h)
share an immutable physical-image lookup per original generation and reusable
RLE scratch. Recovered support retains backing through its last consumer.
A dropped slot is a declared thing with zero support and no box; a boxed
present-empty mask is valid and not dropped. Namespace/image/canvas must match.
Within each category/crowd/raw-ignore group, recovery requires:

1. Valid nonempty original masks, boxes, areas, distinct annotation IDs, and
   original count equal to declared COCONut thing count.
2. Every surviving thing intersects exactly one distinct original candidate.
3. Remaining original count exactly equals dropped-slot count.

Ambiguous/invalid/incomplete groups remain unrecovered. Original annotation IDs
and COCONut segment IDs are independent: sort each remaining set and pair them.
Recovered objects keep COCONut category/flags/segment ID/ordinal and take original
mask/authoritative box/area; provenance retains both IDs.

Subtract recovered union support from all surviving unrecovered thing masks.
Retain supplied COCONut boxes, otherwise derive remaining bounds. Carved area
is remaining foreground count. Fully carved boxed objects retain present-empty
masks; unboxed ones are omitted without recursive recovery. Stuff is unchanged.

Original splits admit independently via stock annotation cache. Bounded download/
source-document failure can leave optional originals unavailable, with warning/
omission, without disabling the other split. Stock validation still requires
usable val2017. Cancellation, capacity/allocation, local I/O, staging, and
publication failures stay fatal. Shared interruption settles parked recovery
without awaiting pending originals/JPEGs and preserves the independent cause.

After originals settle, an immutable proof may reuse a staged native box/run
payload only when recovery cannot change it: no eligible dropped objects,
invalid required labels, or suppressed malformed/degenerate/unlisted reports.
Finalization still binds lineage/admission and rechecks physical/original
generations. Other rows use ordinary recovery/reporting. Compiled projection
and settlement always run, including reused native payloads.

## Overlapping acquisition, labels, and pixels

[BenchmarkCompilePipeline](../src/backend/data/benchmark/detail/benchmark_pipeline.h)
is constructed before source preparation. Source/image joins and selected claims
feed the [shared execution owner](compile-execution.md#owners-and-ready-work):
metadata, headers, pixels, normalization/recovery, labels/gather, archives, cache
writes. Source/recipe owners retain dependency and membership policy.

[BenchmarkCurl](../src/backend/data/benchmark/detail/benchmark_curl.h) admits
connections independently. Artifacts proceed when their own locks/allowances
permit; segmented/whole transfers coexist. Fresh segmentation needs expected
length ≥512 MiB, multiple connections, and nominal ranges ≥64 MiB. Valid stored
ranges can resume on one connection; response validators/ranges govern admission
and ordinary fallback. Open Images keeps one session across bounded groups,
allowing peers to progress while a group retries.

| Product | Prerequisites |
| --- | --- |
| Membership | Release inventory, admitted physical join, reconciliation/sampling |
| Geometry | Header bound to encoded generation; body decode may still fail |
| Native labels | Parsed records/exact support; eligible recovery waits only for its original split |
| Compiled labels | Owned normalized/native labels + canvas; eventual JPEG agreement |
| Pixels | Admitted encoded image + stable arrival-order slot |
| Label placement | That image's arithmetic slot, independent of other label sizes/pixels |
| Publication | Eligible populations/claims/provenance, pixels/labels, compaction, sync, manifest |

Custom annotations/acquisition overlap. Coconut releases, physical resolution,
and optional train/val originals progress independently; pending train recovery
does not block B unlabeled or unrelated native labels. Large/XL duplicate-label
reconciliation never blocks independent pixel conversion.

Each row owns an atomic snapshot/generation/readiness registration; retiring its
last copy withdraws only that observer. Original waits share one subscription
per namespace/generation with weak row references and delivery receipt. Late
joiners can use delivered readiness; row retirement leaves peers unaffected.
Image publications capture generation/attempt/mutation custody. One compact
source-family/image-ID state and stable slot prevent duplicate pixel work.

Image/label writes are independent. Label pinned leases move into bounded file
operations through physical completion. Completion-order windows refill without
waiting for a slow predecessor; no complete-image queue/global label prefix
inventory is required. Useful header geometry survives decode failure/repair;
old publications cannot attach to new generations/attempts.

[BenchmarkImagePublication](../src/backend/data/benchmark/detail/benchmark_image_publication.h)
serves generic extraction/Coconut physical membership. Each member uses actual
encoded length within a **32 MiB** pinned window; per-publisher retained inputs/
cache writes are `clamp(2 × workers,2,8)`. File workers retain bytes/callbacks;
success may transfer identical custody into pixels. Generation-qualified tickets
never borrow archive cursors. Controllers retain repair/inventory policy and
join writes/image completions before inventory seal. Separate image/output
services still share the session CPU budget.

[BenchmarkSplitWriter](../src/backend/data/benchmark/benchmark_writer.cpp)
writes stable mapped pixels/arithmetic labels. Late Large replaces XL labels in
place without reconverting pixels. Selection verifies occupying claims; surviving
canonical claims can replay after preferred withdrawal. Validation can finish
while train prepares. Seal admits eligible slots, compacts, syncs, and checks
persisted header/layout through its descriptor. Holes retain physical IDs.
Failure/cancellation joins borrowed jobs before retirement; compile-local seal
is not an on-disk trust flag.

### Archive and parser continuation

[BenchmarkArchive](../src/backend/data/benchmark/detail/benchmark_archive.h)
retains opened inode, member positions, and decoder state. Safe raw extents use
positional reads; gzip retains a bounded local dictionary/index. Known members
run in physical order under one format context. Pausing returns decoder capacity;
evicted gzip/context-dependent ZIP/extended/sparse tar may replay required prefix.
Control limits can fall back to bounded streaming. There is no persisted seek
sidecar or mandatory EOF/export pass.

Forward replay/known traversal checks cancellation per header and skips at most
**64 headers/grant**. Member reads/copies use at most **8 blocks or 1 MiB/turn**,
stop at member end, validate extents, and check cancellation per block. Arbitrary
consumer callbacks use a separate boundary because they may wait/publish.
Decoder/caller CPU grants share admission; nonpreemptible grants last through
active streams while leaving consumer capacity. Returning decoder capacity does
not release bytes held elsewhere.

Ordinary extraction streams forward. Parallel gzip requires full fixed workspace
and helper CPU admission, never one pool per source. Suspension drops decoder
state without replacement allocation while consumers retain input generation.
Record/fairness waits preserve forward state; idle suspension occurs only when
owned bytes/descriptors can relieve demand, including exceptional drain. Ordinary
arena-only pressure cannot be relieved by an unsupplied library allocation.
[Resource admission](compile-execution.md#selective-readiness-and-resource-pressure)
owns usefulness. Unaccepted input can replay from saved position on the same
inode; accepted writes keep independent bytes and finish during replay.

[JSON discovery](../src/backend/data/benchmark/benchmark_json.cpp) walks structure
once, yields bounded record ranges, and retains canonical rows/offsets/reusable
scratch. Open Images parses CSV once with ordering checked across chunks.
Coconut keeps parsed rows/physical joins across mask work/compatible repair.
RLE stays sparse; RGB ID PNGs read pixels needed for support. JSON chunks/Parquet
groups normalize before later records arrive; XL discovers and consumes ready
pairs. PNG headers determine scratch. Raw USTAR revisits directly; compressed/
context-dependent out-of-order requests share one ordered remainder traversal,
not prefix replay per annotation.

Parquet binds [canonical fields](../src/backend/data/benchmark/coconut/detail/coconut_parquet.h)
with Arrow threading/prebuffering disabled. Row groups share CPU admission.
Image projection retains typed metadata; later projection skips reused fields/
fully reusable payload groups. Forward readers/cursors survive compatible groups;
metadata/staged masks reuse pinned windows. Initial inspection establishes metadata
readiness and the cursor completion without rescanning. Oversized/failed descendant
or next-window admission can retire idle readers; reopening replays physical
prefix without republishing rows. Unrelated scratch contention preserves the
cursor; a contended source lock permits settled projection retirement. Borrowed
PNGs retain record batch/full allocation pool even after reader close.

[CoconutParquetPool](../src/backend/data/benchmark/coconut/detail/coconut_parquet_pool.h)
keeps pinned payload addresses stable. Direct semantic borrowers require reader
window quiescence/reacquisition before the next library call. Staged payload has
independent custody; exceptional source backing retires before semantic admission.
Per-namespace original waits park only affected rows.

[CoconutStaging](../src/backend/data/benchmark/coconut/detail/coconut_staging.h)
appends immutable segments/support/native/deferred data into generation blocks,
normally **4 MiB**. Short aligned claims are separate from file preparation/fill;
published ranges never move and survive append-owner retirement through borrowers.
Gather plans disjoint destinations and groups already-ready rows under **64
images, 256 boxes, 65,536 runs/task**. Empty rows count; large rows continue while
peers progress. Source order, validation, and backing persist without batch-fill
wait. Proven-empty projection is skipped, retaining importer row/duplicate facts.

Physical slots follow ready arrival; annotation order has separate authority:

| Input | Source order |
| --- | --- |
| Stock COCO/Objects365 JSON | Annotation-object byte offset |
| Open Images CSV | Original row byte offset |
| Coconut JSON | Annotation-array position; prefixes include all declared segments |
| Coconut Parquet | Catalog shard → row group → row; prefixes include stuff/omitted things |
| Coconut XL | Lexicographic physical stem, independent of tar order |
| Component provenance | Physical image IDs in canonical component order, preserving namespaces/Large-over-XL and explicitly incomplete extents |

### Memory, descriptors, and storage

[Shared admission](compile-execution.md#fixed-pinned-arenas) owns normal pinned
budget, task-sized leases, library charges, descriptors, and exclusive oversized
lineages. Readers/batches/encoded inputs/output leases retain physical custody
while parked. Source windows follow actual headers/rows and independent completion
windows; future reader growth is separate from immutable backing/ready scratch.
Optional originals/planning/output share admission without per-worker controllers.
Mapped products retain charged files/mappings through last use; immutable metadata/
products may exceed the transient arena budget.

Downloads have source-specific estimates but share the [filesystem ledger](compile-execution.md#file-preparation-and-settlement)
with images/caches/output. Sparse logical length, occupied pages, and promised
growth remain distinct. Complete labels stay in their sample slots.

## Persistent cache and publication

The [decoder](../src/backend/data/compile_image_decoder.cpp) recognizes JPEG/PNG
by content, including PNG under `.jpg`. JPEG uses TurboJPEG; PNG uses STB.
Stable `.jpg` cache names retain original bytes without recompression.
Cache survives failure/cancellation, output replacement, and recipe changes;
[selection/precedence](commands.md#benchmark-cache-selection) has one home.

Compiler staging and final destination are canonicalized against cache paths:
equality or nesting in either direction is rejected. GUI outer staging supplies
its separate final destination. Outputs validate/sync before [atomic publication](compile-execution.md#filesystem-publication).
An empty existing directory is accepted without overwrite, removed only while
empty, and protected against a new occupant. Failure/cancellation preserves prior
healthy output. GUI artifact service creates fresh staging per request, inspects
compatible splits, retires failed staging before retry, and returns published
paths. Gated failures distinguish compile/inspection/publication with original cause.

Cache layout separates `downloads/`, `images/`, `indexes/`, `locks/`. Archives/
JPEGs are shared physical data; Coconut indexes separate edition/namespace.
Custom selected-group proof is `.complete.json`. Coconut uses
`indexes/<source>/<shard>.images.inventory` plus
`images/<source>/<shard>/.recipe-proofs/physical.inventory.complete.json`, binding
artifact, admitted archive identity/size, and known unreadable IDs. These physical
proofs do not define recipe membership or substitute stock for enhanced labels.

| Reuse level | Behavior |
| --- | --- |
| Admitted image-group proof | Custom resolves selection without re-extraction/preliminary JPEG scan; Coconut replays physical inventory/publishes healthy images independently of labels without archive traversal |
| Admitted archive | No network transfer; matching completion retains HTTP identity, preseeded files still require structural admission |
| Incomplete group | Scan/retain valid JPEGs, extract missing selection; publish initial reuse even when only the completion proof is missing |

Consumers still admit headers/decode bytes. [Encoded input](../src/backend/data/benchmark/detail/benchmark_image_input.h)
retains mapped/pooled backing plus header. Header-only reuse requires matching
inode; deferred warm reads hold the inspected file during byte admission.
Path replacement cannot substitute a generation.

Physical inventories build while publishing; no complete census gates first
work. Controllers try existing normalized-v3 release caches even with modern
part caches, only when every artifact/original snapshot is immediately admissible.
Held/missing prerequisites leave independent `.part-<shard>.normalized.bin` work
in charge. Settled inputs merge provenance. Healthy reuse avoids whole-payload
hashing; archives, release caches, and parts remain reusable through repair.

At EOF, close archive descriptors, return unused continuation commitments, and
drop strong source lease before downstream joins. Weak exact-live `ArtifactLease`
reuse precedes physical reacquisition, preventing self-wait. Gated
[input custody](logging.md#benchmark-compilation-traces) distinguishes both.
`CoconutRecipeInputs` joins controllers and reconciles original/physical generations
until consistent, retaining callbacks/transport and its own publication leases;
reconciliation holds no competing lease. Unaffected rows/products survive.
Recovery providers expose train for Base, validation for relabeled validation,
and immediate terminal absence for unrelated namespaces.

Cached COCO views check original annotation identity before binding compile-local
generation. Backing/sibling views stay unchanged. Merge preserves authoritative
view generation and rejects mixed originals/policies/generations. Withdrawn labels
cannot serve replacement pixels; this binding changes no persisted format/identity.

Normalized indexes map immutable backing. Header/layout/image extents admit
membership; full consumption admits boxes/runs once. Derived selections retain
full source identity/backing without copying. Builder checks/serialization seals
are reused. Immutable row proofs bind domain, complete image record, exact spans,
and backing; changed domain/geometry/extents cannot inherit them. Untrusted rows
still admit normally. Mapping survives descriptor closure/replacement/unlink after
path readers settle, without lifecycle locks through downstream work. Published
normalized inodes are never truncated/modified.

Ready rows advance incrementally; final membership/provenance/sync/publication
retain barriers. An eligible unpublished staging inode may seal/sync/rename on
the same filesystem while mapped borrowers persist. An independent destination
gets its own staged copy; published inodes are never repurposed.

Pinned-checksum annotation inputs survive import failure. Repair replaces only
nonmatching inputs; when all match, bounded admission reports source-local
rejections and keeps healthy subsets rather than downloading identical bytes.
This decision hashes even without tracing; read failure is fatal. Diagnostic-only
hashes remain lazy/gated; failed digests retain download generation.

Repairs hold the shared physical lease, settle affected readers, invalidate
corrupt artifacts/JPEGs/dependent proofs, and retain unrelated bytes/rows/products/
pixels. Changed generations withdraw only dependent facts/progress. Coconut
permits **at most three admissions/archive**. Exhausted source-local recovery
omits unavailable images, retaining peers; Custom keeps its quarantine policy.
Both splits need healthy samples. Capacity, allocation/storage/internal failure,
and cancellation stay fatal, retaining cause/image identity. Optional
`benchmark.images.validation_failed` records member/source/shard/image/bytes/reason.

Custom early labels can be replayed from retained canonical annotation products
when repair retires their image generation; stale work cannot attach to replacement
pixels. Cancellation/failures on unchanged generations still propagate.

[CocoAnnotationCache](../src/backend/data/benchmark/detail/benchmark_annotation_cache.h)
serves Custom, Stock validation, and optional originals. Valid normalized indexes
admit before raw download, including absent archive/JSON. Rebuild missing splits
without discarding settled siblings. Each original split owns publication/
withdrawal generation; resource/storage/cancellation failures propagate without
corruption repair/redownload.

Coconut annotation identity binds release/normalization revision and artifacts;
components additionally bind namespace and all physical shards used by complete
backing. Large/XL exclude unrelated archives, while selections retain full-component
dependencies including unselected images. Older completion identities may need
rebuild without changing normalized version 3.

### Recovery-derived annotation caches

Recovery off uses base indexes. Eligible components with usable originals use
`indexes/coconut-<release>/<physical-source>.recovery-<identity>.normalized.bin`
plus `.inventory`/`.complete.json`. Identity binds base inputs, namespace, recovery
policy version **1**, and original annotation identity. Changed facts select a
new product; base/original indexes, physical proofs, downloads, and images remain
reusable. Unavailable originals use base products without a successful derived
cache and retry admission in later compilation.

[Inventory declarations](../src/backend/data/benchmark/coconut/detail/coconut_inventory.h)
derive checked recovery trailers. Images retain recovered COCONut IDs/ordinals/
categories, original IDs, and unresolved omissions. Joint index/inventory/completion
admission rejects invalid/interrupted products for rebuild.

`benchmark_manifest.json` records these current `recipe` facts:

| Field | Meaning |
| --- | --- |
| `recover_dropped_masks` | Selected option |
| `original_annotations.train`, `.validation` | When enabled, admitted original identities or `null` |
| `recovered_objects`, `unresolved_objects` | Selected recovery-eligible COCO component totals |
| `components[].recovery_policy`, `.original_annotation_identity` | Component policy/original; zero/empty for base |
| `components[].recovered_objects`, `.unresolved_objects` | Reconciled component counts |

Counts exclude historical `failed.txt` and later geometry rejection. Cached
products retain counts/replay unresolved reports. Recovery off records zeros;
on reports recovered/unresolved summary. These facts require no diagnostic logging.

## Reading compilation progress

[DatasetCompileTracks](../src/backend/data/compiler/dataset_compile_progress.h)
declares Acquisition, Labels/masks, Pixels. Each carries completed work,
known/unknown total, activity, active/completed state, invalidated work, and unit
`Items|Bytes|Images|Rows`. Generated Rust and CLI share native facts.

| Track | Benchmark accounting |
| --- | --- |
| Acquisition | Artifact transferred/cache-admitted bytes counted once; owning acquisition must settle |
| Labels/masks | Coconut full-import rows or Custom index/sampling items; selected source/component plans must settle |
| Pixels | Completed physical train/val slots; compatible work survives retries, final selection settles eligibility |

Coconut component jobs never increment row count; the Labels phase may report
them separately. Known B alone cannot establish
aggregate total while Large/XL are unknown. Settled actual release extents supply
totals; invalidating undeclared settled extents restores unknown. Metadata-only
imports/original-index jobs add no rows. Completed > known total and incompatible
active/complete facts are errors, never hidden by inflating totals.

Activity can report parsed rows/resolved images, XL archive entries before JSON,
and physical encountered images/artifact before normalization. Those cursors are
not Labels completion. Normalization reports first completion, bounded batches,
and exact final count. Directory acquisition is unnecessary known zero; labels/
pixels count images. Completed acquisition may coexist with active labels/pixels;
speculative counts need not equal final eligibility. Rejection counters reflect
settled omissions. All tracks complete still leaves compaction/sync/publication.

**Compacting** counts eligible pixel/used-label ranges processed, even already
placed ones, excluding padding; it promises neither rewriting nor downloaded
bytes. Benchmark combines both splits; zero-label splits still count pixels.
Completion follows actual range work.

[ProgressReporter](../src/backend/data/benchmark/benchmark_progress.cpp) aggregates
artifact/indexing ledgers. Unknown artifact length makes source/aggregate bytes
unknown; new observations can change totals. At materialization, unfinished
indexing (including unknown rows) takes precedence, then active Pixels, unfinished
admitted Labels, outstanding acquisition. Planning/Compacting/Syncing/Publishing
keep lifecycle boundaries. A phase caption selects one activity while tracks
advance independently.

Sources retain actual bytes, resolved/selected images, retry, cache, resume, and
invalidated-image facts. Resolved includes permitted quarantine; cache writes
count after physical settlement. Projected output bytes are admission estimates,
not writes. Optional `BenchmarkTransferProgress` describes only the latest
artifact's completed/total/retained bytes, attempt/reuse/resume. Zero total means
unknown; retained ≤ completed ≤ known total. Activity change/completion clears
latest transfer, not aggregate source history or every concurrent transfer.

Transfer bytes mean actually written work, separating retained resume bytes.
Segmented counters add in-flight writes to durable ranges; preallocated `.part`
length is not progress. `.part.json` stores identity/ranges. Rejected ranges/
discarded attempts may roll back counters. Ledgers replace contributions instead
of counting retries twice; repair withdraws only affected completed work and
records invalidation. Unknown length stays open until exact size is established.

The [GUI formatter](../src/frontend/iced/src/view/train/dataset/progress.rs) uses
IEC byte units (≥1 KiB: one decimal; smaller nonzero: three; zero: `0 KiB`),
integer-grouped rows/images/items, and `?` for unknown. Matching completed totals
appear once. Positive known totals support bars; unknown/zero/unnecessary work
preserves bar space without percentages. Directory reads **No acquisition needed ·
0 KiB**. Headings are Cached/Complete/Active/Waiting. Active rows retain current
activity/transfer details; completion drops obsolete transfer but retains aggregate
retry/resume/repair. Planning never implies known empty work. Downloading,
Extracting, Compacting captions use bytes; other captions use grouped counts.

[GUI cancellation/settlement](gui-interaction.md#dataset-compilation-controls)
follows native activity/terminal results, never animation. CLI retains exact
bytes, unknown totals, attempts/retained/resume/retry through its
[formatter](../src/backend/data/benchmark/benchmark_compiler.cpp); activity text
does not duplicate encoded transfer quantities.

[100 ms dirty delivery](compile-execution.md#progress-delivery) coalesces ordinary
snapshots before copying/serialization; lifecycle/repair/flush boundaries preserve
ordering. Intermediate activity/throttle decisions may be absent; enabled source
traces retain them independently of pixel probes. Diagnostics authorize no cache
reuse/readiness/completion/publication. Unchanged image counts alone prove
neither deadlock nor network liveness.

## Cache formats and capacity

| Artifact | Current representation |
| --- | --- |
| Compiled splits | [Writer format 11; reader 10/11](datasets.md#compiled-binary-format), fixed 64-slot blocks and measured label capacity |
| Normalized index | Version 3: 256-byte header, 32-byte image/64-byte annotation records, source RLE |
| Download/extraction/group metadata | Schema 2; directory name remains `v1` |
| Coconut inventories | Version 1 (`CNUTIVN1`), declaration-order little-endian checked encoding + SHA-256 trailer; recovery products add policy trailer |
| Manifest | Schema 3: recipe/source/recovery/mappings/resolution/resize/cache facts |

[benchmark_annotations.cpp](../src/backend/data/benchmark/benchmark_annotations.cpp)
owns checked normalized extents/admission/publication. Six canonical
[AnnotationRejectCounts](../src/backend/data/benchmark/detail/benchmark_annotations.h)
fields derive binary/named JSON; guards pin names, `uint64`, and order.
[Coconut annotation identity](../src/backend/data/benchmark/coconut/detail/coconut_annotation_identity.h)
owns policy/input/component/physical-split facts; runtime owners retain lifetimes.

[Inventory declarations](../src/backend/data/benchmark/coconut/detail/coconut_inventory.h)
bind physical archive identity/namespace/shard/member and joint component index/
input/edition/normalization. Strings are ≤4,096 bytes. Encoding seals once for
identity/size/publication. Invalid/incompatible caches rebuild from admitted
inputs, never reinterpret.

[Packed limits](datasets.md#instances-and-masks) and the smaller
[staging budget](datasets.md#staging-and-in-place-compaction) admit exact masks,
counts/coordinates/extents before publication. Final maximum uses eligible complete
payloads, including alignment; all-background uses zero. Recompilation preserves
reusable source caches. [Bounded functional tests](validation.md#benchmark-compilation-evidence)
establish conversion/reuse, not every full-release resolution's capacity or live
network duration. [Measurement procedures](benchmark-performance.md) keep
production-sized work explicit.
