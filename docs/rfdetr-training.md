# RF-DETR training and selective compilation

[Wiki index](README.md) · [Workflow and artifacts](rfdetr-workflows.md) · [Model merging and Resume](model-merging.md) · [Validation evidence](validation.md#training-mathematics-and-compilation-evidence) · [Compilation diagnostics](logging.md#native-selective-compilation)

## Reference scope

Stock Hungarian training targets the mathematics of
[RF-DETR commit e9a138f70cc14e3fddca029a0683055e34ab8f35](https://github.com/roboflow/rf-detr/tree/e9a138f70cc14e3fddca029a0683055e34ab8f35).
Comparisons require equivalent model configuration, admitted tensors,
stochastic samples, and optimizer settings. Native compiled-data membership,
augmentation choices, class layouts and model catalog dimensions retain their
own contracts. Native training drops incomplete effective batches; the pinned
upstream can repeat or pad data to fill accumulation windows. Equal equations
on an admitted sequence therefore do not imply identical epoch trajectories.

The native [runtime](../src/backend/models/rfdetr/core/runtime.cpp) disables
cuBLAS and cuDNN TF32. Upstream selects float32 matmul precision `high`.
Tensor dtype, autocast, and internal matrix/convolution precision are separate
policies. Numerical fixtures control precision and stochastic inputs;
checkpoint import equivalence alone is not a training-mathematics oracle.
The [validation reference](validation.md#training-mathematics-and-compilation-evidence)
owns coverage, tolerances and evidence limits. These checks establish neither
COCO AP, convergence, throughput nor bitwise equivalence to upstream's default
kernel selection.

[nvidia-payload.json](../docker/nvidia-payload.json) pins NGC 26.08 and Torch
`2.14.0a0+4fdf77b940`. Upstream dependency links below explain equations, not the
installed payload's entire implementation. [GPU selection](gpu-execution.md#workflow-device-selection)
and [failure/OOM handling](rfdetr-workflows.md#local-training-failures) have
separate operational contracts.

## Stock objective and mask mathematics

[NativeRfDetrConfig](../src/backend/models/rfdetr/contract/model_config.h) and
the [preset catalog](../src/backend/models/rfdetr/contract/preset_catalog.h)
own the recipe. Fresh training applies current stock loss coefficients
independently of loss metadata embedded in imported weights. Resume retains its
admitted configuration. Stock catalog coefficients are classification 1,
L1 box 5 and GIoU 2; segmentation additionally uses mask BCE 5 and Dice 5,
with point-sampling ratio 16. Matcher classification/L1/GIoU weights remain
2/5/2. Objective coefficients and matching costs have distinct roles.

The ordinary [criterion](../src/backend/models/rfdetr/core/detection_ops.cpp)
retains configured classification policy, including stock IA-BCE and the
focal/varifocal/position-supervised alternatives. Group-DETR assignment operates
independently within each query group. The target normalizer remains a device
scalar: the summed target count for one global logical microbatch, multiplied
by the group count when group losses are averaged, then clamped to at least one. Main,
selected auxiliary decoder, and two-stage encoder outputs contribute their
weighted classification, L1, GIoU and applicable mask losses.

Training geometry follows the pinned
[box operations](https://github.com/roboflow/rf-detr/blob/e9a138f70cc14e3fddca029a0683055e34ab8f35/src/rfdetr/utilities/box_ops.py):
nonnegative center-box extents, FP16/BF16 area promotion, and `1e-7` clamps on
union and enclosing-area denominators. Deployment's box projection remains its
own consumer. Weighted matcher costs retain nonfinite values until the full
layer Cartesian domain is sanitized before image/group slicing. The replacement
uses finite maximum plus maximum absolute magnitude plus one, bounded by the
dtype maximum. A zero-point stock mask matcher retains its exceptional
nonfinite/sentinel behavior.

The shared [mask-loss implementation](../src/backend/models/rfdetr/core/training_mask_loss.cpp)
owns dense/sparse projection, packed categorical target sampling, uncertain
point selection, BCE and Dice. Sparse masks retain spatial features, query
features and bias until selected projection is needed. Both decoder and
encoder skip-block paths apply the learned spatial projection.
Categorical target samples follow nearest `grid_sample` identities, including
half-pixel rounding, without interpolating class membership. Explicit fixture
coordinates use the same production sampling entrypoints.

For sampled logits `z`, targets `y` and mask normalizer `N`, BCE averages
pointwise binary cross entropy per mask and sums those means over `N`.
Dice contributes
`[1 - (2 * sum(sigmoid(z) * y) + 1) / (sum(sigmoid(z)) + sum(y) + 1)] / N`
per mask. Pairwise variants retain every admitted query/target pair with the
same point normalization. Empty matched dense masks sum the empty projection;
empty sparse masks sum each spatial/query/bias operand before multiplication
by zero. These expressions preserve both connected zero gradients and their
representation-specific nonfinite behavior.

The [segmentation depthwise derivative](../src/backend/models/rfdetr/core/segmentation_depthwise.cpp)
follows the pinned
[segmentation head](https://github.com/roboflow/rf-detr/blob/e9a138f70cc14e3fddca029a0683055e34ab8f35/src/rfdetr/models/heads/segmentation.py):
forward follows convolution autocast, while backward retains original input
and weights, casts incoming gradients and input to original weight precision,
and computes input/weight derivatives and bias reduction there. This is an
operation-local native autograd boundary; parallel lanes do not toggle
process-global cuDNN policy. Encoder box heads run on selected memory rather
than every spatial position, preserving selected values and derivatives while
reducing pointwise work to the admitted query count per group.

## Logical lanes and global batch

The canonical [execution configuration](../src/backend/models/rfdetr/contract/execution_plan.h)
separates logical training lanes from physical worker capacity. Defaults are
one lane and **Shared gradients**. Let `B` be the global microbatch image count,
`A` the fixed accumulation count, `L` the configured lane count, and `W` the
selected GPU count:

| Mode | Model/optimizer owners | Microbatches per model attempt, K | Effective batch per model | Images in a full session round |
| --- | --- | --- | --- | --- |
| Shared gradients | One | L × A | B × L × A | B × L × A |
| Independent models | L | A | B × A | B × L × A |
| Periodic averaging | L | A | B × A | B × L × A |

Every model uses all selected GPUs through DDP. Each rank retains the complete
model and its optimizer state; GPU memory is not pooled or sharded. Independent
and periodic modes admit at most 16 models and can use different optimizer
recipes. They begin from the same verified complete initialization, then use
their own seeds and state. [Periodic and final merging](model-merging.md) own
the subsequent synchronization and deployment selection.

Physical workers can be clamped differently on different ranks. This changes
when contributions execute, never `B`, `K`, the logical draw order, or the
intended objective. Complete `B × K` windows are admitted before rank slicing.
Uneven `B % W` and `B < W` are valid; an empty rank contributes zero without
constructing an empty forward batch and still participates in the collective
protocol. Every model must have at least one complete window in an admitted
epoch. Unused tails are reported separately.

The [training session](../src/backend/models/rfdetr/training/train.cpp) orders
communication by epoch, round, and stable model ID. A round attempts at most
one update per nonexhausted model. Count collectives precede gradient buckets,
usage, and finite/overflow agreement within a model's turn. The
[gradient reducer](../src/backend/models/rfdetr/training/training_gradient_reducer.cpp)
uses tensor hooks with native `autograd::grad`, c10d bucket assignment, and
NCCL transport to overlap ready reductions with remaining backward work.
Hook, bucket, stream, and collective Work custody survive cancellation until
physical use settles.

Native facts bind configuration calculations to settings revision and runtime
capacity to operation generation; [GUI controls](gui-interaction.md#lane-and-recipe-controls)
display them without repeating the arithmetic. More lanes cost memory without
guaranteeing throughput.

## Accumulation, optimizer and EMA

For the mode's `K` admitted global logical microbatches, each loss contributes
its gradient divided by **K²**. The pinned
[training module](https://github.com/roboflow/rf-detr/blob/e9a138f70cc14e3fddca029a0683055e34ab8f35/src/rfdetr/training/module_model.py)
divides its automatic-optimization return by K, then
[Lightning 2.6.0's closure](https://github.com/Lightning-AI/pytorch-lightning/blob/2.6.0/src/lightning/pytorch/loops/optimization/automatic.py)
divides it again. Native
[TrainingStep](../src/backend/models/rfdetr/training/detail/training_step.h)
combines that exact factor with AMP scaling. Learning rate is not adjusted to
cancel the extra division or multiplied by `W`. Each microbatch retains its
own target denominator; this is not a single concatenated-batch objective.
With globally summed target count `N`, effective group count `G` (one when
group losses are summed), and DN repetition count `D`, ordinary losses divide
by `max(G × N, 1)`, Match-Free by `G × max(N, 1)`, and DN by `max(D × N, 1)`.

Forward and criterion run inside the selected autocast scope. Backward and
parallel gradient harvesting run after it exits, retaining FP32 probe
derivatives. Distributed gradient **SUM**, unscaling and global clipping follow;
neither counts nor gradients are divided by `W`. Ranks agree finite-loss and
overflow decisions before mutation. A nonfinite loss fails the session;
recoverable FP16 gradient overflow skips the update on every rank.
[NativeOptimizer](../src/backend/models/rfdetr/training/native_optimizer.cpp)
keeps each AdamW parameter's own age, including undefined-gradient gaps and
unequal resumed ages. Bias correction uses those device scalars, following
[PyTorch multi-tensor Adam](https://github.com/pytorch/pytorch/blob/v2.9.0/torch/optim/adam.py).
Clipping retains the device global norm and
`min(max_norm / (norm + 1e-6), 1)` scaling without requesting an unused host
norm. Loss numerators and other sufficient statistics are reduced before
ratios are computed. Each global microbatch and processed image is counted
once, preserving the ordinary temporal weighting of epoch losses.

### Recipes and schedules

Canonical [TrainRecipeSettings](../src/backend/models/rfdetr/contract/train_recipe.h)
serves global defaults and each model. New entries copy settled globals and
retain independent overrides through mode changes. Reset clears only the
selected scope's overrides, retaining its optimizer. IDs are stable/nonreused;
new seeds derive from session seed and ID.

| Catalog default | AdamW | Muon | SGD |
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

All catalog rows use component decay `0.7`, encoder-layer decay `0.8`, and
Step drop epoch `100`; Nesterov defaults off. The momentum setting applies to
Muon and SGD. Native SGD uses coupled weight decay, zero dampening, and skips
undefined gradients. Its first momentum buffer copies the
decayed gradient; later buffers use `momentum × buffer + decayed_gradient`.
Nesterov requires SGD with positive momentum. Existing AdamW/Muon equations,
parameter-group LR factors, and decay selection retain their native policy.

Encoder groups use encoder LR with layer-position decay and the square of
the component factor; selected transformer groups use decoder LR times the
component factor, and remaining groups use decoder LR. SGD bias-warmup roles
derive from full parameter names containing `bias`, without changing which
parameters receive decay. Bias/nonbias groups stay distinct even when their
base LR and decay happen to match.

Frozen parameters remain outside autograd, reduction, decay, and optimizer
updates. State is allocated lazily when a parameter becomes active, preserving
previous named state and per-parameter ages. EMA owns the complete eligible
model independently of optimizer-state allocation.

[TrainingSchedule](../src/backend/models/rfdetr/training/training_schedule.cpp)
persists consumed attempts and microbatches separately from successful updates:

- **Step/Cosine** capture the first admitted epoch's attempt count as their
  reference. LR warmup truncates `reference_attempts × warmup_epochs`; Step
  drops to `0.1` at `lr_drop × reference_attempts`, while Cosine uses the
  post-warmup fraction of the planned attempt budget, clamped to `[0, 1]`.
  Native momentum warmup uses the untruncated reference and restores the target
  at its boundary. Epoch extension keeps the saved reference and consumed
  attempts while extending the planned budget.
- **UltralyticsLinear**, admitted only for SGD, uses
  `f(e) = max(1 − e / E, 0) × (1 − q) + q`, with zero-based epoch `e`, original
  epoch horizon `E`, and minimum factor `q`. Before each epoch, group LR becomes
  `base_group_lr × f(e)`. Warmup length is round-to-nearest-even of
  `min(warmup_epochs, E − 1) × first_epoch_microbatches`, with no 100-step floor.
  Each warmup microbatch interpolates from zero (bias groups: `warmup_bias_lr`)
  to the epoch LR, and from warmup momentum to target momentum. At warmup exit
  it holds the last interpolated LR until the next epoch event and holds the
  last interpolated momentum thereafter. It does not force the target value.
  The original horizon survives Resume; extended epochs use the minimum clamp.

The SGD scheduler follows the linear/warmup behavior of
[Ultralytics commit 7547229b124fa117215e20f28364b4201bad01a4](https://github.com/ultralytics/ultralytics/blob/7547229b124fa117215e20f28364b4201bad01a4/ultralytics/engine/trainer.py),
with the native recipe values above. Native
[scheduler fixtures](../src/backend/models/rfdetr/training/tests/execution_policy.test.cpp)
retain its warmup-exit behavior.
For example, `E=4`, four microbatches per epoch, warmup `0.375`, base LR `0.01`,
`q=0.01`, and momentum `0.9` produce two warmup microbatches. Bias/nonbias LR
are `0.1/0` then `0.055/0.005`; momentum is `0.8` then `0.85`. Those values hold
until epoch one sets both LRs to `0.007525`, leaving momentum `0.85`.
The policy does not import changing accumulation, nominal-batch decay scaling,
or an automatic optimizer choice. Schedule events follow logical order;
overflow consumes data and schedule time but adds no successful-update images.

### Final-epoch policies

`unfreeze_encoder_last_epochs` and `disable_augmentation_last_epochs` default
to zero and are independent. A positive `N` applies before zero-based epoch
`max(0, epochs − N)`. Encoder means `backbone.0.encoder`. Each applied latch
survives Resume and later epoch extension; a pending policy uses the new horizon.
Exact Resume rejects changes to `N` or recipe/data policy. Activation occurs
after work drains, updating active optimizer/reducer bindings and compiled
assumptions while retaining optimizer, scaler, scheduler, EMA, and reusable
augmentation storage.

### EMA

Optional [EMA](../src/backend/models/rfdetr/training/model_ema.cpp) is off by
default. Its first update copies current parameters, as
[AveragedModel](https://github.com/pytorch/pytorch/blob/v2.9.0/torch/optim/swa_utils.py)
does before later averaging. Subsequent updates use
`decay * (1 - exp(-completed_updates / tau))` when tau is positive, or the
configured decay otherwise. On a recoverable finite-loss FP16 gradient
overflow, the optimizer leaves parameters/moments unchanged and the scaler
backs off; the completed attempt still advances EMA over those unchanged
parameters. This follows the pinned automatic-optimization cadence and
[Lightning mixed precision](https://github.com/Lightning-AI/pytorch-lightning/blob/2.6.0/src/lightning/pytorch/plugins/precision/amp.py).
Current-format Resume restores optimizer ages, scaler and EMA count/state.
Overflow diagnostic collection is gated independently of required failure
handling. The [workflow guide](rfdetr-workflows.md#training-and-query-limits)
owns selected-weight evaluation and saved artifacts.

## Sparse data planning and stochastic identity

[TrainingDataPlan](../src/backend/models/rfdetr/training/training_data_plan.cpp)
owns sparse distinct class presence per original image, immutable model shards,
and deterministic epoch schedules. Crowd contributes no support; empty/crowd-only images
remain members. Shared mode uses one full-dataset shard; independent/periodic
modes partition between models before rank-slicing global microbatches.

**Off** uses approximately equal deterministic image shards. **Stratified**
prioritizes scarce support while respecting shard capacity. **Rare repeats +
Stratified** additionally uses `max(1, sqrt(t / f_c))` per supported class,
where `f_c` is its supporting-image fraction in the complete training dataset,
and the largest factor on each image. Empty images use one. Defaults are
threshold `0.001`, maximum factor `10`, and maximum total-draw multiplier `4`.
Admission requires finite threshold
in `(0, 1]`, factor in `[1, 64]`, and multiplier in `[1, 16]`.

Every image has a base occurrence. Seeded fractional rounding proposes extras;
bounded reservoir thinning enforces the draw budget without discarding bases.
The final schedule is shuffled, then complete-window admission reports unused
tails. Unique support, repeated exposure, missing shard classes, and tails are
separate native facts. Repeats change exposure and cannot invent missing support.

The loader consumes the explicit schedule and Resume offset without reshuffling
or choosing shards. Draw and microbatch identities govern augmentation, DN noise,
and mask samples independently of rank, physical worker, completion order, or
padding. Copy-paste donors come from compact logical source descriptors retained
per contribution stream; physical decoded caches only supply the selected data.
Session checkpoints retain/reconstruct these identities and cursors, as described
under [exact Resume](model-merging.md#whole-session-resume).

## Match-Free and denoising adaptations

The canonical [supervision declaration](../src/backend/models/rfdetr/contract/training_supervision.h)
selects Hungarian by default, optional Match-Free assignment, and independently
enabled denoising. All four combinations support detection and segmentation.
[TrainingSupervisionImpl](../src/backend/models/rfdetr/core/training_supervision.cpp)
owns probes, correspondence, noise, isolated query layout and routed objectives.
Deployment omits training-only supervision while preserving ordinary outputs;
full current-format continuation retains its state.

Match-Free takes its learned dense correspondence and sparse weighted-pair
objective from [Beyond Hungarian, sections 3.2–3.4](https://arxiv.org/html/2603.08514v1).
The review found no author-released implementation; native sparsification and
the extensions below are explicit RF-DETR adaptations, not verified parity
with author code. Native sparse topology is chosen from detached dense values:
retain each valid row's winning query to establish positive column maxima,
admit dense entries at least `rho` times that column maximum, and normalize
the retained differentiable row values with a small denominator epsilon.
Dense and sparse weights apply to the same pairwise objective, with independently
configured correspondence/query coefficients.

Segmentation adds target-mask-pooled spatial features to each class/box probe
through a learned projection. Thus different masks can condition distinct
probes even with identical class and box. Shared sampled logits/targets supply
pairwise BCE and Dice for both correspondence and query objectives, alongside
classification/L1/GIoU. Mask work is omitted when both mask coefficients are
zero. Storage follows admitted query/target/point populations, not a full
query-by-target-by-image-pixel tensor. Empty images receive a background focal
classification term once per supervised layer with the same target/group
normalizer; they need no dummy target or correspondence row. These mask and
empty-image behaviors extend the box paper.

DN uses noisy labels/boxes and direct reconstruction with known target
identities, following [DN-DETR](https://arxiv.org/html/2203.01305v3) and its
[official DN components](https://github.com/IDEA-Research/DN-DETR/blob/main/models/DN_DAB_DETR/dn_components.py).
Native Group-DETR groups and DN groups retain isolation; ordinary queries
cannot see DN queries, and DN groups do not see each other or ordinary queries.
The additive task embedding and noise draw over alternative labels are explicit
adaptations. Noise and mask sampling use owned seeded draws without consuming
the ordinary stock sampling stream.

For segmentation, ordinary and DN queries share the existing mask head.
DN reconstructs the directly corresponding target masks using RF-DETR's BCE,
Dice, sampling and normalizer, including selected auxiliary decoder layers;
there is no DN encoder objective. The denominator includes DN group count.
Invalid padding contributes nothing, while empty enabled mask reconstruction
retains connected sparse zeros. Mask reconstruction from noisy box/label
queries is supported by [Mask DINO appendix B.2](https://arxiv.org/html/2206.02777v2)
and its [official criterion](https://github.com/IDEA-Research/MaskDINO/blob/main/maskdino/modeling/criterion.py).
This reuses RF-DETR architecture and mathematics rather than importing Mask
DINO or contrastive DINO. Tests exercise real mask/probe gradients and optimizer
updates; no measured accuracy or convergence benefit is asserted.

## Native selective compilation

Native **selective** mode uses LibTorch/TorchScript tracing; pinned upstream uses
`torch.compile(model, dynamic=True)` with Dynamo/Inductor, graph breaks, and
suppressed failures. Their capture/optimization coverage differs: upstream
partial compilation does not establish a single full graph; native tracing
implies neither equivalent fusion nor speed.

The current bounded tensor regions are:

| Native region | Captured work | Ordinary boundary |
| --- | --- | --- |
| Backbone/projector | Backbone tensor operations and feature projection | Native model orchestration and spatial layout |
| Decoder tail, per layer | Cross-attention residual normalization, two linear layers/ReLU, final residual normalization | Grouped self-attention, DN isolation and custom deformable attention |
| Segmentation spatial tail, per block | Pointwise/residual tail and learned spatial projection | Resize and the custom depthwise derivative |
| Segmentation query transform | Shared query MLP and projection for ordinary, encoder and DN queries | Sparse output structure and supervision policy |
| Lane-owned loss operators | Selected classification, BCE/Dice and pairwise contractions | Target preparation, sampling policy, matching, normalization and optimizer orchestration |

[SelectiveTensorRegion](../src/backend/models/rfdetr/core/detail/selective_compilation.h)
owns one slot per training/evaluation mode. Preparation arms the requested
batch; the first compatible **real forward** records and returns that execution's
outputs. It does not run a dummy training pass or replay the first result.
Identical preparation reuses state; explicit `none` disables selective execution.
At most two input/output tensors use fixed-capacity handoff storage.

Guards cover device, dtype, rank, admitted extents, gradient mode,
requires-grad and effective autocast policy. Backbone/spatial slots require
the prepared batch and fixed dimensions. Decoder-tail and query-transform
slots permit varying batch/query extents while retaining rank and hidden width.
Incompatible extents/signatures run ordinary tensor code without overwriting
the usable slot; an initial short batch leaves the full-batch slot armed.
Changed preparation can replace a slot, but only a fully constructed graph
replaces its predecessor. A recording failure propagates without replaying
failed GPU work and preserves the previous usable graph.

Registered parameters and buffers remain live aliases, so in-place optimizer,
Resume and EMA weight copies are observed. Device changes invalidate the
registered owner. Training/evaluation modes and their autograd graphs retain
separate custody, including outstanding backwards. Permanently zero-probability
dropout is omitted from the backbone and decoder because TorchScript's
optimized CUDA backward otherwise consumed RNG even at probability zero.
Recording, initial reuse and optimized reuse must all preserve the required RNG
stream.

[Loss traces](../src/backend/models/rfdetr/core/detail/traced_loss_cache.h)
have dynamic tensor extents and guard coefficients plus effective CUDA
autocast state; point-count normalization stays outside the trace.
Candidate construction is transactional. Ordinary
[decoder attention](../src/backend/models/rfdetr/core/decoder_attention.cpp)
uses explicit Q/K/V projections and scaled-dot-product attention, preserving
distinct parameter/position gradients even for coincident tensor values.
Grouping, padding and leakage protection remain ordinary native policy.
This does not promise a particular SDPA kernel backend.

Training admits complete effective batches. Training-owned evaluation pads
model input to its configured batch, zero-fills inactive images and trims
predictions before metrics/losses. Native weights prediction and standalone
evaluation carry their requested compilation mode to model construction;
short direct inference batches use the guarded fallback. GUI prediction is
batch one. ONNX and TensorRT retain their own execution engines. CLI
`--compile-mode none|selective` controls native selection; the existing `full`
trace route is separate from this selective contract and its evidence.

[Diagnostics](logging.md#native-selective-compilation) distinguishes recorded
versus executed optimized graphs, fusion node forms, autocast, and TF32.
[Validation](validation.md#training-mathematics-and-compilation-evidence) records
the observed fusion boundary and evidence limits.
