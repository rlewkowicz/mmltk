# RF-DETR mathematics review: compilation, tracing, and export

Historical baseline review; line anchors and findings refer to the revisions
below. See the [review index](README.md) and [current closure/evidence](current-results.md).

Review date: 2026-10-05. Native baseline:
`52586e98f535205cc242dfe74ef5144eef0373bb`. Reference:
`../rf-upstream`, clean at `e9a138f70cc14e3fddca029a0683055e34ab8f35`.
This is a source and existing-evidence review, augmented by one explicitly
attributed standalone diagnostic executed by the main agent. No builds,
tests, standalone diagnostics, or model runs were performed by this reviewer.
There were no product implementation changes in this review.

## Assessment

Selective compilation has a defensible mathematical boundary: trace ordinary
ATen tensor regions, keep the opaque CUDA attention and custom depthwise
backward outside those regions, bind live parameter tensors, and guard reuse.
I found no source proof that this mechanism changes the objective or drops
gradients on the audited selective path. That is a bounded conclusion, not a
claim of complete equivalence or convergence evidence.

Full-model tracing crosses those same opaque operations. The raw CUDA
attention launch has no traced operator representation, and the C++ custom
autograd class does not automatically supply one. Full tracing also does not
preserve the segmentation depthwise custom backward. These are substantial
defects in a selectable capability, separately from several missing guards.
Repair the operator and capture contracts while retaining all three modes.

The investigated `run-0025` used **Selective**, AMP, a frozen encoder,
Match-Free assignment, no denoising, resolution 384, and batches of 18,
according to the main reviewer's log-tool inspection of its run record.
The full-trace defects below do **not** establish a cause of that run's
recorded validation mAP of 0.088479 at epoch 13. The production CUDA selective
path needs its own matched-input evidence.

## Authorities, version boundary, and scope

