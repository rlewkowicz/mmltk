# Benchmark compiler performance investigation

Baseline code: `333032e7`; documentation: `21bdcadc`.

The largest measured remaining elapsed pass is final train compaction: **922.091 seconds, 33.0% of the latest run**. Pixel tasks account for **95.8% of measured task CPU time**. Compute-worker creation is constant at 24. Source review identifies removable scheduling and diagnostic work and unnecessary archive retirement; these measurements do not establish repeated worker creation.

The five selected changes are below. Execution and diagnostic demand come first
in implementation so the other owners share one inexpensive observation path.
Each candidate received an independent Astra max source review across its
callers, resource ownership, failure paths, and measurement alternatives.

| Priority | Change | Aggregate evidence and expected effect |
| ---: | --- | --- |
| 1 | Batch ordered compaction and staged range mutations | 922.091 s on the terminal train path. Use the complete scratch window and amortize sparse allocation/reclamation accounting. |
| 2 | Remove excluded diagnostics and repeated scheduler work | 13.919 million probes, 2.704 million readiness deliveries, 1.735 million record denials. Gate collection upstream, coalesce wakes, reuse record links, and bound controller turns. |
| 3 | Reduce exact CPU quantization and scratch initialization | 8,897.272 cumulative Pixel CPU s. Replace dependent transfer searches with an exact derived lookup, avoid overwrite-only clears, and use full-vector stores. |
| 4 | Retain useful archive state and batch bounded reads/copies | Cold extraction logged repeated decoder restoration; its replay CPU is unmeasured. Retire only custody that can relieve pressure and amortize block-task handoffs. |
| 5 | Reuse final annotation results and batch ready rows | 357,533 Gather bodies and source-proven cache/recovery rework. Reuse immediately available generation-valid caches, avoid proven-empty work, and group small ready rows. |

These are selected mechanisms with correctness and measurement gates. Their
positive elapsed savings have not yet been measured, and overlapping task costs
cannot be added to the separate compaction interval.

## Measurement boundary

Both complete runs used Coconut, Stock validation, dropped-mask recovery, 384×384 Stretch/perceptual, 24 workers on CPUs 0–23, zero warmups, and one repetition. Each published 584,636 train and 5,000 validation images. The only manifest difference is Objects365 patch-32's cache-hit flag (false then true); all source/annotation identities, provenance, box/run counts, and final lengths match. This does not prove byte-identical bin ordering.

These are explicit production-sized profiles, separate from normal tests. Trace filtering retained upstream observation costs. Source and OS cache conditions were not controlled, so the pair does not isolate an end-to-end speedup. Source query exports and exact fields are in `build/validation/benchmark-hotspot-evidence.json`.

| Elapsed boundary | Previous selective run (s) | Latest batched run (s) |
| --- | ---: | ---: |
| Whole compile | 2,766.101 | 2,792.702 |
| Production plus final source joining, before selection | 1,726.164 | 1,842.007 |
| Final selection | 69.108 | 23.131 |
| Train compaction | 965.085 | 922.091 |
| Validation compaction (overlaps train) | 7.974 | 8.837 |
| Train sync | 1.474 | 1.392 |

Selection now settles 239,047 images in 3,736 bounded batches. The observed stage reduction is 45.978 seconds; whole-run time was 26.601 seconds longer. Neither number isolates one code change under these cache conditions.

## Aggregate task costs

The following are latest-run cumulative worker values, not elapsed phase times. Bodies, queue waits, callbacks and lock waits can overlap; do not sum them into an end-to-end budget. File-service I/O is not accounted for in the zero-valued Output task-kind row.

