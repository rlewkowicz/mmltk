# Technical wiki

[Repository introduction and quick start](../README.md) · [Architectural contract](../CONTRACT.md) · [Agent policies](../AGENTS.md)

## Getting started and reference

- [Build and reusable state](build.md): prerequisites, container toolchains,
  package outputs, Firefox staging, declaration isolation, target-local PCHs,
  generated bindings, caches, and build timing.
- [Commands](commands.md): wrapper operations, process snapshots, native CLI,
  desktop options, and model tooling.
- [RF-DETR workflows and artifacts](rfdetr-workflows.md): model/class admission,
  shared weights selection, Transfer/Resume, automatic/manual output, training
  inputs and live progress, EMA, saved history,
  evaluation metrics and samples, and incremental image/video prediction.

## Architecture and frameworks

- [Architecture and source guide](architecture.md): entrypoints, native systems,
  shared Linux/image facilities, reflected CLI and persistence boundaries,
  generated bindings, frontend components, and vendor ownership.
- [GUI interaction and presentation](gui-interaction.md): application wire
  formats, workflow layout and navigation, the retained training dashboard,
  fixed Validation metrics/atlas, the shared image viewer and primary actions,
  numeric editing, shared immediate mouse input, native command settlement,
  retained Explore measurements, paired image geometry, direct/copy acquisition,
  retained redraws, FPS, and resource lifetime.

## Data and backend systems

- [Datasets and compilation](datasets.md): source annotations, the compiled
  binary format, optional perceptual downscaling, loading, batch leases, and
  Explore thumbnail/atlas retention through viewport and augmentation changes.
- [GPU execution and image loading](gpu-execution.md): device/NUMA placement,
  H2D and GDRCopy, reusable checkpoint/export readbacks, Vulkan allocation and
  CUDA import, independent display/compute selection and transfers, provider
  capture, and capability inspection.

## Engineering, validation, and operations

- [Validation](validation.md): native/Rust formatting, cleanup, domain test
  ownership and selection, standalone CUDA/Vulkan and native-link diagnostics,
  rendered workflow/layout/input and
  retained browser acceptance, evidence, and debugging.
- [Headless Wayland](headless-wayland.md): the private NVIDIA Weston session,
  input seat, readiness, deadlines, shutdown, and artifacts.
- [Logging](logging.md): fatal stderr reports, explicit diagnostic activation,
  pixel probes, artifact ownership, rendered UI evidence, nested-field queries,
  Vulkan/FD provenance, correlation, and triage.
- [Planned work](roadmap.md): future directions rather than current capability.

`CONTRACT.md` owns high-level architecture and component handoffs. This wiki
owns detailed commands, formats, implementation explanations, and procedures;
`AGENTS.md` owns the development workflow.
