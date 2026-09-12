# Technical wiki

[Repository introduction and quick start](../README.md) · [Architectural contract](../CONTRACT.md) · [Agent policies](../AGENTS.md)

## Getting started and reference

- [Build and reusable state](build.md): prerequisites, container toolchains,
  package outputs, generated bindings, caches, and build timing.
- [Commands](commands.md): wrapper operations, native CLI, desktop options,
  and model tooling.

## Architecture and frameworks

- [Architecture and source guide](architecture.md): entrypoints, native systems,
  generated boundaries, frontend components, and vendor ownership.
- [GUI interaction and presentation](gui-interaction.md): application wire
  formats, independent annotation input, command settlement, exact displayed
  geometry, direct/copy acquisition, redraws, and physical resource lifetime.

## Data and backend systems

- [Datasets and compilation](datasets.md): source annotations, the compiled
  binary format, loading, batch leases, and retained Explore thumbnails/atlases.
- [GPU execution and image loading](gpu-execution.md): device/NUMA placement,
  H2D and GDRCopy, Vulkan allocation and CUDA import, provider capture, and
  capability inspection.

## Engineering, validation, and operations

- [Validation](validation.md): tidy, cleanup, test selection, standalone
  CUDA/Vulkan diagnostics, retained browser acceptance, evidence, and debugging.
- [Headless Wayland](headless-wayland.md): the private NVIDIA Weston session,
  input seat, readiness, deadlines, shutdown, and artifacts.
- [Logging](logging.md): explicit diagnostic activation, pixel probes, artifact
  ownership, nested-field queries, Vulkan/FD provenance, correlation, and triage.
- [Planned work](roadmap.md): future directions rather than current capability.

`CONTRACT.md` owns high-level architecture and component handoffs. This wiki
owns detailed commands, formats, implementation explanations, and procedures;
`AGENTS.md` owns the development workflow.
