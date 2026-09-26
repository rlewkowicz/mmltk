# Training model merging and continuation

[Wiki index](README.md) · [Training mathematics and lanes](rfdetr-training.md) · [Workflows and history](rfdetr-workflows.md) · [GUI controls](gui-interaction.md#lane-and-recipe-controls)

Periodic averaging, deployment selection, and whole-session continuation operate
on admitted named native tensors before export. Exported graphs are not merged;
independent models retain separate mutable training state.

## Periodic averaging

**Periodic averaging** maintains one working model and optimizer per logical
model. Each uses all selected GPUs. A session round attempts at most one update
per nonexhausted model, in stable model-ID order. **Each epoch** is the default
merge cadence; **Every N rounds** additionally merges at complete round
boundaries. A nonempty residual interval merges at epoch end. Cancellation or
failure cannot publish a merge from an incomplete round.

At a drained boundary, each model's coefficient is its successful global-image
count since the previous merge, normalized by the interval total. Recoverable
AMP overflow advances data and schedule time but contributes no successful
images. A zero-total interval retires without changing parameters. Exhausted
models participate in synchronization even when they contributed no interval
images.

[NativeModelAverage](../src/backend/models/rfdetr/training/detail/model_merging.h)
validates complete named state before mutation: matching names, shapes, dtypes
and devices, finite floating values, and equal nonfloating values. It prepares
an independent average in reusable storage, accumulating in float32 except for
float64 inputs. Work scales with models times tensor elements. Periodic merges
include training-only learned state, then broadcast the result to each model's
ranks. Optimizer moments and ages, scaler, scheduler, EMA, and stochastic clocks
remain model-owned and are not averaged or reset. All model, reducer, stream,
loader, and validation readers settle before installing new values.

Scheduled periodic validation evaluates the synchronized ordinary weights once.
With EMA enabled, each model's own EMA is also evaluated; ordinary synchronized
results remain a distinct metric source. Shared and independent modes evaluate
their selected ordinary-or-EMA source. The
[history reference](rfdetr-workflows.md#saved-history-and-plots) explains source
selection without mixing these trajectories.

## Final deployment selection

The session retains each model's best scheduled-validation candidate from the
selected weight kind: ordinary when EMA is disabled, EMA when enabled. Detection
selection requires finite box AP; segmentation requires finite mask AP and does
not fall back to box AP when mask AP is missing. Candidates must share admitted
initialization, configuration, validation identity, and compatible native state.
The optional test split never participates in candidate ranking or soup choice.

| Final model soup policy | Result |
| --- | --- |
| Mode default | Validation-greedy for Independent models; Off for Shared gradients and Periodic averaging |
| Off | Best individual metric, then earlier epoch, then lower model ID |
| Uniform | Equal average of all admitted candidates |
| Explicit | Normalize per-model finite, nonnegative coefficients; their sum must be positive |
| Validation-greedy | Start with the best candidate, try adding each remaining candidate in metric/model-ID order, and retain an equal-weight trial only on strict validation improvement |

Greedy selection skips duplicate candidate content. It evaluates the exact saved
trial artifact, removes rejected trials, and never overwrites an ingredient.
An average is deployment state with training-only tensors removed; it is not a
resumable optimizer session. Independent and periodic modes and final soups are
experimental capabilities, not accuracy or performance guarantees.

[model_merging.cpp](../src/backend/models/rfdetr/training/model_merging.cpp)
owns selection and publication. `selected.json` is replaced only after the
chosen artifact is saved, validated, checksummed, and evaluated. It records the
method, artifact identity, model IDs, ingredient checksums and normalized
coefficients, validation result, and best individual metric. Failed publication
preserves the previous descriptor and all retained candidates. An optional
final test consumes the frozen selected artifact. Existing inference and export
entrypoints consume its `.pt` path, not `selected.json`.

## Artifact layout and formats

These versions are independent; older application formats have no compatibility
reader or migration. Supported upstream weights retain their separate import
routes.

| Artifact | Current format and purpose |
| --- | --- |
| Native `.pt` | Version 4; a model archive, with continuation fields only in session model files |
| `session.json` | Session format 1; the authoritative whole-session Resume manifest |
| `generations/<generation>/plan.cbor` | Immutable sparse membership, draw schedules/cursors, and stochastic planning state referenced by the manifest |
| `generations/<generation>/model-<ID>.pt` | Immutable working model and named optimizer/scheduler/scaler/EMA continuation for each logical model |
| `model-<ID>-epoch-<N>-ordinary-<identity>.pt` or `…-ema-<identity>.pt` | Immutable deployment candidates; `N` is zero-based |
| `selection-<identity>.pt` | A saved average when final selection produces a soup; Off may directly select a retained candidate |
| `selected.json` | Selection format 1; bounded deployment descriptor, not a Resume checkpoint |
| `run.json`, `metrics.jsonl`, `progress.json` | [History format 3](rfdetr-workflows.md#saved-history-and-plots), independent of continuation |
| GUI settings | Schema 9, declared in [gui_settings.h](../src/controller/contracts/gui_settings.h) |

The public artifact declarations live in
[training_artifacts.h](../src/backend/models/rfdetr/contract/training_artifacts.h);
[model_state.h](../src/backend/models/rfdetr/core/model_state.h) owns the native
archive version. A session manifest is bounded to 4 MiB and a selection
descriptor to 64 KiB. Class layout accompanies native artifacts. Serialization
uses [retained pinned readbacks](gpu-execution.md#checkpoint-and-export-readbacks)
after the session drains; saved tensors cannot race later optimizer writes.

## Whole-session Resume

Resume takes `session.json`, including for a one-model session:

```bash
./mmltk rfdetr train --resume ./output/train/run-0001/session.json \
  --train-compiled ./compiled/train.bin --val-compiled ./compiled/val.bin \
  --output-dir ./output/train/resumed
```

This example assumes the matching default recipe and policy. The scalar CLI
does not restore a complete request from the manifest. Training inputs/settings
must still satisfy native admission; use
`./mmltk rfdetr train --help` for the complete request. The CLI's
[canonical JSON request](commands.md#training-request-selection) can carry the
full model recipes and policies. Individual model `.pt` files, epoch candidates,
and soups are Transfer/deployment inputs, even if an archive contains fields
that belong to a session. They cannot authorize a partial-model Resume.
The [GUI continuation reference](rfdetr-workflows.md#checkpoints-and-continuation)
covers inspection/Prepare Resume and the current weights-only chooser limitation.

[TrainingSessionAdmission](../src/backend/models/rfdetr/training/detail/training_session_checkpoint.h)
validates the entire manifest, every named model, plan, checksum, and continuation
before any live state changes. It checks class/query/supervision configuration,
model membership and recipes, data policy, initialization, precision and scaler,
named optimizer state, scheduler clocks, final-epoch latches, merge state,
successful-image counters, and retained candidate identity. Epoch extension is
supported while preserving consumed clocks and applied latches. Physical worker
capacity does not redefine logical draws or effective batch.

The compiled train/validation identities combine the already loaded format
header (including geometry, classes, and section offsets), file size and
modification time, and resolved model configuration. Startup does not read all
image pixels to checksum a dataset. Moving or copying a compiled file while
preserving its modification time retains this identity; replacing or modifying
it requires a new session. Earlier manifests based on full-file checksums do
not have this metadata identity and cannot authorize exact Resume.

Cancellable inspection retains exact admission evidence and a physical generation
lease, releasing decoded tensors/plans afterward and rechecking file identities
on use. A path, `run.json`, or partial model set cannot authorize Resume.
Transfer starts fresh training state from admitted weights.

[TrainingSessionCheckpoint](../src/backend/models/rfdetr/training/training_session_checkpoint.cpp)
writes one staging generation at a drained session boundary. It saves and
validates the plan and every model, syncs immutable files, and atomically
publishes `session.json` last. Interrupted or failed publication leaves the
previous complete session authoritative. It retains the current generation,
its predecessor, and any older generation with an active reader lease;
retirement requires an exclusive lease. Reader custody therefore survives
publication and cancellation without deleting files still in use.

[GUI output selection](rfdetr-workflows.md#training-inputs-and-output-destination)
and CLI `--output-dir` choose destinations without changing admission or history
format.
