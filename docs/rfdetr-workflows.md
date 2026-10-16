# RF-DETR workflows and artifacts

[Wiki index](README.md) · [Commands](commands.md) · [Datasets](datasets.md) · [Training](rfdetr-training.md) · [GUI](gui-interaction.md#training-validation-and-prediction)

Train, Validate, Predict and Export share model selection/preparation. An explicit
[primary action](gui-interaction.md#training-validation-and-prediction) settles
settings, prepares weights, inspects inputs and starts its owner. Preparation
errors end that request in session Status. Model-card preparation and workflow
execution progress remain separate. Each captures its [compute device](gpu-execution.md#workflow-device-selection);
Train retains ordered multi-GPU and remote-provider execution.

## Backend ownership

[Core model/state admission](architecture.md#native-domain-work) exposes ordinary
C++ APIs. `src/backend/models/rfdetr/training/` separates runtime owners:

| Header | Responsibility |
| --- | --- |
| [training_lanes](../src/backend/models/rfdetr/training/detail/training_lanes.h) | Physical workers, logical contributions, gradient handoff and events |
| [training_metrics](../src/backend/models/rfdetr/training/detail/training_metrics.h) | GPU scalar accumulation and completed metric decision |
| [training_snapshot](../src/backend/models/rfdetr/training/detail/training_snapshot.h) | Ordinary/EMA snapshots and continuation save/load |
| [native_optimizer_private](../src/backend/models/rfdetr/training/detail/native_optimizer_private.h) | Typed AdamW/Muon/SGD groups/state/update/archive |
| [target_builder_private](../src/backend/models/rfdetr/training/detail/target_builder_private.h) | Target staging, scratch and consumer leases |
| [evaluation_runtime](../src/backend/models/rfdetr/training/detail/evaluation_runtime.h) | Evaluation lanes, buffers, validation and sample writer |
| [checkpoint](../src/backend/models/rfdetr/training/checkpoint.h) | Weight normalization/loading and continuation inspection |

Definitions stay with their ordinary source files. Reflected
[metric contracts](../src/backend/models/rfdetr/contract/training_metrics.h)
independently own persisted/browser structure. [Build ownership](build.md#target-declarations-and-precompiled-headers)
defines membership and header isolation. The [ONNX lowerer](../src/backend/models/rfdetr/export/onnx_lowering.cpp)
emits the earliest dependency-ready original ordinal, including nested blocks,
using O(log V) heap operations; dependencies are deduplicated and cycles rejected.

## Class identity and model admission

The immutable [ClassCatalog](../src/backend/data/catalog/class_catalog.h) owns
exact foreground names and joint schemas. These identities are distinct:

| Identity | Meaning |
| --- | --- |
| Source category ID | Dataset adapter identity |
| Foreground reference | Dense zero-based catalog index |
| Output slot | Physical classifier axis, including unused/background slots |
| External output ID | Identity required by an external format |

Compiled data preserves source IDs separately and rejects name truncation.
Training splits need the same ordered catalog/joint schema; standalone evaluation
can admit an explicitly verified permutation of identical names/meaning.
[ModelClassLayout](../src/backend/models/rfdetr/contract/class_layout.h) records
catalog, slot roles, score/no-object encoding and provenance. Fresh native
training uses sigmoid logits, one trailing unused slot and all-negative
no-object supervision. Physical ranking precedes removal of unused/background
slots. Softmax metadata does not enable unsupported softmax execution.

Native [version-5 checkpoints](model-merging.md#artifact-layout-and-formats)
embed complete canonical configuration and class layout; supported historical
version 4 is also readable. External assets need supported embedded metadata,
verified official metadata, or a digest-bound `--class-layout` descriptor.
Version-1 `ModelClassDescriptor` JSON contains artifact SHA-256, layout and
optional named output roles; [admission](../src/backend/models/rfdetr/core/class_layout.cpp)
validates identity, width and role agreement before installation. A filename,
tensor count or bare names list cannot establish mapping.

Unresolved external output can remain visibly raw but cannot enter semantic
evaluation. Products carry their reference domain/catalog. Mask buffers are
consumed only when the result declares them available. Transfer maps class/joint
dependent tensors by exact class and joint names, including classifier, encoder,
DN/Match-Free and GroupPose axes; shared joints must agree in OKS sigmas,
named flip partners and connections. Unmatched destinations keep initialization.
Resume requires exact class/joint layout and continuation, without remapping.

The [checkpoint bridge](../src/backend/models/rfdetr/training/rfdetr_checkpoint_bridge.py)
normalizes model/bare tensor dictionaries, module `state_dict()` and Lightning
envelopes once. Lightning selects `model.` entries, strips that prefix and an
immediate `_orig_mod.`, and rejects empty inventories/collisions. Supplied topology
is evidence checked against canonical configuration; unsupported or conflicting
backbones, attention/projector/head shapes and schema masks fail admission.
Supported GroupPose state is active; non-keypoint bookkeeping is admitted only
in its exact empty form. Verified official legacy normalization is asset-bound,
including query-prefix and unused Preview projection handling.

[State admission](../src/backend/models/rfdetr/core/model_state_load.h) separates
exact, required-core and explicit partial transfer. Prediction/export/ordinary
initialization require a complete compatible core before publication; damaged
archives cannot silently become mostly random models. Reports retain loaded,
missing, unexpected, incompatible and deliberately initialized names. Fresh
transfer owns compatible query adaptation; Resume keeps stricter inventories.

## Model input and detection selection

The [preset catalog](../src/backend/models/rfdetr/contract/preset_catalog.h) owns
all 16 presets, topology, resolution, assets, task coefficients and licenses:

| Task / family | Presets | Default candidates |
| --- | --- | ---: |
| Detection, DINOv2-Small | Nano, Small, Medium, Large | 300 |
| Detection, PE-Core-T | Atto | 300 |
| Detection, PE-Core-T | Femto, Pico | 200 |
| Detection, DINOv2-Base | XLarge, 2XLarge | 300 |
| Segmentation | Nano, Small | 100 |
| Segmentation | Medium, Large | 200 |
| Segmentation | XLarge, 2XLarge | 300 |
| Keypoints, GroupPose | Keypoint Preview | 100 |

Canonical names use `rf-detr-*`; 2XLarge is `xxlarge`, with `2xlarge` aliases.
Lookup is case-sensitive and also admits declared upstream class names. Plus
weights retain their catalog-declared license; consult [NOTICE](../NOTICE).
Atto has no decoder. Keypoint Preview has a dual keypoint projector, background-
first official person layout and the named 17-joint COCO person schema. Dataset
admission supplies class/joint meaning; topology is never inferred from size labels.

Compiled planar RGB float32 lies in `[0,1]` with [stored geometry](datasets.md#resize-geometry).
[GpuBatchPreprocessor](../src/backend/models/rfdetr/core/gpu_batch_preprocessor.h)
normalizes once on GPU: mean `[0.485,0.456,0.406]`, std `[0.229,0.224,0.225]`.
Augmentation uses the same constants. Borrowed/preview pixels remain raw;
separate reusable model-input storage waits for consumers before reuse. Native,
ONNX and TensorRT share preparation for compiled, image and video inputs.

Ordinary image files Stretch to model resolution using selected [RGB resize](datasets.md#rgb-resampling-methods),
default Native, before normalization. CLI `predict --resize-method` and GUI image
radios select it; compiled pixels/video preparation are unaffected. File lanes
retain host/resize capacity and pinned uploads through H2D completion.

[Postprocessing](../src/backend/models/rfdetr/core/postprocess.cpp) sigmoids the
complete query×class axis, stably ranks flattened physical scores, then selects
the budget. Equal scores keep flattened order. Class-layout filtering removes
unused/background entries afterward without refill. Boxes, scores, classes,
masks and joints retain selected query identity. Box corners clip to the canvas
and, for compiled Letterbox, the stored content rectangle.

| Count | Meaning |
| --- | --- |
| Physical queries | Admitted architecture/checkpoint |
| `num_select` | Model default physical ranking budget |
| Output capacity | Checked bound from requested budget and available query/class pairs |
| Survivors | Valid semantic entries after filtering/confidence; may be fewer than capacity |
| COCO maxDets | Independent per-image/per-category evaluation caps |

CLI `evaluate`/`validate --candidate-count 0` uses model defaults;
`--eval-max-dets 0` uses evaluation defaults. Predict's
`--max-dets-per-image` defaults to 500; zero uses model selection count, without
changing evaluation policy. Custom/resumed configurations retain admitted counts.

Keypoint postprocessing selects each physical class's joint rows and fuses
findability-weighted positional uncertainty into detection confidence after
physical ranking. Zero-width schemas skip fusion; inactive joints stay masked.
Joint confidence is sigmoid findability, not ground-truth visibility. Coordinates
and precision transform together through image geometry. Named JSON records
`keypoint_space: original_image_pixels`, joint name, xy, confidence, precision
Cholesky and covariance. Covariance inversion uses double precision before
float output; invalid uncertainty remains unavailable rather than fabricated.

## Shared weights selection

The **RF-DETR Weights** card provides presets, **Custom Weights**, selected path,
preparation and cancellation. Supported custom selections are:

| Workflow | Inputs |
| --- | --- |
| Train | `.pt`, `.pth`, `.ckpt`, `.safetensors`; `.json` session manifests for Resume |
| Validate / Predict | Weights, `.onnx`, `.engine`, `.trt` |
| Export | `.pt` weights |

The [compatibility catalog](../src/controller/contracts/model_selection.h)
classifies extensions; normal artifact/class admission still applies. These
cards have no separate backend-kind/Class layout controls. A new custom
selection clears an unrelated descriptor; CLI/native descriptor routes remain.
Only Train exposes Transfer/Resume. Dialog confirmation, pending selection and
preparation are shared behavior with independent workflow widget identities.
Failures enter [session Status](gui-interaction.md#session-status).

## Run output directories

**Auto Output** starts enabled. Accepted execution reserves the next `run-NNNN`,
starting at `run-0001` and expanding beyond four digits, without overwriting:

| Workflow | Root | Products |
| --- | --- | --- |
| Train | `./output/train` | Checkpoints, history, epoch samples, training evaluation |
| Validate | `./output/validate` | `report.json`, up to six `samples/sample-<dataset-index>.png` |
| Predict | `./output/predict` | Optional image/compiled `predictions.json`, source-specific media |
| Export | `./output/export` | Requested `model.onnx`/`model.engine` and class companions |

**Browse Output** selects manual output. New/empty directories are used directly;
occupied directories receive a numbered child. An exclusive `.mmltk-run-claim`
reserves every run, even with saving disabled. Input preparation never reserves;
reservation follows native admission, and filesystem errors refuse execution.
[Run reservation](../src/controller/services/training/run_output.cpp) owns the
algorithm; systems own outcome policy. Train Resume has additional rules below.

Configured, active and completed paths remain distinct. The card shows admitted
directory, committed artifacts/samples and partial-video recovery; changed manual
selection shows **Next output**. Auto clears manual selection without deletion.
Settings store selections, not resolved automatic paths. CLI output arguments
remain explicit and independent of GUI reservation.

## Export formats

ONNX/TensorRT checkboxes are independent and initially enabled. Neither selected
disables Run Export; native admission rejects it before reservation/model work.
The [export owner](../src/controller/subsystems/export/export_run.cpp) prepares
ONNX once, then TensorRT consumes it. ONNX-only/both retain requested files;
TensorRT-only removes the private ONNX/class companions after readers settle.
Cancellation/failure preserves committed requested artifacts, including ONNX
when engine building fails. CLI `export-onnx` and `build-engine` remain separate.

Export traces a private inference snapshot, preserving source mode, trainability,
graphs and attention route on success/failure. Only the snapshot selects
reference deformable attention/math SDPA. Lowering preserves depthwise precision,
linear/ReLU and SiLU→Sigmoid/Mul. ONNX/TensorRT keep independent [precision policy](gpu-execution.md#model-backend-precision);
export isolation does not imply identical arithmetic. Keypoint exports carry
named output roles and class/joint configuration through artifact companions.

## Training inputs and output destination

Train requires compiled train/validation. **Infer train/validation splits** uses
`train.bin`/`val.bin` under compiled output. Source/output retain Browse; disabling
inference reveals train/validation/optional test text fields without per-split
browsers. Test is never inferred, survives toggles, and must be cleared manually
to disable. A selected test must be readable and share the ordered catalog/joint
schema; absent test does not block training. [Materialization](../src/controller/subsystems/system/compute_intent_materializer.cpp)
and native Dataset inspection own admission.

Train's Browse Output additionally loads supported saved charts without starting
or changing weights/Transfer/Resume. Empty/unrelated folders clear charts;
either `run.json` or `metrics.jsonl` claims history and requires both current
artifacts. Auto clears manual selection; Start selects live charts.
Transfer uses absent/empty manual output directly, populated output gets a child.
Resume reuses manual output only when the admitted session is there and session,
attempt, evaluated weights and class layout match history; otherwise it reserves
a fresh child, even under empty manual output. Auto always reserves a fresh child.
[TrainRunStore](../src/controller/services/training/train_run_store.cpp) owns GUI
admission; CLI uses explicit `--output-dir`.

Validate's **Open Dataset** sets an independent override; empty override follows
Train's effective validation split, including inference. Native settings resolve
it. Changing inherited input cancels pending Start just like explicit input.

## Local training failures

Failed local training publishes bounded child cause/exit status to Status;
signals retain their number. Explicit **CUDA out of memory** includes allocator
detail and suggests reducing batch or lanes. Generic exit/signal alone is not
OOM. Configured workload is preserved; no automatic retry/tuning occurs.
[Failure reporting](logging.md#local-training-failures) owns extraction and
quiet fatal stderr; [training](rfdetr-training.md) owns numerical policy.

## Training and query limits

Continuous boxes remain targets through geometry; vanished raster masks do not
delete valid boxes. Crowd stays in evaluation but not foreground supervision or
copy-paste. Segmentation requires masks for non-crowd targets, distinguishing
missing from present-empty. Hungarian focal costs use configured alpha in dense
and CUDA paths. Keypoint training preserves named schema/visibility and requires
Hungarian without DN; zero-decoder Atto shares that supervision restriction.

More targets than queries is valid: all targets reach supervision; rectangular
assignment matches at most the smaller dimension per group. Automatic queries
respect the model cap, explicit overrides are validated and Resume retains
saved query/selection counts. `run.json` records resolved policy/data limits.

[MatcherWorkspace](../src/backend/models/rfdetr/core/detail/matcher_workspace.h)
packs FP32 per-layer/image matrices with active count
`sum_l sum_i(Q_l × T_i)`. Checked prefixes retain separate input-target and
output offsets; device/NUMA-pinned buffers reuse high-water capacity, with one
settled D2H for assignment. Padded-shape overflow admission and full-domain
nonfinite sanitization still apply. Deformable forward uses overwrite-only
output; backward accumulation requires zeros.

EMA is off by default; GUI **Exponential moving average** or `--use-ema` enables
shadow weights, `--no-ema` disables all shadow/update work. Evaluation selects
EMA temporarily then restores working weights/mode. [Training](rfdetr-training.md#ema)
defines updates, including AMP skips; [merging](model-merging.md) defines source
trajectories and frozen final selection/test. These are not accuracy guarantees.

## Checkpoints and continuation

[Whole-session Resume](model-merging.md#whole-session-resume) requires
`session.json` and the complete immutable generation, even for one model.
Individual archives/candidates/soups are Transfer/deployment inputs.
Train's Transfer/Resume radios never start work. Catalog/weights-only inputs use
Transfer; admitted resumable custom input defaults to Resume. Select and confirm
`session.json` in Custom Weights; successful native inspection enables Resume.
Browse Output selects history only. Saved custom paths use the same inspection/
Prepare Resume path; CLI accepts `--resume`.

Inspection is worker-owned/cancellable and checks archive, continuation,
optimizer/EMA inventory, immutable evidence and physical generation lease.
Confirming the same path refreshes it; errors do not auto-retry. Resume Start
restores saved settings, settles them and passes retained admission directly to
Training, bypassing ordinary weight preparation. Only the still-current explicit
Start can proceed; a path/history manifest alone cannot authorize Resume.

## Live training progress

Startup separates compiled opening from sampling-plan preparation. [Trusted
loading](datasets.md#loading-a-compiled-file) shares mappings/readers; sampling
still reads class support for schedules/continuation. The live card follows the
active local run even while charts show saved history, and disappears at terminal
settlement. Model preparation stays in the weights card.

Train progress uses native current-epoch `completed_images`/`total_images`:
global logical images per reported source, not steps or multiplied rank-local
counts. Complete windows define totals; unused tails are separate. GUI never
reconstructs counts from editable settings. Positive known total enables a bar.
Elapsed time/images per second are native; ETA uses remaining images/rate.
Loss/rate/ETA require available finite observations and positive elapsed/rate;
other phases show phase without stale bars/losses.

## Saved history and plots

History format **3** is independent of [continuation/artifact formats](model-merging.md#artifact-layout-and-formats).
Unsupported history versions are rejected. Canonical [metric declarations](../src/backend/models/rfdetr/contract/training_metrics.h)
include source/session/model identity, execution/query/data facts, round/merge,
distribution and selected-output metadata.

| File | Contents |
| --- | --- |
| `run.json` | Run/attempt, configuration, execution/query facts, catalog/layout, evaluated weights and Resume provenance |
| `metrics.jsonl` | Append-only typed records, sequence/attempt/role, gaps and progress |
| `progress.json` | Latest record, retained sources and optional final result for process client |
| `log.txt` | Epoch summary projections |
| `results.json` | Final projection |

[Telemetry](../src/backend/models/rfdetr/training/telemetry_writer.cpp) serializes
once, appends history before progress publication and retains epoch/final write
order. Training submits bounded records without waiting for UI/disk; intermediate
records coalesce, epoch/terminal records have reserved custody. Contention,
capacity or persistence failure marks incomplete history and posts Status while
training continues; checkpoint failures remain operation failures. Live cadence
is at most once/second plus immediate phase/epoch/terminal updates. Optional
uncomputed values stay absent; LR comes from optimizer/schedule.

The telemetry worker caches primary-device NVML utilization, used/total bytes
and temperature for one second across models, selecting by PCI identity. Rank
zero is sampled under DDP; counters include other applications. Collection adds
no CUDA synchronization, subprocess or worker. Missing NVML/sensors/queries leave
optional gaps without failing training; older format-3 records remain readable.
GUI displays memory in GiB; throughput is epoch-average images/second. CLI/GUI
produce the same telemetry.

Hungarian main components are raw; Match-Free main components are weighted.
Auxiliary/DN are separate weighted groups; correspondence overlaps. Only total
is the complete optimized objective: displayed components must not be summed.

Saved history pages contain at most 32 records. Generation/byte cursors reject
replacement/truncation and wait at incomplete trailing appends. A valid manifest
and metrics stream are required; neither present means empty history. Sources
are `(scope,model_id,weights)`: model/synchronized-session/selected-output/session
and Ordinary/Ema/Soup. Shared ID is zero; other modes retain IDs. Periodic ordinary
validation has synchronized scope. Default source prefers selected weight kind,
then lowest ID; live/saved selections remain independent.

Scheduled evaluation contributes one observation/source/epoch, without duplicate
latest values from live/terminal records. Missing measurements remain gaps.
Selected output separately shows method/artifact/ingredients/checksums/validation;
first-cause model failures retain identity. Optional final test is native output,
not a GUI trajectory. [Dashboard](gui-interaction.md#training-dashboard) defines
bounded storage, axes and interaction.

## Evaluation metrics and retained samples

GUI Validate and CLI `evaluate` select one backend. CLI `validate` retains ordered
multi-backend comparisons/reports. [Evaluator](../src/backend/models/rfdetr/core/evaluator.cpp)
and [metric declarations](../src/backend/models/rfdetr/contract/evaluation_metrics.h)
define:

- Box/mask AP at IoU 0.50–0.95 by 0.05, AP50/AP75, area/per-class details and
  101-point interpolated precision-recall. AR caps are `1,min(10,cap),cap`, default
  `[1,10,500]`, independently of candidates; `--eval-max-dets` changes the cap.
- Keypoint AP/AR uses schema OKS sigmas, 0.50–0.95 thresholds, fixed cap 20 and
  all/medium/large areas; small is unavailable. Annotated joints (visible or
  occluded) contribute; all-unannotated targets follow expanded-box OKS ignore
  semantics. Unequal joint-count groups retain upstream category-count weighting.
- Confidence P/R/F1 uses IoU/OKS 0.50 at the first threshold maximizing macro F1
  among classes with ground truth on the 0.00–1.00 grid by 0.01; it is not an
  average across the AP threshold axis.

Stored source area controls box/mask ranges; [fallback](datasets.md#instance-records)
and optional [mask recovery](benchmark-datasets.md#optional-dropped-mask-recovery)
provide it when needed. Inclusive boundaries are `32²`/`96²`. Unmatched detection
area is mapped back to source units before range exclusion. Crowd determines
initial ignore; raw `ignore` is preserved but not OR-ed in. Ordinary nonignored
GT wins matching priority; crowd uses intersection/detection area and permits
repeat matches without TP/FP or positive denominator contribution. Area-ignored
GT is also excluded. Source order/duplicate annotations are retained; equal IoU
chooses the last within an ignore group, and score ties keep prediction order.
Prefix partial sorting and early settled-threshold exits preserve those rules.

No eligible GT means unavailable metrics. Valid keypoint output with zero
detections contributes zero AP when eligible GT exists; it needs no joint-data
pointer. Task, stride and capacity checks still apply, and nonempty predictions
must supply their class's active joints. Box+mask requires every GT to declare
a mask; empty is valid. Compiled masks do not recover discarded source resolution,
and exact AP retains detection-proportional matching records.

One evaluation pass captures up to six distinct random samples, with pixels,
predictions and GT; smaller populations leave empty atlas cells. [ValidationSamples](../src/controller/subsystems/validate/detail/validation_samples.cpp)
separates progressive capture from immutable displayed detail membership.
Opening detail freezes its view; closing after success returns to completed
population without another inference. An incumbent detail retains its return
atlas during replacement; failed/cancelled replacement restores it. Selection
uses identities paired with displayed pixels. Render refusal retains image/
metadata, permits one automatic retry/request and explicit close retry after
exhaustion. Capture bookkeeping retires only after the requested atlas completes;
logical settlement/browser consumption and [physical retirement](gpu-execution.md#workflow-runtime-retirement)
remain distinct.

[Display confidence](gui-interaction.md#display-confidence) filters retained
preview detections only; it changes no evaluation/results/raw samples/import.
The fixed twelve GUI rows show AP50:95/AP50/AP75, AP by area, AR at recorded caps
and AR by area, with Boxes and available Masks/OKS columns; unavailable is `—`.
Native detail pages hold at most four rows; GUI has no metric/IoU/recall/page
selectors.

Start captures labels/layers/boxes/masks/joints/uncertainty/confidence for required
sample output through [ValidationRunPreview](../src/controller/contracts/validation_display.h).
[Sample output](../src/controller/subsystems/validate/detail/validation_sample_output.cpp)
saves each selected ID once as `samples/sample-<dataset-index>.png`, at native
geometry with class names and Validation composition. Preparation preserves
that payload; later edits/pan/zoom/browser availability cannot change saving.
Required PNG writes settle before success. Save failure retains metrics/report/
completed PNGs and reports image-output cause; interactive adoption is optional.

Training evaluation separately writes `eval_samples/epoch_N.png` (one-based N),
under run root for Shared or `model-<ID>/ordinary|ema/` otherwise. Its retained
per-runtime writer lazily owns CUDA device/worker/stream/event; one pending
future bounds output, Flush propagates errors, destruction settles custody.
These numeric-category mosaics do not reserve standalone Validate output.

## Incremental prediction

GUI Predict accepts compiled data, one image or a local video with batch **1**;
CLI supports `--batch-size`, compiled data and repeatable `--image`, not GUI video
selection. [PredictionSession](../src/backend/models/rfdetr/inference/predict.cpp)
retains backend/workspace and delivers incrementally, requesting source pixels
only for consumers. Latest preview replaces earlier images. Borrowed reads last
through copies; contained preview failure need not fail semantic prediction.

Requested masks materialize only for confidence survivors in bounded chunks.
[Readback](../src/backend/models/rfdetr/inference/prediction_capacity.h) packs
when device bytes plus page-rounded pinned bytes beat dense host storage and the
complete allocation inventory fits the chunk budget; otherwise dense booleans
are used. Preview-only demand performs no encoded-mask host readback. Packing
is row-major low-bit-first, `ceil(width×height/8)` bytes, on current CUDA stream
including legacy default; host RLE waits for settlement. Encoding carries runs
across 64-bit words, ignores tail bits and retains scalar runs/area/limits and
partial state on capacity failure. Storage grows with emitted runs. [Preview
storage](gpu-execution.md#prediction-preview-storage) owns separate GPU pixels/masks.

Video source owns decode/timestamps/EOF; Predict paces and supports interruptible
Pause/Resume/Stop. Unknown totals show completed work/activity. Pending controls
handle events before replies. EOF completes, Stop cancels; latest preview survives.
Image/compiled JSON is default-on independently of media saving. A temporary
sibling becomes `predictions.json` only on success; failure/cancellation removes
it and preserves existing output. It records source/model/backend, class domain/
layout and per-image records, with `row_major_start_length` masks and admitted
names/joint schema. Video never writes prediction JSON.

## Prediction samples and Full video

| Source | Initial choice | Media |
| --- | --- | --- |
| Single image | Save sample on | `sample.png` |
| Compiled dataset | Saving on, Percent %=10 | `samples/sample-<dataset-index>.png` |
| Video | Saving on, Full; Samples count=6 | `prediction.mkv` or `samples/frame-<decoded-index>.png` |

Compiled Percent accepts integers 1–100 and saves `ceil(count×percent/100)`;
Total accepts 1 through inspected count, initially min(6,count). It stays disabled
until count is known, clamps on source change and revalidates at Start. Uniform
subsets are without replacement, retaining the smaller selected/excluded set;
full selection needs no random-index storage. Inference order/limits do not
change. An inference limit smaller than requested samples is refused.

Video Samples reservoirs use actual decoded frames and O(requested count)
storage; declared totals are advisory. Replacement PNG completes before deleting
the old slot. Too few frames retain available samples and report requested/
observed/saved shortfall; decode/write failures keep their own cause and products.

Start captures composition settings. Saving uses native geometry and the same
inference delivery, independently of browser/another pass. Semantic captions use
class names; unresolved IDs show **Raw slot**. PNG publication is atomic with
owned staging and reusable pinned transfers through settlement.

Full uses H.264/Matroska, original timestamps/durations and admitted audio.
Hardware encoding is tried first; software H.264 may be selected before opening,
never mid-file. No codec/container/bitrate controls exist. Pauses affect wall time,
not output timing. Bounded encoder/audio/GPU custody uses worker backpressure.
During work output is `prediction.partial.mkv`; drain/trailer/file settlement and
rename publish `prediction.mkv`. Failure/cancellation keeps partial media/PNGs.
Flushed MKV clusters can remain playable without trailer/rename, but buffered tail
may be lost and no usable cluster means no playback guarantee. Output identifies
partial paths separately from committed artifacts.
