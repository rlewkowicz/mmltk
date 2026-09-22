# RF-DETR workflows and artifacts

[Wiki index](README.md) · [Commands](commands.md) · [Datasets](datasets.md) · [GUI layout](gui-interaction.md#training-validation-and-prediction)

Train, Validate, and Predict use the same native model selection and preparation
facts. Their primary action settles settings, selects the requested model,
prepares missing weights, inspects the selected compiled inputs where needed,
and starts the owning system. Preparation progress belongs to the model card;
execution progress belongs to the workflow. A preparation failure stops that
start request and leaves its typed error visible.

Train launches the packaged sibling CLI through `TrainingSystem`. Validate and
Predict own independent native sessions, GPU work, cancellation, and retained
image products. See the [source guide](architecture.md#native-domain-work) for
their locations.

## Backend ownership

The backend keeps model execution and decoded-state admission in
[ordinary core declarations](architecture.md#native-domain-work). Within
`src/backend/models/rfdetr/training/`, the training loop coordinates these
execution and state boundaries through typed APIs:

| Declaration | Responsibility |
| --- | --- |
| [detail/training_lanes.h](../src/backend/models/rfdetr/training/detail/training_lanes.h) | Training lanes, queued work, gradient handoff, and event custody |
| [detail/training_metrics.h](../src/backend/models/rfdetr/training/detail/training_metrics.h) | GPU scalar accumulation and the completed metric handoff |
| [detail/training_snapshot.h](../src/backend/models/rfdetr/training/detail/training_snapshot.h) | Ordinary/EMA serialization snapshots and continuation save/load coordination |
| [detail/native_optimizer_private.h](../src/backend/models/rfdetr/training/detail/native_optimizer_private.h) | Typed AdamW/Muon state, parameter groups, update, and archive operations |
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

The [compiled format](datasets.md#compiled-binary-format) uses one-byte
foreground references and 32-byte name fields. Names must fit exactly; the
compiler rejects truncation. Source category identity is stored separately.
Training, validation, and test splits for one training run require the same
ordered catalog. Standalone evaluation can use an explicitly verified
permutation of the same exact names.

[ModelClassLayout](../src/backend/models/rfdetr/contract/class_layout.h) records
the ordered foreground catalog, each output slot's role, score encoding,
no-object encoding, and provenance. Fresh native training uses sigmoid logits,
one trailing unused output slot, and all-negative no-object supervision.
Background and unused slots participate in physical ranking and are discarded
after selection, as described [below](#model-input-and-detection-selection).
Current RF-DETR execution requires sigmoid logits; declaring a softmax layout
does not make that execution mode supported.

Native version-3 checkpoints embed the layout. External assets may supply
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

Train and Validate use the same **RF-DETR Weights** card: catalog presets and
**Custom Weights**, with a compact selected path, active preparation progress,
and actionable errors. Train accepts trainable weights (`.pt`, `.pth`, `.ckpt`,
`.safetensors`). Validate also accepts ONNX (`.onnx`) and TensorRT
(`.engine`, `.trt`). The selected extension identifies an input kind through
the native [compatibility catalog](../src/controller/contracts/model_selection.h);
normal artifact and class admission still apply.

The shared card has no separate backend-kind or companion class-layout control
for these two workflows. A new custom selection clears an unrelated descriptor
path. Native descriptors and CLI `--class-layout` retain their supported
admission route. Only Train adds Transfer/Resume controls; Validate has no
continuation preparation or training-output state.

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

The Output card owns **Auto Output**, initially enabled, **Browse Output**, and
a compact selected or active path. Auto keeps the saved destination empty until
Start reserves `gui-train-output/run-0001`, `run-0002`, and so on. The next suffix
comes from existing entries and directory creation reserves it atomically, so
new automatic runs remain distinct after relaunch. A resolved automatic path is
a runtime fact rather than a restored manual destination.

Browse Output switches to manual selection and loads supported saved charts.
An empty or unrelated folder clears the saved charts; a folder claiming history
through either `run.json` or `metrics.jsonl` must satisfy current-format admission
or reports an error. Browsing changes neither weights nor Transfer/Resume mode.
Re-enabling Auto clears the manual selection. Start selects live charts.

Transfer may use an absent or empty manual destination directly. A populated
destination gets a new numeric `run-*` child. Resume reuses a manual run directory
only when the selected full `checkpoint.pt` is in that directory and its attempt,
evaluated-weight choice, and class layout match the current history. Otherwise
Resume reserves a fresh child, including for an empty manual destination.
Automatic mode always reserves a fresh child. The resolved path appears when
the run is admitted. This GUI policy belongs to
[TrainRunStore](../src/controller/services/train_run_store.cpp); CLI training
uses its explicit `--output-dir`.

Validate's **Open Dataset** selects an independent override. While that override
is empty, its compact effective path follows Train's validation split, including
inferred paths. Native settings own this resolution. Changing an inherited
source cancels an unstarted Validate request just as changing an explicit
selection does.

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
keep their padded launch indexing, float arithmetic, mask-point lane assignment,
and reduction order while writing compact addresses. Solver traversal, ties,
loss accumulation, and assignment lifetime are unchanged. Separately, the
[deformable-attention forward wrapper](../src/backend/ml/layers/ms_deform_attn_cuda_wrapper.cpp)
allocates overwrite-only output because the forward kernel fills every active
element; backward accumulation retains its required zero initialization.

EMA is optional and **off by default**. The GUI's **Exponential moving average**
setting and CLI `--use-ema` enable persistent GPU shadow weights, updated at the existing
optimizer-update boundary. `--no-ema` disables them. Disabled EMA creates no
shadow storage or update work.

Each scheduled validation evaluates exactly one weight set: EMA when enabled,
ordinary weights otherwise. Metrics and best-checkpoint selection use that same
set. Evaluation temporarily selects EMA weights with restoration of the working
weights and training mode. There is one validation trajectory, identified by
`evaluated_weights`, rather than a separate EMA chart. An optional final test
uses the selected best checkpoint.

## Checkpoints and continuation

Only the current native RF-DETR **version-3** application checkpoint format is
supported. Upstream external weights keep their separate import routes.
An old application checkpoint is not an upstream artifact or a resumable run.

| Artifact | Contents and use |
| --- | --- |
| `checkpoint.pt` | Full continuation: working model/supervision state, optimizer, scheduler configuration/progress, scaler, and enabled EMA state/update count |
| `checkpoint_epoch_N.pt` | Ordinary epoch weights for inference or transfer; not a full continuation |
| `checkpoint_best_regular.pt` | Selected best ordinary weights when EMA is disabled |
| `checkpoint_best_ema.pt` | Selected best EMA weights when EMA is enabled |
| `checkpoint_fallback_regular.pt` or `checkpoint_fallback_ema.pt` | Selected weights when no best checkpoint was selected |

The best/epoch/fallback artifacts are weights snapshots, not full resume
checkpoints. Class layout accompanies native artifacts. Checkpoint/export
serialization uses [reusable pinned readback storage](gpu-execution.md#checkpoint-and-export-readbacks);
this does not move per-step EMA updates onto the CPU.

Train's weights card owns mutually exclusive **Transfer** and **Resume** radios.
Catalog and weights-only inputs use Transfer and cannot Resume. A resumable
custom checkpoint defaults to Resume; Transfer may still use its weights with
fresh training state. Selecting a file or changing the radio never starts work.

Custom selection starts cancellable native inspection on a worker. Inspection
validates the archive, continuation, optimizer inventory, and required EMA
state before publishing a compact capability. It retains immutable admission
evidence rather than decoded tensors; exact file identity is checked again
before use. Confirming the same custom path deliberately refreshes inspection.
An inspection error stays actionable rather than automatically retrying.

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
`total_images` from the current epoch. These are **rank-local image counts**,
not optimizer steps or world-wide totals. The native loader derives the total
from usable full microbatches after tail, distributed, accumulation, and lane
constraints; completion advances with processed local microbatches. The GUI
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

Current manifests and metric records use **format version 2**, including the
native image-count fields. Version-1 history and other older output directories
are rejected; there is no compatibility reader or migration. Checkpoint version
3 and the [compiled dataset format](datasets.md#compiled-binary-format) are
independent formats.

| File | Authority |
| --- | --- |
| `run.json` | Run/attempt identity, training configuration, execution/query facts, class layout, selected evaluation weights, and resume provenance |
| `metrics.jsonl` | Append-only typed metric records, including sequence, attempt, role, missing-record count, and progress |
| `progress.json` | Latest progress projection, including the typed metric record, consumed by the training process client |
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
and is reported in the GUI while training continues. Checkpoint failures keep
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

Scheduled evaluation contributes one sparse observation per recorded epoch
for the selected weight set. Live and terminal records carrying the latest
evaluation do not add duplicate observations. Missing or unavailable
measurements remain gaps. Optional final-test support and persisted results
remain native; the GUI has no final-test result display or chart trajectory. The
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

Validation chooses up to six distinct random images from the evaluated
population and captures their pixels, predictions, and ground truth during the
same evaluation pass. A smaller population leaves empty cells. The fixed
two-column/three-row atlas and its detail viewer reuse those retained products;
opening detail and changing overlays do not run inference again. Selection
uses the identity paired with displayed pixels, including during replacement
or partial progress.

The GUI presents twelve fixed COCO summary rows: AP 50:95, AP50, AP75,
AP small/medium/large, AR at each of the three recorded caps, and AR
small/medium/large. Boxes and available Masks use separate columns; unavailable
values show `—`. Native detail queries still support pages of at most four
rows, while this view has no metric, IoU, recall, or detail-page controls.
The [Validation workspace](gui-interaction.md#validation-workspace-and-shared-viewer)
owns layout, shared viewer interaction, and the Validation-only GT/Det layer
and compositing rules.

Scheduled training evaluation separately writes `eval_samples/epoch_N.png`.
[TrainingValidationRuntime](../src/backend/models/rfdetr/training/evaluation_run_owner.cpp)
owns an [EvaluationSampleWriter](../src/backend/models/rfdetr/core/sample_output.h)
that lazily retains its CUDA device, worker, settlement stream, and event across
epochs. One pending future bounds output; explicit `Flush` propagates errors,
and destruction settles queued image custody. Independent validation runtimes
can write on different devices without sharing a process-global writer.

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

Output JSON is optional for GUI viewing. When selected, records are streamed
to a temporary sibling file and published by rename only on success.
Cancellation/failure removes the temporary file and preserves an existing
destination. The document records source/model/backend facts, `class_domain`,
`class_layout`, and per-image records. Masks use
`row_major_start_length`; semantic names come only from the admitted catalog.
