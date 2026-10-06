# RF-DETR training objective, gradient, and optimizer review

Historical baseline review; line anchors and findings refer to the revisions
below. See the [review index](README.md) and [current closure/evidence](current-results.md).

Review date: 2026-10-05. Native baseline: `52586e98f535205cc242dfe74ef5144eef0373bb`. Upstream baseline: `e9a138f70cc14e3fddca029a0683055e34ab8f35`; the upstream checkout was clean. This review changed only this report. No build, test, training run, profiler, or diagnostic was executed by this reviewer. Findings below are source analysis and analytical examples, supplemented by the explicitly attributed mechanism diagnostic run by the main agent. Production-path validation remains main-agent work through `./mmltk`.

The authorities read were `AGENTS.md`, `CONTRACT.md`, `docs/README.md`, `docs/rfdetr-training.md`, and the relevant validation and logging documentation. In local source anchors below, `N/` means `src/backend/models/rfdetr/`, and `U/` means `../rf-upstream/src/rfdetr/`. Line numbers refer to the reviewed revisions.

`docker/nvidia-payload.json:2–16` pins the NGC 26.08 payload, CUDA 13.4.1, NVCC 13.4.59, and Torch `2.14.0a0+4fdf77b940`. Public PyTorch source at that exact commit prefix was available and was inspected for autocast, AMP scale updates, C++ optimizers, and distributed reduction. Current online 2.14 documentation supplies guidance, not evidence that the pinned development payload equals every released 2.14 implementation.

## Assessment and current-run scope

The review found two concrete numerical defects worth correcting: stock IA-BCE unnecessarily rounds its matched negative weight through the positive weight's lower dtype, and FP16 scale growth can overflow permanently to infinity. Neither is evidence explaining the current MatchFree run's AP. The reviewed MatchFree equations and their live gradient paths are internally consistent with the stated native adaptation; no source-proven defect in that objective was found.

The main agent supplied `run-0025` metadata obtained through `./mmltk --logs`: nano, resolution 384, batch/validation batch 18, device 1, pretrained nano weights, AMP, Selective compilation, frozen encoder, MatchFree, DN disabled, fused AdamW, accumulation 1, clipping 0.1, no EMA, one lane/GPU. The last supplied validation was epoch 13, AP 0.088479. This metadata is not a controlled experiment. In particular, stock Hungarian/IA-BCE, DN, Muon, distributed normalization, accumulation greater than one, and EMA findings must not be presented as observed causes of that result. AMP precision must be established from the admitted precision record before attributing an FP16-only issue.

| Priority | Finding | Proven scope | Recommended disposition |
| --- | --- | --- | --- |
| P2, confirmed | IA-BCE shares a lower-precision cast between differently typed destinations | Stock IA-BCE under CUDA mixed precision | Cast detached quality independently for each destination; preserve accurate FP32 negative arithmetic |
| P2, confirmed | FP16 scale growth lacks the pinned Torch finite-candidate guard | Enabled scaler at the upper FP32 range | Retain the previous finite scale when growth would overflow; preserve tracker/backoff cadence |
| Policy boundary | Global target clamp differs from upstream DDP at very small global target counts | Multiple ranks and `G*N < W`, including empty-target batches | Preserve native global-batch semantics; qualify the upstream-equivalence claim and cover the boundary |
| Policy exception | Singular DN embedding names pass Muon eligibility | Muon together with DN | Decide and document the intended routing before changing an established checkpoint group policy |
| Existing model finding | Disabled auxiliary output still reaches stock criterion | `aux_loss=false` | Apply the model review's output gate; weighted loss has no extra auxiliary term for fixed samples |

The CUDA review owns custom operation math and matcher finite-precision behavior; the compilation review owns FullTrace capture. This report does not duplicate those findings or transfer FullTrace conclusions to the current Selective configuration.

## Confirmed corrections

### T1 — IA-BCE rounds the negative target through the positive dtype

Native `N/core/detection_ops.cpp:560–573` and upstream `U/models/criterion.py:578–599` construct the same detached positive quality in real arithmetic:

`t = stop_gradient(max(p_positive^alpha * IoU^(1-alpha), 0.01))`.

