# Benchmark compiler performance

[Wiki index](README.md) · [Profile commands](validation.md#data-compilation-and-resampling-profiles) · [Execution and storage](compile-execution.md) · [Recipes and caches](benchmark-datasets.md)

Use the opt-in `data-profile` runner to measure production owners. Ordinary
suites do not compile a complete benchmark dataset. Commands, options, fixture
bounds, and trace selections belong in the [profile reference](validation.md#data-compilation-and-resampling-profiles).
Raw samples, comparisons, and review reports belong under ignored
`build/validation/`; they are local evidence rather than current product facts.

## Choose the measured boundary

| Mode | Boundary and interpretation |
| --- | --- |
| `Compile` | Directory preparation, STB decode/resize, labels, output, and finalization |
| `Pixels` | Benchmark decode/resize/store into memory; excludes file publication and recipe work |
| `Resample` | Resize/store from retained decoded RGB; excludes decoding from timed work |
| `Pipeline` | Bounded benchmark image/label publication, file services, output, and finalization; Archive input instead isolates archive transport |
| `Compaction` | Same-inode movement, metadata, and sync; setup and verification are separate |
| `Coconut` | Complete recipe through publication, with Stock validation and dropped-mask recovery; every warmup/repetition compiles the full population |
| `Loader` | Compilation followed by sequential, shuffled, and overlapped CUDA loading |

Select the smallest boundary containing the suspected cost. Use a complete
Coconut run only for an explicitly requested full-recipe measurement: it can
acquire or repair large source caches and produce large compiled files.
`--num-images` does not limit Coconut. A single complete measurement uses
`--warmup-runs 0 --repetitions 1`.

## Matched comparisons

Retain these facts for both sides:

- Exact source/build identity, command, encoded-input hashes, geometry/order,
  resize policy, worker count, and input/output custody.
- Eligible CPU/core identities, SMT siblings, core types, and frequency policy
  from the [read-only topology diagnostic](gpu-execution.md#read-only-capability-inspection).
  Equal affinity masks spanning different core types do not establish equal
  placement. Record concurrent host work and cache conditions.
- Setup, warmups, every measured sample, production, finalization, and sync;
  keep noisy/slower samples. Reverse execution order for disputed results.
- Exact output/live-payload checks, failure outcomes, trace selection, and
  fixture limits. Pixels/Resample timing relies on independent numerical
  tests for output correctness; a throughput result alone is insufficient.

Measure untraced throughput separately from selected aggregates and full causal
traces. `--trace-file` enables collection; `--trace-prefix` limits demand but
selected aggregates still incur collection/delivery cost. Use the
[log query tool](logging.md) to inspect captures.

## Interpret counters

| Observation | Meaning and limits |
| --- | --- |
| Task body CPU/wall sums, ready waits, callback/lock sums | Overlap across workers and phases; they cannot be added to an elapsed-time budget |
| Task CPU divided by elapsed time | Accounted task core-equivalents, excluding unattributed controller, scheduler, callback, and file-service CPU |
| Worker creation/retirement | Pool lifetime; idle selector wakes are separate scheduling work |
| Allocation requests/bytes | Requests within admitted reusable storage; neither OS allocation count nor peak memory |
| Normal pinned arena bytes | Retained [arena budget](compile-execution.md#fixed-pinned-arenas), excluding exceptional storage, retained products, library controls, file cache, and other process memory |
| Logical output/source bytes | Format extent or memory work; neither physical disk/network traffic nor bandwidth |
| Compaction progress bytes | Eligible live ranges visited, including ranges already placed; excludes padding and need not mean rewritten bytes |
| Archive replay bytes | Decoder cursor work; not physical disk-read bytes |
| Wrapper range requests versus syscall counters | Different units; compare like with like |
| Completion gaps | Captured image-completion intervals; not UI frame time or proof that shorter stalls never occur |
| Bounded speedup | Applies to the measured inputs, geometry, custody, and boundary; full-recipe speedup needs a full-recipe measurement |

`--compaction-cold` syncs and advises only the fixture out of cache. It does not
establish a cold host/device. A Btrfs mount compression setting does not prove
that preallocated output extents are compressed. A lower operation count needs
matched elapsed-time evidence before being described as a throughput gain.

## Production constraints

Attribute work to its existing owner before changing execution. Preserve:

- [Source membership, annotations, repair, and provenance](benchmark-datasets.md),
  including healthy partial labels and valid backgrounds.
- [Compiled layout, exact geometry/masks, and same-inode compaction](datasets.md),
  including source-before-overwrite ordering, bounded scratch, sparse capacity,
  durability, cancellation, and previous-output preservation.
- [CPU admission, event-driven readiness, pinned storage, and physical custody](compile-execution.md).
  CPU compilation stays on the shared CPU execution path; retained state and
  batching cannot add an unaccounted pool or wait for a batch to fill.
- Independent [numerical resampling rules](datasets.md#optional-perceptual-downscaling)
  and [functional compiler coverage](validation.md#benchmark-compilation-evidence).

Current mechanisms include reusable scheduler records, demand-gated diagnostics,
ordered batched compaction, exact CPU quantization, useful-custody archive
retention, generation-valid annotation reuse, and bounded ready-row gathering.
Their owning references specify behavior; no historical measurement is a
current throughput, GPU benefit, peak-RSS, or complete-validation guarantee.
