# Benchmark compiler performance

The five compiler changes and Final Validation are complete at `cecde5c5`.
Bounded matched measurements show compaction and retained-JPEG gains; corrected
small-image scheduling is within noise on the controlled P-core repeats. No new
complete Coconut run was performed. [Implemented changes and bounded measurements](#implemented-changes-and-bounded-measurements)
preserve every measured condition, the earlier regression, and the limits of
those results.

The investigation below records source baseline `333032e7`, preserved at
documentation commit `21bdcadc`. At that baseline, final train compaction was
the largest measured terminal pass: **922.091 seconds, 33.0% of the latest
complete run**. Pixel tasks accounted for **95.8% of measured task CPU time**.
Compute-worker creation was constant at 24. Source review identified removable
scheduling/diagnostic work and unnecessary archive retirement; these
measurements did not establish repeated worker creation.

This table retains the original priority ranking and rationale. Each area
received an independent source review across callers, ownership, failure paths,
and measurement alternatives. Execution/diagnostic demand was implemented first
so the remaining owners could share its demand-gated observation path.

| Original priority | Implemented area | Baseline evidence and selected mechanism |
| ---: | --- | --- |
| 1 | Batch ordered compaction and staged range mutations | 922.091 s on the terminal train path. Full scratch windows and amortized sparse allocation/reclamation accounting. |
| 2 | Remove excluded diagnostics and repeated scheduler work | 13.919 million probes, 2.704 million readiness deliveries, 1.735 million record denials. Upstream demand gates, coalesced wakes, reused record links, and bounded controller turns. |
| 3 | Reduce exact CPU quantization and scratch initialization | 8,897.272 cumulative Pixel CPU s. Exact derived lookup, overwrite sizing, and full-vector stores. |
| 4 | Retain useful archive state and batch bounded reads/copies | Cold extraction logged repeated decoder restoration without replay-CPU attribution. Useful-custody reclamation and amortized block-task handoffs. |
| 5 | Reuse final annotation results and batch ready rows | 357,533 Gather bodies and source-proven cache/recovery rework. Immediately available generation-valid caches, skipped proven-empty work, and grouped ready rows. |

The baseline findings and deferred decision gates remain separate from measured
implementation outcomes. Overlapping task costs cannot be added to the separate
compaction interval. [Profile commands](validation.md#data-compilation-and-resampling-profiles),
[current execution](compile-execution.md), [dataset formats/resampling](datasets.md),
and [benchmark source policy](benchmark-datasets.md) own the operational reference.
Evidence under `build/validation` is ignored local state and is absent from a
fresh checkout.

## Baseline measurement boundary

Both complete runs used Coconut, Stock validation, dropped-mask recovery, 384×384 Stretch/perceptual, 24 workers on CPUs 0–23, zero warmups, and one repetition. Each published 584,636 train and 5,000 validation images, totaling 1,190,741,014,016 logical compiled bytes. The only manifest difference is Objects365 patch-32's cache-hit flag (false then true); all source/annotation identities, provenance, box/run counts, and final lengths match. This does not prove byte-identical bin ordering.

These are explicit production-sized profiles, separate from normal tests. Trace filtering retained upstream observation costs. Source and OS cache conditions were not controlled, so the pair does not isolate an end-to-end speedup. Source query exports and exact fields are in `build/validation/benchmark-hotspot-evidence.json`.

| Elapsed boundary | Previous selective run (s) | Latest batched run (s) |
| --- | ---: | ---: |
| Whole compile | 2,766.100522 | 2,792.701975 |
| Production plus final source joining, before selection | 1,726.164 | 1,842.007 |
| Final selection | 69.108044390 | 23.130501498 |
| Train compaction | 965.084902918 | 922.091084029 |
| Validation compaction (overlaps train) | 7.974 | 8.836998928 |
| Train sync | 1.474 | 1.392 |

The later baseline selection settled 239,047 images in 3,736 bounded batches.
The observed stage reduction was 45.978 seconds; whole-run time was 26.601
seconds longer. Neither number isolates one code change under these cache
conditions. Archive calls differed too: 920,527 versus 103,013.

The [latest complete-run transcript](../build/validation/benchmark-coconut-batched.log),
[trace](../build/validation/benchmark-coconut-batched.jsonl),
[cost spans](../build/validation/benchmark-coconut-batched-spans.jsonl),
[progress export](../build/validation/benchmark-coconut-batched-progress.jsonl),
and [event summary](../build/validation/benchmark-coconut-batched-events.txt)
retain those boundaries. The preceding transcript is
[benchmark-coconut-selective.log](../build/validation/benchmark-coconut-selective.log).

Earlier bounded work, before this investigation, compared diagnostics baseline
`42c97d14` with the later compiler diff recorded in
[benchmark-perceptual-performance.json](../build/validation/benchmark-perceptual-performance.json).
It used 256 images, 24 workers, affinity 0–23, 384×384 Stretch/perceptual, one
warmup and three measured iterations, with matching input hashes/settings.

| Earlier untraced production path | Baseline median (s) | Follow-up median (s) |
| --- | ---: | ---: |
| Directory STB JPEG, seven mixed shuffled geometries | 2.681892 | 2.578329 |
| Directory STB PNG, seven mixed shuffled geometries | 2.529294 | 2.316668 |
| Benchmark Pixels, 32 retained JPEGs in shuffled order | 0.265026 | 0.121570 |

The retained Pixels ratio was 2.18× for decode/resize/store into memory, excluding
acquisition, annotations, disk publication, and compaction. Directory included
its small fixture's finalization. Caches were warm and not forcibly evicted;
mixed affinity did not control core-type distribution. These earlier samples
are distinct from the `333032e7` comparisons below and establish no end-to-end
Coconut speedup. Their ledger retains slower observations and earlier checkpoints.

## Baseline aggregate task costs

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

## Baseline control work and discarded products

Latest complete-run totals: 5,869,294 submission attempts, 1,734,731 record denials, 13,918,999 eligibility probes, and 2,703,834 readiness callback deliveries. Recorded admission-lock wait is 291.306 cumulative seconds; readiness callback wall time is 1,669.852 seconds and completion callback wall time 2,842.254 seconds. These overlapping wall sums establish candidate work, not recoverable elapsed time or CPU attribution. Baseline prefix filtering did not remove upstream snapshots/JSON construction.

Both runs produced 626,608 pixel images but selected 589,636. The 36,972 discarded images are 5.90% of produced images and represent 65,420,918,784 logical RGB-float bytes at 384×384. Their measured compute/physical-write contribution is not separately attributed. The latest 833,695 Labels and 483,204 Recovery calls warrant tracing whether repeated work is necessary for generation-safe early progress.

Final compaction visits about 1.048 TB of live payload, then publishes 1.191 TB of logical file lengths. Logical size and visited bytes are not physical disk traffic. Sparse slots, allocation, and reclamation are relevant. Btrfs mount compression alone does not establish compressed compiled extents, because the writer preallocates used ranges.

## Baseline streaming and overlap

During latest Indexing, no sampled roughly five-second interval had zero new pixel completions; median sampled rate was 355.98 images/s (p10 176.47, p90 462.86). These coarse samples cannot rule out short stalls or establish per-image p99 gaps. The previous run had one 5.467-second zero-pixel interval while labels advanced. Production continued through long recovery and physical-join spans.

The latest four RecoveryDrain spans sum to 5,386.128 seconds and eleven PhysicalWait spans sum to 2,246.604 seconds, overlapping the same 1,842-second production interval. ComponentGather sums to only 19.879 seconds and InventorySeal to 0.350 seconds, with possible nesting. Removing a long named wait cannot recover that duration unless its producer or terminal dependency becomes earlier.

## Baseline finding: final compaction

The shared compactor was the first elapsed-time target. Both Directory and
benchmark compilation already used it, so the change extended that owner.

At 384×384, the baseline mover used 1,769,472 bytes of its admitted 4 MiB scratch.
A full 64-slot pixel run is 108 MiB. Using the full scratch window reduces the
derived number of pixel-copy iterations by approximately 50–58%, depending on
inactive-slot fragmentation. It does not reduce the live pixel bytes copied.
Labels account for less than 0.5% of live train payload but cause many separate
gap punches, source punches, and storage-accounting transactions.

The implementation uses bounded ordered range processing: full admitted scratch
for contiguous pixels, captured label-prefix windows, and batched allocation/
reclamation through the staged-file owner. Rejected batches fall back to the
prior safe operation sizes without losing source bytes or conservative promises.
The [format reference](datasets.md#staging-and-in-place-compaction) and
[storage reference](compile-execution.md#file-preparation-and-settlement) own
these ordering/capacity rules. Optional reflected aggregates distinguish copy
requests, actual syscalls, and mutation timing; disabled observation collects
nothing.

The ownership boundary is
[the shared compactor](../src/backend/data/compiled/compiled_file_compaction.cpp),
[staged inode mutation](../src/backend/data/staged_artifact.cpp), and
[filesystem capacity accounting](../src/backend/data/storage_reservation.cpp).
Directory and benchmark writers retain final metadata writes, sync, failure
joins, and atomic publication. Format 10, physical slot identity, sparse padding,
eligible-only label maxima, and exact content remain unchanged.

The resulting [Compaction profile](validation.md#data-compilation-and-resampling-profiles)
compares staged contents with dense/sparse pixels and mixed labels, including
final sync. The selected measurement range was 1,024–4,096 slots, roughly
1.8–7.3 GB of dense live pixels; the retained matrix used 1,024. Existing
storage/publication cases cover overlap, tight admission, correctness, and
failure without a full Coconut compile.

Same-inode reflink is a separate candidate within this owner. It could avoid
buffered copying for aligned, disjoint live pixel chunks, but requires verified
filesystem support, conservative shared-extent accounting, meaningful error
propagation, and matched-input measurements. It is not a measured improvement.
Changing the compiled format, waiting for all labels before producing pixels,
allocating sparse holes, or removing durability are excluded.

The captured Btrfs mount's compression setting does not identify compiled extent
compression: preallocation affects that behavior. [Btrfs compression exceptions](https://btrfs.readthedocs.io/en/latest/Compression.html#exceptions).
The existing stack captured one hole-punch call; it does not attribute a
percentage of compaction time to that syscall.

| Conditional reduction of train compaction | Saved elapsed time | Whole-run multiplier if every other stage stays equal |
| ---: | ---: | ---: |
| 10% | 92.2 s | 1.034× |
| 25% | 230.5 s | 1.090× |
| 50% | 461.0 s | 1.198× |
| 100%, theoretical ceiling | 922.1 s | 1.493× |

The table is a baseline projection, not a measured speedup bound. Subsequent
bounded results appear below. Detailed source anchors, alternatives, and
required cases are preserved in the local
[compaction review](../build/validation/hotspot-compaction-review.md).

## Baseline finding: exact CPU pixel processing

The baseline CPU resampler already used AVX2/FMA, persistent worker-local
controls, and direct perceptual planar output. The selected changes stayed
inside that implementation:

1. The derived lookup replaces eight dependent SIMD gathers with two, a 75%
   reduction in that instruction count; runtime benefit was initially unmeasured.
   Its 4,097 base codes occupy 16,388 bytes, retained once with the canonical
   Float32 thresholds. [CPU quantization](datasets.md#optional-perceptual-downscaling)
   owns the bucket/correction proof and exact scalar authority.
2. Overwrite sizing removes an unnecessary clear from fully assigned axes and
   state rows. The supplied 384² RGB scratch is 49,152 bytes. Mathematical
   accumulator initialization remains necessary.
3. Complete vectors now use unaligned stores; tails stay masked. Packed RGB lane
   construction and hoisted fractional controls remain possible follow-ups only
   with exact output and matched-input evidence. RGB loads must read exactly
   three valid bytes; four-byte gathers would overread tight final pixels.
4. Retained-RGB profiling now checks the decoder's actual RGB extent before
   allocating/decoding. The baseline charged the JPEG-specific separate output
   allocation, which was zero for STB PNG plans. Decoded products, decoder
   workspace, and compiled destinations retain distinct accounting.

A one-image probe against the baseline confirmed the PNG accounting defect:
a retained 768×768 RGB image required 1,769,472 bytes, but the runner reported
`retained_decoded_bytes=0`. The cached profile target needed no rebuild
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
profile ran 256 images in 0.121570 seconds; it excluded file output and full
recipe control. The later matrix compares `333032e7` at one and 24 workers
using matched Pixels/Resample inputs, with a small Directory mapped-output
control. Setup and trace-enabled causal measurements remain separate.

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

## Baseline finding: scheduling and diagnostic demand

The baseline filtered trace built payloads before rejection. For the public
Coconut adapter this included JSON construction, serialization, a trace mutex,
parsing, and only then prefix filtering. Native execution also collected live
snapshots and diagnostic credit ancestry when only a final summary was requested.
Some of that work ran while callers held their facts mutexes.

The implemented cutover gives trace values one immutable selection policy plus
delivery through public, private, and enriching adapters. Producers gate field
builders, source-file digests, snapshots, and clocks. Aggregate collection is
separate from detailed emission: selecting
`compile.execution.retired` retains complete cumulative counters without
collecting transient ancestry or formatting every rejected admission. Completed
span selection still requires its duration even when its start is excluded.
Default full traces preserve their requested events and failure semantics.

The scheduler cutover coalesces eventfd turns, reuses independent active/ready
links in the fixed record bank, and bounds dispatch while preserving cursors
across yields. Physical completion state remains independent of record reuse.
The [execution reference](compile-execution.md#owners-and-ready-work) owns
admission budgets, finite denied passes, and callback/subscription lifetime;
its [readiness section](compile-execution.md#selective-readiness-and-resource-pressure)
describes acknowledgement races and the later idle-worker wake correction.

These belong in [CompileExecution](../src/backend/data/compile_execution.cpp),
[the benchmark trace boundary](../src/backend/data/benchmark/detail/benchmark_trace.h),
[the public compiler adapter](../src/backend/data/benchmark/benchmark_compiler.cpp),
and [pipeline dispatch](../src/backend/data/benchmark/benchmark_pipeline.cpp).
The selection value is independent of product types, JSON, and resource
state. Existing reflected observation declarations own additional counters;
diagnostic identities remain observational.

The source review also identified ready-row gather batching and empty
Projection admissions. Their implementation stayed in the annotation/import
owners described below; neither justified generic executor auto-batching across
different resource or failure policies.

| Further opportunity | Decision boundary |
| --- | --- |
| Queue-local capacity subscriptions | Returning one file queue's slot currently broadcasts global Records. A complete scoped cutover must signal after actual slot return and safely detach before the consumer retires. Removing the broadcast alone can lose progress. |
| Record-only waits and dirty-progress wake coalescing | Use the existing readiness and progress owners; preserve predicate-before-wait races, exact terminal facts, flush sequences, and failure wakes. |
| Further worker targeting, class-ready masks, or fewer lock sections | The measured idle-broadcast correction is recorded below. Additional changes still require empty-selection/lock-CPU attribution, with class floors and physical retirement/callback boundaries preserved. |
| Aged-credit and physical-service bookkeeping | A faster scan must preserve alias/custody changes that can occur without a resource epoch. Affinity caching must account for nested borrowed dispatch. |
| Extra I/O workers, work stealing, or idle-floor lending | Current evidence does not justify changing these policies. Increasing retained work or delaying newly live classes can conceal the problem and weaken progress guarantees. |

The resulting bounded `Pipeline` mode exercises production source publication,
image and label work, both file queues, and mapped output with 256–1,024 local
synthetic images. Its matrix separates trace-off, selected-summary, and
full-detail capture. Compile/Pixels remain controls with narrower scope.

Selected observations record snapshots built, eventfd writes, notifier batches/
deliveries, submissions, denials, probes per turn, empty turns, completion
spacing, and retirement. The retained evidence keeps every sample and input
hash. Explicit-gate cases cover notification acknowledgement/delivery arrivals,
stale completion handles, cancellation, fitting work behind an oversized blocked
item, and independent progress beside a held peer. Those correctness cases use
bounded inputs and completion gates rather than throughput thresholds.

The baseline review established removable operations, without a measured
elapsed saving. The subsequent results below do not convert overlapping callback
or lock sums into wall-time gains. Full detail and exact source anchors are in the local
[scheduling review](../build/validation/hotspot-scheduling-review.md).

## Baseline finding: input and archive scheduling

The warm input path validates each encoded source, then prepares the consumer's
decode/resample plan from the same retained mapping. Its 1,253,216 Header calls
do not represent two full reads of 626,608 images. Full preload makes one
mapped-to-pinned copy on persistent image services. A cold archive can instead
transfer its existing admitted encoded buffer directly to the pixel consumer.
Both paths retain actual-byte verification before destination writes.

At 24 compute workers, the image file queue has four records and two persistent
services; the output queue has 48 records and two services. They borrow the same
CPU budget. The latest Header bodies total 1,278.385 seconds, but their measured
CPU is 106.588 seconds. The difference is an overlapping off-CPU sum, potentially
including faults, descheduling, locks, and diagnostics. It is not measured disk
wait. Destination allocation also does not prefault every writable page; page
faults and dirty throttling can occur inside pixel stores.

The strongest input change concerns cold archive reuse. A serial decoder retains
a 4 MiB heap/library charge, plus a descriptor for gzip. It does not occupy a
pinned arena slice. Baseline generic resource pressure could retire it for ordinary
arena fragmentation even when returning that custody cannot help. Restarting a
serial compressed archive can replay its prefix. The earlier cold patch-32
capture has 34 pressure events and 60 decoder-admission events, but no direct
replayed-byte or replay-CPU attribution.

The implemented retention decision belongs to the existing execution/admission
owner and evaluates actual blocked requests and candidate allowances. It keeps
an unsupplied decoder through unrelated arena pressure while permitting useful
byte/descriptor reclamation, multiple contributing victims, and mandatory
exceptional drain. Changed demand can notify pressure subscribers while pressure
stays active. [Admission rules](compile-execution.md#selective-readiness-and-resource-pressure)
retain cancellation, fairness, unknown-wait conservatism, and physical custody.

The archive remains the decoder owner. No protected pool, extra eviction manager,
or timing-based retention policy is needed. The exceptional input path's
post-restoration decoder retirement is a separate custody guarantee and remains
required; removing that line alone would be unsafe.

The baseline submitted and joined each data block, including the EOF probe;
copying happened outside that measured body. The selected change now performs
bounded reads/copies under one existing CPU grant, at most eight blocks or 1 MiB
per turn, with member boundaries, per-block cancellation, and shared extent
validation. Arbitrary consumer callbacks retain their execution boundary because
they can wait for resources or publish files. The existing 64-header skip batch
remains separate.

Bounded correctness cases cover unrelated arena pressure, necessary byte/
descriptor pressure, multiple victims, a useful request arriving during existing
pressure, and exceptional drain. The selected causal profile bound was 64 MiB;
the implemented fixture used 128 members. Decoder opens/restores, forward/replay
bytes, copies, CPU, and maximum turn work are collected only when requested.

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

## Baseline finding: annotation reuse and ready-row batching

The baseline recipe probed a full-release native cache only when no modern part
cache was present, so modern runs could re-enter part import with a valid final
component already stored. Two further miss mechanisms were identified: a
recovery-qualified part probe was skipped while originals were pending, and
empty namespaces had no persisted component even though the part route probed
both namespaces. These were source-proven reuse opportunities; the complete
capture did not measure how often all prerequisites were ready at the first probe.

The implemented full-cache attempt includes modern caches when artifacts and
original snapshots are immediately admissible; held prerequisites leave the
independent part callbacks in charge. Incremental cache validation, withdrawal,
physical revalidation, optional legacy extent fields, repair, and ordinal rules
remain. Removing only the part-cache condition would have been unsafe because
the baseline cache-only path could wait for originals. No persisted empty marker
was added.

A second baseline cost was deferred native recovery: finalization rebuilt and
copied boxes/runs even where recovery could not change the staged provisional
payload. A narrow immutable proof now permits reuse while canonical lineage,
row admission, physical/original generations, and final label projection settle.
The proof also excludes suppressed malformed/degenerate/unlisted reports;
unchanged masks can still need final rejection reporting. The
[recovery reference](benchmark-datasets.md#optional-dropped-mask-recovery)
owns that boundary and its ordinary-path cases.

Already-ready normalized rows now share bounded gather tasks; large-row
continuations and independent peers retain progress. Proven empty Projection
work is skipped before admission. [Parser/gather ownership](benchmark-datasets.md#archive-and-parser-continuation)
retains exact placement, validation, row/duplicate bookkeeping, and failure
semantics, with no batch-fill wait.

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
| Reduce final settlement passes | Attribute remaining work after cache reuse. Settlement changes bounded used-label records; it does not rewrite RGB or mask payloads. The baseline elapsed stage was 23.131 seconds. |

The implementation stays in [Coconut recipe/import](../src/backend/data/benchmark/coconut/coconut_recipe.cpp),
[native image ownership](../src/backend/data/benchmark/coconut/coconut_native_image.cpp),
[mask recovery](../src/backend/data/benchmark/coconut/coconut_mask_recovery.cpp), and
[normalized assembly](../src/backend/data/benchmark/benchmark_annotations.cpp).
The local [rework review](../build/validation/hotspot-rework-review.md) records
all preservation requirements and deferred alternatives.

## Comparison method

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
The implementation results below distinguish measured bounded outcomes from
these baseline projections.

The completed work covers shared execution/diagnostic demand, compaction, CPU
pixels, archives, and annotation reuse. Bounded failure retries used the owning
suites. The [final completion gate](validation.md#compiler-throughput-validation-record)
used `all` and packaged Wayland acceptance; neither automatically profiles the
complete benchmark dataset or invokes Firefox-owned tests.

## Implemented changes and bounded measurements

The five selected implementation areas are complete in commits
`ff30e564`, `4db93f1`, `5098f6`, `9385d8`, `de44699b`,
`65e2ba0c`, `9ddb6e98`, and `5bede917`. Validation through `cecde5c5` corrected
readiness callback lifetime and reduced idle compute wakeups. The original
complete-run measurements above remain baseline evidence; a new complete
Coconut run has not been performed.

The initial matrix has 37 matched cases (74 invocations), one warmup and three
measured repetitions. It uses 256 local images, or 1,024 compaction slots below
the 16 GiB cap, with setup and final synchronization reported separately.
All 24 Pipeline/compaction case pairs preserve their output/live-payload hashes;
Pixels/Resample report throughput and checked decoded extents and rely on the
independent numerical tests for output correctness. The baseline production
source remains `333032e7`, preserved at documentation commit `21bdcadc`.
Its nine-file measurement adapter is recorded in
`build/validation/benchmark-throughput-baseline-adapter.patch`
(SHA-256 `b403b34ff280edda147be3c6dd0e659ac05a3dbd41ca7f935cdf2ae544640601`).
It changes the wrapper/profile harness only. Both harnesses pass admitted
retained-image headers into production conversion; baseline production code
remains unchanged.

The initial untraced compaction medians include final metadata and sync:

| Variant | Baseline (s) | Candidate (s) | Elapsed change |
| --- | ---: | ---: | ---: |
| Dense | 1.943623 | 1.818440 | −6.4% |
| Sparse | 1.368839 | 1.189074 | −13.1% |
| Mixed labels | 1.373747 | 1.232477 | −10.3% |

These bounded results do not establish the same percentage saving on the
1.19 TB complete output. Both sides used affinity 0–23; that permits mixed
core placement. Full samples, all initial comparisons, and hash reconciliation
are in [the primary results](../build/validation/benchmark-throughput-primary-results.json).

### Core placement and the scheduling correction

The captured host is an i9-12900K. Its hybrid PMUs identify P logical CPUs
0–15 and E CPUs 16–23; P-core SMT pairs are 0–1, 2–3, and so on. All reported
frequency policies were `performance`. Compute workers are individually
pinned by the production worker pool. An eligible mask spanning both types
still permits different task distributions and control-thread placement.
The initial 0–23 results therefore do not isolate core-type effects.

The read-only I/O diagnostic now records eligible CPU/core/package identities,
SMT siblings, hybrid PMU membership, and frequency policies. Missing topology
attributes are reported as unavailable, not interpreted as a homogeneous CPU.
The capture is
[benchmark-throughput-topology.json](../build/validation/benchmark-throughput-topology.json).

Core-controlled follow-ups use eight distinct P cores
(`0,2,4,6,8,10,12,14`), all eight E cores (`16-23`), or one fixed P/E CPU.
They retain every sample, use two warmups and nine measured repetitions, and
reverse baseline/candidate order for the disputed small-image case. No tests,
builds, or other profile commands ran concurrently with the timed comparisons;
unrelated host activity and frequency variation remain possible.

The original candidate regressed on small images: mixed-core repeats had
0.1371 s baseline versus 0.2029 s candidate medians. Restricting the run to
P cores still showed 0.0670 versus 0.0855 s, so hybrid placement alone did not
explain the regression. Gated counters then established roughly 20,000 empty
CPU selections for 768 tasks on P8, and 87,000 on W24.

Compute workers now wait separately from owner-local readiness notices.
Physical changes wake CPU selectors when queued compute exists, resource
pressure can require idle scratch retirement, or interruption/shutdown needs
their attention. Physical service subscribers and public admission generations
keep their existing delivery semantics. The existing execution owner supplies
the shared ready-work predicate; no extra worker pool, polling loop, capacity
budget, or batch-fill delay is introduced.

The correction passed 149 focused resource/readiness cases with 22,103
assertions. In selected-summary probes, measured median empty selections fell
from 20,151 to 5,086 on P8 and from 87,220 to 12,921 on W24 (about 75% and 85%).
Created/retired compute workers still equal the requested pool size, with one
retirement snapshot per selected-summary run.

Two reversed-order P8 repeats, 18 measured samples per side, have combined
small-image medians **0.068510 s baseline / 0.068697 s candidate**, a 0.27%
difference. The earlier 28% median regression no longer appears in those
controlled repeats. The W24 small-image matched pair is
**0.161200 / 0.119246 s**; its older baseline repeat was 0.137063 s.
A first P8 post-fix pair was unusually slow on both sides
(0.322612 / 0.180380 s) and is retained as noisy evidence, not a headline gain.

The mixed-geometry pooled P8 pair is 0.901859 / 0.805657 s, with measured ranges
0.854255–0.959473 / 0.798618–0.818794 s. An earlier P8 pair, before the idle-wake
correction, was 0.854723 / 0.817518 s. Keep those distributions and code states
separate. The fixture has at most eight images in flight; W24 does not imply
24 concurrent pixel tasks.

### CPU pixel processing

The retained 32-JPEG sample on eight fixed P cores gives:

| Boundary, 256 images | Baseline median (s) | Candidate median (s) | Elapsed change |
| --- | ---: | ---: | ---: |
| Decode plus resize/store | 0.184394 | 0.153668 | −16.7% |
| Resize/store from retained RGB | 0.139750 | 0.107122 | −23.3% |

Each row has two warmups and nine measured samples. Encoded inputs and order
match; retained decoded RGB is 26,834,232 bytes on both sides of Resample.
These timings exclude file output, dataset acquisition, and recipe control.
Perceptual compilation remains on CPU AVX2/FMA; these results do not support a
GPU compiler or extrapolate proportionally to whole-run elapsed time.

The authoritative sample sets are
[core-controlled initial results](../build/validation/benchmark-throughput-core-results.json),
[post-correction comparisons](../build/validation/benchmark-throughput-idle-wake-results.json),
[reversed-order and retained-image results](../build/validation/benchmark-throughput-core-final-results.json),
and [wake counters](../build/validation/benchmark-throughput-idle-wake-counts.json).
Exact commands, warmup counts, input hashes, trace selections, and all samples
remain under `build/validation/benchmark-throughput-*`.

### Compaction, archive, and annotation operation evidence

The six observed compaction runs retain exact live-payload hashes and staged
inode identity. Baseline adapter counts are single-range wrapper requests;
candidate syscall counters are separate observations and must not be compared
as though the baseline measured syscalls.

| Variant | Allocation requests, baseline → candidate batches | Reclaim requests, baseline → candidate batches | Candidate allocation / reclaim syscalls |
| --- | ---: | ---: | ---: |
| Dense | 1,779 → 421 | 1,178 → 453 | 619 / 639 |
| Sparse | 685 → 411 | 832 → 558 | 411 / 558 |
| Mixed labels | 1,269 → 427 | 1,418 → 598 | 988 / 1,153 |

All use the full 4 MiB scratch ceiling, report no capacity fallback, and retain
required truncation and sync. Candidate reclamation accounts for about 54% of
the measured mover interval in these observed cases; it remains a useful
filesystem-specific follow-up target, not a proved whole-run percentage.

The archive profile uses the same 128-member, 6,334,163-byte gzip input in all
four pressure cases and checks every returned member byte. With seven unrelated
arena-pressure episodes it retains one decoder, with zero restores or replay.
Necessary byte pressure and exclusive drainage each retire/restore seven times,
preserving competing admission and exact output. Those cases record 23,281,664
replayed decoded bytes. Decoder cursor bytes are not physical disk-read bytes.
Each variant performs 304 block reads in 128 bounded read turns; maximum turn
work is three blocks and 49,353 copied bytes for this fixture.

Mapped and pooled annotation profiles give the same results. Gathering 256
ready rows uses four tasks for 192 boxes, with no wait for a batch to fill.
DeferredClean reuses all 256 staged native products at finalization, while
retaining the necessary initial 26,112-byte staging/copy work. DeferredRecovery
recovers 192 rows and reuses 64 unchanged rows. Its final compiled hash equals
DeferredClean, despite their deliberately different source annotation hashes.
Every case completes 256 label projections, writes, and final settlements.
These variants prepare native/gather input before their image-publication loop:
they establish operation counts and custody, not full-recipe overlap or an
annotation elapsed-time speedup.

The resize observation counts 219 preparations and 9,648 allocation requests
for 256 mixed-geometry images, identically at W1 and W24. These are requests
inside admitted reusable workspace, not evidence of 9,648 OS allocations.
Likewise, the 653,686,272 cumulative requested bytes are not peak memory usage.

### Completion spacing and remaining limits

Selected completion-gap probes record 765 intervals per side after warmup:

| Input custody | Baseline p95 / p99 / max (ms) | Candidate p95 / p99 / max (ms) |
| --- | ---: | ---: |
| Mapped | 10.066 / 15.890 / 48.577 | 8.551 / 13.965 / 38.244 |
| Pooled | 10.144 / 17.133 / 73.496 | 9.292 / 17.325 / 46.798 |

These are traced W24 mixed-core fixtures, not UI frame timings. The pooled p99
is essentially unchanged; the data supports reduced long gaps in this bounded
sample, not universal smoothness. All four sides have the same compiled hash.
[The causal results](../build/validation/benchmark-throughput-causal-results.json)
retain operation fields, gap samples, and provenance; their command/status
ledgers include the rejected 128-image annotation invocation and corrected
256-image runs.

The original five hotspot rankings still guide follow-up work. Reclamation
syscalls remain substantial; tiny tasks can still incur scheduling overhead;
resize preparation retains repeated work; required archive replay still exists
under useful reclamation; final label projection and settlement remain necessary
even when native payload can be reused. Each further change needs attribution
in its owning production component and matched evidence. No new complete-run
speedup, peak-RSS reduction, physical-I/O bandwidth gain, or GPU benefit is
established by this bounded validation.

### Final validation

The final full build, ordinary suite, and packaged Wayland acceptance passed.
The [validation record](validation.md#compiler-throughput-validation-record)
owns exact commands, case/assertion counts, the unavailable hardware skip,
checkpoints, focused fixes, and retained transcripts. No Firefox-owned suite
or new production-sized benchmark compile was run.
