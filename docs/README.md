# Technical wiki

[Introduction and quick start](../README.md) · [Architectural contract](../CONTRACT.md) · [Agent policies](../AGENTS.md)

## Getting started and reference

- [Build and reusable state](build.md): prerequisites, toolchains, dependencies,
  packaging, generated artifacts, cache invalidation, and build timing.
- [Commands](commands.md): wrapper and native CLI entrypoints, process actions,
  benchmark inspection/cache selection, desktop options, and model tooling.
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
  encoding/decoding limits, compatibility, session admission, and failures.
- [GUI interaction and presentation](gui-interaction.md): layout and controls,
  session Status, retained editing/charts/viewers, input ordering,
  image geometry, rendering, graphics custody, and redraws.

## Data and backend systems

- [RF-DETR training and selective compilation](rfdetr-training.md): reference
  equations, lanes/global batches, recipes/schedulers/EMA, sparse planning,
  target/collective lifetimes, bounded next-batch preparation, supervision
  adaptations, and guarded native tracing.
- [Datasets and compilation](datasets.md): source annotations, format 9, resize
  geometry, perceptual downscaling, loading/leases, and Explore cache residency.
- [Built-in benchmark datasets](benchmark-datasets.md): recipe membership,
  native import/provenance, optional mask recovery, persistent cache/repair,
  overlapping compilation, progress units, and capacity limits.
- [GPU execution and image loading](gpu-execution.md): device/NUMA selection,
  transfers, runtime retirement, inference lanes, readbacks, Vulkan/CUDA import,
  sparse display damage, Upscale preparation, and capability inspection.

## Engineering, validation, and operations

- [Validation](validation.md): required gates, suite/filter selection, tidy,
  test-run replacement, PCH include audits, cleanup and raw-review tooling,
  declaration formatting, diagnostics, evidence ownership, rendered acceptance,
  numerical coverage, fixtures, and evidence limits.
- [Headless Wayland](headless-wayland.md): private NVIDIA Weston setup, input
  seat, readiness, deadlines, shutdown, artifacts, and limitations.
- [Logging](logging.md): automatic build/test/tidy transcripts, opt-in runtime
  diagnostics, fatal stderr, acceptance evidence formats, capture/history
  selection, queries, correlation, triage, and output limits.
- [Planned work](roadmap.md): future directions, distinct from current capability.

`CONTRACT.md` owns high-level architecture and handoffs; this wiki owns detailed
commands, formats, implementation, and procedures; `AGENTS.md` owns development
policy and workflow.
