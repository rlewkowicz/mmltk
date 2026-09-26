# Technical wiki

[Repository introduction and quick start](../README.md) · [Architectural contract](../CONTRACT.md) · [Agent policies](../AGENTS.md)

## Getting started and reference

- [Build and reusable state](build.md): prerequisites, container toolchains,
  package outputs, Firefox staging, declaration isolation, target-local PCHs,
  private native Parquet dependencies, embedded caption font, video encoders,
  generated bindings, caches, and build timing.
- [Commands](commands.md): wrapper operations, complete and focused test routes,
  process snapshots/actions, raw CPD and declaration formatting, canonical
  training requests, native CLI, benchmark cache selection and mask
  inspection/export, desktop options, and model tooling.
- [RF-DETR workflows and artifacts](rfdetr-workflows.md): model/class admission,
  GPU normalization, physical candidate ranking, separate candidate/COCO limits,
  shared weights selection, reserved workflow outputs, independent
  ONNX/TensorRT export, captured validation PNGs, prediction samples and recoverable
  Full video, training inputs, actionable local failures and live progress,
  EMA, saved history, compact matcher costs, evaluation metrics and samples,
  preview-only confidence, packed mask delivery,
  deterministic export ordering, and incremental image/video prediction.
- [Model merging and continuation](model-merging.md): periodic native averaging,
  final Off/Uniform/Explicit/Validation-greedy selection, immutable deployment
  candidates, whole-session Resume, current artifact formats, publication and
  generation leases, and the current GUI manifest-selection limit.

## Architecture and frameworks

- [Architecture and source guide](architecture.md): entrypoints, native systems,
  shared byte/image facilities, reflected CLI and persistence boundaries,
  serialization ownership and lookup, generated bindings, frontend components,
  and vendor ownership.
- [Reflected declarations and authoring](reflection.md): canonical native
  declarations, structural projection, syntax-only policy annotations, ordinary
  ownership, and conservative mechanical formatting.
- [GUI interaction and presentation](gui-interaction.md): application wire
  formats, workflow layout and navigation, Dataset recipe/validation/recovery
  controls and cancellation, shared form expansion/dividers, compact status
  fitting, shared Output/GPU placement and GPU selection, the bounded root Status
  panel, lane/recipe controls, model-aware live/saved training dashboard,
  fixed Validation metrics/atlas and progressive detail return,
  responsive overlay groups, the shared image viewer and primary actions,
  retained integer/decimal editing, prepared captions and texture bindings,
  shared immediate mouse input, native command settlement, Annotation mask work
  and content/damage, destination-owned routing, retained Explore measurements
  and gallery recovery, paired image geometry, the Original display choice,
  upscaling the selected source, importing displayed crop/aspect, direct/copy
  acquisition, retained redraws, FPS, and resource lifetime.

## Data and backend systems

- [RF-DETR training and selective compilation](rfdetr-training.md): pinned
  stock equations, global batch and logical lanes versus physical capacity,
  K-squared accumulation, AdamW/Muon/SGD recipes and schedulers, final-epoch
  policies, sparse data plans, optimizer/EMA policy, Match-Free/DN mask
  adaptations, native LibTorch/TorchScript regions, live parameters,
  signatures and ordinary fallback.
- [Datasets and compilation](datasets.md): source annotations and provenance,
  format 9 and recompilation, Stretch/Letterbox geometry, optional perceptual
  downscaling, CPU SIMD resizing, quantized planar preparation, local batch
  capacity and leases, source proportions in Explore tiles, and atlas retention
  through viewport and augmentation changes.
- [Built-in benchmark datasets](benchmark-datasets.md): Coco custom and COCONut
  membership, three validation choices, native masks and physical provenance,
  optional original-annotation mask recovery, derived cache identity and current
  counts, append-only rejection history, shared persistent cache/repair, stock
  annotation reuse, overlapping acquisition/labels/pixels, independent progress,
  typed transfer versus source totals, readable quantities, partial downloads,
  and format/capacity limits.
- [GPU execution and image loading](gpu-execution.md): native inventory and
  persisted workflow GPU selection, device/NUMA placement, complete session and
  stream retirement, bounded inference lanes and borrowed-input release,
  escaped pinned-storage authority, shared batch count storage,
  H2D and GDRCopy, RGB8 preview storage, reusable checkpoint/export readbacks,
  Vulkan allocation and CUDA import, independent display/compute selection,
  bounded sparse damage transfers, Upscale input preparation and warming,
  provider capture, and capability inspection.

## Engineering, validation, and operations

- [Validation](validation.md): the fixed final test/acceptance gate, unfiltered
  `all` native/JavaScript/Rust/cleanup-tool/log-query coverage and focused selectors,
  native/Rust formatting, ordinary cleanup and raw exact-spelling CPD evidence,
  declaration check/preview/fix, domain test ownership and selection,
  standalone CUDA/Vulkan and native-link diagnostics, retained browser workflow
  acceptance, Dataset presentation/cancel/restart, confidence/layout/input and
  direct Validate-to-Explore and Status evidence, training mathematics,
  distributed/compilation coverage and limits, media settlement/recovery, and
  debugging.
- [Headless Wayland](headless-wayland.md): the private NVIDIA Weston session,
  input seat, readiness, deadlines, shutdown, and artifacts.
- [Logging](logging.md): fatal stderr and actionable training causes, format-3
  training terminals, Catch/TAP/Rust outcomes, workflow GPU evidence,
  explicit diagnostic activation,
  benchmark cache/archive/transfer and release traces, pixel probes, artifact
  ownership, rendered UI and caption evidence, Dataset draw and probe records,
  archived-run selection, nested-field queries, Vulkan/FD provenance,
  explicit intent-request correlation, native compilation graph/fusion
  diagnostics, prediction media acceptance failures, empty test selections,
  and triage.
- [Planned work](roadmap.md): future directions rather than current capability.

`CONTRACT.md` owns high-level architecture and component handoffs. This wiki
owns detailed commands, formats, implementation explanations, and procedures;
`AGENTS.md` owns the development workflow.
