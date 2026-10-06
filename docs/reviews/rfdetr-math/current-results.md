# RF-DETR numerical results and review closure

[Review index](README.md) · [Current training behavior](../../rfdetr-training.md) · [Reproduction and numerical policy](../../validation.md#independent-upstream-diagnostic)

Implementation through `90e82d54` repairs the reviewed native CUDA, tracing,
model admission, initialization, and objective defects while preserving None,
Selective, FullTrace, custom CUDA attention, detection/segmentation,
Hungarian/Match-Free/DN, optimizer recipes, logical lanes, distributed SUM,
Resume/Transfer, ONNX, and TensorRT. FullTrace retains its explicit Match-Free
and DN restrictions. The [baseline reports](README.md) remain historical.

Required matched-state checks passed against independently executed upstream
RF-DETR. Independent training trajectories still differ materially. These
results establish neither AP causality for the reported `run-0025` nor a
convergence, throughput, or accuracy improvement.

## Provenance and retained evidence

The generated evidence root is
[`build/validation/rfdetr-math`](../../../build/validation/rfdetr-math).
Its [execution ledger](../../../build/validation/rfdetr-math/execution-status.md)
is chronological: later successful repairs and completion entries supersede
earlier pending stages and failed attempts. Generated artifacts are local
retained evidence, not checked-in fixtures.

| Identity | Completed evidence |
| --- | --- |
| Upstream | Clean, read-only `../rf-upstream` at `e9a138f70cc14e3fddca029a0683055e34ab8f35` |
| Payload / device | Pinned NGC 26.08; Torch `2.14.0a0+4fdf77b940.nv26.08`; CUDA 13.4; NVIDIA GeForce RTX 4070 SUPER |
| Final packaged build | `build-export-silu-1.log`; runtime `sha256:c8cf3f381a2f12957804b5c143657b3eae709f8c14b29b5160515de58e9f9570` |
| Fresh public command | `./mmltk --diagnose-rfdetr-upstream --upstream ../rf-upstream --output build/validation/rfdetr-math/upstream-final` — exit 0 |
| Verdict | [`upstream-final/verdict.json`](../../../build/validation/rfdetr-math/upstream-final/verdict.json), `passed_required_checks` |
| Manifest SHA-256 | `d11f5e2d15cbc45cb7e11d0e5d7f76842e569e4f1b1cc28b5fbd4129ca304776` |

The [producer manifest](../../../build/validation/rfdetr-math/upstream-final/manifest.json)
retains full model/precision policies, original official-weight hashes,
source/dependency/payload/producer hashes, and tensor inventories. Validation
dependencies live in a private overlay installed with `--no-deps`; donor
Torch/CUDA are preserved. The [build reference](../../build.md#upstream-reference-image)
owns that environment and the [validation guide](../../validation.md#independent-upstream-diagnostic)
owns command admission and artifact semantics.

## Review closure

| Baseline finding | Current disposition |
| --- | --- |
| CUDA-01, matcher finite precision | Native FP32 matching remains explicit policy. Dense and compact paths agree on it. Actual untouched upstream AMP matching is retained as a separate policy observation. |
| CUDA-02 through CUDA-04 | Attention retains the chosen lower-support derivative, propagates nonfinite multiplication outside support, and admits partial batch chunks. Independent CUDA-layer cases passed. |
| C1/C2, opaque full-trace work | Registered dispatcher/autograd operators preserve custom attention and depthwise work/VJPs in recording and replay. Depthwise retains original operands and requested higher-order derivatives. |
| C3/C4, specialization/export | Shared guarded traces record the first compatible real invocation, retain live state, and replace candidates transactionally. Export uses an independent inference snapshot and local attention selection. |
| C5/FWD-05, evidence gaps | Actual upstream production and native model/criterion/optimizer comparisons supplement native round-trip and eager/compiled tests. |
| FWD-01, model admission | External envelopes normalize once; required-core admission rejects incomplete/incompatible production state before publication. Explicit partial transfer and semantic class adaptation remain available. |
| FWD-02/FWD-03 | Fresh initialization follows the admitted upstream construction invariants; `aux_loss=false` omits intermediate ordinary decoder outputs and objective terms. |
| FWD-04 | Projector uses native ATen SiLU; ONNX lowering supplies Sigmoid/Mul. |
| Training IA-BCE and scale growth | Destination-specific quality casts preserve the negative-weight precision; FP16 scale growth rejects an infinite candidate while retaining tracker/backoff semantics. |
| Normalization and Muon observations | World-size-independent native target normalization and established Muon routing/persisted groups remain policy, with their upstream differences documented. |

Runtime validation also isolated repairs beyond those source findings: ATen
linear and grouped/batched operation ordering, deformable sampling conversion
and short-reduction order, sparse-mask FP32 bias promotion, recorded depthwise
precision, the pinned low-precision JIT executor, ambient-autocast exclusion on
replay, and local math-SDPA precision. The
[training reference](../../rfdetr-training.md#native-selective-compilation)
describes the resulting ownership rather than the sequence of failed attempts.

## Matched-state numerical results

The stock manifest contains 24 cases: Nano 384 / SegNano 312 × FP32 / FP16 /
BF16 × batch one / two × train / eval, with two Group-DETR groups for this
bounded matrix. It contains 31 step artifacts: eight Nano FP32 batch-two
training steps and one for each other case. Training covers 12 configurations
and 19 matched updates. This does not cover every catalog model or recipe.

All **312 saved forward tensors have zero numerical difference** in the fresh
controlled comparison with default SDPA backend selection, TF32 disabled, and
matmul precision `highest`, as recorded by the manifest. This is the numerical
max/L2 error result, not a separate byte-equality or signed-zero proof or a claim
about every production precision setting. The earlier ledger's shorthand
“bitwise” is qualified by its final entry.

| Precision | Maximum complete-VJP relative L2 difference | Required relative bound |
| --- | ---: | ---: |
| FP32 | `2.31821448684e-6` | `0.002` |
| FP16 | `0.00537322214884` | `0.015` |
| BF16 | `0.0120553896178` | `0.03` |

All 24 complete-vector comparisons pass both maximum-error and L2 gates with
absolute tolerance `1e-7`. The retained projections are
[`core-forward-final.jsonl`](../../../build/validation/rfdetr-math/core-forward-final.jsonl)
and [`core-complete-vjp-final.jsonl`](../../../build/validation/rfdetr-math/core-complete-vjp-final.jsonl).
Original per-tensor errors remain visible, including near-null key-bias
relative-bound failures. Independent upstream-versus-upstream complete-VJP
maxima were `2.01185716e-6`, `0.00571678650`, and `0.00823595853` respectively;
the native errors are not claimed identical to this measured variation.

At each matched training step, model/criterion/held-input comparisons use saved
upstream weights. Production native AdamW consumes identical upstream clipped
gradients and retains continuous moments/ages. Full parameters, moments, ages,
and exact matched-state discrete identities pass. Update-delta checks report
raw errors and explicitly allow one FP32 parameter interval before the existing
bounds, as specified in the
[numerical policy](../../validation.md#numerical-gates-and-trajectory-observations).
This separates optimizer arithmetic from differing backward accumulation.

The explicit math-SDPA artifacts in `upstream-math` also passed all 24 core
cases and 12 training configurations / 19 updates in
`test-core-math-final-1.log` and `test-training-math-final-1.log`. Default
accelerated backend selection remains available and independently exercised.

## Measured trajectories

Two independent actual upstream generations failed the original strict
eight-update comparison with 2,919 per-tensor discrepancies, including 39
encoder/assignment identity differences. The repeat's held-loss maximum
relative difference was **9.87%** (`0.0987199322979`); its final step was
approximately 5.02%. These observations demonstrate that seeded independent
training is not a strict equality oracle for this execution policy.

The fresh independent **native/upstream** eight-step Nano FP32 trajectory had a
larger maximum held-loss relative difference of **19.88%**
(`0.198786954699`). The single-update AMP configurations reached **11.47%**
(`0.114710746789`). These material differences remain observations, not equality
acceptance gates or proof that all native drift is upstream noise. No AP
experiment, production-dataset rebuild, or long convergence run was performed.

Retained projections:
[`trajectory-held-loss-final.jsonl`](../../../build/validation/rfdetr-math/trajectory-held-loss-final.jsonl),
[`upstream-repeat-held-loss.jsonl`](../../../build/validation/rfdetr-math/upstream-repeat-held-loss.jsonl),
and the upstream repeat comparisons under `upstream-operators/comparisons`.
Measurement mode still requires valid inventories, shapes, dtypes, definedness,
and finite tensors. Stock AMP matcher differences are separately labeled policy
observations, while main AMP reference matching preserves native FP32 policy.

## Representative Match-Free compilation

Match-Free is a native extension, so this is eager/Selective consistency rather
than upstream-objective parity. Official pretrained Nano 384, frozen encoder,
13 groups, FP16/BF16, production `TrainingStep`, and fused AdamW passed forward,
objective, and complete-gradient/clipping checks over three invocations
exercising recording and retained replay before one optimizer update.
Independent eager repeats and optimizer updates remain measured separately.

| Precision | Selective/eager maximum complete-gradient relative L2 | Independent eager-repeat maximum |
| --- | ---: | ---: |
| FP16 | `0.0000745736653373` | `0.0000763521937724` |
| BF16 | `0.000439338657584` | `0.000452732076192` |

[`matchfree-complete-gradients-final.jsonl`](../../../build/validation/rfdetr-math/matchfree-complete-gradients-final.jsonl)
retains these default-backend results. The explicit math-SDPA representative
case also passed after recorded replay stopped re-entering ambient autocast.
This does not reproduce `run-0025`'s complete data/recipe trajectory or identify
the cause of its AP.

## Resize and rendered controls

Five independently produced resize inputs cover identity, enlargement,
shrinking, odd aspect, and high-frequency content. The native Torch methods
pass the declared float-tensor comparisons; saved upstream PIL results retain
the different byte-quantization pipeline. Existing Native/perceptual behavior,
compiler geometry/labels, and trusted compiled metadata remain covered.
The [dataset reference](../../datasets.md#rgb-resampling-methods) owns the choices.

Resampling, data, CLI, settings, dataset-system, and Rust Dataset/Predict
selections passed. Rendered Wayland acceptance used exactly
`workspace_wayland_retained,workspace_wayland_workflows`, with 140,036 passing
assertions in two cases, recorded in `test-wayland-final-1.log`. It exercised
the new selectors, settled settings, compilation/training fixtures, export,
retained state, and browser recovery. This was not the complete Wayland suite.

## Build, wrapper, and acceptance limits

The required full tidy/build/cleanup/tidy sequence completed at `d4c34db6`
(`pre cleanup`) and `6ce95968` (`post cleanup`), followed by full product builds
for evidence-driven runtime fixes. The final focused gates retained earlier
passes and retried affected cases; no `--test all` or complete Wayland run was
claimed. The full test suites were waived for this task. No long quality run or
production dataset recompilation was performed.

Focused acceptance includes CUDA layers (13,519 assertions / seven cases),
the full initial core selection plus affected final matrices, selected training
cases, depthwise higher-order coverage, export (221 / 41 plus the SiLU case),
all inference cases through retained passes and two successful export retries,
resampling (614,074 / 49), data (12,273 / 11), CLI (587 / 29), settings
(2,075 / 51), dataset systems (639 / 11), and Rust Dataset/Predict (10 / two).
One optional saved-personal-preference mutation skipped; earlier core skips
included unavailable GDR hardware and optional absent oracle artifacts.
Later supplied-oracle passes establish the stated independent coverage.

[`release-compile-commands.final.json`](../../../build/validation/rfdetr-math/release-compile-commands.final.json)
retains seven critical first-party commands without unsafe fast-math, FTZ,
imprecise-division, or imprecise-square-root overrides. Installed-payload
`jit-nvrtc-options-4.log` observed three FP32 JIT compilations without such flags;
the bounded BF16 simple-executor probe had zero numerical error and no NVRTC
compilation. Normal FMA remains enabled. These observations do not certify all
libraries, JIT routes, or deployment backends; the
[build reference](../../build.md#numerical-compilation-policy) distinguishes them.

Wrapper checks covered invalid/mismatched/dirty source, dependency and artifact
failures, read-only Git admission, linked worktrees, differently owned sources,
and successful publication. Producer and child-comparison cancellation each
returned 143 in under one second without a verdict and preserved an unrelated
owner sentinel. `oracle-cancel-comparison-final.log` is the final child-stage
record. Final process inspection found no running wrapper or GUI containers;
no idle GUI needed refreshing. No multi-GPU, GDR, untested model/backend, or
dataset-accuracy claim follows from this bounded acceptance.