| Kind | Calls | Body wall sum (s) | CPU sum (s) | Ready wait sum (s) |
| --- | ---: | ---: | ---: | ---: |
| Metadata | 206,592 | 4.697 | 3.861 | 7.334 |
| Header | 1,253,216 | 1,278.385 | 106.588 | 78.184 |
| Projection | 235,031 | 10.442 | 10.247 | 6.403 |
| Archive | 103,013 | 0.993 | 0.967 | 1.7 |
| Normalize | 483,204 | 625.531 | 197.04 | 115.71 |
| Recovery | 483,204 | 57.902 | 13.87 | 59.802 |
| Labels | 833,695 | 761.41 | 51.902 | 918.903 |
| Pixels | 626,608 | 10,738.749 | 8,897.272 | 68.569 |
| Gather | 357,533 | 6.344 | 3.962 | 45.117 |

There were 4,582,096 task-body executions but 4,098,892 admitted/recycled records; retained continuations reuse records. The final snapshot has no active tasks, records, descriptors, or live arena allocations. It records 24 created/retired compute workers, fixed 6 GiB normal pinned backing, allocation high-water 313 and no exceptional peak. These are compiler arena facts, not process peak RSS.

The pixel CPU sum divided by the 1,842.007-second production interval is
4.83 core-equivalents. All recorded task CPU totals 9,285.707 seconds, or
5.04 core-equivalents over that interval. These are task-body accounting
ratios, not whole-process CPU utilization. Scheduler, callback, and file-service
CPU are not fully attributed by this table. The pixel percentage does not
establish saturation of the 24-worker pool or a proportional elapsed-time gain
from a faster kernel.

## Repeated control work and discarded products

Latest totals: 5,869,294 submission attempts, 1,734,731 record denials, 13,918,999 eligibility probes, and 2,703,834 readiness callback deliveries. Recorded admission-lock wait is 291.306 cumulative seconds; readiness callback wall time is 1,669.852 seconds and completion callback wall time 2,842.254 seconds. These overlapping wall sums establish candidate work, not recoverable elapsed time or CPU attribution. Prefix filtering does not remove upstream snapshots/JSON construction.

Both runs produced 626,608 pixel images but selected 589,636. The 36,972 discarded images are 5.90% of produced images and represent 65,420,918,784 logical RGB-float bytes at 384×384. Their measured compute/physical-write contribution is not separately attributed. The latest 833,695 Labels and 483,204 Recovery calls warrant tracing whether repeated work is necessary for generation-safe early progress.

Final compaction visits about 1.048 TB of live payload, then publishes 1.191 TB of logical file lengths. Logical size and visited bytes are not physical disk traffic. Sparse slots, allocation, and reclamation are relevant. Btrfs mount compression alone does not establish compressed compiled extents, because the writer preallocates used ranges.

## Streaming and overlap

During latest Indexing, no sampled roughly five-second interval had zero new pixel completions; median sampled rate was 355.98 images/s (p10 176.47, p90 462.86). These coarse samples cannot rule out short stalls or establish per-image p99 gaps. The previous run had one 5.467-second zero-pixel interval while labels advanced. Production continued through long recovery and physical-join spans.

The latest four RecoveryDrain spans sum to 5,386.128 seconds and eleven PhysicalWait spans sum to 2,246.604 seconds, overlapping the same 1,842-second production interval. ComponentGather sums to only 19.879 seconds and InventorySeal to 0.350 seconds, with possible nesting. Removing a long named wait cannot recover that duration unless its producer or terminal dependency becomes earlier.

## Viable work: final compaction

The shared compactor is the first elapsed-time target. Both Directory and benchmark
compilation already use it; extending that owner avoids a separate benchmark
implementation.

At 384×384, the current mover uses 1,769,472 bytes of its admitted 4 MiB scratch.
A full 64-slot pixel run is 108 MiB. Using the full scratch window reduces the
derived number of pixel-copy iterations by approximately 50–58%, depending on
inactive-slot fragmentation. It does not reduce the live pixel bytes copied.
Labels account for less than 0.5% of live train payload but cause many separate
gap punches, source punches, and storage-accounting transactions.

Implement bounded ordered range processing:

