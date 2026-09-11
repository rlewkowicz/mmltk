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
- [GUI interaction and presentation](gui-interaction.md): ordered annotation
  input, retained buffers, command settlement, publication, browser copies,
  redraws, and physical resource lifetime.

## Data and backend systems

- [Datasets and compilation](datasets.md): source annotations, the compiled
  binary format, loading, and batch leases.
- [GPU execution and image loading](gpu-execution.md): device/NUMA placement,
  H2D and GDRCopy, provider capture, and capability inspection.

## Engineering, validation, and operations

- [Validation](validation.md): tidy, cleanup, test selection, retained browser
  acceptance, evidence requirements, and debugging.
- [Headless Wayland](headless-wayland.md): the private NVIDIA Weston session,
  input seat, readiness, deadlines, shutdown, and artifacts.
- [Logging](logging.md): explicit diagnostic activation, artifact ownership,
  query syntax, correlation, and triage.
- [Planned work](roadmap.md): future directions rather than current capability.

`CONTRACT.md` owns high-level architecture and component handoffs. This wiki
owns detailed commands, formats, implementation explanations, and procedures;
`AGENTS.md` owns the development workflow.
