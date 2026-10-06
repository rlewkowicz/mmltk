# RF-DETR CUDA and primitive mathematics review

Reviewed native commit `52586e98f535205cc242dfe74ef5144eef0373bb` against RF-DETR
`e9a138f70cc14e3fddca029a0683055e34ab8f35`, on 2026-10-05. Scope: ML operations
and layers, RF-DETR matcher/mask CUDA, custom autograd, numerical build policy,
and the input/target sampling boundaries feeding these operations. Authorities
read: `AGENTS.md`, `CONTRACT.md`, `docs/rfdetr-training.md`, and the build,
validation, GPU-execution and wiki references. This is a source review; no
build, test, profiler, Docker invocation or executable was run. Only this report
was changed.

The main actionable result is that the fused CUDA matcher does not reproduce
the pinned upstream's intermediate precision. A finite BF16 example below
changes the predicted assignment. Deformable attention also differs at an exact
lower sampling boundary and suppresses some nonfinite results. These are
source-proven differences, with analytical counterexamples, **not independently
executed GPU reproductions**. None establishes the cause, frequency or size of
an observed training-accuracy failure.

## Findings

| ID | Severity | Source-proven behavior | Production applicability |
| --- | --- | --- | --- |
| CUDA-01 | P1 | Fused matcher changes low-precision intermediate rounding; a finite BF16 case reverses the Hungarian winner. | CUDA Hungarian matching accepts model-produced BF16/FP16 logits without a preceding FP32 conversion. Exact failing-run dtype remains unknown. |
| CUDA-02 | P2 | CUDA attention returns a zero coordinate derivative at the exact lower support boundary where upstream returns a nonzero derivative. | Default native CUDA attention; affects exact representable boundary locations, not every out-of-image sample. |
| CUDA-03 | P2 | An out-of-support sample bypasses multiplication by its attention weight, hiding NaN/Inf that upstream propagates. | Nonfinite attention or cotangents together with empty support; finite ordinary inputs are unaffected by this finding. |
| CUDA-04 | P2 | Default attention rejects batch sizes above 64 that are not multiples of 64, although the reference handles them. | A physical model batch of 65, 66, etc.; occurrence in the reported run is unestablished. |

P1 means a supported numerical route can change training decisions and needs
an explicit equivalence/precision policy. It does not mean the FP32 arithmetic
is intrinsically less accurate. P2 means a bounded correctness or
supported-domain discrepancy. The severity is not an estimate of contribution
to a particular accuracy incident.

### CUDA-01 — fused matching differs from upstream finite-precision assignment