For matched entries, upstream writes `t.to(pos_weights.dtype)` and, independently, `1 - t.to(neg_weights.dtype)`. Native first casts `t` to the positive dtype and then subtracts in that dtype before converting to the negative dtype. These differ whenever the destinations differ in precision.

That condition is reachable in the real training scope. `pos_weights=zeros_like(logits)` remains BF16/FP16 when logits are low precision. `neg_weights=pow(sigmoid(logits),2)` is FP32 under CUDA autocast. Quality's `pow` operations are also FP32. The pinned ATen autocast table assigns `pow` to FP32 (`aten/src/ATen/autocast_mode.h:791–808`); the upstream training step passes model outputs directly to the criterion inside mixed precision, without a blanket FP32 output cast (`U/training/module_model.py:598–606`). [Pinned autocast implementation](https://github.com/pytorch/pytorch/blob/4fdf77b940/aten/src/ATen/autocast_mode.h#L791), [current AMP operation policy](https://docs.pytorch.org/docs/2.14/amp.html#cuda-ops-that-can-autocast-to-float32), [Lightning 2.6 mixed-precision context](https://github.com/Lightning-AI/pytorch-lightning/blob/2.6.0/src/lightning/pytorch/plugins/precision/amp.py#L101).

The loss is `sum(neg*z - log_sigmoid(z)*(pos+neg))/D`, so the rounded negative coefficient changes the objective and potentially its parameter gradients. This is a small precision loss, not a wholesale classification-formula error. Its effect on AP is unmeasured. The current MatchFree path constructs its own FP32 focal pair costs and does not call this IA-BCE branch.

The cohesive correction is local: keep `pos_weights`' independent assignment cast, and compute `1 - t.to(neg_weights.dtype())` for the negative assignment. Do not force the full loss or matcher to lower precision to mimic rounding.

Minimal expression pair for a pinned-LibTorch diagnostic, inside a CUDA BF16 autocast scope:

```cpp
const auto fp32 = torch::TensorOptions().device(torch::kCUDA).dtype(torch::kFloat32);
const auto z = torch::zeros({1}, fp32.dtype(torch::kBFloat16));
const auto pos = torch::zeros_like(z);
const auto neg = torch::pow(z.sigmoid(), 2.0);
const auto t = torch::tensor({0.501F}, fp32); // Detached matched quality.
const auto positive_cast = t.to(pos.scalar_type());
const auto native_negative =
    (torch::ones_like(positive_cast) - positive_cast).to(neg.scalar_type());
const auto upstream_negative = 1.0 - t.to(neg.scalar_type());
```

Expected dtypes: `z`, `pos`, and `positive_cast` BF16; `neg`, `t`, and both final negative tensors FP32. Expected negative values: native `0.5`, independent-destination result approximately `0.499000013`. FP16 yields native `0.4990234375` against the same FP32 reference. This quality is attainable, rather than an invalid IA-BCE input: with `p=0.5`, `alpha=0.25`, choose valid overlapping boxes with IoU `0.5*(0.501/0.5)^(4/3)`.

The main agent demonstrated the dtype/value mechanism with `t=0.3333F` on the pinned payload: positive dtype BF16, negative dtype Float, native negative update approximately `0.664062`, independent-destination update `0.6667`. This was a literal-expression diagnostic, not the full criterion. Next exercise the real criterion with one matched target, a stable assignment, low-precision logits, and nontrivial IoU; compare loss and VJP against the upstream expression with independent casts. Include BF16, FP16, and an FP32 control, and eager/Selective loss replay where that operation is admitted. Use actual `pow`, not a test-only `square()` substitution that can change autocast dispatch. Existing `N/core/tests/upstream_training_parity.test.cpp:279–383` supplies an independent stock formula in FP32 but does not exercise this mixed-destination condition.

### T2 — FP16 scale growth can become permanently infinite