Reviewed repository authorities: [CONTRACT.md](../../../CONTRACT.md),
[AGENTS.md](../../../AGENTS.md), [training](../../rfdetr-training.md),
[build](../../build.md), [validation](../../validation.md#training-mathematics-and-compilation-evidence),
and [compilation logging](../../logging.md#native-selective-compilation).
[The payload manifest](../../../docker/nvidia-payload.json) pins NGC 26.08,
Torch `2.14.0a0+4fdf77b940` with NVIDIA distribution suffix
`.nv26.8.63802676`, CUDA 13.4.1/NVCC 13.4.59, and TensorRT 11.2.1.2.
The NGC donor digest is
`sha256:33ef5fc15e8937602d64022209cdb2777b32dadf742f41332023d946041b3c14`.

Online documentation was inspected on the review date. Public PyTorch source
at **4fdf77b940** was inspected for the tracer, C++ custom autograd, graph
executor, autocast pass, CUDA graph interface, and AOTI package loader.
That is substantially stronger than applying an old stable-version example to
this alpha, but is not binary verification of all NVIDIA patches. Current
2.14 documentation describes supported directions; the main agent's bounded
diagnostic below verifies the decisive mechanism against the installed
payload without executing the product model.

Low-level interpolation derivatives, CUDA kernels, and effective compiler
math flags belong to [the CUDA review](01-cuda-ops.md). Model equations,
initialization, and optimizer/loss arithmetic belong to the other review
reports. Here, equivalence means preserving the forward function, its chosen
vector-Jacobian product, live state, RNG consumption, admitted input domain,
precision policy, and observable failures across execution mechanisms.

## Actual mode and caller map

`CompilationMode` is the canonical enum in
[model_config.h:16](../../../src/backend/models/rfdetr/contract/model_config.h#L16).
The CLI exposes it for Train, Predict, and Evaluate in
[rfdetr_cli_options.h:140](../../../src/entrypoints/cli/rfdetr_cli_options.h#L140),
156, and 263. Defaults are Selective in
[workflow_requests.h:39](../../../src/backend/models/rfdetr/contract/workflow_requests.h#L39)
and 202.

| Mode | What executes | Important limits |
| --- | --- | --- |
| None | Native C++ module orchestration, ATen operators, registered module state, and custom C++ autograd/CUDA kernels. | Already compiled C++ code; it does not mean interpreted Python. |
| Selective | The same native orchestration with lane-owned TorchScript traces for the backbone, decoder tensor tails, segmentation tensor tails/query transforms, and selected loss contractions. | Explicit signatures; fixed geometry regions and dynamic batch/query tensor regions have different rules. Custom attention/depthwise backward remain outside the traces. |
| FullTrace | A TorchScript graph recorded from one synthetic whole-model forward, with train/eval graph slots and flat output dictionaries. | Not Dynamo, Inductor, a compiled training step, or a sound capture of arbitrary C++ control flow/custom code. Current defects are detailed below. |

Production full-trace reachability is concrete:

- Training prepares the master in
  [training_model.cpp:62–71](../../../src/backend/models/rfdetr/training/training_model.cpp#L62),
  and each forward lane in
  [training_lanes.cpp:207–230](../../../src/backend/models/rfdetr/training/training_lanes.cpp#L207).
  Epoch activation invalidates/reprepares the master and lanes at
  `training_model.cpp:224–237` and `training_lanes.cpp:261–274`.
  [workflow_requests.cpp:59](../../../src/backend/models/rfdetr/contract/workflow_requests.cpp#L59)
  rejects FullTrace with Match-Free or denoising, **not** ordinary Hungarian
  training. Model-level Match-Free/denoising guards also throw at
  `core/model.cpp:1005–1011,1058`. These explicit unsupported combinations
  must remain explicit unless actually implemented.
- Native Predict loads weights, moves to CUDA, freezes inference weights,
  and prepares the selected mode in
  [predict.cpp:175–200](../../../src/backend/models/rfdetr/inference/predict.cpp#L175).
  Additional lanes clone and prepare it at 235–251. Evaluate copies the
  inference execution configuration into a Predict request and uses that
  session in [validate.cpp:51–75](../../../src/backend/models/rfdetr/inference/validate.cpp#L51).
  Neither route rejects FullTrace.
- During training validation, immutable snapshots and extra lanes inherit
  the mode through `make_inference_clone` in
  [evaluation_run_owner.cpp:555–575](../../../src/backend/models/rfdetr/training/evaluation_run_owner.cpp#L555).
  Candidate selection and final evaluation also explicitly prepare the
  requested mode in [train.cpp:756–773](../../../src/backend/models/rfdetr/training/train.cpp#L756).
- These native CUDA callers leave `force_pytorch_deformable_attn_` false.
  Thus [model.cpp:556–581](../../../src/backend/models/rfdetr/core/model.cpp#L556)
  selects the custom CUDA attention path. CPU execution or an explicit
  `set_force_pytorch_deformable_attn(true)` selects the ATen reference path.
  The public setter is used by export, clones copying an existing policy,
  and tests; it is not a normal production training precision option.
- Normal ONNX export creates a **fresh, uncompiled** model in
  [model_export.cpp:238–262](../../../src/backend/models/rfdetr/export/model_export.cpp#L238),
  and temporarily forces reference deformable attention before a separate
  export trace. ONNX/TensorRT artifact inference does not use the native
  compilation-mode branches (`predict.cpp:204–223`). The export defects below
  have a different scope from FullTrace.

Upstream likewise has more than one mechanism. Its training hook uses opt-in
CUDA `torch.compile(self.model, dynamic=True, ...)`, enables Dynamo
`suppress_errors` and `capture_scalar_outputs`, and may disable Inductor's
`coalesce_tiling_analysis` in
[module_model.py:415–469](https://github.com/roboflow/rf-detr/blob/e9a138f70cc14e3fddca029a0683055e34ab8f35/src/rfdetr/training/module_model.py#L415).
It compiles the model, not necessarily the whole optimizer/loss workflow,
and permitted graph breaks/fallbacks still matter. Upstream inference offers
**both** Python `torch.jit.trace` and `torch.compile(mode="reduce-overhead")`,
with dtype/batch/resolution preparation and exception cleanup in
[detr.py:1397–1477](https://github.com/roboflow/rf-detr/blob/e9a138f70cc14e3fddca029a0683055e34ab8f35/src/rfdetr/detr.py#L1397).
Calling native tracing “upstream torch.compile in C++” would be inaccurate.

## Selective semantics and lifetime audit

[selective_compilation.cpp:18–209](../../../src/backend/models/rfdetr/core/selective_compilation.cpp#L18)
establishes the following useful guarantees:

- A recorded region registers the actual owner parameter/buffer tensors as
  module attributes. It does not copy training weights into constants.
  Optimizer updates and `copy_` state loading therefore remain visible.
- Train/eval slots are distinct. Inputs guard device including ordinal,
  dtype, rank, extents, `requires_grad`, ambient GradMode, and effective
  autocast enablement/dtype. Fixed regions admit their prepared batch;
  mismatches execute the ordinary region without replacing the cached graph.
  Dynamic query regions relax only batch/query extents of compatible rank-3
  inputs with matching extents/device; hidden width remains guarded.
- Recording uses the **first real invocation** and returns its actual
  outputs. It does not execute a dummy/replay pair that would consume an
  extra stochastic forward or discard its autograd graph. The autocast weight
  cache is disabled during recording so converted live weights are recorded
  as operations rather than accidentally captured cached tensors.
- Replacement is transactional: the candidate graph is published after
  successful recording. Failure restores the previous admitted preparation
  if available and propagates the exception; it does not silently retry
  potentially failed GPU work in eager mode.

The traced regions are mathematically ordinary tensor functions. For example,
the decoder tail is
`z = LN(residual + cross_attention); y = LN(z + W2 ReLU(W1 z))` in
[model.cpp:644–658](../../../src/backend/models/rfdetr/core/model.cpp#L644).
Grouped self-attention and deformable attention remain outside that tail.
Its permanently zero dropout is deliberately omitted: the implementation
documents unwanted RNG consumption from JIT's optimized `native_dropout`
backward at `p=0`. No nonzero dropout behavior is thereby removed from this
configured model.

The segmentation trace takes both the residual and the already-computed
depthwise convolution result, then performs normalization, pointwise
projections, GELU, learned scaling, and residual addition. The custom
depthwise VJP is consequently attached to an **input** of the trace and
remains traversable by autograd. Query projection is a separate dynamic
tensor region. See [modules.cpp:254–339](../../../src/backend/models/rfdetr/core/modules.cpp#L254).
Target-dependent matching, denoising generation, sampling, and sparse/dense
mask representation decisions are not converted into one captured C++ branch.

The [loss cache](../../../src/backend/models/rfdetr/core/detail/traced_loss_cache.h#L15)
has a different, narrower contract: it traces pure parameter-free tensor
functions on detached contiguous operands, then always replays on the real
operands. For such ATen functions, detached **example inputs** do not detach
the live arguments' gradients. Signatures contain device type, dtype, rank,
coefficients, and CUDA autocast policy, but not extents or GradMode. Audited
focal/varifocal/position/IA-BCE and mask contractions have no hidden
data-dependent C++ decisions beyond guarded coefficients. Normalization by
target count and point count stays outside these graphs in
[detection_ops.cpp:118–156](../../../src/backend/models/rfdetr/core/detection_ops.cpp#L118)
and [training_mask_loss.cpp:119–154](../../../src/backend/models/rfdetr/core/training_mask_loss.cpp#L119).
Do not generalize this cache to arbitrary stateful, stochastic, or
shape-specialized functions without changing its contract.

State ownership also supports the intended semantics:

- `commit_normalized_state` (`model.cpp:1370–1381`) copies in place with
  rollback. Inference clone rebinding occurs **before** compilation
  (`model.cpp:1520–1578`). `freeze_inference_weights` at 1506–1518 sets
  `requires_grad(false)` and retains immutable tensor storage; it is not
  `torch::jit::freeze` and does not freeze training parameters into constants.
- A clone of mutable training state owns a detached snapshot; a clone of
  frozen state may share immutable storage. Each owns its graph/cache state.
  Producer-stream settlement precedes releasing snapshot sources.
- Epoch unfreezing invalidates traces after changing parameter gradient
  policy. Lane reconfiguration drains work before invalidation/rebinding.
  Destruction/retirement is coordinated with stream completion, rather than
  treating trace identity as a GPU completion event.
- The native model already has fixed feature geometry/device/dtype ownership
  in `model.cpp:518–541,926–933`; changing spatial geometry intentionally
  throws. Dynamic query-tail support is not a claim of whole-model
  multiscale support like upstream's dynamic compile route.

## Findings requiring correction or focused evidence

### C1 — High: FullTrace cannot represent the custom CUDA attention operation

**Trigger:** any admitted native CUDA FullTrace model with ordinary fused
deformable attention, including ordinary Hungarian training and native
Predict/Evaluate. This is not the Selective path.

**Source proof:** the model records its entire `forward` at
[model.cpp:1427–1463](../../../src/backend/models/rfdetr/core/model.cpp#L1427).
The attention C++ autograd function calls a native wrapper and casts its
result at
[ms_deform_attn_autograd.cpp:26–38](../../../src/backend/ml/layers/ms_deform_attn_autograd.cpp#L26).
That wrapper creates `at::empty`, launches a CUDA kernel using raw tensor
pointers, and returns a view at
[ms_deform_attn_cuda_wrapper.cpp:118–124](../../../src/backend/ml/layers/ms_deform_attn_cuda_wrapper.cpp#L118).
There is no dispatcher operator registration around this operation.

At the pinned PyTorch commit, C++ `Function::apply` calls the custom forward
under disabled GradMode and installs an autograd node, but contains no JIT
custom-op recording; its source explicitly says “TODO Add tracing here.”
The `is_traceable` field concerns compiled-autograd backward support, not
automatic recording of arbitrary CUDA launches.
[Pinned custom_function.h:432–503](https://github.com/pytorch/pytorch/blob/4fdf77b940/torch/csrc/autograd/custom_function.h#L432).

Consequently the tracer can see allocation, casts, and views but has no
instruction that evaluates the attention sum
`sum_(level,point) attention_weight * bilinear_sample(value, location)`.
A replay cannot be the original function merely because the first recorded
forward was correct. Missing computation and missing custom derivative are
source-established. The main agent's installed-payload diagnostic below also
observed the missing raw computation and custom VJP in a minimal C++ Function.
It did not execute native attention: the precise production symptom—
uninitialized replay values, loss of gradients, or an earlier JIT failure—
remains unmeasured. Do not describe a particular bad mAP or crash as observed
evidence.

**Minimal correction:** put the operation behind a real dispatcher schema
and backend registration, preserve its chosen CUDA/reference forward and
custom autograd/AMP behavior, and call the operator through the dispatcher
inside whole-model capture. A schema plus only a CUDA kernel is insufficient
for correct training. Confirm graph operator presence, forward replay,
VJPs, and a parameter update in focused acceptance evidence for the repaired
full mode; do not run an extra optimizer step as a production capture check.

### C2 — High: FullTrace discards the segmentation depthwise custom VJP

**Trigger:** FullTrace segmentation training, including a model whose
deformable attention is explicitly forced to the reference implementation.

[segmentation_depthwise.cpp:12–42](../../../src/backend/models/rfdetr/core/segmentation_depthwise.cpp#L12)
uses a C++ `Function` whose forward calls `at::conv2d`, while backward
disables autocast, converts incoming gradient and input to the original
weight dtype, and invokes `convolution_backward`, plus bias reduction.
Whole-model tracing observes the ATen convolution but does not encode that
C++ backward contract. Graph replay differentiates its recorded operators;
it does not rediscover the custom C++ Function. This follows from the same
pinned `Function::apply` implementation cited for C1.

The real convolution formula can agree while AMP gradients use different
operands/rounding from the native custom VJP. The magnitude has not been
measured here. Selective avoids this boundary intentionally
(`modules.cpp:316–323`). Registering this operation with an autograd dispatch
implementation is the corresponding full-model fix; preserve upstream's
depthwise backward policy rather than accepting an ordinary convolution VJP
only because a forward comparison passes.

### C3 — High: FullTrace has no application guard for its specialized capture

**Trigger:** reuse under a different preparation batch or captured host
policy, and potentially changes in input dtype/AMP/gradient policy.

[model.cpp:1391–1405](../../../src/backend/models/rfdetr/core/model.cpp#L1391)
returns immediately when its train/eval compiled boolean is set, even if a
new batch was requested. The graph records a synthetic batch in parameter
dtype, while native C++ host size queries enter views/expands at
`model.cpp:286–306,326–336,444–465`. These are concrete scalar values during
C++ tracing. Replay at `1059–1110` goes directly to `module.forward` before
the ordinary native geometry checks. Changing the force-reference attention
flag at `1470–1474` does not invalidate either full graph.

Preparation happens before the real AMP scope in training and before native
inference precision selection at `predict.cpp:193–200`. FullTrace records no
autocast/GradMode/parameter-policy signature and performs no parity check.
This does **not** prove that every subsequent AMP operation ignores autocast:
the pinned JIT has an
[autocast pass and ambient dispatcher support](https://github.com/pytorch/pytorch/blob/4fdf77b940/torch/csrc/jit/passes/autocast.cpp#L213).
Those facilities cannot reconstruct erased C++ decisions or missing custom
operators. Their exact numerical behavior is an evidence case, not an
assumed explanation for poor training.

**Minimal correction:** prepare full mode lazily on its first admitted real
invocation under the actual execution policy, with a transactional candidate
and an explicit signature. Guard batch/geometry, device, dtype, train/eval,
GradMode, effective autocast, parameter gradient policy, and any host policy
that changes graph structure. Preserve existing fixed-geometry errors and
tail behavior. A changed request must select a compatible graph, rebuild
safely, or fail explicitly; it must not run a stale specialization. Keep the
full output schema equivalent, including `include_masks` handling of sparse
outputs currently unconditionally unpacked at 1073 and 1085.

### C4 — Medium: export's state changes are not isolated from other owners

[model_export.cpp:42–74](../../../src/backend/models/rfdetr/export/model_export.cpp#L42)
temporarily changes **process-global** SDPA backend flags. Its mutex
serializes exporters only; model forward paths do not acquire it. Concurrent
training/inference can therefore observe math-SDPA selection while export
runs and a different kernel after restoration. The real attention formula
is unchanged, but accumulation, precision, performance, and reproducibility
can change. Simultaneous production execution was not reproduced here.

The public model-taking export overload at 86–161 additionally changes the
caller's train/eval mode, restores force-reference to `false` rather than the
previous policy, and does not neutralize existing graph state. An armed
selective region may try to start nested tracing, which the
[pinned tracer explicitly rejects](https://github.com/pytorch/pytorch/blob/4fdf77b940/torch/csrc/jit/frontend/tracer.cpp#L443).
A previously compiled full graph bypasses the new reference-attention flag.
The normal session's fresh uncompiled export model avoids those cached-model
cases; this is not evidence that every normal ONNX export fails.

**Minimal correction:** export from a dedicated snapshot with explicit export
semantics and operation-local reference attention/SDPA, preserving the
caller's state and concurrent lanes. Keep class-layout admission, cancellation,
GPU retirement, and atomic artifact publication. Do not solve this by
globally disabling optimized attention or removing model-taking export.

### C5 — Medium evidence gap: tests do not establish the advertised full path

The raw C++ tracer API's two `false` arguments mean `strict=false` and
`force_outplace=false`; they are **not** Python's `check_trace` option.
Its implementation runs the callback once. Neither native trace owner
implements an independent automatic comparison.
[Pinned tracer declaration](https://github.com/pytorch/pytorch/blob/4fdf77b940/torch/csrc/jit/frontend/tracer.h#L202),
[implementation](https://github.com/pytorch/pytorch/blob/4fdf77b940/torch/csrc/jit/frontend/tracer.cpp#L433).
Python `torch.jit.trace` has separate checking facilities, with limitations
for mode-dependent behavior and control flow; checking a forward alone also
does not prove VJP equivalence.
[Official archived trace API documentation](https://docs.pytorch.org/docs/2.9/generated/torch.jit.trace.html).

Existing source coverage is useful but bounded:

| Existing evidence source | What it exercises | Material exclusion |
| --- | --- | --- |
| `core/tests/selective_compilation.test.cpp:71–195` | First-real-forward RNG behavior, live weight mutation, preparation changes/failure preservation, train/eval slots, short batches, dynamic queries, mixed dtypes, outstanding backwards. | Not a full production graph. |
| `training/tests/training_supervision.test.cpp:2332–2517` | Composed eager/selective model, four accumulated microbatches, segmentation/Match-Free/denoising, FP32/FP16/BF16, losses, VJPs, optimizer update, live state, eval/tails. | **Both models force reference deformable attention at 2362**; tiny 64-pixel model, batch 2, three queries; hardware skips permitted. |
| `core/tests/upstream_training_parity.test.cpp:536` and loss fixtures | Loss-cache policy changes with outstanding backward and local equation parity. | No end-to-end compiled production CUDA training trajectory. |
| FullTrace searches | Contract rejection of unsupported supervision at `contract/tests/training_supervision.test.cpp:108`. | No full-model eager/full forward, VJP, or update equivalence case located. |
| `inference/tests/prediction_session.test.cpp:1980–2110` | Small native/ONNX export flow; direct CPU ORT versus CUDA-native tensors at 2044–2069, plus GPU evaluation metrics. | No equivalent broad TensorRT tensor oracle; equality of a small AP fixture does not bound logits/masks or decision-boundary drift. |
| `export/tests/onnx_lowering.test.cpp:266–300` | Numerical antialiased bicubic resize against ORT, alongside graph-structure cases. | No direct numerical SDPA-lowering case located. |

The validation reference records tolerances rather than bitwise identity:
roughly 0.004 absolute/relative for principal composed outputs/losses,
0.02 for BF16 spatial features, and separate gradient/update tolerances.
Its historical `compilation-execution-evidence-attempt7.jsonl` description
reports 1,163 executor-reuse records: 26 evaluation backbone records with
33 TensorExpr groups each, and no counted groups in the remaining 1,137
records, including training. This is **documentation-reported historical
evidence**, not a fresh query or rerun by this reviewer. It establishes no
general training fusion or throughput benefit. Reusing a TorchScript graph
and generating fused CUDA kernels are different claims.

## Export mathematics and precision differences

Export is deployment specialization: evaluation mode, static batch/resolution,
reference deformable attention, parameter/buffer initializers, and an empty
dynamic-axes map (`model_export.cpp:94–141`). These choices are not by
themselves mathematical defects or training-weight rebinding errors.
The lowerer rejects unsupported operators and validates graph structure;
the publication path reopens and verifies class metadata before publishing.
It does not run a numerical parity gate before publication.

The audited lowerings preserve these real-valued expressions:

- Grid sampling maps mode, padding mode, and `align_corners` explicitly
  (`onnx_lowering.cpp:1718–1736`).
- Supported SDPA becomes `softmax((Q K^T) * scale) V`, with default
  `scale = 1/sqrt(d)` (`1738–1783`). Nonzero dropout, masks, causality, and
  GQA fail explicitly. This is the correct real formula on its admitted
  domain; intermediate dtype, scaling order, fused accumulation, and
  rounding are not guaranteed identical to a selected PyTorch SDPA kernel.
- Antialiased bicubic resize supplies its half-pixel/antialias/exclusion and
  cubic coefficient attributes (`1682–1715`), with the numerical fixture
  noted above. Static shape lowering is intentional here.

Precision policy differs across engines even when all visible tensors are
FP32:

| Route | Source policy | Consequence for comparisons |
| --- | --- | --- |
| Native ATen | `core/runtime.cpp:138–142` disables cuBLAS/cuDNN TF32 and denormal flushing; enables JIT graph optimization. Native `allow_fp16` may choose BF16 on capable hardware (`predict.cpp:194–200`). | Record actual dtype/autocast/internal matmul policy, not only the request boolean. |
| ONNX Runtime CUDA | `backend/ml/runtime/backend_onnx.cpp:66–78` enables all graph optimizations and uses CUDA provider defaults without setting `use_tf32`. | Official CUDA EP default is `use_tf32=1`; a nominal FP32 comparison may use a different multiplication policy. |
| TensorRT | `tensorrt_runtime.h:38–39` permits FP16 and TF32; backend creation forwards the FP16 permission but leaves TF32 true. `tensorrt_runtime.cpp:333–334` clears TF32 only when explicitly false. | TensorRT 11 is strongly typed. The current runtime explicitly logs model-defined precision and that automatic FP16 lowering is not applied (`410–418`); the FP16 permission does not itself convert an FP32 graph. |
| Upstream training | `training/trainer.py:553` requests float32 matmul precision `high`. | Already differs from native's TF32-disabled reference, independently of trace capture. |

These are source-established policy differences, not proof of the observed
training-quality problem. The ORT default and V2 option used to control it
are documented in the
[CUDA execution provider reference](https://onnxruntime.ai/docs/execution-providers/CUDA-ExecutionProvider.html).
TensorRT's type and TF32 rules are documented in
[precision control](https://docs.nvidia.com/deeplearning/tensorrt/latest/inference-library/precision-control.html)
and [accuracy considerations](https://docs.nvidia.com/deeplearning/tensorrt/latest/inference-library/accuracy-considerations.html).
Preserve accelerated policies while adding an explicit common comparison
policy and truthful metadata. Do not promise bitwise identity or silently
change every engine's normal settings. NVCC/host unsafe-math flag inspection
is owned by the CUDA review.

## Realistic mechanisms and migration choices

The word “compile” covers different contracts. Modern PyTorch `torch.compile`
captures Python through Dynamo and uses AOTAutograd/Inductor for compiled
regions; it is not a callable compiler for arbitrary existing C++
`torch::nn::Module` code. Ordinary LibTorch ATen, modules, and autograd remain
the supported native building blocks.
[Compiler FAQ](https://docs.pytorch.org/docs/stable/user_guide/torch_compiler/torch.compiler_faq.html),
[C++ frontend documentation](https://docs.pytorch.org/cppdocs/).
The current [official compiler tutorial](https://docs.pytorch.org/tutorials/intermediate/torch_compile_tutorial.html#benefits-over-torchscript)
also identifies TorchScript tracing/scripting as deprecated and demonstrates
the control-flow difference; that lifecycle status does not justify dropping
an existing mode without preserving its required behavior.

| Approach | Mathematical/autograd and shape contract | Native feasibility and cost |
| --- | --- | --- |
| Preserve selective traces; repair full traces with dispatcher operators and guards | Correct on declared tensor regions with audited ATen/custom VJPs and explicit host specialization. Shape/control-flow changes need guards or graph variants. Backend rounding still needs tolerances. | **Recommended immediate correction.** Low-to-medium migration; retains None/Selective/FullTrace and current module/state/lane ownership. TorchScript is deprecated, so this is a repair of an existing pinned integration, not a claim that it is the preferred new general compiler. |
| Native ATen plus dispatcher-registered fused C++/CUDA operators | Ordinary eager autograd composes registered VJPs. Operators explicitly own shapes, AMP, aliasing, and mutation. Exact kernel arithmetic remains the implementer's responsibility. | Supported pure-C++ training foundation, illustrated by torchvision below. Useful whether the enclosing route is eager, traced, or exported. Do not duplicate the model or optimizer into handwritten fused kernels. |
| C++ CUDA graph capture/replay for stable tensor regions | Captures actual GPU operations, so raw kernel launches can be present. Forward and backward must be captured/integrated deliberately. Fixed addresses/shapes, RNG handling, stream dependencies, and gradient-buffer lifetimes are required; CPU decisions do not replay. | Viable later performance work; medium-to-high engineering cost. This reduces launch overhead, not an Inductor algebraic compiler. Dynamic matching/denoising/target policy remains outside, or requires explicit supported buckets. Preserve eager and current compiled capabilities during any deliberate cutover. |
| Python capture with `torch.export` plus AOTInductor C++ package execution | Export has explicit shape constraints and fails unsupported capture rather than silently pretending to cover arbitrary branches. AOTI packages provide native inference execution and optional input size/dtype/stride checks. | Strong current **inference deployment** alternative, requiring Python preparation and canonical state/model mapping. Medium-to-high migration. The C++ loader is not a drop-in replacement for this trainable native module/optimizer. Keep ONNX/TensorRT where their artifacts are required. |
| Python `torch.compile` for the training model | Dynamo guards, graph breaks, AOTAutograd, and dynamic-shape machinery support training, subject to supported operations and custom-op registration. It can still differ numerically and has backward/autocast assumptions. | Accurate way to request upstream's actual compiler stack, but requires a Python capture/execution component or a separately engineered explicit forward/backward artifact boundary. High architectural cost; no documented one-call pure-C++ native-model equivalent was identified. |
| TorchScript scripting/manual graph construction | Scripted control flow can be represented where tracing would specialize it; scripted operators can use autograd. | `torch::jit::compile` compiles TorchScript source, not existing C++ source. Rewriting this model in strings or parallel hand-built IR adds a second model definition and does not repair opaque operator registration. High maintenance cost; not recommended. |

Concrete primary implementations supporting this comparison:

1. **C++ dispatcher and custom autograd:** torchvision v0.24.0
   [deform_conv2d.cpp](https://github.com/pytorch/vision/blob/v0.24.0/torchvision/csrc/ops/deform_conv2d.cpp)
   defines a schema and calls a typed dispatcher handle;
   [autograd/deform_conv2d_kernel.cpp](https://github.com/pytorch/vision/blob/v0.24.0/torchvision/csrc/ops/autograd/deform_conv2d_kernel.cpp)
   registers an Autograd implementation wrapping a C++ Function and
   redispatches below autograd. This is the relevant pattern missing from
   native attention/depthwise boundaries. The current
   [official C++/CUDA custom-op guide](https://docs.pytorch.org/tutorials/advanced/cpp_custom_ops.html)
   additionally requires fake/meta behavior for compiler capture and
   appropriate autograd registration. Registration/opcheck is not a proof
   that the derivative equation is correct.
2. **Actual native CUDA graph API:** pinned
   [ATen CUDAGraph.h](https://github.com/pytorch/pytorch/blob/4fdf77b940/aten/src/ATen/cuda/CUDAGraph.h)
   exposes capture, replay, reset, graph pools, and generator handling.
   NVIDIA's pinned
   [CUDA 13.0 simpleCudaGraphs.cu sample](https://github.com/NVIDIA/cuda-samples/blob/v13.0/Samples/3_CUDA_Features/simpleCudaGraphs/simpleCudaGraphs.cu)
   demonstrates C++ construction/capture/replay, not model differentiation.
   [PyTorch CUDA graph constraints](https://docs.pytorch.org/docs/2.14/notes/cuda.html#cuda-graphs)
   and [NVIDIA graph semantics](https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/cuda-graphs.html)
   explain the static-address, synchronization, and CPU-work restrictions.
   Python `make_graphed_callables`' autograd integration is not automatically
   supplied by invoking the C++ `capture_begin` method.
3. **Actual native AOTI loader:** pinned
   [model_package_loader.h](https://github.com/pytorch/pytorch/blob/4fdf77b940/torch/csrc/inductor/aoti_package/model_package_loader.h)
   provides package execution and constant management. The
   [official AOTInductor tutorial](https://docs.pytorch.org/docs/2.14/user_guide/torch_compiler/torch.compiler_aot_inductor.html)
   exports/compiles in Python and executes in C++ under `c10::InferenceMode`;
   it documents `AOTI_RUNTIME_CHECK_INPUTS=1`. Constant updates do not imply
   a training autograd API.

`torch.export` itself is **not inference-only**: current documentation
explicitly describes Training IR usable with eager autograd, and inference
IR obtained through decompositions. That fact does not establish a supported
C++ AOTI training pipeline for this native module.
[Export IR documentation](https://docs.pytorch.org/docs/2.14/user_guide/torch_compiler/export.html#export-ir-training-vs-inference).
Likewise, choosing Dynamo is not automatically more numerically accurate:
its current documentation calls out compiled-backward autocast assumptions
that must agree with the execution scope.
[Compiled autograd semantics](https://docs.pytorch.org/docs/2.14/user_guide/torch_compiler/torch.compiler_backward.html).

## Bounded installed-payload diagnostic for C1

This probe is deliberately independent of the product model. A
custom Function computes `3*x` through raw host writes after an ATen
allocation and correctly defines its VJP as `3*g`. Zeros replace the
production kernel's uninitialized allocation so missing replay computation
is deterministic. CPU tensors suffice: the issue is JIT representation of a
C++ Function/raw operation, not CUDA interpolation. It allocates three floats
and replays three times; it does not require a dataset or product rebuild.

The main agent saved the following source and executed it through the wrapper:

```sh
./mmltk --diagnose-gpu-program --torch build/validation/rfdetr-math/trace-custom-function.cpp
```

[mmltk:2997–3083](../../../mmltk#L2997) supplies pinned Torch headers, ABI,
and libraries; it uses existing development/runtime images with separate
120-second compilation/execution limits and artifacts under
`build/diagnostics`. The main agent reported **exit 0** and these observations:

- Header version `torch=2.14.0`; runtime image
  `sha256:1fd71a53a17ac5d55ac16bb91c897d278938f92b5905b3f345b72caad1e8fa42`.
- The graph contained only the tensor operation `aten::zeros_like`, plus its
  constants; it contained no custom operation representing the raw writes.
- Initial recorded output was `[3, 6, 9]` and its VJP was `[3, 3, 3]`.
- All three graph replays returned `[0, 0, 0]` with `requires_grad=false`;
  `missing_custom_boundary_observed=1`.

Evidence locations supplied by the main agent:
[source](../../../build/validation/rfdetr-math/trace-custom-function.cpp),
[transcript](../../../build/validation/rfdetr-math/trace-custom-function.log),
and executable artifacts in `build/diagnostics/gpu-program.5QoEdX`.
The version string printed by the C++ header is reported verbatim; the
manifest's full NVIDIA distribution version remains the version authority.
Exit 0 means the expected missing-boundary mechanism was observed; exit 2
would mean it was not and would require inspecting the graph/results.

```cpp
#include <ATen/ATen.h>
#include <torch/csrc/autograd/autograd.h>
#include <torch/csrc/autograd/custom_function.h>
#include <torch/csrc/jit/frontend/tracer.h>
#include <torch/csrc/jit/runtime/graph_executor.h>
#include <torch/version.h>
#include <iostream>
#include <string>

class OpaqueTriple final : public torch::autograd::Function<OpaqueTriple> {
public:
  static at::Tensor forward(torch::autograd::AutogradContext*,
                            const at::Tensor& input) {
    TORCH_CHECK(input.device().is_cpu() && input.scalar_type() == at::kFloat
                && input.is_contiguous());
    auto output = at::zeros_like(input);
    const auto* src = input.data_ptr<float>();
    auto* dst = output.data_ptr<float>();
    for (int64_t i = 0; i < input.numel(); ++i) dst[i] = 3.0F * src[i];
    return output;
  }
  static torch::autograd::variable_list backward(
      torch::autograd::AutogradContext*,
      torch::autograd::variable_list gradients) {
    return {gradients.at(0) * 3.0};
  }
};

int main() {
  try {
    auto x = at::tensor({1.0F, 2.0F, 3.0F});
    x.set_requires_grad(true);
    auto trace = torch::jit::tracer::trace(
        {x}, [](torch::jit::Stack args) {
          return torch::jit::Stack{OpaqueTriple::apply(args.at(0).toTensor())};
        }, [](const torch::autograd::Variable&) { return std::string{}; },
        false, false);
    auto first = trace.second.at(0).toTensor();
    auto grad = torch::autograd::grad({first.sum()}, {x}).at(0);
    std::cout << "torch=" << TORCH_VERSION << '\n'
              << *trace.first->graph << '\n';
    bool observed = at::allclose(first, x.detach() * 3.0)
                 && at::allclose(grad, at::full_like(x, 3.0));
    std::cout << "first=" << first << " first_gradient=" << grad << '\n';
    torch::jit::GraphExecutor executor(trace.first->graph, "opaque_triple");
    for (int i = 0; i < 3; ++i) {
      torch::jit::Stack stack{x};
      executor.run(stack);
      auto replay = stack.at(0).toTensor();
      std::cout << "replay=" << i << " value=" << replay
                << " requires_grad=" << replay.requires_grad() << '\n';
      observed &= at::allclose(replay, at::zeros_like(x))
               && !replay.requires_grad();
    }
    std::cout << "missing_custom_boundary_observed=" << observed << '\n';
    return observed ? 0 : 2;
  } catch (const std::exception& error) {
    std::cerr << "custom-function trace diagnostic: " << error.what() << '\n';
    return 1;
  }
}
```

This is an actual installed-payload observation attributed to the main agent,
separate from this reviewer's source inference. It does not replace a
production full-model CUDA case, establish any magnitude of training drift,
or verify C2's AMP VJP.

## Concrete handoff for the implementation plan

1. Use the recorded bounded diagnostic as installed evidence for C1's
   missing-operator mechanism, then implement dispatcher-visible attention
   and depthwise operators with
   precise schemas, mutation/alias behavior, preserved custom derivatives,
   dtype/AMP handling, and explicit export reference behavior. Primary
   owners: `src/backend/ml/layers/ms_deform_attn_op.cpp`,
   `ms_deform_attn_autograd.cpp`, related declaration/registration and build
   wiring, and `src/backend/models/rfdetr/core/segmentation_depthwise.cpp`.
   Coordinate derivative corrections with the CUDA review; do not invent a
   second attention implementation merely for tracing.
2. Correct full-mode capture/reuse in `core/model.cpp` and its owned graph
   state, reusing selective signature/lifecycle concepts where they actually
   coincide. Preserve all mode selection, native parameter names/state,
   train/eval separation, batch tails, immutable inference clones, loss
   policies, Match-Free/denoising support restrictions, and explicit GPU
   failure propagation. Do not add a silent eager rescue after a failed
   CUDA launch or remove FullTrace to avoid repairing it.
3. Isolate export in `export/model_export.cpp` and operation-local attention
   owners; retain existing ONNX lowering/publication and model-taking API
   behavior without changing the caller's state. Add direct numerical
   evidence for lowerings and cross-engine policies before changing their
   equations. Keep native/ORT/TensorRT precision choices explicit in their
   existing execution configuration, not a parallel compiler-policy registry.
4. Extend existing owning suites with required behavior cases: first record,
   first replay, optimized replay, nontrivial perturbed inputs, independent
   input/parameter VJPs, and one update for CUDA FullTrace detection and
   segmentation; FP32/FP16/BF16 where supported; freeze/unfreeze, in-place
   state copy, train/eval, prepared batch changes and tail batches,
   signature mismatches, concurrent lanes, and failed replacement custody.
   Require graph presence of custom operations so a numerically accidental
   zero-input pass cannot hide missing work.
5. Separately establish the actual Selective run's route: production CUDA
   deformable attention, Nano 384 geometry, AMP/frozen encoder, Match-Free,
   representative target counts, accumulated microbatches, current loss
   coefficients, fixed stochastic samples, and a parameter update against
   None and the upstream reference. Cover unfreezing/Resume and encoder/EMA
   validation snapshots as distinct cases. The tiny forced-reference
   composed fixture remains valuable, but is not this evidence.
6. For deployment, compare logits, boxes, and masks directly for FP32-native,
   CPU ORT, CUDA ORT, and TensorRT under an explicitly matched arithmetic
   policy, then assess existing accelerated policies separately. Include
   large-magnitude SDPA operands, mask interpolation boundaries, and
   nontrivial score/ranking decisions; preserve current allowed tolerances
   unless independently justified. Do not substitute matching AP on an
   almost-empty fixture for tensor or VJP evidence.

Performance work should follow demonstrated correctness and an actual
profile: the current evidence does not show broad training fusion. Native
CUDA graphs and AOTInductor inference are realistic options for specific
boundaries, but neither is a reason to replace working native training with
an unsupported C++ compiler API or to drop an existing selectable capability.
