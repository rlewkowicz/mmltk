# Technical wiki

[Introduction and quick start](../README.md) · [Architectural contract](../CONTRACT.md) · [Agent policies](../AGENTS.md)

## Getting started and reference

- [Build and reusable state](build.md): cold-checkout bootstrap, independent
  native tools graph and immutable runtime closure, product toolchains,
  dependencies, packaging, generated artifacts, cache invalidation, and timing.
- [Commands](commands.md): native driver/tool and application CLI entrypoints,
  process actions, benchmark inspection/cache selection, explicit data profiling,
  desktop options, and model tooling.
- [RF-DETR workflows and artifacts](rfdetr-workflows.md): model/class admission,
  preprocessing, output reservation, training history/progress, evaluation,
  export, and image/video prediction and saving.
- [Model merging and continuation](model-merging.md): periodic averaging, final
  deployment selection, artifact formats, publication, leases, and exact Resume.

## Architecture and frameworks

- [Architecture and source guide](architecture.md): implementation owners,
  shared facilities, serialization, generated boundaries, frontend components,
  presentation, and vendor ownership.
- [Reflected declarations and authoring](reflection.md): canonical schemas,
  registration, all twelve annotation macros, scoped CLI declarations,
  local macros/aliases, structural projection, and safe mechanical formatting.
- [Generated Rust and application CBOR](application-wire.md): canonical native
  schema, generator ownership and consumption, record representations,
  revisioned Dataset progress and native units, encoding/decoding limits, compatibility,
  session admission, and failures.
- [GUI interaction and presentation](gui-interaction.md): layout and controls,
  session Status, retained editing/charts/viewers, input ordering,
  image geometry, rendering, graphics custody, and redraws.

## Data and backend systems

- [RF-DETR training and native compilation](rfdetr-training.md): reference
  equations, lanes/global batches, recipes/schedulers/EMA, sparse planning,
  target/collective lifetimes, bounded next-batch preparation, supervision
  adaptations, guarded Selective/FullTrace execution, precision, and compiler choices.
- [Datasets and compilation](datasets.md): source annotations, format 10 fixed
  slots, in-place compaction, prepared batch capacity, resize geometry,
  prepared CPU/native/perceptual/Torch resampling, loading/leases, and Explore cache residency.
- [Compilation execution and working storage](compile-execution.md): shared
  Directory/benchmark ownership, live-class CPU floors and elastic work,
  persistent task records, bounded dispatch, task-sized leases in fixed pinned
  arenas, selective readiness/wakeups and useful reclamation, exceptional inputs,
  bounded image/file services and retained continuations, file
  preparation/completion, filesystem capacity transactions, atomic publication,
  and bounded progress delivery.
- [Built-in benchmark datasets](benchmark-datasets.md): recipe membership,
  native import/provenance, optional mask recovery, persistent cache/repair,
  recipe readiness, bounded archive traversal and Parquet continuation,
  shared encoded-image publication, ready-row gathering, native payload reuse,
  batched final provenance settlement, source-generation reconciliation,
  immutable row-admission and cache custody,
  healthy subsets and incomplete provenance, row/component progress accounting,
  and capacity limits.
- [Benchmark compiler performance](benchmark-performance.md): historical
  complete-run costs, implemented optimizations, matched bounded/core-controlled
  measurements, causal work counts, deferred alternatives, and evidence limits.
- [GPU execution and image loading](gpu-execution.md): device/NUMA selection,
  transfers, runtime retirement, inference lanes, readbacks, Vulkan/CUDA import,
  canvas descriptor/submission custody, sparse display damage, Upscale
  preparation, and capability inspection.

## Engineering, validation, and operations

- [Validation](validation.md): required gates, native tool fixtures, suite/filter
  selection, product/tool tidy, test-run replacement, PCH include audits,
  three cleanup profiles and raw-review tooling,
  declaration formatting, symbol/address and GPU diagnostics, evidence ownership,
  rendered acceptance, numerical coverage, benchmark ownership/recovery and
  Firefox validation records, opt-in CPU/data profiles, complete Coconut and
  bounded pipeline/archive/compaction profiles, historical fixed-storage
  measurements, fixtures, and evidence limits.
- [RF-DETR numerical reviews and acceptance](reviews/rfdetr-math/README.md):
  four historical source reviews, current repair closure, actual upstream
  matched-state gates, measured trajectory differences, and quality limits.
- [Headless Wayland](headless-wayland.md): private NVIDIA Weston setup, input
  seat, readiness, deadlines, shutdown, artifacts, and limitations.
- [Logging](logging.md): automatic build/test/tidy transcripts, opt-in runtime
  diagnostics, fatal causes and process identity, acceptance evidence formats, capture/history
  selection, compiler cost/arena observations, partial-line windows, numeric
  statistics, correlation, triage, and output limits.
- [Planned work](roadmap.md): future directions, distinct from current capability.

`CONTRACT.md` owns high-level architecture and handoffs; this wiki owns detailed
commands, formats, implementation, and procedures; `AGENTS.md` owns development
policy and workflow.
