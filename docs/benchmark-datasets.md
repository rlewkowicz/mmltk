# Built-in benchmark datasets

[Wiki index](README.md) · [Source formats and loading](datasets.md) · [Dataset controls](gui-interaction.md#dataset-compilation-controls) · [Commands](commands.md#benchmark-cache-selection) · [Diagnostics](logging.md#benchmark-compilation-traces)

The native benchmark compiler prepares `train.bin`, `val.bin`, and
`benchmark_manifest.json` from retained source data. Both recipes use the
existing COCO80 foreground catalog, [resize geometry](datasets.md#resize-geometry),
and [compiled format 8](datasets.md#compiled-binary-format).

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
Every offered image and required mask must be available after bounded recovery
for publication to succeed.

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
Large's heterogeneous rows join through declared filenames and image records.
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
and ordinal remain intact; absent/null area comes from mask support.
An image with no admitted things still has an image record.

A declared thing with neither mask pixels nor an authoritative box is omitted
from normalized object metadata and counted as a dropped instance; its image
and other objects remain. Compilation appends the image/member, physical and
release image IDs, object/category IDs, release, and reason to `failed.txt` in
the nearest `.cache` ancestor of the benchmark cache (normally
`.cache/failed.txt`). A custom cache outside `.cache` keeps the report at its
own root. The report uses one JSON object per line and retains earlier entries.
The first rejection produces a concise progress warning; report-write failure
does not interrupt compilation.

Current import admission bounds each encoded PNG to 64 MiB, each decoded image
to 64 Mi pixels, each axis to 32,767, and each segment list to 65,535 entries.
The canonical limits are in
[CoconutImportLimits](../src/backend/data/detail/coconut_annotations.h).
These precede the separate compiled-format capacity checks below.

## Persistent cache and publication

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
workers settle. COCONut's physical archive owner permits at most three structural
admissions per archive across that recovery. COCONut never converts exhausted
recovery into silent image loss; Coco custom retains its established quarantine
behavior.

[CocoAnnotationCache](../src/backend/data/detail/benchmark_annotation_cache.h)
is the shared stock-annotation admission owner for Coco custom and Coconut's
Stock validation. It discovers valid normalized indexes before requesting raw
annotations, so a valid stock index remains usable with the raw archive/JSON
absent. A missing split can be rebuilt while an already settled split is
retained. Typed storage-capacity failures and cancellation propagate directly;
they do not trigger corruption repair or needless re-downloads.
[benchmark_storage.cpp](../src/backend/data/benchmark_storage.cpp) owns capacity
checks and concurrent reservations.

## Reading compilation progress

[ProgressReporter](../src/backend/data/benchmark_progress.cpp) owns coherent
phase, current-source, byte, image, and retry observations. Current source means
the latest observed work during concurrent acquisition; it is not an exclusive
scheduler owner. The artifact adapter preserves explicit global activity when
no current source is selected. The GUI renders those facts through its existing
artifact progress view.

| Fact | Meaning |
| --- | --- |
| Downloading completed/total | Aggregated artifact bytes for the acquisition scope; any observed unknown-size contribution keeps the total unknown |
| Extracting completed/total | Each participating source with a known denominator or completion contributes one million units; resolved-image counts take precedence over byte counts |
| Current activity | Source, archive/artifact, operation, actual bytes and known total or explicit unknown-total text, with attempt/resume/re-download context |
| Source image counts | Resolved selected images, including permitted quarantine outcomes; successful cache writes report only after physical writes settle |
| Projected output bytes | Planned compiled-output upper bound used for storage admission, not bytes already written |
| Pixels completed/total | Images compiled in the current output attempt; attempt changes retain explicit restart context |

For example, completed COCO and Open Images sources plus 170,161 resolved
Objects365 images out of 408,551 produce
`1000000 + floor(170161 * 1000000 / 408551) + 1000000 = 2416498`
out of `3000000`. This plateau identifies remaining Objects365 acquisition.
It alone establishes neither deadlock nor external network liveness; archive
byte progress can advance while the resolved-image fraction is unchanged.

Fresh, resumed, retried, and re-downloaded transfers report bytes actually
written. A resumed transfer distinguishes retained bytes from its new work.
Segmented downloads add in-flight written bytes to durable completed ranges;
the preallocated `.part` file length is not a transfer counter. `.part.json`
retains resume identity and range progress. Range rejection or discarded
attempt work can deliberately roll progress back. Source contributions are
replaced rather than added twice, and extraction retry withdrawal is explicit.
Unknown totals remain open-ended until the downloader establishes an exact size.

Transfer and scan observations use existing bounded observation points.
Enabled [benchmark traces](logging.md#benchmark-compilation-traces) expose cache,
archive, transfer, and retry context independently of pixel probes. Diagnostic
records do not authorize cache reuse, completion, or publication.

## Cache formats and capacity

These formats have independent versions:

| Artifact | Current representation |
| --- | --- |
| Compiled `train.bin` / `val.bin` | [Format 8](datasets.md#compiled-binary-format), with the same packed layout and additional admitted source-kind values |
| Normalized annotation index | Version 3, 256-byte header, 32-byte image records, 64-byte annotation records, and source-mask RLE |
| Download, extraction, and image-group metadata | Cache schema 2; the cache directory name remains `v1` |
| COCONut physical/component inventory | Version 1 (`CNUTIVN1`), declaration-order little-endian encoding with checked strings/counts and a SHA-256 trailer |
| `benchmark_manifest.json` | Schema 3 compilation facts, including recipe, source identities, mappings, resolution, resize/resampling policy, and cache root |

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

The [packed format limits](datasets.md#instances-and-masks) still apply. The
compiler rejects unrepresentable offsets, counts, coordinates, or extents
before publishing output. Exact COCONut masks
are not simplified or sampled to fit those limits. Bounded local fixtures
establish the documented conversion/reuse behavior; they do not establish
full production-release capacity at every resolution or the duration/liveness
of live downloads. [Validation ownership](validation.md#benchmark-compilation-evidence)
locates those cases.
