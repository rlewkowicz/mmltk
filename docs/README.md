# Technical wiki

[Introduction](../README.md) · [Architecture contract](../CONTRACT.md) · [Engineering policy](../AGENTS.md)

## Getting started and reference

| Guide | Use it for |
| --- | --- |
| [Build](build.md) | Bootstrap, toolchains, packaging, generated files, and caches |
| [Commands](commands.md) | Wrapper, application CLI, process inspection, and model tooling |
| [RF-DETR workflows](rfdetr-workflows.md) | Models, inputs, outputs, training artifacts, validation, prediction, and export |
| [Merging and continuation](model-merging.md) | Session formats, Resume, Transfer, averaging, and final selection |

## Architecture and frameworks

| Guide | Use it for |
| --- | --- |
| [Source ownership](architecture.md) | Systems, components, dependencies, and source locations |
| [Reflection](reflection.md) | Canonical declarations, schema authoring, and native projections |
| [Application wire](application-wire.md) | Generated Rust, protocol records, validation, and transport |
| [GUI interaction](gui-interaction.md) | Iced state, controls, Live analysis, viewer behavior, rendering, and graphics custody |

## Data and backend systems

| Guide | Use it for |
| --- | --- |
| [RF-DETR training](rfdetr-training.md) | Objectives, recipes, lanes, precision, and native compilation |
| [Datasets](datasets.md) | Directory inputs, compiled formats, keypoints, augmentation, and resampling |
| [Compilation execution](compile-execution.md) | Scheduling, pinned storage, cancellation, and file settlement |
| [Benchmark datasets](benchmark-datasets.md) | Recipe selection, mask recovery, progress, cache formats, repair, and compiler internals |
| [Benchmark performance](benchmark-performance.md) | Current measurement methods and interpretation |
| [GPU execution](gpu-execution.md) | Devices, transfers, inference lanes, readbacks, and Wayland/Vulkan/CUDA handoffs |

## Validation and operations

| Guide | Use it for |
| --- | --- |
| [Validation](validation.md) | Focused tests, tidy, deduplication, numerical gates, profiles, and acceptance |
| [Headless Wayland](headless-wayland.md) | Private NVIDIA Weston setup, input, readiness, deadlines, and shutdown |
| [Logging](logging.md) | Transcripts, opt-in diagnostics, fatal reports, log queries, and artifact custody |

`CONTRACT.md` owns architectural outcomes. These guides own current commands,
formats, behavior, and procedures. `AGENTS.md` owns engineering policy.