- Keep pixel-run coalescing and use the full admitted scratch. Capture every
  chunk completely before an overlapping forward write.
- Capture up to one block's label prefixes in a scratch-bounded window, then
  batch page-covered destination allocation and consumed-source reclamation.
  Retain separate writes for nonadjacent live prefixes.
- Add shared range-span operations to `StagedArtifact`, with one capacity
  admission and reconciliation per mutation batch. Single-range callers use
  the same implementation. Absent occupied-map entries are not proof of holes.
- Treat batch size as a maximum. On nonmutating capacity rejection, split down
  to the prior safe operation size; retain source bytes and conservative promises
  through partial failure. Cancellation remains independent of progress.
- Emit one optional reflected aggregate per split for moved/stationary bytes,
  read/write calls and bytes, allocation/reclamation spans and durations,
  truncation, and scratch high-water. Disabled observation collects nothing.

The ownership boundary is
[the shared compactor](../src/backend/data/compiled/compiled_file_compaction.cpp),
[staged inode mutation](../src/backend/data/staged_artifact.cpp), and
[filesystem capacity accounting](../src/backend/data/storage_reservation.cpp).
Directory and benchmark writers retain final metadata writes, sync, failure
joins, and atomic publication. Format 10, physical slot identity, sparse padding,
eligible-only label maxima, and exact content remain unchanged.

A bounded production-compactor profile should compare identical staged contents
at 1,024 and 4,096 slots, with dense/sparse pixels, mixed labels, overlap, and
tight storage admission. Include final sync in the comparison so delayed work
cannot masquerade as a saving. This is roughly 1.8–7.3 GB of live pixels, not
a full Coconut compile. Existing storage/publication cases provide correctness
and failure coverage.

Same-inode reflink is a separate candidate within this owner. It could avoid
buffered copying for aligned, disjoint live pixel chunks, but requires verified
filesystem support, conservative shared-extent accounting, meaningful error
propagation, and matched-input measurements. It is not a measured improvement.
Changing the compiled format, waiting for all labels before producing pixels,
allocating sparse holes, or removing durability are excluded.

