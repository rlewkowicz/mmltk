# RF-DETR model, forward, and input/output equivalence review

Historical baseline review; line anchors and findings refer to the revisions
below. See the [review index](README.md) and [current closure/evidence](current-results.md).

Review date: 2026-10-05. This is a source review, not a validation result.

## Reference and claim boundaries

The native reference is commit `52586e98f535205cc242dfe74ef5144eef0373bb` in
`/home/ryan/Repos/win/cplusplusloader`. The independently read upstream is the
clean, read-only checkout `/home/ryan/Repos/win/rf-upstream`, commit
[`e9a138f70cc14e3fddca029a0683055e34ab8f35`](https://github.com/roboflow/rf-detr/tree/e9a138f70cc14e3fddca029a0683055e34ab8f35).
Source anchors below use `N/` for
`src/backend/models/rfdetr/` in the native checkout and `U/` for
`src/rfdetr/` in upstream. Other native paths are repository relative.

The governing sources read were `AGENTS.md`, `CONTRACT.md`, the training and
workflow references, and the build/validation documentation. In particular,
[`docs/rfdetr-training.md:7`](../../rfdetr-training.md)
limits upstream equivalence to equivalent configuration, admitted tensors,
stochastic samples, and optimizer settings. Native data membership,
augmentation, class layouts, catalog, Match-Free, DN, multilane execution, and
optimizer choices retain their own contracts. No capability should be removed
to make an upstream comparison pass.

[`docker/nvidia-payload.json:2`](../../../docker/nvidia-payload.json#L2)
pins NGC `26.08-py3` at digest
`sha256:33ef5fc15e8937602d64022209cdb2777b32dadf742f41332023d946041b3c14`,
Torch `2.14.0a0+4fdf77b940` and CUDA `13.4.1` / NVCC `13.4.59`.
The online PyTorch 2.14 documentation and C++ `main` documentation describe
current APIs, not proof of the installed alpha binary's complete behavior.
Public PyTorch C++ sources at `4fdf77b940` were also inspected; they identify the
payload's public base revision, without establishing whether NGC applied
additional patches. NVIDIA's archived CUDA 13.4.1 numerical documentation
matches the selected CUDA series. No container, model, compiler, test, or
benchmark was executed in this review.

The default catalog forward computation is substantially aligned with the
pinned upstream. The most actionable defects are checkpoint-name admission,
cold/partial initialization, and publication of auxiliary outputs when they
are disabled. Exact end-to-end upstream equality remains unestablished by the
existing full-model fixtures. FP32 score promotion, custom image resizing,
and native precision settings are separate numerical policies, not evidence
of a broken detector.

| ID | Severity and status | Concrete scope |
| --- | --- | --- |
| FWD-01 | High; verified source defect, conditional on artifact format | Lightning `state_dict` prefixes are preserved; ordinary inference can accept zero loaded core tensors. |
| FWD-02 | Medium; verified initialization mismatch | Unloaded native encoder box heads are nonzero, transformer initialization differs, and DINO tokens/positions use a different distribution. |
| FWD-03 | Medium; verified output-contract mismatch | `aux_loss=false` still publishes ordinary intermediate decoder outputs and sends them to the criterion. |
| FWD-04 | Low; verified primitive difference, numerical size unmeasured | Projector uses `x * sigmoid(x)` instead of the upstream SiLU primitive. |
| FWD-05 | Medium; verified evidence gap | Checkpoint full-forward comparison executes native code on both sides, at very small spatial sizes. |

There is no evidence in this review that any of these findings explains the
reported bad AP. The supplied run context is Nano, pretrained, frozen encoder,
AMP, selective compilation, Match-Free, and DN disabled. Prefix admission and
cold initialization must not be attributed to that run without its actual
artifact inventory and load summary. The active-supervision transfer path is
stricter than the ordinary inference/stock-Hungarian path identified below.

## Forward equations checked against actual upstream callers

These are source-level equivalence conclusions under the stated scope. They
are not claims of bitwise equality across attention backends or precisions.

| Boundary | Native and upstream anchors | Equation and assessment |
| --- | --- | --- |
| Catalog topology | `N/contract/preset_catalog.h:53`, `N/contract/model_config.cpp:103`; `U/config.py:801`, `:838`, `:879`; `U/models/lwdetr.py:785` | The ten catalog rows agree with current detection Nano/Small/Medium/Large and segmentation Nano through 2XL: small DINO width 384, depth 12; decoder width 256; detection patch 16 and segmentation patch 12; correct resolutions, windows, decoder depths and query counts. Upstream `num_classes=90` becomes a 91-slot detector; native 91 is not an off-by-one defect. Older Base/Large/SegPreview, keypoints, registers, other projector scales and arbitrary upstream overrides are not established native catalog capabilities. |
| Patch and learned positional embeddings | `N/core/model.cpp:249`, `:260`, `:269`, `:286`; `U/models/backbone/dinov2_with_windowed_attn.py:336`, `:392`, `:430` | Strided patch convolution, class token, learned position addition, and window partition agree for catalog settings. Resized learned positions use FP32 bicubic interpolation, `align_corners=false`, antialias enabled, then restore position-table dtype. No clamp is added to learned positions. Matching-grid square inputs bypass interpolation. |
| DINO window/global schedule and outputs | `N/core/model.cpp:379`, `:415`, `:436`; `U/models/backbone/dinov2.py:58`; `U/models/backbone/dinov2_with_windowed_attn.py:759`, `:809`, `:1314` | Both retain the historical global blocks at zero-based indices 3, 6, 9 for stage list `[3,6,9,12]`, and return normalized hidden states after stages 3, 6, 9, 12. Changing global blocks to 2, 5, 8, 11 would break checkpoint behavior. Window stitching and removal of class tokens agree. |
| DINO block math | `N/core/model.cpp:320`, `:362`, `:369`, `:383`; `U/models/backbone/dinov2_with_windowed_attn.py:513`, `:549`, `:735` | Pre-LN with epsilon `1e-6`; separate Q/K/V, six heads of width 64, scale `1/sqrt(64)`; residual learned layer scales; exact GELU MLP with width multiplier 4. Native SDPA uses zero dropout. Default upstream attention dropout and drop path are zero (`U/config.py:1055`, `U/models/_defaults.py:103`); omission is equivalent only within that default scope. |
| P4 projector | `N/core/model.cpp:168`, `:184`, `:208`, `:223`, `:485`; `U/models/backbone/projector.py:24`, `:104`, `:165`, `:194`, `:272` | Four 384-channel maps concatenate, then the C2f split/three bottlenecks/concatenate/projection and final channel LN agree. Both LN implementations permute to NHWC and call native `layer_norm` with epsilon `1e-6`; there is no custom biased/unbiased variance discrepancy here. SiLU primitive difference is FWD-04. |
| Spatial sine positions and padding | `N/core/modules.cpp:66`, `:129`, `:206`; `N/core/model.cpp:111`, `:1115`, `:1137`; `U/models/position_encoding.py:127`; `U/models/transformer.py:304`, `:529`, `:678` | Position helper uses FP32 cumulative valid coordinates, normalization by last coordinate plus `1e-6`, `2*pi`, frequency `10000^(2*floor(i/2)/D)`, and y then x channels. Full-valid cache uses arange 1 through extent. Native model forward always requests full-valid positions; with padding those differ from upstream's mask-aware helper, but this tensor is not consumed by the audited decoder in either implementation (`N/core/model.cpp:706`). This is not demonstrated output corruption. Padding still reaches proposal generation, valid ratios, and deformable attention. |
| Two-stage proposal construction | `N/core/model.cpp:722`, `:829`, `:849`; `U/models/transformer.py:92`, `:284`, `:385` | Cell centers are `(x+0.5)/valid_W`, `(y+0.5)/valid_H`; widths/heights are `0.05*2^level`. Validity interval is strictly `(0.01,0.99)`, with padded/invalid memory zeroed and proposals zero or infinity according to reparameterization. Per-group encoder projection and LN precede classification ranking. `topk=min(num_queries, HW)` and maximum class logit determine selected rows. |
| Selected encoder box head | `N/core/model.cpp:864`; `U/models/transformer.py:385–440`; `N/core/tests/upstream_training_parity.test.cpp:727` | Both now gather memory before evaluating the pointwise box MLP. For fixed selection, `gather(MLP(X),I)=MLP(gather(X,I))`; this retains both values and VJPs and avoids unnecessary head work. Do not restore an obsolete all-position MLP under a mistaken parity claim. |
| Reference coordinates and refinement | `N/core/model.cpp:118`, `:141`, `:660`, `:875`; `U/models/transformer.py:64`, `:468`, `:659`, `:714` | Reparameterized boxes use `xy'=xy+delta_xy*wh`, `wh'=exp(delta_wh)*wh`; the sigmoid alternative remains available. Proposal references are detached before decoder seeding. Reference input multiplies xywh by valid ratios in the coordinate dtype, with y,x,w,h sine ordering. Lite refinement reuses references; iterative refinement detaches next-layer references and normalizes output hidden states separately. No extra clamp is introduced. |
| Grouped decoder self-attention | `N/core/decoder_attention.cpp:6`, `:15`, `:44`; `N/core/model.cpp:615`; `U/models/transformer.py:958–993` | `Q=Wq(tgt+pos)`, `K=Wk(tgt+pos)`, `V=Wv(tgt)`, followed by scaled attention and output projection. Native batch-major group packing and upstream group-major packing implement independent identical blocks, then restore original query order. Native explicitly projects Q/K/V, preserving separate positional and target derivatives even at coincident values. SDPA boolean masks correctly invert padding semantics. DN isolation is an additional native route. |
| Cross-attention and decoder tail | `N/core/model.cpp:542`, `:563`, `:615`; `U/models/ops/modules/ms_deform_attn.py:175`, `:225`; `U/models/transformer.py:977–993` | Attention weights normalize over all levels and points. For 4D references, sample locations are `r_xy + offsets/P * r_wh * 0.5`; for 2D references they are `r_xy+offsets/(W,H)`. Value padding is zeroed. Post-attention LN/residual and ReLU feedforward equations agree, with decoder LN epsilon `1e-5`. Custom sampler accumulation/backward details belong to the CUDA review. |
| Segmentation dense/sparse and encoder branch | `N/core/modules.cpp:221`, `:238`, `:254`, `:297`, `:307`; `N/core/model.cpp:1140`, `:1245`; `U/models/heads/segmentation.py:143`, `:191`, `:215`, `:243`, `:271`; `U/models/lwdetr.py:496`, `:599` | Resize to `(image_H//4,image_W//4)` by bilinear interpolation with `align_corners=false`, no antialias. Apply sequential residual depthwise blocks, learned spatial 1x1 projection, query residual MLP and learned projection. Dense masks are `sum_c spatial[b,c,h,w]*query[b,q,c]+bias`; sparse output retains these exact factors. The encoder `skip_blocks` path still applies learned spatial/query projections, matching pinned upstream. Default bottleneck ratio 1 does not mean identity projection. |
| Train/eval output selection | `N/core/model.cpp:1140`, `:1212`, `:1220`; `U/models/lwdetr.py:487`, `:496`, `:566` | Training uses all query groups; evaluation uses the first group. Training mask output is sparse; ordinary eval output is dense. Final decoder head output is selected. Encoder outputs remain separate except the decoder-free branch. Disabled auxiliary publication differs as FWD-03. |

Mask conventions deserve separate names: a padding mask marks invalid pixels
with true; SDPA boolean attention masks admit entries with true. The native
inversion is correct. The current official
[SDPA documentation](https://docs.pytorch.org/docs/2.14/generated/torch.nn.functional.scaled_dot_product_attention.html)
also notes backend-dependent outputs and automatic kernel selection. Equality
of the attention formula does not prove equality of the selected fused kernel.

For bilinear resize with `align_corners=false`, the sample center is
`(out_index+0.5)*input_extent/output_extent-0.5`, with edge padding. This is
distinct from zero-padded deformable/grid sampling. The audited learned-position,
mask-feature, and mask-output resizes agree on the relevant settings.
[PyTorch interpolation documentation](https://docs.pytorch.org/docs/2.14/generated/torch.nn.functional.interpolate.html)
also warns that CUDA half-precision interpolation gradients can be inaccurate
and CUDA gradients nondeterministic. These documented limitations are not
native-only defects.

## FWD-01 — Checkpoint envelopes are admitted without matching their names

**Severity:** High. **Status:** verified by source, conditional trigger; no
claim that the current run loaded such an artifact.

**Trigger:** a valid upstream Lightning checkpoint uses
`state_dict["model.<parameter>"]`, or
`state_dict["model._orig_mod.<parameter>"]`, and passes native preset/class
admission. Supply a recognized preset and an admitted class-layout descriptor
when the filename or embedded metadata alone cannot resolve them. A bare
checkpoint with insufficient class/preset evidence can fail earlier, so this
is not a claim that every arbitrary file reaches inference.

**Proof:**

- `N/training/rfdetr_checkpoint_bridge.py:46–67` accepts `state_dict` and
  assigns it directly to `model`. Export at `:156–179` emits original names.
  It does not strip `model.` / `_orig_mod.` or map Lightning
  `hyper_parameters` to `args`.
- Upstream explicitly strips those prefixes and maps the metadata envelope in
  `U/models/weights.py:376–406`, then runs compatibility checks at `:425`.
- `N/core/model.cpp:1312–1316` matches exact names. Every prefixed tensor is
  unexpected; none loads. `:1351–1364` reports missing tensors but permits
  this state for `PartialExact` and `PartialFreshTransfer`.
- `N/inference/predict.cpp:185–193` constructs a model, calls
  `load_normalized_state(..., false)`, discards the summary, and freezes the
  result. This admits an initialized detector instead of the checkpoint's
  detector when all names are prefixed. It is a functional mapping failure,
  independent of rounding tolerances.
- `N/training/training_snapshot.cpp:31–37` selects
  `PartialFreshTransfer` for inactive supervision and `FreshTransfer` for an
  active route. Thus ordinary stock transfer can also retain unloaded core
  tensors; Match-Free/DN active transfer instead rejects unexpected/missing
  required names. Shape-incompatible non-class tensors are similarly skipped
  by the partial paths (`N/core/model.cpp:1342`).

**Affected owners/files:** checkpoint normalization in
`N/training/rfdetr_checkpoint_bridge.py`; required model-state admission in
`N/core/model.cpp` and `N/core/model_state.cpp`; inference admission in
`N/inference/predict.cpp`; stock fresh-transfer admission in
`N/training/training_snapshot.cpp`. Class evidence must continue to be owned
by the existing class-artifact/layout boundary.

**Minimal functionality-preserving correction:** normalize recognized external
envelopes once, before tensor-name/class/topology inspection. Detect name
collisions after normalization. Map supported Lightning metadata without
guessing class semantics. Require a complete compatible inference core at the
production boundary, with an explicit allowlist for legitimate non-inference
state; keep explicitly requested partial-transfer capability and semantic
class remapping. Do not replace every load with an indiscriminate strict load:
upstream now registers an empty `_kp_active_mask` even with keypoints disabled
(`U/models/lwdetr.py:202`, `:251–254`), and native training-supervision state is
also legitimately absent from stock pretrained weights. Nonempty unsupported
keypoint state must not be silently treated as an equivalent detector.

**Focused first-party validation:** extend the existing checkpoint fixtures
with the same actual upstream-generated detector state in legacy `model`,
Lightning `model.`, and compiled-Lightning `model._orig_mod.` envelopes. Check
complete required-core name/shape coverage and equivalent raw outputs after
normalization; add collision, empty-loaded-core, damaged tensor shape, class
layout, and allowed inactive-buffer cases. Exercise the production inference
caller, stock transfer, active transfer, and an explicitly requested partial
transfer separately. Existing four-tensor storage fixtures should remain
valid as storage tests, without being admitted as complete production models.

## FWD-02 — Native initialization is not upstream initialization

**Severity:** Medium. **Status:** verified, relevant only to tensors retained
from construction or newly introduced by transfer. Complete loaded tensors
are unaffected.

**Trigger:** direct fresh model construction, an intentionally partial
pretrained transfer, newly initialized class rows, or a missing core tensor
admitted by FWD-01. The production pretrained Match-Free run does not establish
this trigger; its load inventory is needed first.

**Proof:**

1. Native `N/core/model.cpp:779–782` constructs independent encoder box MLPs.
   Only the decoder box MLP's last layer is zeroed at `:968–971`.
   Upstream zeroes the decoder head and deep-copies it to every encoder group
   (`U/models/lwdetr.py:210–222`). At initialization upstream therefore has
   `delta_encoder=0`, hence `box_encoder=proposal` under reparameterization;
   native's random last layer generally changes centers and sizes immediately.
2. Upstream applies Xavier uniform to transformer parameters of rank greater
   than one, then restores deformable-attention-specific initialization
   (`U/models/transformer.py:257`, `:276–282`). Native performs the special
   deformable initialization (`N/core/model.cpp:586`) but has no corresponding
   transformer-wide Xavier pass in `:768–791` / `:660–672`.
   The public pinned [LibTorch Linear implementation](https://raw.githubusercontent.com/pytorch/pytorch/4fdf77b940/torch/csrc/api/src/nn/modules/linear.cpp)
   uses Kaiming uniform and fan-in uniform biases by default; constructing a
   Linear does not supply upstream's transformer override.
3. Native class token and position table use `randn` at
   `N/core/model.cpp:263–265`. Upstream's model `post_init` uses truncated
   normal with configured standard deviation `0.02` for these tensors and
   linear/convolution weights, and zeros their biases
   (`U/models/backbone/dinov2_with_windowed_attn.py:244`, `:881–905`, `:1254`). Native
   does not apply that backbone initialization. Encoder class heads also start
   independently rather than as copies of the decoder class head.

This is not merely a different random seed. Zero versus nonzero encoder box
output is a deterministic construction invariant, and the initialization
distributions differ. Matching distributions alone would still not establish
same-seed tensor identity because constructor order consumes random draws
differently.

**Affected owner/files:** the ordinary model constructors and initialization
in `N/core/model.cpp`; transfer admission in `N/core/model.cpp:1278` determines
which initialized values survive.

**Minimal functionality-preserving correction:** specify and implement the
upstream initialization order for the shared detector/backbone tensors,
including encoder copies after final head initialization and restoration of
MS-deformable initialization after the transformer sweep. Preserve the native
supervision initialization and its RNG-state preservation at
`N/core/model.cpp:973–982`. Keep semantic class-row matching; do not replace it
with upstream's positional row tiling. Identify whether unmatched native class
rows intentionally have a different policy and document/test that policy
separately.

**Focused first-party validation:** assert zero final encoder/decoder box-head
weights and biases, exact proposal-preserving initial box outputs, matching
initial group-head copies, configured DINO initialization, and unaffected
loaded tensors. Compare against tensors/invariants exported by actual pinned
upstream construction, not two native models with equal seeds. Include
complete pretrained load, explicitly partial transfer, a new foreground class,
and extension enabled/disabled with unchanged ordinary initialization RNG.

## FWD-03 — Disabled auxiliary loss still publishes auxiliary model outputs

**Severity:** Medium. **Status:** verified nondefault output-contract mismatch.

**Trigger:** `aux_loss=false` with more than one decoder layer. This field is
part of `N/contract/model_config.h:59` and is imported by
`N/training/rfdetr_checkpoint_bridge.py:128`; it is not a hypothetical flag.

**Proof:** native forward puts every nonfinal decoder layer into
`outputs.aux_outputs` without testing the flag (`N/core/model.cpp:1220–1223`).
Upstream only creates that output when `self.aux_loss` is true
(`U/models/lwdetr.py:571–577`). Native criterion includes every supplied
auxiliary layer in matcher work and loss computation
(`N/core/detection_ops.cpp:705–734`).

The default weight dictionary does honor the flag
(`N/core/detection_types.cpp:20`), and the weighted reduction iterates that
dictionary (`N/core/detection_ops.cpp:753`). Consequently this does **not**
prove that unwanted auxiliary losses are directly added to the ordinary
objective. It does prove a different returned structure, unnecessary
assignments/loss tensors, and extra stochastic sampling on mask routes that
use random samples (`N/core/detection_ops.cpp:242`, `:252`, `:630`). Exact
downstream RNG impact depends on retained coordinates and native sampling
keys; that effect was not executed here.

**Affected owners/files:** output assembly in `N/core/model.cpp`; criterion
and sample indexing in `N/core/detection_ops.cpp`; extension consumers in
`N/core/training_supervision.cpp`; trace output schema is jointly owned with
the compilation reviewer.

**Minimal functionality-preserving correction:** make ordinary published
auxiliary outputs follow the admitted stock configuration. Retain intermediate
features needed by explicitly enabled Match-Free/DN auxiliary supervision,
with coordinated extension/output ownership rather than deleting those
features. Keep the encoder branch's upstream behavior distinct from the
decoder auxiliary flag. Update trace pack/unpack and sample-layer indexing
together if their ordinary output inventory changes.

**Focused first-party validation:** two- and four-layer models with the flag
on/off, detection and segmentation, train/eval, eager/selective. Assert output
inventory, weighted objective, and draw-state behavior with fixed and random
mask samples. Include Match-Free/DN configurations that independently request
auxiliary supervision so their capability survives. The training/objective
reviewer should own the final RNG/objective acceptance conditions.

## FWD-04 — Projector SiLU is algebraically equal but uses different rounding

**Severity:** Low. **Status:** verified primitive mismatch; no measured model
error or quality impact.

**Trigger:** any projector ConvX call; the distinction is especially visible
if activation tensors themselves use FP16/BF16 rather than the usual FP32
LayerNorm output under autocast.

**Proof:** `N/core/model.cpp:198` evaluates `out * sigmoid(out)` as two tensor
operations. Upstream `U/models/backbone/projector.py:69–74`, `:119–123` uses
`nn.SiLU`. The real-valued equation is the same. The
[pinned public CUDA SiLU implementation](https://raw.githubusercontent.com/pytorch/pytorch/4fdf77b940/aten/src/ATen/native/cuda/ActivationSiluKernel.cu)
evaluates the expression in `opmath_t` and rounds the result into the output
type once; the expanded version materializes a rounded sigmoid before
multiplication. Its backward graph also differs from the SiLU backward
primitive. Even FP32 division versus reciprocal-times-multiply need not be
bitwise equal.

Ordinary CUDA autocast puts eligible `layer_norm` in FP32, so it would be wrong
to assert that every AMP projector activation necessarily suffers an extra
BF16 rounding. The
[current AMP operator policy](https://docs.pytorch.org/docs/2.14/amp.html)
distinguishes FP32 norms/softmax from lower-precision linear/convolution
operations and input-driven promotion; explicit `out=` operations have
different eligibility. Actual dispatch should be recorded in validation.

**Affected owner/file:** native projector activation in `N/core/model.cpp`.

**Minimal functionality-preserving correction:** use the existing
`torch::silu` / `torch::nn::functional::silu` primitive in the shared eager and
selective projector path. It retains the intended activation and its
autograd support; no handwritten replacement kernel is needed. Availability
is confirmed by the
[pinned C++ functional implementation](https://raw.githubusercontent.com/pytorch/pytorch/4fdf77b940/torch/csrc/api/include/torch/nn/functional/activation.h)
and [current C++ functional documentation](https://docs.pytorch.org/cppdocs/api/nn/functional.html).

**Focused first-party validation:** compare projector output and VJP against
upstream SiLU on signed inputs, near zero and in both saturation tails, FP32
and supported FP16/BF16/autocast modes. Check the actual normalized activation
dtype and the selective recorded path. Set empirical tolerances from the
pinned backend; do not infer a training convergence fix from this local change.

## FWD-05 — Full-forward fixtures do not establish independent upstream parity

**Severity:** Medium evidence gap. **Status:** verified from test construction.

**Trigger:** treating the existing checkpoint-parity pass or native
eager/selective pass as proof that the complete RF-DETR model matches upstream.

**Proof and retained value of the current coverage:**

- `N/training/tests/checkpoint_parity.test.cpp:104` constructs
  `NativeRfDetrModel seeded_model`, serializes it to the upstream *file format*
  at `:106`, normalizes it, then constructs two more `NativeRfDetrModel`
  instances at `:123–124`. Both forwards at `:132–134` call native code. This
  is a useful storage/name roundtrip and deterministic native equality test;
  the variable named `upstream_model` is not an upstream implementation.
- `N/core/tests/parity_fixture_support.cpp:8–12` contains only detection Nano
  at 32 pixels and Seg Nano at 24 pixels. Those inputs have four patch tokens,
  unlike normal 384/312-pixel execution. The separate synthetic storage
  fixtures at `:37–51` contain only query, reference, class weight and class
  bias tensors. They cannot verify a complete backbone/head checkpoint.
- `N/core/tests/upstream_training_parity.test.cpp:727` independently checks
  selected encoder MLP values/VJPs; `:233` checks learned encoder mask
  projection; `:765` checks a full mask block through an update. These are
  valuable local equation oracles, not an executed upstream full forward.
- `N/core/tests/selective_compilation.test.cpp:222`, `:276`, `:347` uses
  independent grouped attention and decoder-tail equations/VJPs. The
  complete-model production-step fixture at
  `N/training/tests/training_supervision.test.cpp:2332` compares native eager
  and selective execution, not upstream Python. Its success cannot detect a
  mathematical error shared by both native paths.
- `docs/validation.md:1113–1193` already explains source coverage and limits;
  this review does not reinterpret that inventory as executed evidence.

**Affected owners/files:** existing checkpoint, native-ops, upstream-parity,
selective and production-step suites named above; first-party fixture
generation and wrapper registration needed to obtain independent artifacts;
`docs/validation.md` / `docs/rfdetr-training.md` for eventual recorded scope.

**Minimal functionality-preserving correction:** keep the existing roundtrip
and native compilation comparisons, label their evidence accurately, and add
first-party oracle fixtures generated by importing and executing actual
upstream at the pinned commit. The generator must not reimplement native
equations or modify upstream. Export admitted configuration, checkpoint hash,
named tensor inventory, input/mask bytes, dtypes/backend flags, intermediate
activations, selected encoder indices, raw heads, and fixed-vector VJPs. Use
those artifacts from the existing first-party suites; Python is a validation
producer, not a replacement native training/inference route.

**Focused validation matrix:**

| Case | Evidence it adds |
| --- | --- |
| Nano 384, Seg Nano 312, batch 1 and 2 | Real 576/676-patch-token backbones, pretrained names/shapes, first-group eval, normal query counts, one-window versus two-window routing. |
| Seg Small 384 and one large endpoint, e.g. Seg 2XL 768 | Patch-12/two-window partition, six decoder stages, 192x192 mask features and realistic reductions/interpolation. |
| Train Nano with 13 groups and 300 queries/group | Actual 3900-query training layout, group ordering and masking; tiny fixtures do not exercise this regime. |
| Resolution change and rectangular right/bottom padding, dimensions compatible with patch/window geometry | Learned-position interpolation, valid ratios, proposal boundaries, spatial flatten/stitch order; compare padding-aware intermediates separately from unused full-valid sine positions. |
| Full pretrained state, Lightning envelopes, explicit partial transfer, new semantic class row | Distinguishes constructor, admission, class remapping and forward behavior. |
| CPU FP32, CUDA controlled FP32, CUDA FP16/BF16 autocast | Separates equations from autocast/SDPA/reduction choices. GPU absence must report a skip, not support a precision claim. |
| Fixed upstream/native tensors and fixed sampled coordinates, then raw VJPs before an optimizer step | Separates forward differences from loss, augmentation and optimizer differences. Native extension cases retain their own independent equations. |

Use exact assertions for inventories, query identities, intended zero/copy
initialization and mask polarity. Compare floating tensors by dtype/backend
with maximum absolute/relative error and a useful aggregate error; retain the
first differing stage. Also record top-k identities, boxes in source pixels,
and mask sign disagreements, because small smooth errors can cross discrete
selection thresholds. Existing suite selection remains through `./mmltk`;
no validation was run in this review.

## Numerical and data policies that must remain explicit

### FP32 postprocessing changes low-precision score ties

Native `N/core/postprocess.cpp:33–34` promotes logits and boxes to FP32 before
sigmoid/ranking/geometry. Upstream `U/models/postprocess.py:133–144`, `:163–169`
preserves their incoming dtype. Both use stable descending flattened
query/class order, then map selected indices to query and class.

This can change actual selected identity, not just score decimals. Consider
BF16 logits `[7,8]` in physical flattened order with `K=1`, no slot filtering,
and two different associated boxes. Their sigmoids are approximately
`0.999089` and `0.999665`. Both round to `1` in BF16, so upstream's stable tie
selects the first entry; native FP32 sigmoid distinguishes them and selects
the second. FP16 logits `[9,10]` give the analogous saturation example. This
is an analytic counterexample from the source ordering and representable
values, not an executed GPU result. FP32 box arithmetic also differs from
performing corner conversion and source-size scaling in half precision.

**Classification:** verified numerical-policy difference, not a demonstrated
defect. Native's higher precision may be desirable. Preserve it unless an
explicit product requirement chooses upstream-native-dtype output semantics.
The minimal action is to state the comparison boundary: compare raw heads,
then compare each postprocessor under its declared dtype policy; when seeking
equal postprocessed results, explicitly promote both sides to the same dtype.
Add the saturated-score example, unsaturated near ties, non-square source
sizes, large pixel extents, and mask-query identity to the existing
`N/core/tests/native_ops.test.cpp:733`, `:784` postprocessing coverage. Do not
hide a changed selected query inside an allclose tolerance.

Mask materialization itself gathers by selected query and performs bilinear
resize in the mask tensor dtype followed by `>0`
(`N/core/postprocess.cpp:140–160`; `U/models/postprocess.py:173` onward).
Native semantic class slots are filtered after physical ranking
(`N/core/postprocess.cpp:50–68`), an intentional class-layout contract. Preserve
sparse COCO slots, reordered classes, ignored/background roles and stable
selected-mask identity.

### File-image and video preprocessing do not share upstream's full path

Upstream converts image values to floating point before antialias-free
bilinear resize and ImageNet normalization (`U/detr.py:2554–2557`,
`:2582–2586`). Native file prediction resizes RGB8 to RGB8 with AVIR and then
converts to floating CHW (`N/inference/predict.cpp:1191–1210`;
`src/backend/imaging/resample/image_resize.cpp:31`, `:89–118`). Native video
prediction resizes float CHW with `upsample_bilinear2d_out(..., false)`
(`N/inference/predict.cpp:1181–1183`). GPU normalization uses the same RGB means
`[0.485,0.456,0.406]` and standard deviations `[0.229,0.224,0.225]`, then casts
to the selected output dtype (`N/augmentation/gpu_augment_cuda.cu:29–53`).

AVIR filtering and a rounded uint8 intermediate are not equivalent to the
upstream float bilinear operation. Therefore identical decoded source pixels
do not alone establish identical admitted model tensors across file, video
and upstream prediction. No measured AP impact follows from this observation.

**Classification:** a data-policy difference within the repository's explicit
custom preprocessing/augmentation scope. Preserve AVIR, perceptual modes,
video support, compiled datasets and all augmentation capabilities. For model
comparison, supply identical already-normalized input tensors first. Then
separately characterize each preprocessing route with impulses, checkerboards,
RGB ramps, non-square dimensions, identity resize and down/up sampling. If an
upstream preprocessing comparison route is later required, make that explicit
without replacing or reducing existing routes.

### AMP, TF32 and backend choices are part of a numerical comparison

Native runtime disables cuBLAS/cuDNN TF32 and sets the library's CPU
flush-denormal option false (`N/core/runtime.cpp:138–140`). This does not by
itself establish CUDA denormal handling. Upstream selects float32 matmul precision
`high` (`U/detr.py:53`, `U/training/trainer.py:553`). Native inference chooses
its supported lower precision from device capability
(`N/inference/predict.cpp:194–200`). Equal visible tensor dtypes therefore do
not establish equal internal math settings.

Record actual parameter/input/activation dtypes, autocast state, SDPA backend,
TF32 flags and reduced-precision GEMM reduction flags for every oracle. The
[PyTorch numerical-accuracy reference](https://docs.pytorch.org/docs/2.14/notes/numerical_accuracy.html)
explains why batched versus sliced execution, reduction order and TF32 can
change results, and why disabling TF32 alone does not control FP16/BF16
reduction policy. NVIDIA documents single-round FMA versus separate rounding
and nonassociative reductions in its
[CUDA 13.4.1 floating-point guide](https://docs.nvidia.com/cuda/archive/13.4.1/floating-point/index.html).
These facts justify measured tolerances; they do not justify accepting any
error size, losing query identity, or claiming equality without measurement.

The expanded SiLU finding, selected-memory MLP, grouped attention batch order
and custom CUDA reductions require local error attribution before blaming
compilation. Selective/full-trace implementation correctness is owned by the
separate compilation report. Nothing here identifies selective compilation
as the cause of the supplied run's AP.

## Open-source C++ references examined and practical use

These are inspected source alternatives, not recommendations to replace the
native system wholesale.

| Reference/version | What was inspected | Appropriate use and limitation |
| --- | --- | --- |
| PyTorch public base `4fdf77b940` | [C++ Linear](https://raw.githubusercontent.com/pytorch/pytorch/4fdf77b940/torch/csrc/api/src/nn/modules/linear.cpp), [C++ functional activations](https://raw.githubusercontent.com/pytorch/pytorch/4fdf77b940/torch/csrc/api/include/torch/nn/functional/activation.h), [CUDA SiLU](https://raw.githubusercontent.com/pytorch/pytorch/4fdf77b940/aten/src/ATen/native/cuda/ActivationSiluKernel.cu) | Strongest direct C++ reference for invoking the same ATen operator and distinguishing default module initialization from RF-DETR overrides. Standard LibTorch primitives retain autograd and avoid a second handwritten implementation. Public-base source is not a substitute for checking NGC dispatch and runtime flags. |
| `infracv/rf-detr-cpp` tag `v0.0.1` (release identifies commit `63280ce`) | [C++ postprocess](https://raw.githubusercontent.com/infracv/rf-detr-cpp/v0.0.1/src/core/postprocess.cpp), [CUDA preprocessing](https://raw.githubusercontent.com/infracv/rf-detr-cpp/v0.0.1/src/core/cuda_preprocess.cu), [CUDA mask decode](https://raw.githubusercontent.com/infracv/rf-detr-cpp/v0.0.1/src/core/mask_decode.cu) | Useful independently implemented TensorRT inference boundary comparator. It ranks raw logits using `partial_sort` without a stable secondary key, applies FP32 sigmoid afterward, uses a single background index, and drops nonpositive boxes. Its fused bilinear preprocess folds normalization constants. Those differ from pinned upstream sigmoid-first stable ranking, native semantic layouts, and operation rounding. It is not a training/VJP oracle or a drop-in preservation of native capabilities. |
| NVIDIA TensorRT `release/10.15` source branch | [Multiscale deformable sampling CUDA](https://raw.githubusercontent.com/NVIDIA/TensorRT/release/10.15/plugin/multiscaleDeformableAttnPlugin/multiscaleDeformableIm2ColCuda.cuh) | Useful independent check of normalized-coordinate conversion and bilinear support. Its half specialization performs half arithmetic in places where a float-accumulating implementation differs. This versioned branch was examined, not the installed TensorRT 11.2.1.2 binary, and should be pinned to an immutable commit before fixture generation. It supplies neither complete RF-DETR model semantics nor an interchangeable training backward. |

## Scoped follow-up order and unresolved questions

1. Establish checkpoint required-core admission and envelope normalization
   first (FWD-01). The import boundary should expose a reviewable coverage
   report without requiring verbose diagnostics to discover that a production
   model loaded no core tensors. Preserve explicit partial transfers and
   class-layout validation.
2. Introduce independently generated upstream fixtures within the existing
   first-party validation ownership (FWD-05), starting with pretrained Nano
   and Seg Nano at their real resolutions. Record current differences before
   changing model math.
3. Correct deterministic initialization invariants and stock auxiliary-output
   publication (FWD-02/FWD-03), coordinating extension and compilation owners.
   Then align the projector activation primitive (FWD-04) and measure its local
   forward/VJP effect. Do not fold unrelated optimizer, data or CUDA changes
   into these corrections.
4. Characterize postprocess and preprocessing policies separately. Retain
   native functionality and document controlled comparison settings. Extend
   to grouped training, padding, large segmentation shapes and supported AMP
   modes; the main agent owns all eventual `./mmltk` execution.

Remaining uncertainties:

- Actual tensor load coverage for the reported artifact has not been inspected
  here. Full pretrained coverage would eliminate the cold-construction
  explanation for covered tensors. Run-specific AP attribution requires
  matched images, labels, admitted tensors, weights, selection policy and
  measured intermediate divergence.
- Import metadata reconstructs topology mainly from a catalog preset:
  `N/training/rfdetr_checkpoint_bridge.py:106–153` carries only selected fields,
  and `N/core/model_state.cpp:283–299` rebuilds from the preset. Same-shaped
  upstream overrides such as a different window schedule or refinement policy
  are not proven equivalent merely because tensors fit. Determine the precise
  accepted external-config contract and reject/explicitly route unsupported
  semantics; do not silently broaden the current equivalence claim to every
  upstream configuration.
- Public pinned upstream adds inactive keypoint metadata that older released
  checkpoints may lack. Oracle admission must distinguish harmless inactive
  state from genuinely unsupported keypoint behavior. Native detection and
  segmentation capabilities should remain unchanged.
- No complete upstream/native forward or VJP was executed at production
  shapes, and no achieved error tolerance or pass result is asserted. CUDA
  sampler/backward and objective scaling conclusions belong to the other
  reviewers; this report supplies model-side boundaries and cross-owner
  findings.

Only this report was edited. No implementation, upstream files, action plan,
build artifacts, tests, or commits were changed by this reviewer.