**Owners and trigger.**
[detr_matcher_cuda.cu:54](../../../src/backend/models/rfdetr/core/detr_matcher_cuda.cu#L54)
converts each class logit to FP32 before sigmoid and log-sigmoid-equivalent
arithmetic. Lines 61–64 similarly perform box conversion, distance and GIoU in
FP32. The production CUDA route at
[detection_ops.cpp:321](../../../src/backend/models/rfdetr/core/detection_ops.cpp#L321)
only makes logits/boxes contiguous; it does not establish that they were already
FP32. CUDA selects this route at lines 432–445. Native classification heads emit
linear outputs directly at `core/model.cpp:1203,1248`; upstream does likewise at
`models/lwdetr.py:530,566`.

The actual upstream counterpart is
[matcher.py:183–207](https://github.com/roboflow/rf-detr/blob/e9a138f70cc14e3fddca029a0683055e34ab8f35/src/rfdetr/models/matcher.py#L183),
called on gathered original-dtype logits at lines 445–447. Its final cost is
converted to FP32 only at line 727, or line 843 on the full route. Native's own
dense fallback retains these tensor operations at
[detection_ops.cpp:272](../../../src/backend/models/rfdetr/core/detection_ops.cpp#L272).
The scalar equation agrees:

\[
 p=\sigma(z),\qquad
 c_{\mathrm{cls}}=\alpha(1-p)^2\operatorname{softplus}(-z)
 -(1-\alpha)p^2\operatorname{softplus}(z).
\]

The sequence of rounding operations does not. At pinned Torch `4fdf77b940`,
CUDA autocast lists `pow` as FP32, while `sigmoid`/`log_sigmoid` are not promoted
by that list. The latter's CUDA kernel uses an FP32 internal calculation but
returns the input scalar dtype. Upcasting the logit before both operations
removes those BF16/FP16 rounding points.
[Pinned autocast policy](https://github.com/pytorch/pytorch/blob/4fdf77b940/aten/src/ATen/autocast_mode.h#L791),
[pinned log-sigmoid kernel](https://github.com/pytorch/pytorch/blob/4fdf77b940/aten/src/ATen/native/cuda/ActivationLogSigmoidKernel.cu#L22).

**Concrete finite assignment counterexample, analytically predicted.** One
image, one target of class 0, two queries, `alpha=.25`, stock matching weights
`class=2, bbox=5, giou=2`; BF16 logits `[0,8]`; FP32 normalized center boxes:

| Item | `(cx,cy,w,h)` |
| --- | --- |
| Target and query 0 | `(.1,.1,.05,.05)` |
| Query 1 | `(.88375,.88375,.05,.05)` |

Every box has positive extent and stays inside the image. Query 1 has L1 cost
`1.5675`, hence weighted L1 cost approximately `7.8375`; its weighted negative
GIoU is approximately `1.985614`, while query 0's is `-2`. Upstream BF16 sigmoid
at 8 rounds to 1, `softplus(8)` rounds to 8, and `softplus(0)` rounds to
`.69140625`. Thus its unweighted classification costs are `-.08642578125`
and `-6`. The native FP32 costs are approximately `-.08664340` and
`-5.99622786`; the table applies the stock classification multiplier of 2.

| Cost | Query 0 | Query 1 | Winner |
| --- | ---: | ---: | --- |
| Upstream rounded intermediates + geometry | `-2.17285156` | `-2.1768857` | Query 1 |
| Native FP32 intermediates + geometry | `-2.17328680` | `-2.1693414` | Query 0 |

The table assumes CUDA autocast is active for the upstream cost operations.
The displayed native decimals are elementary real-valued calculations rounded
for presentation, not measured device results. Their approximately `1e-3`
winner margins are much larger than FP32 last-bit uncertainty here. Preserve
the actual enclosing autocast scope when materializing this oracle; low-dtype
intermediate rounding exists outside autocast too, but its subsequent tensor
rounding sequence differs from the one tabulated here.

**Forward/backward consequence.** Matching runs under `NoGradGuard`
(`detection_ops.cpp:360`), so the cost kernel itself has no backward. Its discrete
indices select which query receives each target; changed indices therefore
change subsequent classification, box and mask gradients and optimizer updates.
This matters to finite-precision training equivalence even though the native
individual costs can be more accurate approximations to real arithmetic.

**Contract interpretation.** `docs/rfdetr-training.md:7–24` targets the pinned
mathematics, distinguishes tensor/autocast/internal precision, and explicitly
does not claim bitwise equivalence to upstream's default kernels. It does not
specifically promise preservation of BF16 sigmoid/log-sigmoid rounding. Thus
the demonstrated difference is an equivalence and precision-policy gap, not
proof that FP32 promotion is a product bug or lowers AP. Reproducing reduced
precision merely to obtain bitwise agreement is not recommended.

**Related mask precision difference requiring a measured oracle.**
`detr_matcher_cuda.cu:78–101` accumulates BCE, intersections and denominators in
FP32 with one warp per pair. Upstream
[box_ops.py:194–234](https://github.com/roboflow/rf-detr/blob/e9a138f70cc14e3fddca029a0683055e34ab8f35/src/rfdetr/utilities/box_ops.py#L194)
uses BCE tensors and `einsum` contractions. Pinned CUDA autocast promotes BCE
but assigns `einsum` to the lower-precision list; the scripted helpers and
actual enclosing autocast mode must be captured in an execution oracle.
[Pinned contraction policy](https://github.com/pytorch/pytorch/blob/4fdf77b940/aten/src/ATen/autocast_mode.h#L757).
For an FP16-autocast contraction that retains that policy, `P=4096`, all logits
20 and all target samples 0 predicts a negative-BCE contraction of 81920, which
overflows its FP16 result before division by 4096; the fused kernel instead
returns approximately 20. This is a conditional diagnostic case, not an
executed assertion about the scripted upstream helper. The native exceptional
fallback at `detection_ops.cpp:454–494` runs only if the compact native costs
contain a nonfinite value; it cannot detect a value that overflowed only in the
upstream operation sequence. Sparse mask logits need not be low precision:
sampling and bias addition at lines 253–257 can produce FP32 logits even under
AMP, so inspect intermediates rather than infer dtype from the training mode.

**Minimal correction.** Keep compact cost storage, batched launches, the solver
and accurate FP32 accumulation. Establish one documented matcher precision
policy across the fused path, dense exceptional fallback and comparison oracle.
For an FP32 cost policy, upcast operands before the corresponding upstream
oracle operations and compare that controlled reference, while retaining a
separate differential against untouched upstream AMP to measure assignment and
update changes. Confirm any fallback changes preserve its full-domain sentinel
contract. This isolates implementation errors from an intentional precision
improvement without reducing accuracy to force agreement.

If strict equivalence to stock reduced-precision execution is explicitly
selected instead, a focused option is to calculate its rounded
sigmoid/log-sigmoid terms once per query/class, then pass them into compact
pairwise kernels. Do not expand a full cross-image matrix. Extend whichever
policy is selected consistently to mask contractions and low-dtype box
conversion after the effective scripted operation dtypes are established.

**Focused validation.** Compare individual class/L1/GIoU/mask terms, compact
costs and assignment indices with the pinned upstream on FP32, actual FP16
outputs and actual BF16 outputs; include the finite example above, saturated
logits, near ties, large sampled-mask contractions, empty images and exceptional
cost replacement. Follow at least one changed assignment through the loss VJP
and one optimizer update. Run both eager and retained compiled loss helpers.

### CUDA-02 — lower-boundary attention coordinate gradient is dropped

The production model invokes the CUDA custom autograd operation at
[model.cpp:562–582](../../../src/backend/models/rfdetr/core/model.cpp#L562).
Upstream actually invokes the PyTorch reference unconditionally at
[ms_deform_attn.py:249–252](https://github.com/roboflow/rf-detr/blob/e9a138f70cc14e3fddca029a0683055e34ab8f35/src/rfdetr/models/ops/modules/ms_deform_attn.py#L249),
despite an old reference-function docstring suggesting debugging-only use.
That reference uses bilinear, zero-padded, `align_corners=False` grid sampling
and then weighted summation at
[ms_deform_attn_func.py:45–75](https://github.com/roboflow/rf-detr/blob/e9a138f70cc14e3fddca029a0683055e34ab8f35/src/rfdetr/models/ops/functions/ms_deform_attn_func.py#L45).

For normalized location `(u,v)`, pixel coordinates are
`x=W*u-.5`, `y=H*v-.5`. The kernel's backward applies interpolation only when
`x>-1 && y>-1 && x<W && y<H` at
[ms_deform_im2col_cuda.cuh:284–288](../../../src/backend/ml/layers/detail/ms_deform_im2col_cuda.cuh#L284).
At `x=-1`, the forward value is zero but the selected right-sided bilinear
coordinate derivative need not be zero. Pinned ATen computes that derivative
from each in-bounds corner, even if its forward interpolation weight is zero.
[Pinned CUDA grid-sampler backward](https://github.com/pytorch/pytorch/blob/4fdf77b940/aten/src/ATen/native/cuda/GridSampler.cu#L284).

**Exact trigger and proof.** Set `B=Q=heads=C=levels=points=1`, spatial
`H=1,W=2`, value `[1,1]`, attention 1, location `u=-.25,v=.5`, and output
cotangent 1. All inputs are exactly representable. Both forwards return 0.
In the upstream selected bilinear branch, the right neighbor at `(0,0)` has
value 1, so `d(output)/dx=1`, and `dx/du=W=2`. Upstream therefore returns
`d(output)/du=2`; native skips the point and returns 0. The height counterpart
is symmetric. This is about equality to the lower support limit; sampling well
outside the footprint correctly has zero derivative. At the upper limit `x=W`,
the selected upstream branch has no in-bounds right neighbor and this particular
counterexample does not apply.

**Owners/correction.** `detail/ms_deform_im2col_cuda.cuh`, its CUDA launch owner
and `layers/tests/ms_deform_attn.test.cpp`. Admit the lower boundary for backward
and retain each corner's independent bounds check, matching ATen's chosen
piecewise derivative. Coordinate rounding and exceptional propagation must
remain explicit when adjusting the shared sampling helper. Do not replace the
optimized reduction or remove custom attention.

**Focused validation.** Assert all three VJPs independently at exact `-1`
pixel support, its adjacent representable coordinates, half-pixels and image
edges; cover both axes and `H/W=1,2` and larger extents. Compare to the pinned
ATen operation, not finite differences across a nondifferentiable knot. Include
FP32 and production mixed-dtype AMP operands, channels below and above each
reduction dispatch threshold, and several points contributing to one pixel.

### CUDA-03 — empty attention support hides nonfinite multiplication

**Trigger/proof.** Use a finite 1×1 value, location `(2,2)` and an attention
weight of NaN or infinity. The upstream sampler returns 0, then its ordinary
tensor multiplication computes `0*NaN` or `0*Inf`, yielding NaN. Native's forward
at
[ms_deform_im2col_cuda.cuh:393–397](../../../src/backend/ml/layers/detail/ms_deform_im2col_cuda.cuh#L393)
skips both sampling and multiplication and returns its initialized zero. With a
NaN output cotangent, upstream's attention-weight gradient is `0*NaN`, while
the native backward guard also leaves that gradient zero. The same structural
concern applies to a nonfinite valid corner with a zero interpolation weight
at a support boundary. These are exceptional-value semantics, not an assertion
that ordinary softmax produces infinite finite-input weights.

**Owners/correction.** Same attention kernel and autograd boundary as CUDA-02.
Preserve the upstream sampled-value multiplication and corresponding VJP
semantics when the footprint is empty; retain fast ordinary finite sampling.
Do not replace nonfinite operands by finite defaults. Correct propagation lets
the existing training finite/overflow policy observe the actual failed
computation.

**Focused validation.** Finite/NaN/±Inf weights crossed with inside, exact-edge
and empty-support locations; finite/NaN output cotangents; nonfinite pixels at
zero-weight valid corners; and multiple points where only one is invalid. Check
which outputs and gradients are finite as well as their values.

NaN *coordinates alone* are not asserted to be a mismatch. Current public
[PyTorch grid-sample documentation](https://docs.pytorch.org/docs/main/generated/torch.nn.functional.grid_sample.html)
describes a NaN interpretation, but the pinned CUDA implementation's
`safe_downgrade_to_int_range` maps nonfinite pixel coordinates to an out-of-range
sentinel before indexing. With zero padding that matters to the actual oracle.
[Pinned GridSampler.cuh:129–158](https://github.com/pytorch/pytorch/blob/4fdf77b940/aten/src/ATen/native/cuda/GridSampler.cuh#L129).
The existing test's asserted zero result for NaN coordinates is therefore not
proof of a discrepancy with this pinned CUDA backend.

### CUDA-04 — chunking exposes an unnecessary batch divisibility restriction

`im2col_step_` is fixed at 64 in `core/model.cpp:605`.
[ms_deform_attn_cuda_wrapper.cpp:71–79](../../../src/backend/ml/layers/ms_deform_attn_cuda_wrapper.cpp#L71)
uses `step=min(batch,64)` and requires exact divisibility. Thus otherwise-valid
batch 65 is rejected before computation. The reference loops over feature
levels and reshapes the batch; it imposes no such divisibility condition.
Native request declarations admit every positive batch size at
`contract/workflow_requests.h:71,93,161`.

**Owners/correction.** `layers/detail/ms_deform_attn_cuda_launch.h`,
`layers/ms_deform_attn_cuda_wrapper.cpp`, `layers/ms_deform_attn_cuda.cu`, and
layer tests. Keep bounded chunks but give the final chunk its actual batch
extent, using consistent forward/backward offsets and output coverage. Preserve
the existing integer-range validation for each chunk. Do not solve this by
removing native CUDA attention or silently changing the requested batch.

**Focused validation.** Forward and all VJPs for batches 63,64,65,127,128,129,
plus small custom steps and a nondefault CUDA stream. Check final-chunk writes
against the reference. This is a source-proven supported-shape limitation;
there is no evidence that the reported accuracy run used such a batch.

## Operation trace and mathematical assessment

### Attention layout, accumulation and autograd

Native values are contiguous `[B,S,heads,C]`, locations
`[B,Q,heads,L,P,2]`, weights `[B,Q,heads,L,P]`; output is `[B,Q,heads*C]`.
`ms_deform_attn_cuda_wrapper.cpp:35–102` checks rank, matching dimensions,
same CUDA device, positive dimensions, bounded integer products, at most 16
levels, and a CPU int64 canonical level layout whose total area equals `S`.
The public autograd boundary makes noncontiguous inputs contiguous and converts
FP16/BF16 operands to FP32; FP64 is explicitly rejected. It saves those FP32
operands, returns the value dtype, computes backward in FP32, then casts each
gradient to its corresponding original dtype at
[ms_deform_attn_autograd.cpp:9–56](../../../src/backend/ml/layers/ms_deform_attn_autograd.cpp#L9).

For finite ordinary points the reviewed kernel implements

\[
 O_{bqhc}=\sum_{l,p} A_{bqhlp}
 \sum_{i,j\in\{0,1\}}\beta_{ij}(x,y)V_{b,l,y_0+i,x_0+j,h,c},
\]

with zero for out-of-bounds corners. Its VJP scatters
`g*A*beta` to values, sums `g*sample` across channels for each weight, and sums
`g*A*d(sample)/dx * W` / `.../dy * H` for locations
(`ms_deform_im2col_cuda.cuh:43–162`). Those scale factors and the value stride
agree with upstream. CUDA-02/03 identify the exception to the piecewise and
nonfinite semantics.

Forward assigns every output once; backward starts with `zeros_like` gradients
(`ms_deform_attn_cuda_wrapper.cpp:118,135–144`). Value scatter uses atomics.
Location/weight reductions use serial/shared-tree reductions for small channel
counts, with an odd-tail reduction, and shared/multiple-block or global-atomic
paths for large counts (`ms_deform_im2col_cuda.cuh:203–359`, CUDA dispatch).
The shared reduction launch groups whole channel populations, so the inspected
barriers do not reveal a partial-block divergent synchronization defect.
Different atomic arrival/reduction orders can produce small nondeterministic
roundoff; upstream CUDA grid-sampler backward also scatters atomically. Its
documentation explicitly warns about backward nondeterminism.
[Current PyTorch grid-sample reference](https://docs.pytorch.org/docs/main/generated/torch.nn.functional.grid_sample.html).

Two numerical limits require evidence rather than a blanket fix:

* Native `u*W-.5` folds an upstream sequence `g=2*u-1`, then
  `((g+1)*W-1)/2`. Rounding can differ near interpolation knots. Quantify this
  with the pinned backend before changing contraction throughout a kernel.
* Under AMP, upstream grid sampling promotes mixed input/grid types; native
  always calculates in FP32 but returns the value dtype. The following
  `output_proj` also autocasts its input, so an API dtype difference alone does
  not prove an extra rounding difference in the complete model. Compare the
  projection's forward and backward as a composed boundary.

The raw CUDA launch is not an ATen dispatcher operation:
`ms_deform_attn_cuda_wrapper.cpp:118–124` records an allocation, launches through
a pointer, then views the result. The implications for whole-model tracing and
custom depthwise VJPs are owned by
[the compilation review](02-compilation-tracing.md); ordinary autograd support
does not establish graph-capture correctness.

### Box geometry and matcher storage

[box_geometry.cuh](../../../src/backend/ml/ops/box_geometry.cuh) and
[detection_geometry.cpp:14–89](../../../src/backend/models/rfdetr/core/detection_geometry.cpp#L14)
agree with the pinned upstream's nonnegative center-box extents and equations:

\[
 I=\prod_d\max(0,\min(b^+_d,c^+_d)-\max(b^-_d,c^-_d)),\quad
 U=A_b+A_c-I,\quad
 \mathrm{GIoU}=I/\max(U,10^{-7})-(E-U)/\max(E,10^{-7}).
\]

The tensor route separately constructs corner branches and promotes FP16/BF16
areas to FP32, preserving the intended branch gradients and denominator clamp.
The raw CUDA pair operation admits contiguous CUDA FP32/FP64 `[N,4]` inputs,
validates device/dtype and pair counts, and is selected only when neither box
requires gradients. Loss VJPs therefore use tensor operations, not the raw
forward-only kernel. The scalar min/max helpers explicitly propagate NaNs.
[Upstream box formulas](https://github.com/roboflow/rf-detr/blob/e9a138f70cc14e3fddca029a0683055e34ab8f35/src/rfdetr/utilities/box_ops.py#L27).

The compact matcher maps padded work to each image's actual `Q*T` costs using
int64 offsets at `detr_matcher_cuda.cu:25–41`; normal admitted metadata gives
one writer per cost. Mask matching assigns a whole warp to one pair; every lane
shares the pair-validity decision before the full-mask shuffle. No atomics or
cross-warp dependency are needed for those cost reductions. Empty images and
invalid metadata are handled by the owning preparation/admission route;
early-returning raw kernels do not initialize arbitrary invalid output slots.
This is not a substitute for retaining those upstream owner checks.

### Mask sampling and differentiable losses

The no-gradient matcher uses a custom FP32 border sampler only when its input
and coordinates are FP32 (`detection_ops.cpp:112–116`). Other dtypes use tensor
grid sampling. Its CUDA sampler explicitly rounds `2*u-1` separately, uses a
rounded FMA for align-corners-false pixel unnormalization, clamps to the border,
and uses round-to-nearest-even for categorical samples at
[training_mask_ops_cuda.cu:12–51](../../../src/backend/models/rfdetr/core/training_mask_ops_cuda.cu#L12).
That closely follows the pinned CUDA grid sampler; it is not an approximate
reciprocal/fast-transcendental optimization.

Packed categorical targets are int64 words with logical pixel bits; nearest
sampling extracts one bit, and transformed sampling applies inverse affine
coordinates, erasure and occluder membership (`training_mask_ops_cuda.cu:55–129`).
There is intentionally no target gradient. The differentiable prediction path
uses `grid_sample` at
[training_mask_loss.cpp:156](../../../src/backend/models/rfdetr/core/training_mask_loss.cpp#L156),
with uncertainty-point selection under no-grad and the final prediction sample
outside it at lines 168–224. Hence the matcher-only forward CUDA optimization
does not silently remove the prediction loss's spatial/query gradients.

For sampled logits `z`, truth `t` and `p=sigmoid(z)`, the direct loss route uses
per-instance mean BCE and smoothed Dice:

\[
 L_{\mathrm{BCE}}=N^{-1}\sum_n P^{-1}\sum_j
 \operatorname{BCEWithLogits}(z_{nj},t_{nj}),\qquad
 L_D=N^{-1}\sum_n\left(1-
 \frac{2\sum_j p_{nj}t_{nj}+1}{\sum_jp_{nj}+\sum_jt_{nj}+1}\right).
\]

Here `N` is the supplied mask normalizer, not necessarily the local mask count.
These are ATen autograd operations (`training_mask_loss.cpp:119–154`), so
`dBCE/dz=(p-t)/(N*P)` and the ordinary quotient/sigmoid Dice derivative are
retained. The fused matcher uses the same real-valued equations but the
different precision sequence in CUDA-01. Zero-point matcher samples preserve
the existing nonfinite/sentinel behavior; the direct-loss sampler's minimum
point policy is separate and should not be unified by accident.

The low-level mask wrapper is weaker than the attention wrapper: it does not
guard the tensor device or validate all same-device/positive-extent assumptions
(`training_mask_ops_cuda_wrapper.cpp:6–99`). It also passes one row stride for
both base and occluder transforms. Current target construction allocates both
contiguous `N×6` at `training/target_builder.cpp:123–125`; higher-level sampling
checks mask extents/devices (`training_mask_loss.cpp:35–40,94–104`), and training
owns device placement. No production violation was established. A future raw
caller, mixed-device input or differently strided transform can violate this
implicit contract; do not describe the raw wrapper as independently safe for
arbitrary admitted CUDA tensors. This remains an admission-boundary follow-up,
not an assigned cause of current model divergence.

### Depthwise backward and model inputs

[segmentation_depthwise.cpp:14–37](../../../src/backend/models/rfdetr/core/segmentation_depthwise.cpp#L14)
saves original input/weight, forwards through grouped `conv2d`, disables
autocast for backward, casts cotangents and input to the saved weight dtype,
uses `convolution_backward` for input/weight, and sums the cotangent over
batch/spatial axes for bias. This follows the original-weight precision rule
of upstream
[segmentation.py:98–140](https://github.com/roboflow/rf-detr/blob/e9a138f70cc14e3fddca029a0683055e34ab8f35/src/rfdetr/models/heads/segmentation.py#L98).
Native operation-local algorithm selection differs from upstream's temporary
global cuDNN disable; numerical parity still requires the existing derivative
oracle. The separate compilation review owns whether a traced call preserves
this custom VJP.

Input normalization uses FP32 arithmetic with ImageNet RGB means
`(.485,.456,.406)` and standard deviations `(.229,.224,.225)`, followed by explicit
round-to-nearest FP16/BF16 conversion at
`augmentation/gpu_augment_cuda.cu:29–90`. The vector path is confined to fully
valid groups of four and scalar paths cover odd widths/tails. Padding is zero
in normalized space; erased/invalid augmentation support is filled with the
mean before normalization. The persistent preprocessor guards its selected
device and respects the producer/consumer stream lifetime at
[gpu_batch_preprocessor.cpp](../../../src/backend/models/rfdetr/core/gpu_batch_preprocessor.cpp).
Upstream uses the same normalization constants and normalized center-box target
convention in
[datasets/transforms.py:49–100](https://github.com/roboflow/rf-detr/blob/e9a138f70cc14e3fddca029a0683055e34ab8f35/src/rfdetr/datasets/transforms.py#L49).

Native affine/color/noise/blur/copy-paste augmentation and compiled image
resampling are product adaptations documented in `docs/rfdetr-training.md`,
not identical realizations of upstream Albumentations. For example, the
four-uniform approximate Gaussian in `gpu_augment_cuda.cu:93–94` is a bounded
approximation, not an exact standard-normal sampler. No equivalence claim is
made for that distribution or the full AVIR/perceptual resize implementation.
Target construction (`training/target_builder.cpp:647–742`) preserves logical
mask membership through packed source masks, transforms and occluder indices;
continuous transformed boxes can survive empty raster support by design.
Align image and categorical-target sampling at exact half-pixel boundaries in
future evidence: `imaging/sampling.h:39–40` uses direct `u*extent-.5`, whereas the
loss sampler deliberately models grid-sample's two-stage normalization.

## CUDA/C++ flags and floating-point policy

The declared binary payload is CUDA **13.4.1**, NVCC **13.4.59**, Torch
**2.14.0a0+4fdf77b940.nv26.08**, from the pinned NGC 26.08 donor in
[docker/nvidia-payload.json](../../../docker/nvidia-payload.json). This is a
vendor distribution based on a PyTorch development commit, not an assertion
that an unmodified public release was installed. Primary PyTorch source links
above are pinned to that declared base commit; live `docs/main` pages are
current context, not proof of binary identity.

Evidence inspected:

| Evidence | Observed policy |
| --- | --- |
| `cmake/MmltkToolchain.cmake:46–58,173–184,223–268`; `cmake/MmltkOptions.cmake` | GCC 16.2, CUDA C++23, explicit GCC host and unsupported-host override; host Release `-O3`, native ISA including `-mfma`; CUDA Release `-O3`. |
| `.cache/cmake/release/compile_commands.json:1930,1936,2548,2566` | Actual recorded commands for attention, box IoU, matcher and mask CUDA; `nvcc`, C++23, `-O3`, relocatable device code and architecture/LTO options, with no unsafe-math override. |
| Same compilation database: `1048,2404,2518,2836` | Image resize, model, depthwise and augmentation commands checked; no unsafe-math override. |
| Release `CMakeCache.txt:278–340`; cached analysis/gui compile databases | No `--use_fast_math`, `-ffast-math`, `-Ofast`, `--ftz`, `--prec-div`, `--prec-sqrt`, `--fmad`, reciprocal/finite/unsafe-math override found in the inspected artifacts. |
| `tools/check_toolchain_invariants.py:114–115` | Checker rejects ambient `NVCC_PREPEND_FLAGS`, `NVCC_APPEND_FLAGS`, `NVCC_CCBIN` when inspecting an environment; source existence alone is not evidence it ran for a given binary. |

NVCC 13.4 documents ordinary defaults as `ftz=false`, `prec-div=true`,
`prec-sqrt=true`, `fmad=true`. `--use_fast_math` would additionally select
flushing and approximate division/square root. Its absence therefore matters;
`-O3` and FMA alone do not imply those unsafe approximations.
[NVCC 13.4 numerical options](https://docs.nvidia.com/cuda/cuda-compiler-driver-nvcc/index.html#use-fast-math-use-fast-math).

The reviewed primitive CUDA contains ordinary `expf`/`log1pf`, not explicit
`__expf`/`__logf`/`__fdividef` fast approximations. Explicit rounding intrinsics
in categorical sampling and `fmaf` in image interpolation/moments are not
evidence of dangerous fast math. A fused multiply-add rounds the combined
operation once, which can be *more* accurate, although different from an
upstream graph that intentionally materializes two rounded operations.
[CUDA 13.4 round-to-nearest intrinsic contract](https://docs.nvidia.com/cuda/cuda-math-api/cuda_math_api/group__CUDA__MATH__INTRINSIC__SINGLE.html),
[NVIDIA floating-point guide](https://docs.nvidia.com/cuda/floating-point/index.html).
Keep FMA and normal optimization unless a concrete operation boundary requires
separate rounding to preserve its reference semantics.

No-fast-math is not a guarantee that all subnormals survive. PTX ISA 9.4 states
that global `atom.add.f32` without `.noftz` flushes subnormal inputs/results;
shared-memory addition differs, and `.noftz.f32` has architecture restrictions.
Native and upstream scatter paths both use floating atomics. Actual generated
instructions and hardware must be checked before attributing an underflow
difference to compiler flags.
[PTX 9.4 atomic operation semantics](https://docs.nvidia.com/cuda/parallel-thread-execution/index.html#parallel-synchronization-and-communication-instructions-atom).

`core/runtime.cpp:138–140` disables cuBLAS/cuDNN TF32. This is a deliberate
native runtime precision policy, not a missing fast-math compiler flag.
Prebuilt Torch/cuDNN/cuBLAS kernels, JIT-generated kernels, future environment
overrides and cache freshness cannot be certified from first-party compilation
commands. No SASS/PTX or binary provenance was extracted in this review.

## Existing evidence and limits

| Existing source/artifact | What it establishes by inspection | Missing evidence for these findings |
| --- | --- | --- |
| `layers/tests/ms_deform_attn.test.cpp:8–28` | Independent reference forward and three VJPs, FP32, `B=2`, heads 2, channels 4; forward tolerance `1e-4`, gradient tolerance `2e-4`. | Mixed precision, exact support-boundary VJP, large/odd reduction branches, batch tails. |
| Same file:32–60 | Output overwrite across steps and channels 1/7/32; forward edge cases and asserted empty output for nonfinite coordinates. | Nonfinite weights/cotangents and reference comparison for those exceptional cases; backward at edges. |
| `core/tests/upstream_training_parity.test.cpp:279` | Grouped class/box objective and derivative equations with FP32 logits. | Fused matcher costs from actual half/BF16 logits and near-tie assignments. |
| Same file:384–445 | AMP scopes, dense/sparse masks and loss VJPs. | Class logits are explicitly FP32 at 409/422 and there is one query, so changed matching cannot be detected. |
| Same file:191–230,624 onward,848 onward,869 onward | Depthwise original-weight precision, exceptional matcher behavior, packed-mask admission, epsilon/geometry promotion cases. | Does not establish the additional triggers identified here; production activation distributions are not measured. |
| `core/tests/matcher_workspace.test.cpp:270` | Compact batched versus individual kernel storage/assignment route checks. | Comparing two invocations of the same kernel does not independently validate its mathematical cost sequence. |
| `docs/validation.md:1151–1158` | The complete-model selective fixture explicitly forces reference attention. | It cannot establish composed training parity for the production custom CUDA attention. |
| Existing CMake compile databases/caches | Concrete recorded compiler commands. | They do not establish that every current binary was rebuilt from this exact source or that a test passed. |

The current PyTorch numerical-accuracy reference also cautions that different
mathematically equivalent implementations need not be bitwise identical. That
supports using justified tolerances for ordinary reductions, not ignoring
assignment changes, missing VJPs or differing finite/nonfinite behavior.
[Current PyTorch numerical-accuracy notes](https://docs.pytorch.org/docs/main/notes/numerical_accuracy.html).

## Proposed implementation and validation sequence

1. Establish a pinned upstream/native oracle with identical tensors, weights,
   point coordinates and effective autocast scopes. Record each compared
   tensor's dtype, shape, device, stride, finite mask and cost contribution.
   Materialize CUDA-01's two-query case and CUDA-02/03's exact analytical cases
   before changing product behavior. Retain CPU/reference coverage while adding
   actual CUDA execution to the existing owner suites.
2. Resolve the matcher's precision policy with the oracle evidence. Preserve
   FP32 computation where it improves numerical accuracy; make compact and
   dense paths and the controlled reference coherent across class, geometry
   and masks. Quantify untouched-upstream assignment/update differences
   separately. Retain every matching mode, exceptional replacement and batching;
   do not add lower-precision rounding merely for bitwise agreement. Verify
   downstream VJPs and one update, not only a relaxed final-cost tolerance.
3. Correct the attention's support-boundary derivative and exceptional
   multiplication semantics together, then complete tail-chunk support. Keep
   FP32 accumulation, appropriate atomics, stream/device guards and every
   reduction path. Add focused finite, nonfinite, channel-width and chunk-tail
   cases to the existing layer suite.
4. Integrate the compilation review's preservation of raw custom operations
   and custom VJPs. Compare eager, selective and full compilation on identical
   mixed-precision inputs with **production CUDA attention enabled**, through
   forward, backward and an update. Separately compare no-grad inference.
5. Execute through `./mmltk` only, using the existing ML-layer and RF-DETR core,
   training and augmentation suites and the repository's required final
   validation workflow. Preserve successful evidence and report hardware skips
   explicitly. Validate throughput/memory after numerical acceptance so the
   correction preserves the intended optimized functionality.

Unresolved facts are the failing run's exact artifact, GPU, dtype and
compilation route; the frequency of near-tie assignments and exact support
boundaries in its activations; the vendor binary's identity relative to public
Torch commit `4fdf77b940`; effective scripted mask autocast behavior; emitted
atomic/subnormal instructions; and full image-resampling equivalence, which is
not claimed for the native augmentation/compiled-data adaptations. Their
resolution is necessary before attributing a training collapse to any one
kernel or numerical option.
