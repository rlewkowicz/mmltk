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
| [detail/evaluation_runtime.h](../src/backend/models/rfdetr/training/detail/evaluation_runtime.h) | Evaluation lanes, prediction buffers, and scheduled validation lifetime |
| [checkpoint.h](../src/backend/models/rfdetr/training/checkpoint.h) | Ordinary checkpoint application, normalization, weight loading, and continuation inspection API |

Runtime definitions stay in the corresponding ordinary source files; sources
that consume retained modules remain registered importers. Canonical
[training metric declarations](../src/backend/models/rfdetr/contract/training_metrics.h)
own persisted/browser record structure, independently of these resource owners.
The [build reference](build.md#target-declarations-and-precompiled-headers)
describes compilation and header isolation.

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

Compiled format 7 retains its existing one-byte foreground references and
32-byte name fields. Names must fit exactly; the compiler rejects truncation.
Training, validation, and test splits for one training run require the same
ordered catalog. Standalone evaluation can use an explicitly verified
permutation of the same exact names.

[ModelClassLayout](../src/backend/models/rfdetr/contract/class_layout.h) records
the ordered foreground catalog, each output slot's role, score encoding,
no-object encoding, and provenance. Fresh native training uses sigmoid logits,
one trailing unused output slot, and all-negative no-object supervision.
Background and unused slots are removed before detection ranking. Current
RF-DETR execution requires sigmoid logits; declaring a softmax layout does not
make that execution mode supported.

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

## Training and query limits

An image may contain more targets than the resolved query count. All targets
remain available to supervision; the SciPy-derived rectangular assignment
solver still matches at most the smaller matrix dimension in each query group.
This changes target admission and storage sizing, not the solver or loss
definition. Automatic query selection still respects the model's automatic cap,
explicit overrides retain their validation, and resume retains the checkpoint's
query and selection counts.

The implementation starts at
[train.cpp](../src/backend/models/rfdetr/training/train.cpp) and
[detection_ops.cpp](../src/backend/models/rfdetr/core/detection_ops.cpp).
`run.json` records the observed dataset limits and resolved query policy.

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

In Train's Output card, select a checkpoint and use **Inspect checkpoint**.
Inspection validates the current archive, continuation, optimizer inventory,
and required EMA state before exposing **Resume**. Resume restores saved
training settings and passes the same continuation admission used by CLI
training. A path or run-history manifest alone cannot make an artifact resumable.
See the [checkpoint API](../src/backend/models/rfdetr/training/checkpoint.h),
[checkpoint I/O](../src/backend/models/rfdetr/training/checkpoint_io.cpp),
and [continuation validation](../src/backend/models/rfdetr/training/training_continuation.cpp).

## Saved history and plots

Current history uses **format version 1**. There is no history reader or
migration for older output directories.

| File | Authority |
| --- | --- |
| `run.json` | Run/attempt identity, training configuration, execution/query facts, class layout, selected evaluation weights, and resume provenance |
| `metrics.jsonl` | Append-only typed metric records, including sequence, attempt, role, missing-record count, and progress |
| `progress.json` | Latest progress projection consumed by the training process client |
| `log.txt` | Epoch summary projections |
| `results.json` | Final result projection |

The declarations are in
[training_metrics.h](../src/backend/models/rfdetr/contract/training_metrics.h).
The independent [telemetry writer](../src/backend/models/rfdetr/training/telemetry_writer.cpp)
owns serialization and file writes. Training submits bounded records without
waiting for charts, browser delivery, or telemetry disk I/O. Intermediate live
records can coalesce; epoch and terminal records have reserved queue custody.
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

**Open saved run** inspects the selected output directory without starting
training. **Load history / More history** fetch bounded pages; **Current live
run** returns to live data. The reader uses generation and byte-boundary cursors,
rejects replaced/truncated streams, and retries an incomplete trailing append.
Resume can reuse the selected current run directory only when its full
`checkpoint.pt`, attempt identity, selected weights, and class layout agree.
A fresh start, or a resume that does not match that directory, creates a new
`run-*` child when the selected directory is nonempty.
This GUI directory policy belongs to
[TrainRunStore](../src/controller/services/train_run_store.cpp).

The retained Iced chart groups losses, AP, precision/recall/F1, learning rates,
and throughput. It offers step/epoch axes and a log-loss scale. Bounded summaries
retain endpoints and extrema; missing records, unavailable metrics, and attempt
changes remain disconnected. Sparse observations have markers so they remain
visible without bridging gaps. Hidden Train views keep ingesting current
records without rebuilding chart geometry and prepare their retained summaries
when shown. Plot objects, series, and GPU buffers retain useful capacity.
These are implementation properties, not a measured overhead or throughput
guarantee.

## Evaluation metrics and retained samples

GUI Validate evaluates the selected model/backend once. CLI `rfdetr evaluate`
also selects one backend; CLI `rfdetr validate` retains its ordered
multi-backend comparison/report behavior. The
[evaluation declarations](../src/backend/models/rfdetr/contract/evaluation_metrics.h)
and [evaluator](../src/backend/models/rfdetr/core/evaluator.cpp) define:

- Box and, when available/requested, mask AP over IoU 0.50–0.95 in 0.05 steps,
  with AP50 and AP75.
- AR at detection caps `1`, `min(10, budget)`, and the resolved detection
  budget; the budget is reported rather than assumed to be 100.
- All/small/medium/large area and per-class details, with 101-point interpolated
  precision-recall curves.
- Precision, recall, and F1 at the threshold maximizing macro F1 over classes
  with ground truth on the 101-point 0.00–1.00 confidence grid. The selected
  threshold is reported; ties retain the first threshold. These confidence metrics use
  IoU 0.50 and are not averages across the AP IoU axis.

No eligible ground truth produces unavailable metrics rather than a fabricated
zero result. Area values and geometry derive from compiled annotations. Original
source/crowd information absent from that format cannot be reconstructed, and
quantized compiled annotations can differ from original-annotation evaluation.
Exact AP retains compact matching records proportional to evaluated detections;
it is not a constant-memory statistic.

Validation chooses up to six distinct random images from the evaluated
population and captures their pixels, predictions, and ground truth during the
same evaluation pass. A smaller population leaves empty cells. The fixed
three-column/two-row atlas and its detail viewer reuse those retained products;
opening detail and changing overlays do not run inference again. Selection
uses the identity paired with displayed pixels, including during replacement
or partial progress.

Metrics details are fetched in pages of at most four rows. The GUI exposes
IoU and recall selectors over generated axes, plus separate prediction and
ground-truth box, mask, and label controls. Labels are local Iced presentation;
box/mask composition belongs to Validation's native renderer.

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