`N/training/training_ops.cpp:245–252` multiplies the float scale by its growth factor unconditionally at the growth boundary. The pinned Torch CUDA implementation computes a candidate and stores it only if finite (`aten/src/ATen/native/cuda/AmpKernels.cu:166–190`). [Pinned AMP scale-update kernel](https://github.com/pytorch/pytorch/blob/4fdf77b940/aten/src/ATen/native/cuda/AmpKernels.cu#L166).

An admitted finite scale `2^127`, growth factor `2`, and a successful growth boundary produce infinity natively. Subsequent overflow backoff leaves `infinity * 0.5` infinite. Consequently scaled gradients can repeatedly overflow and updates remain skipped; inverse scale becomes zero at `training_ops.cpp:273`. Current checkpoint admission rejects nonfinite scale state (`N/training/training_continuation.cpp:13–15`), so this can also make continuation publication fail. This is a boundary defect, not proof it has occurred in a real run. The scaler is enabled only for admitted FP16 precision (`N/training/training_model.cpp:51`), not merely whenever AMP is enabled.

Mirror the pinned finite-candidate rule and reset the growth tracker normally whether or not growth was accepted. Preserve ordinary growth, overflow backoff, skip-update semantics, and the saved finite scale. A focused standard test can construct `GradScaler(true, ldexp(1.0F,127), 2.0F, 0.5F, 1)`: `update(false)` should keep the finite initial scale and reset its tracker; `update(true)` should halve it. Also cover a normal growth and disabled-scaler control. Existing overflow coverage at `N/training/tests/stock_training_parity.test.cpp:240–275` uses a small scale and tests recoverable gradient overflow, not overflow of the scale itself.

The main agent's diagnostic independently evaluated native float growth and the installed `at::_amp_update_scale_` at `0.75*FLT_MAX`: native multiplication by two became infinity; ATen retained approximately `2.55212e38` and reset the growth tracker to zero. The wrapper command was `./mmltk --diagnose-gpu-program --torch build/validation/rfdetr-math/precision-boundaries.cpp`, exit 0, with `precision_boundaries_observed=1`. Its source and transcript are `build/validation/rfdetr-math/precision-boundaries.cpp` and `.log`; retained diagnostic artifacts are `build/diagnostics/gpu-program.M5vsLC`, runtime digest `sha256:1fd71a53a17ac5d55ac16bb91c897d278938f92b5905b3f345b72caad1e8fa42`. An initial probe-construction dtype failure was corrected and retained separately. This evidence establishes the two expression mechanisms, not execution of the full native criterion or optimizer. The reviewer did not execute the diagnostic.

## Stock assignment and loss equivalence

At the equation level, the reviewed ordinary losses agree with upstream, subject to T1, the distributed clamp boundary below, the CUDA review's finite-precision observations, and matching the same sample coordinates and outputs.

| Area | Native / upstream anchors | Result |
| --- | --- | --- |
| Hungarian cost | `N/core/detection_ops.cpp:263–358`; `U/models/matcher.py:183–206,618–620,727,812–843` | Focal positive-minus-negative class cost, weighted L1, negative GIoU, optional pair mask costs, and independent per-group rectangular assignment implement the stock objective. Matching is outside differentiation. |
| Focal, varifocal, position loss | `N/core/detection_ops.cpp:118–151,574–589`; `U/models/criterion.py:254–323,620–720` | Correct branch precedence and target construction. Focal's mean over queries is compensated by query count, leaving the intended sum over batch/query/classes per target normalizer. Position quality is normalized by the per-image maximum; varifocal is a separate branch. |
| Box terms | `N/core/detection_ops.cpp:539–549,617–628`; `U/models/criterion.py:753–788` | Matched L1 sum and `1-GIoU` sum use the common divisor; IoU targets for class quality are detached. |
| Group, auxiliary, encoder terms | `N/core/detection_ops.cpp:702–768`; `N/core/detection_types.cpp:10–27`; `U/models/criterion.py:1021–1154`; `U/models/lwdetr.py:857–876` | Each supervised layer is matched independently. Weight dictionary determines inclusion, and group averaging is in the divisor, not another hidden division. Encoder supervision uses its own predictions. |
| Sampled mask terms | `N/core/training_mask_loss.cpp:119–224`; `N/core/detection_ops.cpp:630–660`; `U/models/criterion.py:326–405,790–900` | Per-mask mean point BCE plus smoothed Dice, then matched-mask sum divided by target count. Uncertainty sampling oversamples by 3 and keeps 75% uncertain points; target nearest and prediction bilinear sampling remain distinct. Empty selections keep zero gradient edges. |

For Dice, the inspected formula is `1-(2*sum(sigmoid(z)*y)+1)/(sum(sigmoid(z))+sum(y)+1)`. BCE uses a mean over sampled points before the target-count reduction. Pairwise matching uses its corresponding point-normalized costs, not the direct matched-mask reduction. Sparse selected masks reconstruct selected query/spatial products with bias and retain gradients into all three operands (`training_mask_loss.cpp:20–29`, `detection_ops.cpp:636–655`). Custom packed sampling and pair-cost kernels remain the CUDA review's authority.

The actual upstream AMP boundary matters for matcher comparison. `U/training/module_model.py:598–606` does not recast model outputs; Lightning's mixed-precision `forward_context` enables autocast. `_focal_classification_cost` therefore receives the produced dtype and computes sigmoid/log-sigmoid before the later final FP32 cost conversion. The mask matcher imports and calls **eager** `batch_sigmoid_ce_loss` and `batch_dice_loss` (`U/models/matcher.py:36,812–815`). The scripted aliases in `U/util/box_ops.py:210,234` do not establish a scripted matcher call. The criterion separately calls its scripted matched-mask CE/Dice at `criterion.py:895–896`. Scripting must not be mistaken for an unconditional wrapper-wide FP32 promotion.

The CUDA report's fused matcher uses an accurate FP32 calculation from input logits, while native dense fallback preserves more upstream intermediate rounding (`N/core/detection_ops.cpp:273–292`). Retain FP32 accuracy; make the fallback and controlled mathematical oracle agree on the chosen precision contract, then measure upstream AMP assignment differences separately. An arbitrarily small cost perturbation can flip an assignment near a tie, so tensor tolerances alone do not establish assignment parity. Neither upstream bit patterns nor a native-vs-native match prove the mathematical oracle correct. PyTorch and CUDA explicitly distinguish mathematical equivalence from identical floating-point execution. [PyTorch numerical accuracy](https://docs.pytorch.org/docs/2.14/notes/numerical_accuracy.html), [CUDA 13.4 floating-point guidance](https://docs.nvidia.com/cuda/floating-point/index.html).

### Disabled auxiliary loss

The model review found native auxiliary outputs still emitted at `N/core/model.cpp:1223` with `aux_loss=false`, unlike `U/models/lwdetr.py:571`. The stock criterion consumes all supplied layers, but `N/core/detection_types.cpp:20` excludes their weights. Thus there is extra matching/criterion work without an extra term in the weighted objective for fixed outputs and sample coordinates. MatchFree and DN already gate their supervised auxiliary layer lists.

There is a conditional RNG effect for segmentation: without semantic sampling keys, extra auxiliary matcher draws precede main-loss uncertainty sampling and can change the sampled main loss. Production logical draw identities ordinarily provide independent semantic sampling keys; native matcher and direct loss keep main layer identity zero (`detection_ops.cpp:416–429,652–655`). Therefore a main-loss RNG change is not established for the production keyed path or current run. Gate output construction in the owning model while preserving internal decoder stages needed for refinement; test the objective with explicit samples and separately the unkeyed RNG behavior.

## MatchFree and denoising are intentional extensions

### MatchFree

Upstream stock RF-DETR has no corresponding MatchFree training route. The primary comparison is the dense correspondence and dual objective in *Beyond Hungarian*, sections 3.2–3.4. Its core is row-softmax query correspondence, sparsification based on row winners and column thresholds, and separate dense/sparse weighted pair objectives. The reviewed source retains differentiable correspondence values and pair costs rather than silently detaching an entire objective. This is consistent with the displayed equations; the paper does not provide a uniquely executable specification for every RF-DETR adaptation. I did not locate author reference code in the paper/source search. [Primary MatchFree paper](https://arxiv.org/html/2603.08514v1).

Native source behavior is explicit:

- `N/core/training_supervision.cpp:295–320,554–577`: learned class/box GT probes and projected decoder query features produce `A=softmax(Q*K^T/sqrt(d))` in FP32. Segmentation additionally conditions probes on target-mask-pooled spatial features.
- `training_supervision.cpp:382–392`: detached values choose discrete row winners and positive column maxima; surviving **live** dense values are normalized by retained row sum plus epsilon. The `column_max>0` guard deliberately excludes columns that were never selected; this is a native resolution of an otherwise ambiguous zero-threshold case.
- `training_supervision.cpp:579–606`: each valid target/query pair uses all-class focal BCE, L1, and `1-GIoU`, with matching-cost weights. The all-class negative term is retained, and the target class replaces its negative contribution with the positive focal term. This is different from the stock IA-BCE objective by design.
- `training_supervision.cpp:776–899`: `alpha*sum(A*C) + beta*sum(S*C)` is divided by `G*max(global_N,1)` when group averaging is selected. Main, selected decoder auxiliary, and selected encoder terms are added once. The reported correspondence scalar is a subset of the total, not a second addition. Sparse topology is nondifferentiable, but both selected weight values and costs receive gradients.
- `training_supervision.cpp:830–839,844–886` and `N/core/training_mask_loss.cpp:226–246`: empty images get one background focal term per layer; shared uniform mask points supply pairwise BCE/Dice. These segmentation, empty-image, group, and auxiliary choices are native capabilities, not claims of stock or paper bitwise parity.

No correction to these equations is justified by this source audit. A controlled independent objective test should use a small correspondence matrix with strict margins from argmax/threshold boundaries, explicit targets and mask samples, and independently expanded focal/L1/GIoU costs. Verify VJPs into logits, boxes, query features, GT/projection parameters, and mask operands, including the direct `C*dA` term. Use finite differences only within a fixed sparse topology; crossing a threshold is a different mathematical case. Existing supervision tests already cover substantial portions of these identities; extend only demonstrated coverage gaps rather than replacing them with a duplicate implementation.

### Denoising

`N/core/training_supervision.cpp:394–416,444–522,634–774` creates owned noisy queries, remembers exact target identities, and applies direct reconstruction loss without Hungarian reassignment. Positive targets and each DN repetition contribute the known focal/L1/GIoU objective divided by `max(D*global_N,1)`; native matched-mask terms use those same known identities. Ordinary and DN losses are combined once, and no DN encoder loss is manufactured.

The primary DN reference perturbs labels/boxes, separates ordinary queries from DN information, and supervises known queries with focal/L1/GIoU (`dn_components.py:63–104,121–132,183–220`). Native differences are deliberate: additive task embedding instead of an appended indicator coordinate; replacement labels exclude the original; DN groups cannot attend ordinary queries, whereas the original reference permits that direction. Native segmentation and Group-DETR isolation extend the reference. Preserve these capabilities and test their stated semantics rather than relabeling them as stock RF-DETR equivalence. [Official DN-DETR implementation](https://github.com/IDEA-Research/DN-DETR/blob/main/models/DN_DAB_DETR/dn_components.py), [DN-DETR paper](https://arxiv.org/html/2203.01305v3).

## Gradients, reduction, and update semantics

### Accumulation, autocast, and clipping

`N/training/detail/training_step.h:15–33` applies `scale/K^2` to each logical microbatch. The apparently unusual second division matches this upstream automatic-optimization path: `U/training/module_model.py:611` divides the returned loss by `K`, and Lightning's closure divides again by `accumulate_grad_batches`. Changing native to the usual `1/K` would change the admitted reference objective. Each microbatch uses its own global target divisor; this is not equivalent to pooling all microbatches' targets before normalization. [Lightning 2.6 closure normalization](https://github.com/Lightning-AI/pytorch-lightning/blob/2.6.0/src/lightning/pytorch/loops/optimization/automatic.py#L55).

The model and criterion run in the selected autocast scope; `autograd::grad(..., allow_unused=true)` runs after it exits (`training_step.h:20–27`, `N/training/training_lanes.cpp:423–455`). MatchFree probes explicitly use FP32. Leaving autocast before backward follows the documented execution model. The reducer uses Tensor gradient hooks (`N/training/training_gradient_reducer.cpp:122–131`), which do run for tensors passed to functional `autograd::grad`; warnings about ordinary DDP's accumulator-node hooks should not be applied to this custom Tensor-hook reducer. [AMP guidance](https://docs.pytorch.org/docs/2.14/amp.html), [autograd Tensor-hook semantics](https://docs.pytorch.org/docs/2.14/notes/autograd.html#backward-hooks-execution).

Target counts are summed globally per microbatch before criterion normalization (`N/training/training_ops.cpp:116–145`). The reducer adds contributions, performs SUM collectives, and reduces a usage inventory so parameters unused everywhere keep undefined `.grad` (`training_gradient_reducer.cpp:147–169,347–385`; `N/training/training_distributed.cpp:173`). That distinction preserves optimizer missing-gradient policy rather than turning every unused parameter into a decay update.

The final sequence is reduction, nonfinite check/unscale, agreed failure/overflow decision, clipping, optimizer step or skip, scaler update, zero-grad, schedule accounting, EMA (`N/training/training_model.cpp:332–363`). Clipping is one global norm over active gradients and multiplies by `min(max_norm/(norm+1e-6),1)` (`N/training/native_optimizer.cpp:897–919`); it is not per parameter or before unscale. No extra world-size or lane division was found.

### Distributed clamp: a deliberate native objective differs at the boundary

Let `N` be global target count, `G` the group divisor, `W` world size, and `S_r` a rank's stock numerator. Upstream `U/models/criterion.py:536–543` returns `max(G*N/W,1)` and ordinary DDP averages gradients. Thus its effective global gradient is

`sum_r grad(S_r) / max(G*N,W)`.

Native uses SUM gradients and `max(G*N,1)` (`training_ops.cpp:315–320`), yielding

`sum_r grad(S_r) / max(G*N,1)`.

They agree when `G*N>=W` or `W=1`. They need not agree for sparse/empty global target batches. For `W=2,G=1,N=1`, upstream produces half the native gradient; with `N=0`, background classification still has a numerator, so the distinction persists. The DDP averaging premise is documented by PyTorch. [DDP documentation](https://docs.pytorch.org/docs/stable/generated/torch.nn.parallel.DistributedDataParallel.html).

Native documentation explicitly requires a world-size-independent global batch objective and supports ranks with no local images. Preserve that architecture. Qualify upstream equivalence for the clamp boundary and add a small exact mathematical case to existing distributed coverage. Do not introduce a hidden `W` divisor to imitate upstream's sparse-batch exception. Apply the same care when reasoning about `sum_group_losses`, where the effective group divisor is one.

### Optimizers and parameter state

AdamW's native eager, foreach, and fused routes implement the intended decoupled decay and per-parameter correction:

`m <- beta1*m+(1-beta1)*g`, `v <- beta2*v+(1-beta2)*g^2`,
`p <- (1-lr*wd)*p - lr*(m/(1-beta1^age))/(sqrt(v/(1-beta2^age))+eps)`.

AMSGrad takes the historical maximum of the uncorrected second moment before correction. Undefined gradients skip moments, decay, and age. `N/training/native_optimizer.cpp:397–469` agrees with the pinned C++ AdamW algorithm; backend grouping retains individual ages rather than substituting one group counter. The fused route calls ATen's fused AdamW after native unscale/overflow admission, avoiding a second unscale or hidden skip. Backend changes can change rounding and cannot promise bitwise trajectories. [Pinned LibTorch AdamW](https://github.com/pytorch/pytorch/blob/4fdf77b940/torch/csrc/api/src/optim/adamw.cpp#L60).

SGD (`native_optimizer.cpp:819–837`) uses coupled decay, first momentum buffer equal to the first decayed gradient, subsequent `beta*buffer+g`, and the configured Nesterov update. Undefined gradients skip the update. This matches zero-dampening LibTorch SGD, including its first-step convention. [Pinned LibTorch SGD](https://github.com/pytorch/pytorch/blob/4fdf77b940/torch/csrc/api/src/optim/sgd.cpp#L52).

Muon (`native_optimizer.cpp:228–241,272–287,671–695`) matches the inspected author's update: momentum interpolation, Nesterov interpolation, 4D flattening, BF16 Newton–Schulz with five steps and coefficients `(3.4445,-4.775,2.0315)`, norm epsilon `1e-7`, and post-transform aspect scaling. Its auxiliary Adam uses `(0.9,0.95)` and `1e-10`; both branches replace undefined gradients by zeros and still update state/decay. This differs intentionally from AdamW/SGD missing-gradient behavior. [Author Muon equations and implementation](https://github.com/KellerJordan/Muon/blob/master/muon.py#L3).

One routing exception needs a policy decision, not an automatic formula fix. `muon_parameter_eligible` (`native_optimizer.cpp:16–23`) excludes plural `embeddings` and specific detector names. Native DN modules use singular `denoising_label_embedding` and `denoising_task_embedding` (`N/core/training_supervision.cpp:317–318`); both `.weight` tensors therefore enter Muon through `native_optimizer.cpp:1063`. The author's guidance routes input embeddings to auxiliary Adam. Conversely `self_attn.in_proj_weight` fails the native `.weight` name test despite being a matrix. These are proven routing facts; neither proves divergence of the selected native recipe. Establish the intended named parameter inventory and document exceptions. If routing changes, update optimizer group/Resume admission coherently; silently loading a prior Muon tensor's state as Adam state is not exact Resume. This combination is absent from the current run.

Frozen encoder parameters retain named eligibility but stay outside the active optimizer/reducer views; lazy activation preserves prior state and ages (`N/training/training_model.cpp:64,131–138`; `native_optimizer.cpp:1058–1073`; `N/training/detail/training_epoch_policy.h`). The constant encoder layer count used for LR decay corresponds to the admitted native architecture catalog; upstream supports other model families, so comparing an unsupported upstream variant alone is not evidence of a native LR bug.

### EMA and exact Resume

Native EMA first copies current parameters, then applies `d=decay*(1-exp(-(completed+1)/tau))` before advancing its count (`N/training/model_ema.cpp:14–27`). The production upstream comparison is `U/training/callbacks/ema.py:102,318–336`, which uses `AveragedModel`, not the older standalone `training/model_ema.py`. Native's first-copy behavior and completed-attempt cadence, including recoverable FP16 overflow, are consistent with the selected automatic-optimization path. The shadow inventory includes eligible frozen parameters so later activation retains the complete model.

Current continuation persists the recipe and supervision configuration, per-parameter optimizer state, scaler, EMA state/count, scheduler clocks and absolute LRs, epoch-policy latches, logical draw plan/cursor/donors, and partial epoch metrics (`N/training/detail/training_continuation.h:25–36`; `training_continuation.cpp:69–97,137–170`; `training_session_checkpoint.cpp:36–56,126–151`). Resume rejects incompatible recipe/data/accumulation/supervision changes. These are stronger native continuation semantics than merely importing upstream weights. Logical identities seed augmentation, DN, and mask sampling; permanent zero-dropout paths explicitly avoid spurious RNG consumption (`N/core/model.cpp:644–645`).

No missing continuation field or parameter-age reset was proved in this audit. That is not a runtime proof of exact continuation. Preserve the existing split-versus-continuous tests and compare named parameter tensors, optimizer state/ages, scaler, EMA, scheduler clocks, and next logical sample identities at a checkpoint boundary, including a pending unfreeze boundary and an AMP-skipped attempt. Changing a physical optimizer backend is mathematical continuation with potentially different rounding, not a guarantee of identical bits.

## Evidence quality and focused plan work

Existing tests have useful independent components:

- `N/core/tests/upstream_training_parity.test.cpp:279–383` independently expands stock IA/focal, L1, and GIoU with group counts; the mask fixture beginning at line 384 supplies explicit sampled coordinates. Its FP32 setup cannot detect T1, and its classification loop does not establish AMP coverage for every configured classification branch.
- `N/training/tests/stock_training_parity.test.cpp:110–181` checks a literal AdamW reference with undefined gaps, unequal ages, accumulation, clipping, AMSGrad, and resume. Lines 183–239 separately check EMA, schedules, and clipping; lines 240–275 check recoverable overflow. Earlier backend comparisons are useful implementation comparisons, not independent objective oracles.
- `N/core/tests/training_supervision.test.cpp` checks explicit correspondence/sparsification, expanded pair losses, live gradients, empty/padded targets, group divisors, FP32 probe behavior, DN identities/noise/isolation, and training-only removal. Integration fixtures comparing two invocations of the same native owner cannot by themselves establish paper equivalence or training quality.
- `N/training/tests/training_gradient_fixture.cpp:459` and its consumers validate lane/rank/capacity behavior across AdamW, Muon, and SGD using native optimizers on both sides. Preserve that systems coverage, while recognizing that it cannot independently validate an optimizer equation shared by both sides.

Recommended required work is narrow:

1. Use the main agent's retained T1 dtype/value evidence; correct the single destination-cast boundary and add focused mixed-precision criterion/VJP coverage to the owning existing suite.
2. Use the retained T2 finite scale growth evidence; adopt the pinned finite-candidate rule and cover subsequent backoff, tracker reset, and disabled behavior.
3. Integrate the other reviewers' confirmed model/CUDA/compilation corrections without deleting assignment, optimizer, AMP, DN, segmentation, or compilation capabilities. For disabled auxiliaries, preserve the fixed-sample weighted objective and explicit keyed sampling semantics.
4. State the distributed clamp exception in the training reference and cover `W=2,G=1,N=1` plus empty targets in the existing mathematical normalization tests. Keep native global-batch invariance.

The Muon/DN routing inventory and an independent small-matrix Muon update oracle are useful follow-up work if the plan changes that recipe; they do not block an AdamW-only correction. Broad cross-products of every optimizer, assignment, precision, lane count, and image size are not necessary to demonstrate the two confirmed defects.

For the current MatchFree concern, the most useful separate acceptance experiment is a fixed small real training fixture using the current recipe and imported weights, with explicit forward values, component losses, gradients before/after clipping, and the resulting AdamW update compared against an independently expanded FP32 MatchFree objective. This can establish the training step before a controlled Hungarian-versus-MatchFree quality experiment. AP attribution requires such controlled evidence; neither an equation audit nor two agreeing native implementations establishes the cause of poor validation quality.

## Directly useful open-source C++ references

These candidates were inspected as concrete implementation references, not suggested as wholesale replacements:

| Repository/source | Useful boundary | Fit for native semantics |
| --- | --- | --- |
| [PyTorch pinned C++ AdamW](https://github.com/pytorch/pytorch/blob/4fdf77b940/torch/csrc/api/src/optim/adamw.cpp) | Independent first/second moments, per-tensor age, epsilon placement, AMSGrad, undefined-gradient behavior | Suitable eager equation oracle. A replacement must retain native named state admission, physical backend choices, and lazy activation. |
| [PyTorch pinned C++ SGD](https://github.com/pytorch/pytorch/blob/4fdf77b940/torch/csrc/api/src/optim/sgd.cpp) | Coupled decay and first-step/Nesterov momentum | Suitable independent small-tensor oracle; does not own native recipe clocks, group roles, or exact session continuation. |
| [PyTorch pinned CUDA AMP kernels](https://github.com/pytorch/pytorch/blob/4fdf77b940/aten/src/ATen/native/cuda/AmpKernels.cu) | Nonfinite check/unscale and guarded scale growth | Native already uses the unscale primitive. The growth guard can be adopted without introducing another training owner. |
| [PyTorch pinned C++ distributed reducer](https://github.com/pytorch/pytorch/blob/4fdf77b940/torch/csrc/distributed/c10d/reducer.cpp) | Bucket gradients, unused-parameter accounting, transport completion | Useful source comparison for custody and usage handling. Ordinary DDP normalization/hook assumptions are not a drop-in match for native functional gradients, lane contributions, and global SUM semantics. |

The [Muon author's implementation](https://github.com/KellerJordan/Muon/blob/master/muon.py) and [official DN implementation](https://github.com/IDEA-Research/DN-DETR/blob/main/models/DN_DAB_DETR/dn_components.py) are primary equation references written in Python, not C++ execution alternatives. No new optimizer, reducer, or Python training dependency is needed for the focused corrections above.
