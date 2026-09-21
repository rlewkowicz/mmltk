# Technical wiki

[Repository introduction and quick start](../README.md) · [Architectural contract](../CONTRACT.md) · [Agent policies](../AGENTS.md)

## Getting started and reference

- [Build and reusable state](build.md): prerequisites, container toolchains,
  package outputs, Firefox staging, declaration isolation, target-local PCHs,
  private native Parquet dependencies, generated bindings, caches, and build timing.
- [Commands](commands.md): wrapper operations, process snapshots, native CLI,
  benchmark cache selection, desktop options, and model tooling.
- [RF-DETR workflows and artifacts](rfdetr-workflows.md): model/class admission,
  GPU normalization, physical candidate ranking, separate candidate/COCO limits,
  shared weights selection, Transfer/Resume, automatic/manual output, training
  inputs and live progress, EMA, saved history,
  evaluation metrics and samples, and incremental image/video prediction.

## Architecture and frameworks

- [Architecture and source guide](architecture.md): entrypoints, native systems,
  shared native/image facilities, reflected CLI and persistence boundaries,
  serialization ownership and lookup, generated bindings, frontend components,
  and vendor ownership.
- [GUI interaction and presentation](gui-interaction.md): application wire
  formats, workflow layout and navigation, Dataset recipe/validation controls,
  the retained training dashboard,
  fixed Validation metrics/atlas, the shared image viewer and primary actions,
  retained numeric editing and captions, shared immediate mouse input, native
  command settlement, Annotation content/damage, retained Explore measurements,
  paired image geometry, Original aspect restoration, upscaling the selected
  source, importing displayed images, direct/copy acquisition, retained redraws,
  FPS, and resource lifetime.

## Data and backend systems

- [Datasets and compilation](datasets.md): source annotations and provenance,
  format 8 and recompilation, Stretch/Letterbox geometry, optional perceptual
  downscaling, quantized planar preparation, local batch capacity and leases,
  and Explore thumbnail/atlas retention through viewport and augmentation changes.
- [Built-in benchmark datasets](benchmark-datasets.md): Coco custom and COCONut
  membership, three validation choices, native masks and physical provenance,
  shared persistent cache/repair, stock annotation reuse, progress units,
  partial downloads, and format/capacity limits.
- [GPU execution and image loading](gpu-execution.md): device/NUMA placement,
  H2D and GDRCopy, reusable checkpoint/export readbacks, Vulkan allocation and
  CUDA import, independent display/compute selection and damage transfers,
  Upscale warming, provider capture, and capability inspection.

## Engineering, validation, and operations

- [Validation](validation.md): native/Rust formatting, cleanup, domain test
  ownership and selection, standalone CUDA/Vulkan and native-link diagnostics,
  rendered workflow/layout/input and
  retained browser acceptance, evidence, and debugging.
- [Headless Wayland](headless-wayland.md): the private NVIDIA Weston session,
  input seat, readiness, deadlines, shutdown, and artifacts.
- [Logging](logging.md): fatal stderr reports, explicit diagnostic activation,
  benchmark cache/archive/transfer traces, pixel probes, artifact ownership,
  rendered UI evidence, nested-field queries, Vulkan/FD provenance, correlation,
  and triage.
- [Planned work](roadmap.md): future directions rather than current capability.

`CONTRACT.md` owns high-level architecture and component handoffs. This wiki
owns detailed commands, formats, implementation explanations, and procedures;
`AGENTS.md` owns the development workflow.