The current Btrfs mount's compression setting does not identify compiled extent
compression: preallocation affects that behavior. [Btrfs compression exceptions](https://btrfs.readthedocs.io/en/latest/Compression.html#exceptions).
The existing stack captured one hole-punch call; it does not attribute a
percentage of compaction time to that syscall.

| Conditional reduction of train compaction | Saved elapsed time | Whole-run multiplier if every other stage stays equal |
| ---: | ---: | ---: |
| 10% | 92.2 s | 1.034× |
| 25% | 230.5 s | 1.090× |
| 50% | 461.0 s | 1.198× |
| 100%, theoretical ceiling | 922.1 s | 1.493× |

There is no measured positive speedup bound yet. Detailed source anchors,
alternatives, and required cases are preserved in the local
[compaction review](../build/validation/hotspot-compaction-review.md).

## Viable work: exact CPU pixel processing

The shared CPU resampler already uses AVX2/FMA, persistent worker-local controls,
and direct perceptual planar output. The next changes target work that remains
inside that implementation.

1. Derive an immutable inverse lookup from the existing Float32 transfer
   thresholds. A 4,097-entry table for 4,096 power-of-two buckets gives an exact
   base quantization code; one canonical-threshold comparison corrects it.
   Prove the one-correction bound from the actual table, and retain the scalar
   search as the authority. This replaces eight dependent SIMD gathers with two,
   a 75% reduction in that instruction count, with unmeasured runtime benefit.
   The straightforward table is 16,388 bytes, retained once or in bounded worker
   controls.
2. Use the existing overwrite-sizing operation for perceptual axes and state
   rows that are fully assigned before use. The supplied 384² RGB scratch is
   49,152 bytes; the current bind/resize sequence requests an unnecessary clear.
   This does not permit removing mathematical accumulator initialization.
3. Use ordinary unaligned vector stores for complete eight-lane blocks and
   keep masked tails. Consider packed RGB lane construction and hoisted
   fractional control only if exact output and matched-input profiles improve.
   Packed RGB loads must read exactly three valid bytes; a convenient
   four-byte gather would overread tight final pixels.
4. Correct the profiling runner's decoded-RGB admission before PNG measurements.
   It currently charges the JPEG-specific separate output allocation, which is
   zero in STB PNG plans. Expose the decoder's checked actual RGB extent,
   preflight retained bytes before allocating/decoding, and retain distinct
   decoded-image, decoder-workspace, and compiled-destination accounting.

A one-image probe against the unchanged source confirms the PNG accounting
defect: a retained 768×768 RGB image requires 1,769,472 bytes, but the runner
reports `retained_decoded_bytes=0`. The cached profile target needed no rebuild
and exited successfully. The [query export](../build/validation/hotspot-png-cap-probe-query.jsonl)
records the input and result. This establishes profiler accounting behavior;
it is not a reproduced production allocation failure or a throughput comparison.

The owners are [CPU perceptual downscaling](../src/backend/imaging/resample/detail/perceptual_downscale.cpp),
[its numerical authority](../src/backend/imaging/resample/detail/perceptual_downscale_math.h),
[shared image decoding](../src/backend/data/compile_image_decoder.cpp), and
[the existing profiler](../src/backend/data/tests/profile_runner.cpp).
No decoder policy, filter, pixel format, or compiled quantization changes.

Exactness covers every transfer threshold and bucket boundary with adjacent
Float32 values, signed zero, subnormals, NaN/infinities, low-variance behavior,
FMA and compensated traversal, tight/padded RGB/RGBA/float views, lane tails,
poisoned scratch reuse, and failure before destination writes. Existing scalar
and image oracles remain independent of the derived lookup.

The bounded retained-JPEG sample has 432 fractional-perceptual and 80 AVIR
images: 1.063 cumulative seconds of decode and 4.129 seconds of resize/store.
These overlap across workers and are sample-local. The last untraced Pixels
profile ran 256 images in 0.121570 seconds; it excludes file output and full
recipe control. Compare the new code against `333032e7`, at one and 24 workers,
using matched Pixels/Resample inputs and a small Directory compile for mapped
output. Keep setup and trace-enabled causal measurements separate.

| Other feasible mechanism | Evidence needed before selecting it |
| --- | --- |
| Persistent AVIR fixed coefficients or bounded geometry reuse | Attribute fixed-bank/geometry setup; separate immutable coefficients from arena-bound mutable scratch and admit retained storage. The sample's entire AVIR resize is only 9.01% of its pixel-body wall sum. |
| AVIR row output sink or tiled intermediate | Preserve its exact byte quantization, border behavior, preparation failures, and application-owned planar conversion policy; measure removed traffic against added control. |
| STB tail allocation growth and tighter PNG/JPEG plans | Count real copied bytes and codec/bit-depth/interlace/progression frequencies; prove peak workspace bounds and preserve malformed-input behavior and no heap fallback. |
| Coefficient/reconstruction fusion or transformed-color row reuse | Attribute cache/compute costs, preserve per-cell arithmetic order, and bound scratch; register spills and extra state can erase savings. |
| Non-temporal mapped stores, prefetch, or wider SIMD | Measure real mapped destinations and supported CPU instructions, including completion fences, tails, frequency effects, and cache contention. |
| GPU batching or streaming decode/resample | Establish full byte parity, validated source completion, admitted staging, transfer/retirement costs, and throughput against the existing 24-worker CPU path. |

There is no GPU compiler today. The augmentation CUDA implementation uses
different threshold preparation and large-footprint reduction order, so its
tolerance tests do not establish exact compiled-byte parity. For a 640×480 RGB8
source, a GPU round trip transfers about 2.691 MB/image if it returns planar
floats, or 1.364 MB/image if it returns quantized RGB8 for CPU conversion.
Those are payload calculations, not measured PCIe performance. GPU availability
alone is insufficient reason to add another execution path.

CPU batching should only amortize already-ready work. The existing 384-wide
SIMD lanes are full; waiting for a batch or geometry match would add latency.
Detailed alternatives, source anchors, and tests are in the local
[pixel review](../build/validation/hotspot-pixels-review.md).

## Viable work: scheduling and diagnostic demand

The strongest control-path change is one cutover inside the existing execution
and trace owners. A filtered trace currently builds payloads before rejection.
For the public Coconut adapter this includes JSON construction, serialization,
a trace mutex, parsing, and only then prefix filtering. Native execution also
collects live snapshots and diagnostic credit ancestry when only a final
summary was requested. Some of that work runs while callers hold their facts
mutexes.

Give trace values one immutable selection policy plus delivery, propagated
through public, private, and enriching adapters. Gate field builders,
source-file digests, snapshots, and clocks at their owning producer. Separate
aggregate collection from detailed emission: selecting
`compile.execution.retired` must retain complete cumulative counters without
collecting transient ancestry or formatting every rejected admission. Completed
span selection still requires its duration even when its start is excluded.
Default full traces preserve their requested events and failure semantics.

The scheduler itself has three concrete opportunities:

- Coalesce eventfd writes for an already-pending notifier turn. Under the
  admission mutex, detach the current causes and clear the armed state before
  invoking callbacks. Arrivals during callback delivery must arm the next turn.
  Interruption, withdrawal, shutdown, local retry, and subscription destruction
  retain their existing guarantees.
- Replace active/ready list-node allocation and linear removal with reusable
  links in the fixed task-record bank. Preserve independent active/ready
  membership, class ordering, bypass cursors, and physical completion custody.
  Completion state must remain fresh for each physical step, because callers
  can retain old handles after a record is reused.
- Bound each pipeline dispatch turn by useful work and probes, retaining its
  stage and denied-workspace cursors. Queue one immediate local wake for
  genuinely unexamined work, then let other owners receive callbacks. A fully
  denied finite pass must park; it must not restart itself indefinitely.
  Arm resource/record interests before testing admission, so narrowing demand
  cannot miss the only capacity return.

These belong in [CompileExecution](../src/backend/data/compile_execution.cpp),
[the benchmark trace boundary](../src/backend/data/benchmark/detail/benchmark_trace.h),
[the public compiler adapter](../src/backend/data/benchmark/benchmark_compiler.cpp),
and [pipeline dispatch](../src/backend/data/benchmark/benchmark_pipeline.cpp).
The selection value should be independent of product types, JSON, and resource
state. Existing reflected observation declarations own additional counters;
diagnostic identities remain observational.

The source review also identified ready-row gather batching and empty
Projection admissions. Those belong in the annotation/import owners and are
coordinated with the rework review below. They do not justify generic executor
auto-batching across different resource or failure policies.

| Further opportunity | Decision boundary |
| --- | --- |
| Queue-local capacity subscriptions | Returning one file queue's slot currently broadcasts global Records. A complete scoped cutover must signal after actual slot return and safely detach before the consumer retires. Removing the broadcast alone can lose progress. |
| Record-only waits and dirty-progress wake coalescing | Use the existing readiness and progress owners; preserve predicate-before-wait races, exact terminal facts, flush sequences, and failure wakes. |
| Worker wake targeting, class-ready masks, or fewer lock sections | Measure empty selections and lock CPU after the first cutover. Preserve class floors and keep bodies, callbacks, capture destruction, and resource retirement outside admission locks. |
| Aged-credit and physical-service bookkeeping | A faster scan must preserve alias/custody changes that can occur without a resource epoch. Affinity caching must account for nested borrowed dispatch. |
| Extra I/O workers, work stealing, or idle-floor lending | Current evidence does not justify changing these policies. Increasing retained work or delaying newly live classes can conceal the problem and weaken progress guarantees. |

A bounded `Pipeline` mode in the existing profiler should exercise production
source publication, image and label work, both file queues, and mapped output
using 256–1,024 local synthetic images. Compare trace-off, selected-summary, and
full-detail capture separately. Existing Compile/Pixels modes are useful
controls but do not exercise this complete benchmark control path.

Record snapshots built, eventfd writes, notifier batches/deliveries, submissions,
denials, probes per turn, empty turns, completion spacing, and retirement only
when requested. Preserve all samples and input hashes. Explicit-gate tests must
cover arrivals during notification acknowledgement/delivery, stale completion
handles, cancellation, fitting work behind an oversized blocked item, and a
continuously replenished source beside a held peer. No timing-sensitive
throughput assertion or larger dataset is needed for those correctness cases.

No positive elapsed saving is yet measured. The report establishes removable
operations and their owners; it does not convert overlapping callback or lock
sums into wall-time gains. Full detail and exact source anchors are in the local
[scheduling review](../build/validation/hotspot-scheduling-review.md).

## Viable work: input and archive scheduling

The warm input path validates each encoded source, then prepares the consumer's
decode/resample plan from the same retained mapping. Its 1,253,216 Header calls
do not represent two full reads of 626,608 images. Full preload makes one
mapped-to-pinned copy on persistent image services. A cold archive can instead
transfer its existing admitted encoded buffer directly to the pixel consumer.
Preserve both paths and the actual-byte verification before destination writes.

At 24 compute workers, the image file queue has four records and two persistent
services; the output queue has 48 records and two services. They borrow the same
CPU budget. The latest Header bodies total 1,278.385 seconds, but their measured
CPU is 106.588 seconds. The difference is an overlapping off-CPU sum, potentially
including faults, descheduling, locks, and diagnostics. It is not measured disk
wait. Destination allocation also does not prefault every writable page; page
faults and dirty throttling can occur inside pixel stores.

The strongest input change concerns cold archive reuse. A serial decoder retains
a 4 MiB heap/library charge, plus a descriptor for gzip. It does not occupy a
pinned arena slice. Current generic resource pressure can retire it for ordinary
arena fragmentation even when returning that custody cannot help. Restarting a
serial compressed archive can replay its prefix. The earlier cold patch-32
capture has 34 pressure events and 60 decoder-admission events, but no direct
replayed-byte or replay-CPU attribution.

Make retirement usefulness an operation of the existing execution/admission
owner, evaluated from actual blocked requests and candidate allowances:

- Retain an unsupplied decoder during ordinary arena-only pressure when aggregate
  bytes and descriptors have headroom.
- Reclaim matching byte or descriptor custody even when multiple owners must
  cooperate. One victim need not make the whole request fit by itself.
- Preserve mandatory exclusive/oversized drain, aged-request priority,
  cancellation, explicit pause, EOF, and physical retirement before release.
- Wake pressure subscribers when a newly blocked request changes the useful
  victims, including while the aggregate pressure flag stays true. Unknown
  external waits remain conservative; known failures carry their actual request.

The archive remains the decoder owner. No protected pool, extra eviction manager,
or timing-based retention policy is needed. The exceptional input path's
post-restoration decoder retirement is a separate custody guarantee and remains
required; removing that line alone would be unsafe.

The second selected change batches bounded archive block reads and copies under
one existing CPU grant. Today each data block, including the EOF probe, submits
and joins a task; its copy happens outside that measured body. Start with at most
eight blocks or 1 MiB per turn, stop at the member boundary, and check cancellation
per block. Share the current extent/bounds validator between the APIs. Arbitrary
consumer callbacks retain their present execution boundary because they can wait
for resources or publish files. Existing 64-header skip batching already exists.

Use a 32–64-member generated archive for correctness and at most 64 MiB for a
causal profile. Exercise unrelated arena pressure, necessary byte/descriptor
pressure, multiple victims, a useful request arriving during existing pressure,
and exceptional drain. Observe decoder opens/restores, forward versus replayed
bytes, copy bytes, CPU, and maximum turn work only when requested. Require fewer
unnecessary restores or handoffs while competing work continues to progress.

Other input changes need the single bounded Pipeline profile's attribution:

| Candidate | Required evidence before a production change |
| --- | --- |
| Limit excessive pre-Pixels look-ahead | Measure retained bytes and residence by Input/Output stage. The observed 165 parked continuations also include labels/native staging. Derive any limit from existing service capacity, preserve useful running/ready pixels and fitting-tail bypass, and avoid another queue owner. |
| Header service preparation or page advice | Show source faults dominate; operate on the same opened, generation-bound input with bounded admitted work. Avoid a full preload just to inspect a header. |
| Destination prefault or different mapped writes | Show Output/Pixel faults dominate and improve end-to-end throughput rather than moving the wait into a serial service. |
| Direct pread or additional file services | Attribute record wait, CPU-grant wait, physical I/O, and callback work first. Preserve descriptor/buffer custody, shared CPU accounting, and cancellation. |
| Parallel gzip, persistent checkpoints, or changed cache locality | Show remaining forward-inflate/locality cost after replay and task reductions; account for extra storage, identity validation, workspace, and physical lifetime. No default parallel decoder per source is selected. |

This work belongs in [BenchmarkArchive](../src/backend/data/benchmark/benchmark_archive.cpp),
[admission](../src/backend/data/detail/compile_admission.h), and the existing
[encoded publication owner](../src/backend/data/benchmark/benchmark_image_publication.cpp).
The local [input review](../build/validation/hotspot-input-review.md) records exact
call paths, required cases, file sets, and the deferred decision gates.

## Viable work: annotation reuse and ready-row batching

The current recipe probes a full-release native cache only when no modern part
cache is present. Modern runs can therefore re-enter part import even with a
valid final component already stored. Two further miss mechanisms exist: a
recovery-qualified part probe is skipped while originals are pending, and empty
namespaces have no persisted component even though the part route probes both
namespaces. These are source-proven reuse opportunities; the latest capture
does not measure how often all prerequisites were ready at the first probe.

Extend the existing full-cache attempt to modern caches using immediately
available artifact admissions and original snapshots. If a prerequisite is
held, the existing independent part callbacks take over immediately. Preserve
incremental cache validation, withdrawal after late proof failure, physical
revalidation, optional legacy extent fields, repair, and source ordinal rules.
Deleting the part-cache condition alone is unsafe because the current cache-only
path can wait for originals. This change needs no new persisted empty marker.

A second opportunity is deferred native recovery. The first pass already stages
a provisional normalized product and compact recovery support. Finalization can
then rebuild and copy boxes/runs even where recovery cannot change them. Extract
the applicability rule once in the recovery owner. A narrow immutable proof can
permit a final native view to share its existing staged payload while updating
the canonical lineage and row admission.

The proof must also exclude rows with suppressed malformed/degenerate/unlisted
object reports. Unchanged masks can still require final rejection reporting.
Preserve physical/original generation rechecks, exact ordinals/provenance,
required-label validity, final compiled-label projection, and geometry quarantine.
Unstaged vectors and rows needing recovery take the ordinary path. The selected
shortcut avoids its own new staging range/copy, while required native output
continuations remain in place.

Finally, group already-ready normalized rows under aggregate caps of 64 images,
256 boxes, and 65,536 runs per task. Count empty rows; keep large-row continuations
and independent peer progress. The same gather owner retains exact disjoint
placement, validation, and failure-to-seal behavior. Move the existing empty
Projection guard before admission while retaining all surrounding row and
duplicate bookkeeping. Neither change waits for a group to fill.

The discarded-product count has the following exact aggregate reconciliation:

| Source | Physical images | Final component rows before quarantine | Outside component |
| --- | ---: | ---: | ---: |
| COCO train2017 | 118,287 | 118,200 | 87 |
| COCO unlabeled2017 | 123,403 | 121,021 | 2,382 |
| COCO val2017 | 5,000 | 5,000 | 0 |
| Objects365 v2 Large selected patches | 137,435 | 103,010 | 34,425 |
| Objects365 v2 XL selected patches | 242,483 | 242,481 | 2 |

The 36,896 images outside components plus 76 final quarantines account for all
36,972 discarded products. Quarantine comprises 74 unlabeled images whose
required foreground labels were all rejected and two v2 canvas mismatches.
All Objects365 sources in this Stock manifest are v2. Aggregate similarity to
patch-32's population does not prove that the entire patch was unused.

Early exclusion remains a separate candidate. Current native caches retain
healthy subsets; missing rows and complete raw source extents do not prove
negative membership. A safe exclusion would require a complete, current proof
from existing source owners before pixel dispatch, preserving discovery,
ambiguity checks, cache health, repair, and withdrawal when inputs change.
Unknown membership must continue to make independent progress.

| Further opportunity | Required evidence before selection |
| --- | --- |
| Persist validated empty namespaces or bypass late merging | Measure remaining reparses after full-cache reuse; prove successful EOF, complete source identity, healthy-subset equivalence, and already-written ordinal adjustments in the existing component schema. |
| Promote an unchanged compiled provisional label | Prove exact payload, generation, geometry, write custody, and priority equivalence. Final projection remains required in the selected implementation. |
| Narrow original-generation invalidation | Establish the canonical dependency for every row; generation zero alone does not distinguish pending from independent rows. |
| Write native output directly into already-prepared staging | Measure immediate claim frequency; physical growth/wait remains outside CPU grants and the continuation remains on a miss. |
| Tighten recovery scratch or fuse adjacent Parquet operations | Prove full temporary bounds and preserve quiesced Arrow lifetime, healthy prefixes, parse failure, and replay cursor semantics. |
| Reduce final settlement passes | Attribute remaining work after cache reuse. Current settlement already changes bounded used-label records; it does not rewrite RGB or mask payloads. Its whole elapsed stage is only 23.131 seconds. |

The implementation stays in [Coconut recipe/import](../src/backend/data/benchmark/coconut/coconut_recipe.cpp),
[native image ownership](../src/backend/data/benchmark/coconut/coconut_native_image.cpp),
[mask recovery](../src/backend/data/benchmark/coconut/coconut_mask_recovery.cpp), and
[normalized assembly](../src/backend/data/benchmark/benchmark_annotations.cpp).
The local [rework review](../build/validation/hotspot-rework-review.md) records
all preservation requirements and deferred alternatives.

## Measurement and implementation acceptance

Use the same source baseline, encoded fixture hashes, geometry/order, worker
affinity, and cache state for each bounded comparison. Pipeline fixtures use
256–1,024 local images; compaction starts at 1,024 slots with a checked 16 GiB
working cap. Pixel/resample profiles retain their existing bounds, with corrected
PNG accounting. Archive and annotation cases use small generated sources and
explicit gates. Full Coconut remains a deliberate opt-in profile.

Measure untraced throughput separately from selected aggregates and full-detail
causal traces. Preserve every sample, setup time, production/finalization/sync
boundaries, exact output checks, and failure outcomes. A material reduction in
operations or allocations with no material matched-case slowdown is evidence
for a structural optimization; a whole-run speedup requires its own measurement.
No projected improvements in this report are measured implementation results.

The executable [action plan](../actionplan.md) sequences shared execution and
diagnostic demand, compaction, CPU pixels, archive handling, and annotation
reuse. Bounded failure retries use the owning suites. The final completion gate
uses `all` and packaged Wayland acceptance; neither automatically profiles the
complete benchmark dataset or invokes Firefox-owned tests.
