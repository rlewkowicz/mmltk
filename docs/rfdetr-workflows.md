# RF-DETR workflows and artifacts

[Wiki index](README.md) · [Commands](commands.md) · [Datasets](datasets.md) · [Training reference](rfdetr-training.md) · [GUI layout](gui-interaction.md#training-validation-and-prediction)

Train, Validate, Predict, and Export share model selection/preparation. Their
[primary action](gui-interaction.md#training-validation-and-prediction) settles
settings, selects/prepares weights, inspects required compiled inputs, and starts
the owning system. Preparation failure ends that request with its typed error
in session Status. The model card shows preparation; the workflow shows execution
progress.

Train launches the packaged sibling CLI through `TrainingSystem`. Validate and
Predict own independent native sessions, GPU work, cancellation, and retained
image products. See the [source guide](architecture.md#native-domain-work) for
their locations. Each workflow captures its own
[GPU selection](gpu-execution.md#workflow-device-selection) at admission;
Train retains ordered multi-device execution and remote-provider behavior.

## Backend ownership

The backend keeps model execution and decoded-state admission in
[ordinary core declarations](architecture.md#native-domain-work). Within
`src/backend/models/rfdetr/training/`, the training loop coordinates these
execution and state boundaries through typed APIs:

| Declaration | Responsibility |
| --- | --- |
| [detail/training_lanes.h](../src/backend/models/rfdetr/training/detail/training_lanes.h) | Physical training workers, queued logical contributions, gradient handoff, and event custody |
| [detail/training_metrics.h](../src/backend/models/rfdetr/training/detail/training_metrics.h) | GPU scalar accumulation and the completed metric handoff |
| [detail/training_snapshot.h](../src/backend/models/rfdetr/training/detail/training_snapshot.h) | Ordinary/EMA serialization snapshots and continuation save/load coordination |
| [detail/native_optimizer_private.h](../src/backend/models/rfdetr/training/detail/native_optimizer_private.h) | Typed AdamW/Muon/SGD state, parameter groups, update, and archive operations |
| [detail/target_builder_private.h](../src/backend/models/rfdetr/training/detail/target_builder_private.h) | Target staging, scratch storage, and consumer leases |
| [detail/evaluation_runtime.h](../src/backend/models/rfdetr/training/detail/evaluation_runtime.h) | Evaluation lanes, prediction buffers, scheduled validation lifetime, and retained sample-output writer |
| [checkpoint.h](../src/backend/models/rfdetr/training/checkpoint.h) | Ordinary checkpoint application, normalization, weight loading, and continuation inspection API |

Runtime definitions stay in the corresponding ordinary source files; sources
that consume retained modules remain registered importers. Canonical
[training metric declarations](../src/backend/models/rfdetr/contract/training_metrics.h)
own persisted/browser record structure, independently of these resource owners.
The [build reference](build.md#target-declarations-and-precompiled-headers)
describes compilation and header isolation.

The [ONNX lowerer](../src/backend/models/rfdetr/export/onnx_lowering.cpp)
orders ready nodes with a min-heap of their original ordinals. Each emitted
node is the earliest eligible node in the original block, including nested
blocks; ready-queue insertion/removal costs O(log V) for V nodes. Dependency
deduplication, cycle rejection, and exported order retain their existing rules.

## Class identity and model admission

The immutable [ClassCatalog](../src/backend/data/catalog/class_catalog.h) owns
exact foreground names and dense zero-based references. Four identities stay
distinct:

| Identity | Meaning |
| --- | --- |
| Source category ID | Identity supplied by the dataset adapter |
| Foreground reference | Dense index into the owning catalog |
| Model output slot | Physical classifier axis, which may include unused or background slots |
| External output ID | Identity required by an external export format |

[Compiled class fields](datasets.md#compiled-binary-format) preserve source
category identity separately and reject name truncation. A training run's
train/validation/test splits require the same ordered catalog; standalone
evaluation permits an explicitly verified permutation of the same exact names.

[ModelClassLayout](../src/backend/models/rfdetr/contract/class_layout.h) records
the ordered foreground catalog, each output slot's role, score encoding,
no-object encoding, and provenance. Fresh native training uses sigmoid logits,
one trailing unused output slot, and all-negative no-object supervision.
Background and unused slots participate in physical ranking and are discarded
after selection, as described [below](#model-input-and-detection-selection).
Current RF-DETR execution requires sigmoid logits; declaring a softmax layout
does not make that execution mode supported.

Native version-4 checkpoints embed the layout. External assets may supply
supported embedded metadata, verified asset metadata, or a digest-bound
descriptor selected with `--class-layout`. The version-1
`ModelClassDescriptor` JSON record contains the artifact SHA-256, layout, and
optional named output roles; its canonical declaration and
[codec/admission implementation](../src/backend/models/rfdetr/core/class_layout.cpp)
define the format. Admission validates artifact/descriptor identity, output
width, roles, and agreement before installing the model. Filenames, tensor
counts, and a bare list of names do not establish a semantic mapping.

An unresolved external layout can produce visibly raw output-slot references,
but cannot enter semantic evaluation. Predictions and annotation products carry
their reference domain and catalog with the data. Mask buffers are used only
when the current result declares masks available.

Fresh transfer initialization maps verified class-dependent state by exact
semantic identity, including applicable classifier, encoder, denoising, and
match-free class axes. Unmatched destination state keeps its initialized value.
Resume instead requires the exact native class layout and compatible saved
state; it does not perform transfer remapping.

## Model input and detection selection

Compiled files contain planar RGB float32 in `[0,1]`, using the
[stored resize geometry](datasets.md#resize-geometry). The shared
[GpuBatchPreprocessor](../src/backend/models/rfdetr/core/gpu_batch_preprocessor.h)
normalizes evaluation and prediction input once on the GPU with
`(RGB - mean) / std`, using mean `[0.485, 0.456, 0.406]` and standard deviation
`[0.229, 0.224, 0.225]`. Training augmentation uses these same normalization
constants. Borrowed compiled pixels and retained preview pixels remain
raw; normalization writes reusable, separately owned model-input storage and
settles its consumers before reuse. Native, ONNX, and TensorRT prediction
paths share this preparation, including compiled, ordinary-image, and video
inputs.

[Postprocessing](../src/backend/models/rfdetr/core/postprocess.cpp) applies
sigmoid over the full physical query-by-class output, stably ranks flattened
scores, and selects the requested candidate count. Equal scores retain physical
flattened order. Only then does the admitted class layout remove unused and
background slots and project foreground references; discarded candidates are
not refilled. Boxes, scores, classes, and masks retain the same selected query
identity. Continuous predicted corners clip to the model canvas, then to the
stored image-content rectangle for compiled Letterbox input.

These counts serve different purposes:

| Count | Authority and meaning |
| --- | --- |
| Physical query count | Model architecture and admitted training/checkpoint configuration |
| Candidate count (`num_select`) | Model configuration's default physical ranking budget; validation's `--candidate-count 0` selects it |
| Output capacity | Checked storage bounded by the requested budget and available physical query/class pairs |
| Surviving detections | Actual valid entries after semantic filtering and any requested confidence threshold; may be fewer than capacity |
| COCO maxDets | Per-image, per-category evaluation caps, independent of the model candidate budget |

The existing [preset catalog](../src/backend/models/rfdetr/contract/preset_catalog.h)
supplies these default candidate counts:

| Task and preset sizes | Candidates |
| --- | ---: |
| Detection Nano, Small, Medium, Large | 300 |
| Segmentation Nano, Small | 100 |
| Segmentation Medium, Large | 200 |
| Segmentation XLarge, 2XLarge | 300 |

Admitted custom configurations and resumed checkpoints retain their own
selection counts. CLI `evaluate` and `validate` expose `--candidate-count`
independently of `--eval-max-dets`; zero selects the model default and shared
evaluation default respectively. Prediction's `--max-dets-per-image` selects
its candidate/output limit, defaulting to 500; zero uses the admitted model's
selection count. It does not change COCO evaluation policy.

## Shared weights selection

Train, Validate, Predict, and Export use the same **RF-DETR Weights** card:
catalog presets and **Custom Weights**, with a compact selected path and active
preparation progress. Operational errors appear in the session
[Status panel](gui-interaction.md#session-status). Train's chooser accepts
trainable weights (`.pt`, `.pth`, `.ckpt`,
`.safetensors`). Validate and Predict also accept ONNX (`.onnx`) and TensorRT
(`.engine`, `.trt`). Export's custom chooser accepts `.pt` weights. The selected
extension identifies an input kind through
the native [compatibility catalog](../src/controller/contracts/model_selection.h);
normal artifact and class admission still apply.

The shared card has no separate backend-kind or companion class-layout control
for these workflows. A new custom selection clears an unrelated descriptor
path. Native descriptors and CLI `--class-layout` retain their supported
admission route. Only Train adds Transfer/Resume controls; Validate has no
continuation preparation or training-output state. The shared component owns
custom-selection confirmation, pending dialog admission, preparation, and
cancellation; each workflow retains independent widget identities and state.

## Run output directories

Each workflow uses the shared [right-column layout](gui-interaction.md#training-validation-and-prediction).
**Auto Output** is enabled initially. Accepted execution reserves the next
`run-NNNN` beneath the [canonical workflow root](../src/controller/contracts/workflow_output.h),
starting at `run-0001` and growing beyond four digits when needed. Existing
entries are never overwritten.

| Workflow | Automatic root | Products within a run |
| --- | --- | --- |
| Train | `./output/train` | [Checkpoints](#checkpoints-and-continuation), [history](#saved-history-and-plots), epoch samples, and training-owned evaluation |
| Validate | `./output/validate` | `report.json`, up to six `samples/sample-<dataset-index>.png` |
| Predict | `./output/predict` | Image/compiled `predictions.json` when enabled, plus the source-specific [saved media](#prediction-samples-and-full-video) |
| Export | `./output/export` | Requested `model.onnx` and/or `model.engine`, with applicable class companions |

**Browse Output** selects a manual directory. A new or empty selection is used
directly; an occupied directory receives a fresh numbered child. An exclusive
`.mmltk-run-claim` directory reserves the destination, including a run with
media/report saving disabled. Selecting or preparing inputs never reserves a
run. Reservation follows native input admission, and filesystem errors refuse
execution without overwriting earlier products. The shared
[reservation utility](../src/controller/services/training/run_output.cpp) owns the
filesystem algorithm; each workflow owns its run and terminal outcome.

Configured selections remain distinct from active/completed paths. The card
shows the admitted directory, committed artifacts, sample count/directory and
recent sample, or a recoverable partial-video path. A different manual selection
appears as **Next output**. Re-enabling Auto clears the manual selection without
deleting files. Settings persist current selections rather than an automatic
run's resolved path. Current-format Train history and Resume have the extra
admission described below; older application settings and saves have no
migration route. CLI output arguments remain explicit and independent of these
GUI reservations.

## Export formats

**ONNX** and **TensorRT** are independent checkboxes, both initially enabled.
Selecting neither disables **Run Export**; native admission also rejects that
request before directory reservation or model work.

The [export run](../src/controller/subsystems/export/export_run.cpp) converts
the prepared weights to ONNX once. TensorRT consumes that same ONNX. ONNX-only
retains `model.onnx`; both formats retain `model.onnx` and `model.engine`.
TensorRT-only keeps the ONNX and its class companions in a private temporary
directory until engine readers settle, then removes the intermediate.
Failure or cancellation preserves already committed requested artifacts,
including ONNX when a later engine build fails. Format choices do not change
the prepared model identity. Native `export-onnx` and `build-engine` remain
separate reusable CLI operations.

## Training inputs and output destination

Train requires compiled train and validation splits. **Infer train/validation
splits** resolves `train.bin` and `val.bin` from the compiled output directory.
Dataset source and compiled output retain their browse buttons. With inference
disabled, text fields expose the train, validation, and optional test paths;
there are no per-split browse buttons. The stored optional test path survives
inference toggles and is not inferred from `test.bin`. Clear that text field
with inference disabled to disable the final test.
An absent test split does not block training. A selected test split must be a
readable compiled artifact with the same ordered class catalog as train and
validation, or native admission rejects the start. The
[request materializer](../src/controller/subsystems/system/compute_intent_materializer.cpp)
and [dataset inspection](../src/controller/subsystems/system/dataset_system.cpp)
own these checks.

Train uses the shared [output reservation](#run-output-directories). Its Output
card additionally owns saved-history selection.

Browse Output switches to manual selection and loads supported saved charts.
An empty or unrelated folder clears the saved charts; a folder claiming history
through either `run.json` or `metrics.jsonl` must satisfy current-format admission
or reports an error. Browsing changes neither weights nor Transfer/Resume mode.
Re-enabling Auto clears the manual selection. Start selects live charts.

Transfer may use an absent or empty manual destination directly. A populated
destination gets a new numeric `run-*` child. Native Resume output admission
reuses a manual run directory only when the admitted session manifest is in
that directory and its session, attempt, evaluated-weight choice, and class
layout match the current history. Otherwise
Resume reserves a fresh child, including for an empty manual destination.
Automatic mode always reserves a fresh child. The resolved path appears when
the run is admitted. This GUI policy belongs to
[TrainRunStore](../src/controller/services/training/train_run_store.cpp); CLI training
uses its explicit `--output-dir`.

Validate's **Open Dataset** selects an independent override. While that override
is empty, its compact effective path follows Train's validation split, including
inferred paths. Native settings own this resolution. Changing an inherited
source cancels an unstarted Validate request just as changing an explicit
selection does.

## Local training failures

An unsuccessful local run publishes the child's bounded useful failure cause
and exit status to [session Status](gui-interaction.md#session-status). Signal
termination retains the signal number. An
explicit **CUDA out of memory** cause includes available allocator detail and
guidance to reduce **batch size** or **training lanes**, then start again.
The application preserves the configured workload and does not retry or tune
training automatically. A signal or generic unsuccessful exit alone is not
classified as CUDA OOM.

The [failure reporting reference](logging.md#local-training-failures) owns
process-output extraction, terminal projection, and quiet fatal reporting.
Training equations, Match-Free/DN objectives, AMP, and batch admission retain
the [training reference](rfdetr-training.md) contracts.

## Training and query limits

Continuous dataset boxes remain detection targets through geometric
augmentation. Mask support and occlusion are separate from box authority;
mask disappearance alone does not delete a valid detection target. Crowd
annotations remain in the compiled data for evaluation but are excluded from
foreground supervision and copy-paste donors. Segmentation training requires
present masks for its non-crowd targets; a present empty mask stays distinct
from a missing one. The Hungarian focal classification cost uses the configured
focal alpha in both ordinary and CUDA matcher paths.

An image may contain more targets than the resolved query count. All eligible
targets remain available to supervision; the SciPy-derived rectangular assignment
solver still matches at most the smaller matrix dimension in each query group.
This changes target admission and storage sizing, not the solver or loss
definition. Automatic query selection still respects the model's automatic cap,
explicit overrides retain their validation, and resume retains the checkpoint's
query and selection counts.

The implementation starts at
[train.cpp](../src/backend/models/rfdetr/training/train.cpp) and
[detection_ops.cpp](../src/backend/models/rfdetr/core/detection_ops.cpp).
`run.json` records the observed dataset limits and resolved query policy.

Each training lane's
[MatcherWorkspace](../src/backend/models/rfdetr/core/detail/matcher_workspace.h)
packs float32 costs as contiguous per-layer, per-image matrices. For layer query
counts `Q_l` and image target counts `T_i`, the active payload contains
`sum_l(sum_i(Q_l * T_i))` values. Checked layer and target prefixes locate those
matrices; input target lookup offsets remain separate from compact output
offsets. Device and GPU-local pinned host buffers retain high-water capacity,
and one settled D2H copy exposes the complete active payload to the existing
solver. The former padded-shape overflow admission remains enforced.

The [CUDA cost kernels](../src/backend/models/rfdetr/core/detr_matcher_cuda.cu)
write those compact addresses while retaining full-domain nonfinite
sanitization before group/image assignment. The
[training reference](rfdetr-training.md#stock-objective-and-mask-mathematics)
owns the stock cost and loss equations and exceptional cases. Separately, the
[deformable-attention forward wrapper](../src/backend/ml/layers/ms_deform_attn_cuda_wrapper.cpp)
allocates overwrite-only output because the forward kernel fills every active
element; backward accumulation retains its required zero initialization.

EMA is optional and **off by default**. The GUI's **Exponential moving average**
setting and CLI `--use-ema` enable persistent GPU shadow weights, updated at the
completed optimizer-attempt boundary, including recoverable AMP skips.
The [training reference](rfdetr-training.md#accumulation-optimizer-and-ema)
defines first-copy, later averaging, and continuation behavior. `--no-ema` disables them. Disabled EMA creates no
shadow storage or update work.

Evaluation temporarily selects EMA, then restores working weights and training
mode. [Merging/selection](model-merging.md) owns ordinary-versus-EMA trajectories,
periodic synchronized validation, frozen final selection/test, and publication.
[Training](rfdetr-training.md) owns pinned Hungarian mathematics (including the
extra accumulation divisor), opt-in Match-Free/DN box/mask objectives, and
selective tracing; [validation](validation.md#training-mathematics-and-compilation-evidence)
owns their evidence limits.

## Checkpoints and continuation

[Artifact formats](model-merging.md#artifact-layout-and-formats) and
[whole-session admission](model-merging.md#whole-session-resume) govern Resume:
even one model requires `session.json` and its complete immutable generation.
Individual archives/candidates authorize Transfer only.

Train's weights card owns mutually exclusive **Transfer** and **Resume** radios.
Catalog and weights-only inputs use Transfer and cannot Resume. An admitted
resumable custom selection defaults to Resume. Selecting a file or changing the
radio never starts work. The current custom chooser/confirmation accepts weights
extensions and rejects a newly selected `.json` manifest; Browse Output only
loads history. A custom path already present in settings follows native
inspection and the existing Prepare Resume path. The CLI accepts a manifest
through `--resume`; see [whole-session Resume](model-merging.md#whole-session-resume).

Custom selection starts worker-owned cancellable inspection of the archive,
continuation, optimizer inventory, and required EMA state. Its compact capability
retains immutable evidence with identity rechecked on use, as defined by
[Resume admission](model-merging.md#whole-session-resume). Confirming the same
path refreshes inspection; errors remain actionable without automatic retry.

Start in Resume mode restores the saved training settings, settles that
restoration, and passes the native continuation admission used by CLI training.
Only the still-current explicit Start request may proceed after preparation.
A path or run-history manifest alone cannot make an artifact resumable.
See the [checkpoint API](../src/backend/models/rfdetr/training/checkpoint.h),
[checkpoint I/O](../src/backend/models/rfdetr/training/checkpoint_io.cpp),
and [continuation validation](../src/backend/models/rfdetr/training/training_continuation.cpp).

## Live training progress

The separate progress card follows the active local run even while the dashboard
shows saved history. It disappears when that run completes, fails, or is
cancelled. Preparing a model remains a separate stage in the model card.

During the Train phase, the card uses native `completed_images` and
`total_images` from the current epoch. These are **global logical image counts**
for the reported source, not optimizer steps or a multiplication of rank-local
observations. Native planning derives the total from complete admitted windows
and reports unused tails separately; completion counts each processed global
microbatch once. The GUI
does not reconstruct either count from dataset size or editable batch settings.
See [train.cpp](../src/backend/models/rfdetr/training/train.cpp) and the
[progress declaration](../src/backend/models/rfdetr/contract/training_metrics.h).

A positive known total supports the image progress bar. The card shows the
native epoch elapsed time and measured images/second; ETA estimates the
remaining epoch images from that rate. Rate and ETA remain unavailable until
there is a loss observation, completed work, and positive finite elapsed time
and rate. Total, classification, and box losses likewise show only available
finite observations. Starting, validation, stopping, and other non-Train phases
show their phase without a stale image bar or loss/rate display.

## Saved history and plots

Current run manifests and metric records use **format version 3**, including
typed source scope, session/model identity, round/merge facts, distributions,
execution facts, and selected-output metadata. Older output directories are
rejected; there is no compatibility reader or migration. The
[continuation formats](model-merging.md#artifact-layout-and-formats) and
[compiled dataset format](datasets.md#compiled-binary-format) are independent.

| File | Authority |
| --- | --- |
| `run.json` | Run/attempt identity, training configuration, execution/query facts, source catalog, class layout, selected evaluation weights, and resume provenance |
| `metrics.jsonl` | Append-only typed metric records, including sequence, attempt, role, missing-record count, and progress |
| `progress.json` | Latest typed record plus retained source state and optional final result, consumed by the training process client |
| `log.txt` | Epoch summary projections |
| `results.json` | Final result projection |

The declarations are in
[training_metrics.h](../src/backend/models/rfdetr/contract/training_metrics.h).
The independent [telemetry writer](../src/backend/models/rfdetr/training/telemetry_writer.cpp)
owns serialization and file writes. It materializes each typed record's JSON
once, appends it to `metrics.jsonl`, then moves that value into the
`progress.json` projection. History append still precedes progress publication;
epoch and final projections follow their existing write order.
Training submits bounded records without waiting for charts, browser delivery,
or telemetry disk I/O. Intermediate live records can coalesce; epoch and terminal
records have reserved queue custody.
Contention, capacity exhaustion, or persistence failure marks history incomplete
and produces a session Status notice while training continues. Checkpoint failures keep
their ordinary operation-failure behavior.

Live samples are submitted at most once per second, with immediate phase,
epoch, and terminal updates. Scalars extend the existing loss handoff;
learning rates come from optimizer groups and schedule state. Optional values
remain unavailable when the relevant loss or metric was not computed.
Hungarian main loss components are raw, while match-free main components are
weighted. Auxiliary and denoising groups are separate weighted values;
correspondence is an overlapping breakdown. Only total represents the complete
optimized objective, so these displayed components must not simply be summed.

**Browse Output** selects saved history without starting training. The frontend
automatically fetches pages of at most 32 records into the bounded dashboard;
Start returns it to live data. The reader uses
generation and byte-boundary cursors, rejects replaced/truncated streams, and
does not advance past an incomplete trailing append. Opening history requires
both a valid current manifest and its `metrics.jsonl`; a directory with neither
is a valid empty history selection.

Native source identity is `(scope, model_id, weights)`: model,
synchronized-session, selected-output, or session scope, with Ordinary, Ema, or
Soup weights. Shared mode uses model ID zero. Independent/periodic modes retain
stable model IDs; periodic ordinary validation uses synchronized-session scope.
The default chart source prefers the run's selected weight kind and then the
lowest model ID. Saved and live views retain separate source selections.

Scheduled evaluation contributes one sparse observation per source and recorded
epoch. Live and terminal records carrying the latest evaluation do not duplicate
it. Missing or unavailable measurements remain gaps. **Selected output** shows
the final artifact, method, ingredient coefficients/checksums, and validation
summary separately from epoch curves. Native first-cause identities preserve
per-model failures. Optional final-test results remain native; the GUI has no
final-test chart trajectory. The
[training dashboard](gui-interaction.md#training-dashboard) owns chart selection,
axes, retained interaction, bounded summaries, and rendering behavior. Throughput
belongs to live progress rather than a dashboard curve.

## Evaluation metrics and retained samples

GUI Validate evaluates the selected model/backend once. CLI `rfdetr evaluate`
also selects one backend; CLI `rfdetr validate` retains its ordered
multi-backend comparison/report behavior. The
[evaluation declarations](../src/backend/models/rfdetr/contract/evaluation_metrics.h)
and [evaluator](../src/backend/models/rfdetr/core/evaluator.cpp) define:

- Box and, when available/requested, mask AP over IoU 0.50–0.95 in 0.05 steps,
  with AP50 and AP75.
- AR at per-image, per-category detection caps `1`, `min(10, cap)`, and the
  resolved evaluation cap. The shared default is **[1, 10, 500]** for boxes
  and masks, independent of model candidate counts; an explicit
  `--eval-max-dets` changes the cap.
- All/small/medium/large area and per-class details, with 101-point interpolated
  precision-recall curves.
- Precision, recall, and F1 at the threshold maximizing macro F1 over classes
  with ground truth on the 101-point 0.00–1.00 confidence grid. The selected
  threshold is reported; ties retain the first threshold. These confidence metrics use
  IoU 0.50 and are not averages across the AP IoU axis.

The shared evaluator uses stored source-area metadata for both box and mask
area ranges. Generic compilation's [area fallback](datasets.md#instance-records)
applies when the source omitted area; optional COCONut
[mask recovery](benchmark-datasets.md#optional-dropped-mask-recovery) supplies
the area of its recovered or carved annotations before resizing. Small, medium,
and large ranges share the inclusive boundaries at `32²` and `96²` source pixels.
Unmatched detections
outside the current area range are ignored; their box or mask area is converted
back to source units using the stored resize geometry.

For COCO bbox/segmentation preparation, crowd determines the initial ignore
state. Raw `ignore` metadata is preserved but is not OR-ed into that state.
Nonignored ordinary ground truth has matching priority over ignored ground
truth. Crowd overlap divides intersection by detection area and permits repeated
matches. Matched crowd detections contribute neither true nor false positives,
and crowd is excluded from the positive denominator. Area-ignored
ground truth also stays outside the positive denominator. Matching preserves
source order, including distinct duplicate annotations; equal-IoU choices
follow the last annotation within the same ignore group. Score ties retain
stable prediction order through 101-point precision accumulation.

Per-category selection partially sorts only the retained prefix when predictions
exceed maxDets, using the same score, NaN, and source-index ordering as the full
sort. Matching stops scanning candidates once all ten IoU thresholds for that
detection and area have settled. These shortcuts preserve the ordering and
ignore/crowd rules above.

No eligible ground truth produces unavailable metrics rather than a fabricated
zero result. Box-and-mask evaluation requires every ground-truth annotation to
declare a mask; a declared empty mask is valid. Masks remain rasters at compiled
resolution. Original-area metadata and continuous boxes do not recover discarded
source segmentation detail or establish source-resolution segmentation parity.
Exact AP retains compact matching records proportional to evaluated detections;
it is not a constant-memory statistic.

Validation captures pixels, predictions, and ground truth for up to six distinct
random images from the evaluated population during that same pass.
The [atlas/detail viewer](gui-interaction.md#validation-workspace-and-shared-viewer)
reuses them, leaving empty cells for a smaller population.
[ValidationSamples](../src/controller/subsystems/validate/detail/validation_samples.cpp)
keeps progressive population membership separate from the immutable membership
used by displayed detail. Opening detail freezes that view while capture
continues. Successful settlement retains all useful captures, and closing
detail returns to that population without repeating inference or source
preparation. Selection uses the identity paired with displayed pixels,
including during replacement or partial progress.

A detail view from an earlier generation keeps its incumbent return atlas
while a newer run is incomplete. A failed or cancelled replacement restores
the incumbent products. Render refusal keeps the last completed image and its
paired metadata, permits one automatic retry per request, and allows an
explicit close retry after ordinary refusal is exhausted. Native render
completion retires current capture bookkeeping only when its atlas matches
the full requested population. Logical settlement and browser consumption
remain independent. [GPU execution](gpu-execution.md#workflow-runtime-retirement) owns physical
retirement and borrowed-frame lifetime.

Validate's [Display confidence](gui-interaction.md#display-confidence) filters
only those retained preview detections. It is independent of the evaluator's
reported macro-F1 threshold, candidate selection, COCO accumulation, and CLI
reports. Preview edits preserve the evaluation generation, results, and raw
samples; ground-truth import remains unchanged.

The twelve fixed GUI COCO summary rows are AP 50:95, AP50, AP75, AP small/medium/large, AR at the
three recorded caps, and AR small/medium/large. Boxes and available Masks have
separate columns; unavailable values show `—`. Native detail queries support
pages of at most four rows, though the [view](gui-interaction.md#validation-workspace-and-shared-viewer)
has no metric, IoU, recall, or detail-page controls.

At the accepted primary Start, Validate captures layer visibility, label
visibility, boxes/masks, and Display confidence in a typed
[ValidationRunPreview](../src/controller/contracts/validation_display.h).
That payload survives settings settlement and model preparation. The native
[sample-output owner](../src/controller/subsystems/validate/detail/validation_sample_output.cpp)
saves each selected identity once as `samples/sample-<dataset-index>.png` at
complete native geometry, using class names and the [Validation composition rules](gui-interaction.md#validation-workspace-and-shared-viewer).
Viewer edits, pan/zoom/scroll, and browser availability cannot change the captured
save. Required PNG writes settle before success.
A save failure preserves computed metrics, the completed report and PNGs,
and reports the image-output error. Optional interactive adoption does not
control metrics or required saves.

Scheduled training evaluation separately writes `eval_samples/epoch_N.png`
with one-based epoch `N`. Shared mode uses the run root; independent/periodic
model evaluation uses `model-<ID>/ordinary/` or `model-<ID>/ema/` beneath it.
[TrainingValidationRuntime](../src/backend/models/rfdetr/training/evaluation_run_owner.cpp)
owns an [EvaluationSampleWriter](../src/backend/models/rfdetr/core/sample_output.h)
that lazily retains its CUDA device, worker, settlement stream, and event across
epochs. One pending future bounds output; explicit `Flush` propagates errors,
and destruction settles queued image custody. Independent validation runtimes
can write on different devices without sharing a process-global writer.
These training-owned mosaics retain their numeric-category caption behavior;
they are separate from standalone Validate's individual named PNGs and do not
reserve a standalone Validate run.

## Incremental prediction

GUI Predict accepts a compiled dataset, one ordinary image, or one local video
file. It always materializes **batch size 1** and has no batch-size control.
CLI `rfdetr predict` retains `--batch-size`, compiled input, and repeatable
`--image` inputs; local-video selection belongs to the GUI.

[PredictionSession](../src/backend/models/rfdetr/inference/predict.cpp) retains
its admitted backend and reusable working storage. Completed records are
delivered incrementally, with source pixels requested only for consumers that
need them. Predict keeps the latest preview instead of retaining all run
images. Borrowed source storage remains held until receiver copies finish;
semantic prediction can still complete when a contained preview failure occurs.

Only threshold survivors materialize requested masks, in bounded chunks.
[PredictionMaskReadback](../src/backend/models/rfdetr/inference/prediction_capacity.h)
chooses packed readback when its extra device bytes plus page-rounded pinned
host bytes are smaller than the dense host payload and the complete retained
allocation inventory fits the existing chunk budget. Otherwise encoding uses
dense boolean readback. Packing does not enlarge survivor/chunk admission;
preview-only demand performs no encoded-mask host readback.

The [existing raster packer](../src/backend/models/rfdetr/core/mask_pack_cuda_wrapper.cpp)
stores row-major pixels low bit first in `ceil(width * height / 8)` bytes per
mask on the current CUDA stream, including Torch's legacy default stream.
Host encoding begins after that stream settles. The shared
[packed RLE encoder](../src/backend/models/rfdetr/core/evaluator.cpp) scans bounded
64-bit words, carries runs across word boundaries, and ignores unused tail bits.
It preserves scalar start/length runs, area, aggregate run limits, and partial
state on capacity failure. Storage grows with emitted runs rather than the
full run allowance. GPU preview masks retain their own representation and
custody; [preview storage](gpu-execution.md#prediction-preview-storage) owns
decoded RGB8 and CHW pixel handling.

Video decoding, frame timestamps, and end-of-file are owned by
[VideoFileSource](../src/backend/media/video/video_file_source.h).
Predict supplies pacing and interruptible Pause/Resume/Stop. Unknown frame
totals show activity/completed work instead of false percentage completion.
Controls share the owning system's pending-operation admission, including when
a pause event arrives before its reply. EOF completes the run; Stop cancels it.
The latest completed preview remains available after either outcome.

Image and compiled-dataset JSON is enabled by default through the native
`write_report_json` setting, independently of the media-saving controls.
When enabled, `predictions.json` is streamed
to a temporary sibling file and published by rename only on success.
Cancellation/failure removes the temporary file and preserves an existing
destination. The document records source/model/backend facts, `class_domain`,
`class_layout`, and per-image records. Masks use
`row_major_start_length`; semantic names come only from the admitted catalog.
Video writes no prediction JSON, including with media saving disabled.

## Prediction samples and Full video

Predict's Output card retains independent saving choices for each source:

| Source | Initial choice | Saved product |
| --- | --- | --- |
| Single image | **Save sample** enabled | `sample.png` |
| Compiled dataset | Saving enabled, **Percent %** = 10 | `samples/sample-<dataset-index>.png` |
| Video | Saving enabled, **Full** selected; Samples count = 6 | `prediction.mkv`, or `samples/frame-<decoded-index>.png` in Samples mode |

Compiled **Percent %** accepts integers 1–100 and saves
`ceil(dataset_count * percent / 100)`. **Total** accepts 1 through the matching
inspected dataset count and initially clamps 6 to that count. The input is
disabled until that source's count is known; source changes clamp it and native
Start revalidates the population. Smaller selections are uniform subsets
without replacement; a full selection needs no random index storage.
Selection retains the smaller of the selected and excluded index sets.
Saving leaves inference order and `limit_images` unchanged. If an independent
inference limit admits fewer images than the requested sample count, Start
refuses that combination instead of changing coverage.

Video **Samples** uses reservoir sampling over actually decoded frames, with
bounded storage proportional to the requested count. Declared frame totals
are advisory. A replacement PNG publishes completely before the previous slot
file is removed. A request larger than the decoded population preserves all
available samples and fails with an explicit requested/observed/saved shortfall.
Decode and write failures retain their own cause and completed output.

The accepted run captures labels, boxes, masks, colors, and confidence for
saving. Media is composed at native source geometry from the same incremental
prediction delivery; it requires neither a visible browser nor another inference
pass. Semantic captions use class names; unresolved output identities remain
explicit **Raw slot** labels. PNG publication is atomic, and its writer retains
owned staging and reusable pinned readback storage through settlement.

**Full** writes H.264 in Matroska, preserving source presentation timestamps,
frame durations and original audio tracks. Source audio must be admitted by the
container format. The native media owner tries hardware encoding, then selects
software H.264 before opening output if hardware admission fails; it never
changes encoders within a file. No codec/container/bitrate control is exposed.
Inference pauses affect wall-clock progress rather than output timing.
Encoder, audio packet, and GPU-read custody stay bounded under worker-side
backpressure. The [media source and sink](architecture.md#workflow-output-and-media-handoffs)
own demux, timing and mux policy separately from prediction.

During execution the file is `prediction.partial.mkv`. Successful draining,
trailer/file settlement and rename publish `prediction.mkv`. Cancellation or
failure retains the partial path and completed PNGs. Incrementally flushed MKV
clusters remain available after interruption without a final rename or trailer;
an unfinished buffered tail can be lost. A failure before any usable cluster
does not guarantee playable media. The Output card exposes the retained partial
path without describing it as a completed artifact.
