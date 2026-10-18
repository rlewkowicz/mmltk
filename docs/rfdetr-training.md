# RF-DETR training and native compilation

[Wiki index](README.md) · [Workflows](rfdetr-workflows.md) · [Merging and Resume](model-merging.md) · [Validation](validation.md#training-mathematics-and-compilation-evidence) · [Compilation diagnostics](logging.md#native-selective-compilation)

## Reference scope

[Source pins](../tools/oracle/rfdetr_sources.json) identify the upstream core,
plus models and dependencies used by numerical references. Comparisons require
equivalent configuration, admitted tensors, stochastic samples and optimizer
settings. Native compiled-data membership, augmentation, class layout and catalog
dimensions remain native policy. Training drops incomplete effective batches;
upstream may repeat/pad data to fill accumulation windows, so matched equations
do not imply matching epoch trajectories.

The native [runtime](../src/backend/models/rfdetr/core/runtime.cpp) disables
cuBLAS/cuDNN TF32; ordinary upstream imports select float32 matmul precision `high`.
Controlled comparisons override that policy as recorded in their manifests;
[encoder evidence](validation.md#encoder-pretraining-evidence) uses explicit IEEE
FP32 controls. Tensor dtype, autocast and internal matrix/convolution precision
are distinct. The
[container payload](../docker/nvidia-payload.json) pins NGC/Torch;
[validation](validation.md#training-mathematics-and-compilation-evidence) defines
numerical gates, controlled inputs and evidence limits. Checkpoint equivalence
alone establishes neither mathematical parity nor AP, convergence, throughput
or bitwise agreement under default kernels. Independent trajectories retain
measured differences. [Device selection](gpu-execution.md#workflow-device-selection)
and [failure handling](rfdetr-workflows.md#local-training-failures) are separate.

## Stock objective and mask mathematics

[Configuration](../src/backend/models/rfdetr/contract/model_config.h) and the
[preset catalog](../src/backend/models/rfdetr/contract/preset_catalog.h) own task
coefficients. Fresh training applies the stock recipe independently of imported
loss metadata; Resume retains admitted configuration. Detection/segmentation
classification/L1/GIoU coefficients are 1/5/2; mask BCE/Dice are 5/5 with point
ratio 16. Keypoint Preview uses classification 2 and joint L1/findability/
visibility/NLL coefficients 1/1/1/1. Matcher classification/L1/GIoU costs are
2/5/2; matching costs and objective coefficients are distinct.

The [criterion](../src/backend/models/rfdetr/core/detection_ops.cpp) supports
stock IA-BCE and configured focal/varifocal/position-supervised alternatives.
Group-DETR assigns independently within query groups. The target denominator
is a device scalar: summed targets in one global logical microbatch, multiplied
by group count when group losses are averaged, then clamped to at least one.
Final, selected auxiliary decoder and configured encoder outputs contribute
weighted task losses. `aux_loss=false` omits intermediate ordinary decoder
outputs/terms while retaining final and configured encoder outputs.

Dense and compact CUDA matcher costs use FP32. Untouched upstream AMP may round
costs differently and change assignments; references retain that stock result
as an observation and supply FP32 operands to upstream matcher functions for
the main comparison. IA-BCE separately casts detached quality into positive
and negative destination dtypes before scattering matched entries.

Training boxes use nonnegative center-box extents, FP16/BF16 area promotion and
`1e-7` union/enclosing-area clamps. Deployment projection is separate. Weighted
matcher costs keep nonfinite values until the complete layer Cartesian domain
is sanitized before slicing groups/images. Replacement is finite maximum plus
maximum absolute magnitude plus one, bounded by dtype maximum. Zero-point stock
mask matching retains its exceptional nonfinite/sentinel behavior.

[Mask losses](../src/backend/models/rfdetr/core/training_mask_loss.cpp) own
projection, packed categorical sampling, uncertain points, BCE and Dice.
Sparse masks retain spatial/query features and bias until selected projection;
decoder and encoder skip paths both apply learned spatial projection. Original
FP32 bias is added after the autocast matrix product, preserving promotion.
Categorical samples use nearest `grid_sample` identities, including half-pixel
rounding, without interpolating membership. Explicit fixture coordinates use
production sampling.

For sampled logits `z`, targets `y` and normalizer `N`, BCE averages points per
mask then sums over `N`. Dice per mask is
`[1 − (2 sum(sigmoid(z)y) + 1) / (sum(sigmoid(z)) + sum(y) + 1)] / N`.
Pairwise variants use the same normalization for every admitted pair. Empty
dense matches sum the empty projection; empty sparse matches sum each spatial,
query and bias operand before multiplying by zero. Both preserve connected zero
gradients and representation-specific nonfinite behavior.

[Depthwise segmentation autograd](../src/backend/models/rfdetr/core/segmentation_depthwise.cpp)
follows forward convolution autocast but saves original input/weights. Backward
casts incoming gradients/input to original weight precision and calculates
input/weight derivatives and bias reduction there. Its recorded custom operator
retains autocast dtype and original operands. CUDA forward locally bypasses
cuDNN selection; ordinary backward uses the pinned depthwise CUDA kernel, with
ATen double-backward tape when differentiable VJPs are requested. Parallel lanes
do not toggle global cuDNN policy. Encoder box heads operate on selected memory,
retaining selected derivatives while bounding pointwise work by admitted queries.

[Deformable attention](../src/backend/ml/layers/ms_deform_attn_autograd.cpp) is a
registered CUDA/autograd operation retained in FullTrace. It preserves lower
support-boundary derivatives, nonfinite multiplication outside support, tail
batch chunks, normalized-coordinate conversion and short weighted-reduction
order. CUDA atomic backward accumulation permits rounding differences.

### Keypoint objective

[GroupPose losses](../src/backend/models/rfdetr/core/keypoint_ops.cpp) use the
admitted per-class joint schema. Unannotated, occluded and visible targets remain
distinct: findability supervises annotated joints, visibility supervises visible
joints, and coordinate losses use finite annotated locations with valid positive
box area. L1 is divided by square-root area and active-joint count. Precision
uses lower-triangular parameters `(exp(log_l11), l21, exp(log_l22))`; Gaussian NLL
uses area-normalized Mahalanobis error and the log determinant. Invalid local
operands are sanitized before differentiable arithmetic; an empty valid set
contributes a connected zero. Pairwise matching uses the same four task costs
with class-local joint populations. [Prediction/OKS semantics](rfdetr-workflows.md#model-input-and-detection-selection)
remain separate from supervision visibility.

## Construction and tensor operation order

[Model construction](../src/backend/models/rfdetr/core/model.cpp) owns DINO
initialization, the Xavier sweep/deformable resets, cloned decoder vectors,
zero references/final box layers and independent encoder heads copied from
initialized detector heads. PE-Core-T and DINOv2-Small/Base topology comes from
the preset configuration; zero-decoder Atto uses encoder proposals directly.
GroupPose retains its class/joint masks, conditional query initialization and
joint attention. Loading replaces admitted tensors; partial transfer keeps
unmatched initialized destinations.

Native linear calls retain ATen operation order; the registered
[linear/ReLU boundary](../src/backend/models/rfdetr/core/linear.cpp) preserves
the upstream fused operation and derivative. Projector SiLU and grouped
attention preserve their native tensor layouts. Proposal ranking normalizes
full projected encoder memory; the single-group differentiable path normalizes
gathered pre-normalization rows. Algebraically equivalent slicing or bias/
activation expansion can change finite-precision values and gradients.
Installed-payload comparisons establish these choices; [numerical accuracy](https://docs.pytorch.org/docs/stable/notes/numerical_accuracy.html)
explains the general batched-versus-sliced limitation.

## Pretrained encoder initialization

[From scratch](rfdetr-workflows.md#training-initialization) initializes the complete
RF-DETR model under the requested seed, then replaces every encoder parameter and
buffer with admitted pretrained state. Projectors, proposals, decoder, task heads
and training-only supervision keep their seeded initialization. Dataset-owned
class/joint layout and ordinary stock recipe/query admission still apply.

The canonical [encoder assets](../src/backend/models/rfdetr/contract/encoder_pretraining.h)
own immutable source revisions, SHA-256, exact byte lengths, geometry and adaptation
revision. The [preset catalog](../src/backend/models/rfdetr/contract/preset_catalog.h)
maps the [16 presets](rfdetr-workflows.md#model-input-and-detection-selection)
to three original safetensors files:

| Original source | Native family | Source geometry |
| --- | --- | --- |
| `facebook/dinov2-small` | DINOv2-Small | Width 384, 12 layers, 6 heads, patch 14, 37×37 positions |
| `facebook/dinov2-base` | DINOv2-Base | Width 768, 12 layers, 12 heads, patch 14, 37×37 positions |
| `timm/vit_pe_core_tiny_patch16_384.fb` | PE-Core-T | Width 192, 12 layers, 3 heads, patch 16, 24×24 positions |

All sources have three input channels, one class-prefix token and no register
tokens. DINOv2 also carries its mask token. Acquisition and notices belong to the
[asset cache](build.md#model-asset-cache), independent of model execution.

[Native import](../src/backend/models/rfdetr/core/encoder_pretraining.cpp) holds an
opened, length- and digest-verified source through commit and checks it has not
changed. The bounded safetensors decoder rejects malformed/duplicate names,
unsupported or nonfinite tensor data, invalid shapes/offsets and incomplete
payload coverage. Complete source and destination inventories must agree with the
selected family. PE's exact classifier/attention-pool exclusions are checked by
name and shape; an arbitrary extra tensor or missing encoder tensor is an error.
No partial encoder load falls back to random weights.

Patch adaptation applies the pinned timm antialiased bicubic resize matrix and
its pseudoinverse, with default pseudoinverse tolerances, to the original FP32
patch kernels; equal patch size is identity.
Spatial positions use FP32 antialiased bicubic interpolation with
`align_corners=false`, preserving the prefix token exactly. The
[shared position operation](../src/backend/models/rfdetr/core/detail/position_embedding.h)
also serves runtime DINOv2/PE interpolation. Adaptation targets the preset's patch
and stored-position geometry without altering source width/layers/heads.

All exact and adapted tensors are staged before a rollback-protected encoder
commit. Import/adaptation currently runs once on CPU FP32 before moving the model
to CUDA; CPU placement is an implementation choice, not a mathematical requirement.
Distributed training imports only on rank zero, agrees source admission/provenance,
then uses the ordinary complete-model broadcast. Forward, backward, optimizer,
EMA and subsequent training remain on the existing CUDA execution path.

The initialized-model identity covers the resulting full model. Optional source
[provenance](model-merging.md#encoder-provenance) remains descriptive; saved
continuation does not depend on rerunning import. Independent
[encoder comparisons](validation.md#encoder-pretraining-evidence) check original
source coverage, adaptation, nonencoder preservation and bounded features for
every preset.

## Logical lanes and global batch

[Execution configuration](../src/backend/models/rfdetr/contract/execution_plan.h)
separates logical lanes from physical capacity. Defaults: one lane, Shared
gradients. Let `B` be global microbatch images, `A` accumulation, `L` lanes and
`W` selected GPUs:

| Mode | Models/optimizers | Microbatches per attempt K | Effective batch/model | Images/full session round |
| --- | --- | --- | --- | --- |
| Shared gradients | One | L × A | B × L × A | B × L × A |
| Independent models | L | A | B × A | B × L × A |
| Periodic averaging | L | A | B × A | B × L × A |

Every model uses all selected GPUs through DDP, with complete model/optimizer
state per rank; memory is not pooled or sharded. Independent/periodic modes
admit at most 16 models with one verified initialization and independent seeds,
recipes and state. [Merging](model-merging.md) defines synchronization/selection.

Rank-specific worker clamps change execution timing, never `B`, `K`, logical
draw order or objective. Complete `B × K` windows precede rank slicing. Uneven
`B % W` and `B < W` are valid: empty ranks skip forward and contribute zero while
participating in collectives. Every model needs one complete window per admitted
epoch; unused tails are reported separately.

Each rank shares one immutable compiled mapping/index per admitted file and a
NUMA-local reader pool across model, validation and final-test loaders. Resolved
aliases/hard links reuse file generations. Loaders keep independent schedules,
cursors, staging and cancellation; stopping one settles only its reads. Rank
processes retain separate GPU/NUMA resources; file pages share the OS page cache.

The [session](../src/backend/models/rfdetr/training/train.cpp) orders epoch,
round and stable model ID, attempting at most one update per nonexhausted model
per round. Counts precede gradient buckets, usage and finite/overflow agreement.
The [reducer](../src/backend/models/rfdetr/training/training_gradient_reducer.cpp)
combines tensor hooks/`autograd::grad`, c10d buckets and NCCL to overlap reduction
with backward. Hooks, buckets, streams and collective Work survive cancellation
until physical settlement. [GUI facts](gui-interaction.md#lane-and-recipe-controls)
are bound to settings revision or operation generation; Rust does not repeat
batch arithmetic. More lanes consume memory without guaranteeing throughput.

## Attempt execution and target lifetime

[TrainingLanes](../src/backend/models/rfdetr/training/training_lanes.cpp) owns
per-worker streams, model state, augmentation, matcher and target scratch.
One worker uses the working model; parallel workers copy changed parameters to
versioned replicas before forward. Active backbones/projectors/decoders use
LayerNorm, including spatial LayerNorm. There is no SyncBatchNorm collective;
parallel lanes reject mutable running-statistic buffers.

[TargetScratch](../src/backend/models/rfdetr/training/target_builder.cpp) retains
pinned metadata/packed-mask staging and device destinations. Nonempty lanes have
`max(2,A)` slots; empty lanes allocate no forward payload. Compatible views reuse
high-water capacity. Host refill waits for its H2D event; device overwrite also
waits for the final consumer event. Full rings wait for one DMA, not the device.

Each `TargetConsumerLease` orders upload before DN forward or criterion and
survives loss, backward/gradient harvesting and matcher completion. Retirement
records the actual consumer stream. Partial upload failure cannot authorize
reuse, and stale aliases cannot authorize replacement-storage reuse. Loader
pixels independently release after augmentation's last source read.

[Target counts](../src/backend/models/rfdetr/training/training_ops.cpp) publish
one int64 per global microbatch in logical order, independent of wave packing.
Consumers wait through events and use FP32 device normalizers. All `K` count
Work objects survive wave closure; scalar conversion protects slot reuse, and
a final dependency joins the optimizer-decision stream.

The reducer retains gradient/usage Work separately. Single-rank execution uses
host usage bookkeeping without usage transfers/collectives. One rank,
one contribution and one slot may adopt exclusive resizable dense gradients
with matching layout; aliases/expanded gradients copy to retained buckets.
Accumulated/distributed paths retain bucket semantics. Events order optimization;
globally unused gradients remain undefined under each recipe's missing-gradient
policy. Atto's unused decoder inputs/heads retain declared trainability while
optimizer admission excludes inactive state.

[Metric handoff](../src/backend/models/rfdetr/training/training_metrics.cpp)
packs finite controls, loss/scalar sums and availability plus per-microbatch
sufficient statistics into one FP32 packet. Distributed execution performs one
SUM; one rank needs none. Ratios follow global reduction and retain temporal
weighting. One pinned readback/completion exposes the numerical decision; count/
reducer finalization releases completed Work without another host wait. Other
training boundaries can still synchronize.

### Bounded next-batch preparation

[TrainingModel](../src/backend/models/rfdetr/training/training_model.cpp) may
prepare exactly one future batch on the existing worker pool after submitting
current backward, before metric settlement. Eligibility: Shared gradients,
`K=1`, nonempty local rank and a next batch in the same epoch. It reads immutable
annotations/prospective donors, constructs augmentation and uploads targets;
it holds no future loader pixels and performs no future forward or collective.

Admission verifies epoch/rank/microbatch, draw keys, image IDs and shape against
the acquired batch. Only then are donor history/clocks/cursors committed and
counts/forward submitted against current parameters; future pixel augmentation
and selected donor uploads also wait. Exceptions surface only if that batch is
admitted. Discard joins/retire storage without replacing the current result
with an unadmitted failure. Epoch changes, Resume, reconfiguration and failures
discard pending preparation first. Checkpoints record only committed state;
transient preparation is reconstructed. Cancellation closes admission and wakes
count consumers; submitted GPU/collective resources remain held through proven
physical completion.

## Accumulation, optimizer and EMA

Each of `K` admitted global microbatch losses contributes its gradient divided
by **K²**: upstream automatic optimization divides by K, then Lightning's
closure divides again. [TrainingStep](../src/backend/models/rfdetr/training/detail/training_step.h)
combines that factor with AMP scaling. LR does not cancel it or multiply by W.
Each microbatch keeps its own denominator; this is not concatenated-batch loss.
For global targets `N`, effective groups `G` (one when summing group losses) and
DN repetitions `D`, denominators are:

| Objective | Denominator |
| --- | --- |
| Ordinary | max(G × N, 1) |
| Match-Free | G × max(N, 1) |
| DN | max(D × N, 1) |

Upstream stock loss divides by `max(G × N / W,1)` before DDP averaging, giving
`max(G × N,W)`; native SUM uses `max(G × N,1)`. They agree for one GPU or
`G × N >= W`; with `W=2,G=1,N=1`, upstream gradients are half native gradients.
Native retains a world-size-independent objective and empty-rank support.
This is separate from activation LayerNorm.

Forward/criterion use selected autocast; backward and parallel harvesting occur
afterward, retaining FP32 probe derivatives. Gradient SUM, unscale and global
clipping never divide by W. Ranks agree before mutation: nonfinite loss fails;
finite-loss FP16 gradient overflow skips optimization everywhere. Scaler growth
installs only finite multiplied scales, resets its interval tracker and retains
backoff; BF16 has no FP16 scaler-growth policy.

[NativeOptimizer](../src/backend/models/rfdetr/training/native_optimizer.cpp)
retains each AdamW parameter's device age through missing gradients and Resume.
Fused AdamW is requested by default and uses LibTorch fused multi-tensor updates
for real CUDA parameters on compute capability ≥8.0; supported foreach/eager
paths cover other cases. Compatible device/dtype groups retain individual ages.
Muon/SGD remain independent update paths. Clipping uses the device global norm
and `min(max_norm/(norm+1e-6),1)` without an unused host norm. Statistics reduce
before ratios; each global microbatch/image counts once.

### Recipes and schedules

The [model catalog](../src/backend/models/rfdetr/contract/preset_catalog.h) supplies
Train defaults when selecting a preset or custom weights. The rows
select the upstream `TrainConfig` AdamW recipe below, plus 100 epochs, batch 4,
accumulation 1, inherited validation batch, gradient clipping `0.1`, EMA enabled
with decay `0.993` and tau 100, mixed precision and fused optimization enabled,
and an unfrozen encoder. Queries follow model topology; evaluation allows 500
detections. Upstream detection, segmentation and keypoint classes share these
defaults; task objective coefficients remain in their catalog rows.

Selection replaces global and per-model recipes and clears their overrides,
preserving lane membership, seeds, dataset/output paths and execution placement.
Explicit edits submitted with selection take precedence. Later edits and saved
settings retain their values; Resume restores the saved request without applying
catalog defaults. Native augmentation and loading policy remain independent.

[TrainRecipeSettings](../src/backend/models/rfdetr/contract/train_recipe.h)
serves globals and each model. New entries copy settled globals; existing entries
retain overrides through mode changes. Reset clears only the selected scope's
overrides, preserving optimizer. IDs are stable/nonreused; seeds derive from
session seed and model ID.

| Default | AdamW | Muon | SGD |
| --- | ---: | ---: | ---: |
| Decoder LR | 0.0001 | 0.0002 | 0.01 |
| Encoder LR | 0.00015 | 0.0003 | 0.001 |
| Weight decay | 0.0001 | 0.0005 | 0.0001 |
| Momentum setting | 0.95 | 0.9 | 0.9 |
| Scheduler | Step | Cosine | UltralyticsLinear |
| Warmup epochs | 0 | 3 | 3 |
| Warmup momentum | 0 | 0.8 | 0.8 |
| Minimum LR factor | 0 | 0.01 | 0.01 |
| Bias warmup LR | 0 | 0 | 0.1 |

All use component decay 0.7, encoder-layer decay 0.8 and Step drop epoch 100;
Nesterov defaults off. Momentum applies to Muon/SGD. SGD uses coupled decay,
zero dampening and skips undefined gradients. Its first momentum buffer copies
the decayed gradient; later buffers use `momentum × buffer + decayed_gradient`.
Nesterov requires SGD and positive momentum. Muon substitutes zero for undefined
trainable gradients in matrix and auxiliary Adam branches, advancing momentum,
decay and auxiliary ages on an update.

Muon routes rank-two/rank-four weights with explicit detector/embedding
exclusions. Singular DN label/task embeddings enter Muon; packed self-attention
`in_proj_weight` enters auxiliary Adam because its name lacks `.weight`.
Persisted groups retain that policy; changes require coherent Resume admission.
Encoder LR includes layer-position decay and component factor squared; selected
transformer groups use decoder LR × component factor, others decoder LR. SGD
bias warmup uses names containing `bias`, independently of decay; bias/nonbias
groups remain distinct even when base LR/decay match.

Frozen parameters are excluded from autograd/reduction/decay/updates. State is
allocated lazily on activation, retaining named state/ages. EMA independently
owns the complete eligible model.

[TrainingSchedule](../src/backend/models/rfdetr/training/training_schedule.cpp)
separates consumed attempts/microbatches from successful updates:

- **Step/Cosine:** retain the first admitted epoch's attempt count. LR warmup
  truncates `reference_attempts × warmup_epochs`. Step drops to 0.1 at
  `lr_drop × reference_attempts`; Cosine uses the post-warmup planned-budget
  fraction clamped to `[0,1]`. Momentum warmup uses the untruncated reference
  and restores target at the boundary. Extension retains reference/consumption
  and extends planned attempts.
- **UltralyticsLinear:** SGD only. Epoch factor is
  `max(1−e/E,0) × (1−q)+q`, with zero-based epoch, original horizon E and minimum
  factor q. Epoch LR is base group LR × factor. Warmup length rounds to nearest
  even `min(warmup_epochs,E−1) × first_epoch_microbatches`, without a 100-step
  floor. Each warmup microbatch interpolates LR from zero (bias: `warmup_bias_lr`)
  to epoch LR and momentum from warmup to target. At exit the last LR holds
  until the next epoch and the last momentum holds thereafter; target is not
  forced. Resume keeps the original horizon; extended epochs use minimum clamp.

For `E=4`, four microbatches/epoch, warmup 0.375, base LR 0.01, q=0.01 and
momentum 0.9: two warmup microbatches yield bias/nonbias LR `0.1/0`, then
`0.055/0.005`, and momentum `0.8`, then `0.85`. Epoch one sets both LRs to
0.007525, leaving momentum 0.85. This scheduler does not import changing
accumulation, nominal-batch decay scaling or automatic optimizer selection.
Overflow consumes data/schedule time but contributes no successful-update images.

### Final-epoch policies

`unfreeze_encoder_last_epochs` and `disable_augmentation_last_epochs` default to
zero independently. Positive N applies before epoch `max(0,epochs−N)`; encoder
means `backbone.0.encoder`. Applied latches survive Resume/extension; pending
policies follow the new horizon. Exact Resume rejects changed N/recipe/data
policy. Activation drains work and refreshes optimizer/reducer/graph bindings
while retaining optimizer, scaler, scheduler, EMA and augmentation storage.

### EMA

[EMA](../src/backend/models/rfdetr/training/model_ema.cpp) is off by default.
First update copies current parameters; later decay is
`decay × (1−exp(−completed_updates/tau))` for positive tau, otherwise configured
decay. Recoverable finite-loss FP16 overflow leaves parameters/moments unchanged
and backs off the scaler, but the completed attempt still advances EMA over
unchanged parameters. Resume restores ages, scaler and EMA state/count.
Overflow diagnostics remain independently gated. [Workflows](rfdetr-workflows.md#training-and-query-limits)
defines evaluation weight selection and artifacts.

## Sparse data planning and stochastic identity

[TrainingDataPlan](../src/backend/models/rfdetr/training/training_data_plan.cpp)
owns sparse class support, immutable shards and deterministic epoch schedules.
Physical compiled IDs survive holes; crowd supplies no class support, while
empty/crowd-only images remain members. Shared uses the whole dataset;
independent/periodic partition between models before slicing ranks.

| Policy | Meaning |
| --- | --- |
| Off | Approximately equal deterministic image shards |
| Stratified | Prioritize scarce support within shard capacities |
| Rare repeats + Stratified | Image factor is the maximum supported-class `max(1,sqrt(t/f_c))`; f_c is full-dataset supporting-image fraction; empty images use one |

Rare-repeat defaults: threshold 0.001, maximum factor 10, total-draw multiplier 4;
admitted ranges are finite `(0,1]`, `[1,64]` and `[1,16]`. Every image keeps its
base occurrence. Seeded fractional rounding proposes extras, bounded reservoir
thinning enforces the budget, then shuffle/complete-window admission reports
tails. Support, exposure, missing shard classes and tails are distinct facts;
repeats cannot create missing support.

Loaders consume schedules/Resume offsets without reshuffling. Draw/microbatch
identity governs augmentation, DN noise and mask samples independently of rank,
worker, completion or padding. Copy-paste donors use retained logical descriptors;
physical caches supply selected pixels. Checkpoints retain/reconstruct identities,
cursors and donor history.

### Augmentation preparation and transfer

[Defaults](../src/backend/models/rfdetr/contract/workflow_requests.h): geometry,
resize, color, noise, blur and occlusion each use probability 0.25 and strengths
0.0375–0.375. Strength scales signed effects about zero and size/contrast/
saturation about one. Copy-paste probability is 0.80, donor scale 0.625–1.375.
Saved overrides retain full precision and authority.

The [training augmenter](../src/backend/models/rfdetr/training/gpu_augment.cpp)
resolves plans before submission. Only selected donors differing from the
materialized slot cause reads/uploads; another annotation from resident pixels
updates support only. Skipped pastes never upload unselected donors. Donor
history follows admitted contributions even without a paste. Physical caches
retain original pixels and upload support only for replaced slots.

The [GPU executor](../src/backend/models/rfdetr/augmentation/gpu_augment.cpp)
uses two parameter-staging slots. Staging completion follows its last host read/
upload; a separate consumer event protects pixels/donors/resources. Pointwise
CUDA processes four adjacent pixels with aligned float4 where valid, scalar
edges/widths otherwise, including reversal. Remapping/copy-paste retain their
sampling path; this is not a measured throughput claim.

[Annotation support](../src/backend/models/rfdetr/augmentation/annotation_support.cpp)
uses analytic continuous-box visibility when geometry permits; erasure, donor
occlusion and masks use output pixel centers. Visibility-only queries stop at
first support; target construction computes complete area/bounds. Continuous
boxes survive mask disappearance. Donor RLE, known-empty masks, half-open
erasure and categories retain common training/preview meaning. Joint geometry,
flip correspondence and visibility follow the admitted schema; [data semantics](datasets.md)
defines their persisted meaning.

## Match-Free and denoising adaptations

[Supervision](../src/backend/models/rfdetr/contract/training_supervision.h)
defaults to Hungarian, with optional Match-Free and independent DN. Detection/
segmentation models with decoder layers support all four combinations.
Zero-decoder Atto and keypoint models require stock Hungarian without DN;
capability admission rejects unsupported combinations. [TrainingSupervisionImpl](../src/backend/models/rfdetr/core/training_supervision.cpp)
owns probes, correspondence, noise, isolated queries and routed objectives.
Deployment omits training-only state; full continuation retains it.

Match-Free adapts learned dense correspondence and sparse pair objectives from
[Beyond Hungarian, §§3.2–3.4](https://arxiv.org/html/2603.08514v1), without an
established author-code parity claim. Detached topology retains each valid row's
winning query, admits entries ≥rho times the column maximum, then normalizes
retained differentiable rows with epsilon. Dense/sparse weights use the same
pair objective with independent correspondence/query coefficients.

Segmentation adds learned target-mask-pooled spatial features to class/box
probes. Shared sampled logits/targets provide pairwise BCE/Dice alongside class/
L1/GIoU; zero mask coefficients omit mask work. Storage follows query/target/
point populations, not image pixels for every pair. Empty images add one
background focal term per supervised layer with target/group normalization,
without dummy targets. These mask/empty-image rules extend the box paper.

DN follows [DN-DETR](https://arxiv.org/html/2203.01305v3): noisy labels/boxes
reconstruct known targets. Ordinary queries cannot see DN queries; DN groups
see neither ordinary nor other DN groups. Native task embeddings and
alternative-label noise are adaptations. Owned seeded DN/mask draws leave the
ordinary stock stream untouched.

Ordinary/DN segmentation shares the mask head and RF-DETR BCE/Dice/sampling;
selected auxiliary decoder layers contribute, encoder DN does not. Denominators
include DN repetitions. Invalid padding contributes nothing; empty enabled mask
reconstruction retains connected sparse zeros. [Mask DINO appendix B.2](https://arxiv.org/html/2206.02777v2)
supports mask reconstruction from noisy queries, without importing its complete
architecture. No accuracy/convergence benefit is asserted.

## Native selective compilation

Native Selective/FullTrace use pinned LibTorch/TorchScript. Upstream uses
`torch.compile(model,dynamic=True)` with Dynamo/Inductor graph breaks and
suppressed failures. Capture, fusion and performance are not equivalent.
Selective is the native training default and includes lane-owned criterion
traces. Optimizer fusion is independent.

| Mode / CLI | Captured boundary |
| --- | --- |
| None / `none` | Ordinary C++ orchestration, ATen and custom CUDA/autograd |
| Selective / `selective` | Bounded model/criterion regions below |
| FullTrace / `full` | Guarded whole-model forward, including registered operations; criterion, backward invocation, optimizer and host policy stay separate |

| Selective region | Captured work | Ordinary boundary |
| --- | --- | --- |
| Backbone/projector | Backbone tensors and projection | Orchestration/spatial layout |
| Decoder tail/layer | Cross-attention residual norm, linear/ReLU pair, final residual norm | Group self-attention, DN isolation, deformable attention |
| Segmentation spatial tail/block | Pointwise/residual tail and spatial projection | Resize/custom depthwise derivative |
| Segmentation query transform | Shared ordinary/encoder/DN MLP and projection | Sparse structure/supervision |
| Lane loss operators | Selected classification/BCE/Dice/pair contractions | Targets, sampling, assignment, normalization, optimizer |

[SelectiveTensorRegion](../src/backend/models/rfdetr/core/detail/selective_compilation.h)
uses [TensorTrace](../src/backend/models/rfdetr/core/tensor_trace.cpp), with one
slot per train/eval mode. Preparation arms a batch; the first compatible real
forward records and returns its own outputs, with no dummy pass or first-result
replay. Identical preparation reuses state; `none` disables it. Handoffs support
at most two input/output tensors.

Guards include device/dtype/rank/extents, grad mode/requires-grad, effective
AMP dtype/cache policy, attention backend/precision and registered owner tensor
signatures. Backbone/spatial regions require prepared batch/fixed dimensions;
decoder/query regions allow varying batch/query extents with fixed rank/width.
Mismatch runs ordinary code while retaining the usable slot; an initial short
batch leaves the full-batch slot armed. Replacement is transactional: recording
failure propagates without replaying failed GPU work or losing the prior graph.

Registered parameters/buffers are live aliases, observing in-place optimizer,
Resume and EMA copies. Device changes invalidate owner registration. Train/eval
graphs retain separate custody, including outstanding backwards. Permanently
zero dropout is omitted where optimized CUDA backward would otherwise consume
RNG. Recording and every reuse preserve required stochastic identity.

Recording preserves the caller's autocast weight cache, including shared and
pre-cached parameter casts, so gradient accumulation follows ordinary eager
execution. The graph disables its local JIT autocast rewrite; replay excludes
ambient dispatcher autocast while keeping recorded casts, protecting FP32
operations from recasting. FP16/BF16 graphs use the function-local simple
executor; FP32 retains profiling. No process-global fuser/cache/autocast toggle
implements this policy. [AMP](https://docs.pytorch.org/docs/stable/amp.html)
explains local precision regions and JIT autocast limitations.

[Loss traces](../src/backend/models/rfdetr/core/detail/traced_loss_cache.h)
guard coefficients/effective CUDA autocast and allow dynamic extents; point
normalization stays outside. [Decoder attention](../src/backend/models/rfdetr/core/decoder_attention.cpp)
uses explicit Q/K/V plus SDPA, preserving distinct parameter/position gradients
when values coincide. Grouping/padding/leakage remain native policy. Accelerated
backend selection is default; model-local math SDPA casts once and locally
excludes autocast, preserving boolean/additive masks without promising a kernel.

Annotations, target topology, schedules, failure/metric decisions and host work
remain ordinary CPU policy. Hungarian retains CPU assignment after GPU costs;
Match-Free replaces assignment, not orchestration. Neither tracing nor future
preparation captures an entire training step.

Training admits complete effective batches. Its evaluation pads to configured
batch with zero inactive images, then trims predictions before metrics/losses.
Standalone native prediction/evaluation carries compilation mode to construction;
short batches use guarded fallback. GUI prediction is batch one. ONNX/TensorRT
use independent engines. CLI selection is `--compile-mode none|selective|full`.

### FullTrace and export

FullTrace shares first-real-forward recording, live aliases, transactional mode
slots and fallback. Specialization includes input-mask presence and requested
mask outputs. Reflected output traversal retains the complete decoder/encoder/
sparse-mask tensor inventory. Target-dependent DN/Match-Free capture is rejected;
those extensions use None or Selective.

ONNX export uses an independent inference clone with private mode/caches/graphs.
Mutable sources copy weights; sealed immutable sources may share immutable
storage. Only the clone selects reference deformable attention/math SDPA.
Lowering retains depthwise precision, linear/ReLU and SiLU→Sigmoid/Mul.
[Export](rfdetr-workflows.md#export-formats) defines failure/publication.

### Compiler choices and native feasibility

TorchScript is a pinned integration and is [deprecated upstream](https://docs.pytorch.org/tutorials/intermediate/torch_compile_tutorial.html#benefits-over-torchscript).
Registered dispatcher/autograd operators keep opaque CUDA forward/VJP inside
eager/traced execution; registration alone does not validate derivatives.
[Diagnostics](logging.md#native-selective-compilation) distinguishes recording,
executed graphs, fusion forms, autocast and TF32. [Validation](validation.md#training-mathematics-and-compilation-evidence)
defines evidence limits; selectable modes do not establish identical arithmetic
or faster execution.
