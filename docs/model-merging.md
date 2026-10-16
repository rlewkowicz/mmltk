# Training model merging and continuation

[Wiki index](README.md) · [Training](rfdetr-training.md) · [Workflows/history](rfdetr-workflows.md) · [GUI](gui-interaction.md#lane-and-recipe-controls)

Periodic averaging, final selection and whole-session Resume operate on admitted
named native tensors before export. Exported graphs are not merged; mutable
training state remains per model.

## Periodic averaging

Each logical model owns working weights/optimizer and uses all selected GPUs.
Rounds attempt at most one update per nonexhausted model in stable ID order.
Default cadence is Each epoch; Every N rounds also merges at complete round
boundaries. Nonempty residual intervals merge at epoch end. Failure/cancellation
cannot publish an incomplete-round merge.

At a drained boundary, coefficients are successful global images since last
merge divided by interval total. Recoverable AMP overflow advances data/schedule,
not successful images. Zero total retires without mutation; exhausted models
still synchronize. [NativeModelAverage](../src/backend/models/rfdetr/training/detail/model_merging.h)
validates full named state, shapes/dtypes/devices, finite floats and equal
nonfloats before mutation. Independent reusable output accumulates float32
(float64 remains float64), costing models × tensor elements.

Merges include training-only learned tensors and broadcast to every rank/model.
Optimizer moments/ages, scaler, scheduler, EMA and stochastic clocks neither
average nor reset. All model/reducer/stream/loader/validation readers settle
before installation. Periodic validation evaluates synchronized ordinary weights
once and each model's EMA separately if enabled; shared/independent modes evaluate
their selected ordinary-or-EMA source. [History](rfdetr-workflows.md#saved-history-and-plots)
keeps trajectories distinct.

## Final deployment selection

Retain each model's best scheduled-validation candidate: ordinary without EMA,
EMA otherwise. Required finite metric is box AP for detection, mask AP for
segmentation, OKS AP for keypoints; no task falls back to another metric.
Candidates share admitted initialization/configuration/validation identity and
compatible state. Test data never ranks candidates or soups.

| Policy | Selection |
| --- | --- |
| Mode default | Validation-greedy for Independent; Off for Shared/Periodic |
| Off | Best individual metric, then earlier epoch, then lower model ID |
| Uniform | Equal average of all admitted candidates |
| Explicit | Finite nonnegative model coefficients, normalized from positive sum |
| Validation-greedy | Best first; try remaining metric/model-ID order, retaining equal-weight additions only on strict validation improvement |

Greedy skips duplicate content, evaluates exact saved trials, removes rejected
trials and never overwrites ingredients. Averages exclude training-only tensors
and are deployment weights, not resumable sessions. Independent/periodic/soups
are experimental, without accuracy/performance guarantees.

[Selection publication](../src/backend/models/rfdetr/training/model_merging.cpp)
replaces `selected.json` only after save, validation, checksum and evaluation.
It records method/artifact, model IDs, ingredient hashes/coefficients, validation
and best-individual metric. Failure preserves prior descriptor/candidates.
Optional final test uses frozen selection. Inference/export accepts its `.pt`,
not the descriptor.

## Artifact layout and formats

Formats are independent. Native writer **5** includes full canonical configuration;
reader also admits **4** within historical DINOv2-Small non-keypoint topology.
Import compatibility alone does not authorize Resume. Other listed artifacts
require their current schema; upstream imports are separate.

| Artifact | Format / purpose |
| --- | --- |
| Native `.pt` | 5, reads 4; continuation fields belong only to session model files |
| `session.json` | Session 1; authoritative complete Resume manifest |
| `generations/<generation>/plan.cbor` | Immutable sparse membership, draws/cursors and stochastic plan |
| `generations/<generation>/model-<ID>.pt` | Immutable working state and named optimizer/scheduler/scaler/EMA |
| `model-<ID>-epoch-<N>-ordinary-<identity>.pt` / `…-ema-…` | Immutable deployment candidates; N is zero-based |
| `selection-<identity>.pt` | Saved soup; Off can select a candidate directly |
| `selected.json` | Selection 1; bounded deployment descriptor |
| `run.json`, `metrics.jsonl`, `progress.json` | Independent [history 3](rfdetr-workflows.md#saved-history-and-plots) |
| GUI settings | [Schema 9](../src/controller/contracts/gui_settings.h) |

[Artifact declarations](../src/backend/models/rfdetr/contract/training_artifacts.h)
and [model state](../src/backend/models/rfdetr/core/model_state.h) own formats.
Session/selection bounds are 4 MiB/64 KiB. Native artifacts carry class/joint
layout; [pinned readbacks](gpu-execution.md#checkpoint-and-export-readbacks)
serialize after drain, without racing optimizer writes.

## Whole-session Resume

Resume requires `session.json`, including one-model sessions:

```bash
./mmltk rfdetr train --resume ./output/train/run-0001/session.json \
  --train-compiled ./compiled/train.bin --val-compiled ./compiled/val.bin \
  --output-dir ./output/train/resumed
```

This assumes matching default policy/recipe. Scalar CLI does not reconstruct
the full request: inputs/settings must satisfy admission; use train help or the
[canonical JSON request](commands.md#training-request-selection) for complete
model recipes/policies. Individual `.pt`/candidates/soups remain Transfer inputs,
even if containing continuation fields. [GUI Resume](rfdetr-workflows.md#checkpoints-and-continuation)
uses Custom Weights, native inspection and Prepare Resume; Browse Output is history.

[Admission](../src/backend/models/rfdetr/training/detail/training_session_checkpoint.h)
validates manifest, all models, plan, checksums and continuation before mutation:
class/joint/query/supervision configuration, membership/recipes, data policy,
initialization, precision/scaler, named optimizer state, scheduler clocks,
final-epoch latches, merge state, successful images and candidate identity.
Epoch extension preserves consumed clocks/applied latches. Physical capacity
does not redefine draws/effective batch.

Dataset identity uses admitted source metadata (geometry/classes/offsets), file
size/mtime and resolved model configuration, without hashing all pixels. Moves/
copies preserving mtime retain identity; changed content requires a new session.
Identity schema 2 uses canonical configuration/class serialization. Schema 1
retains its frozen DINOv2-Small non-keypoint projection and original compiled-10
header identity, admitting supported historical sessions/candidates unchanged.
New publication uses schema 2. Missing/mismatched identity cannot be replaced by
an inferred population.

Cancellable inspection retains immutable evidence/physical generation lease,
releases decoded tensors/plans and rechecks identities on use. Paths, `run.json`
or partial model sets cannot authorize Resume. Transfer starts fresh state.
[Checkpoint publication](../src/backend/models/rfdetr/training/training_session_checkpoint.cpp)
stages and validates plan/all models at a drained boundary, syncs immutable files,
then atomically publishes `session.json` last. Interruption preserves the prior
complete session. Current, predecessor and reader-leased generations survive;
retirement needs an exclusive lease. GUI/CLI destinations do not change admission
or history formats.
