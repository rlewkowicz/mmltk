# RF-DETR Nano mAP discrepancy review

Reviewed 2026-09-18. Investigation only; no implementation changes or model runs.

## Assessment

There are confirmed differences capable of changing both predictions and COCO scores. The strongest inference defect is **missing ImageNet normalization in standalone validation of compiled datasets**. The compiled validation input is also letterboxed, while upstream Nano uses a square stretch. Separately, our compiled annotations and evaluator cannot reproduce the original COCO evaluation protocol: crowd information and original annotation areas are discarded, and boxes are rounded to integer coordinates.

The basic AP reduction looks mathematically sound for the non-crowd annotations it receives. The 80-class mapping also looks consistent. Neither conclusion establishes end-to-end parity: the inputs, ground truth, candidate limits, and some postprocessing behavior differ.

**The exact cause of the screenshot's 8.50-point AP gap is not experimentally established.** These findings concern the current source tree. The screenshot does not identify its model file, dataset, backend, precision, image count, or executable revision. In particular, the normalization finding applies to the standalone compiled-dataset Validate path; training-time evaluation uses a different preprocessor.

## Recorded result and upstream baseline

Source: [user-provided screenshot](</home/ryan/Pictures/Screenshots/Screenshot From 2026-09-18 13-36-28.png>). The displayed column is **Boxes**.

| Metric | Screenshot, fraction | Percentage |
|---|---:|---:|
| AP 50:95 | **0.3990** | **39.90** |
| AP50 | **0.5857** | **58.57** |
| AP75 | 0.4243 | 42.43 |
| AP small | 0.0965 | 9.65 |
| AP medium | 0.3437 | 34.37 |
| AP large | 0.6058 | 60.58 |
| AR @ 1 | 0.3296 | 32.96 |
| AR @ 10 | 0.5373 | 53.73 |
| AR @ 500 | 0.5950 | 59.50 |
| AR small | 0.2438 | 24.38 |
| AR medium | 0.5770 | 57.70 |
| AR large | 0.8106 | 81.06 |

The official checkout documents detection Nano at **384 × 384**, **48.4 AP50:95**, and **67.6 AP50**. Its methodology specifies the full **5,000-image COCO val2017** split, with accuracy evaluated through the Single Artifact Benchmarking harness. See [local benchmark documentation](../rf-upstream/docs/learn/benchmarks.md), lines 16–36, and the [published benchmark page](https://rfdetr.roboflow.com/learn/benchmarks/).

The numerical differences are **8.50 AP points** and **9.03 AP50 points**. The first is approximately a 17.6% relative reduction. This comparison is conditional on using the corresponding pretrained detection Nano and validation set. The segmentation Nano number is a different benchmark.

There is no apparent percentage or field-display error: [results.rs](src/frontend/iced/src/view/validate/results.rs), lines 19–36 and 63–82, displays the corresponding summary fields directly with four decimal places. `0.3990` means 39.90 AP when expressed on the published 0–100 scale.

## Evidence and scope

| Repository | Inspected revision |
|---|---|
| This repository | `de070ce116bfe30bb8c1d5b5a85a412373239174`, plus the existing working-tree changes |
| Official `../rf-upstream` | `e9a138f70cc14e3fddca029a0683055e34ab8f35` |
| Example `../rf-detr-cpp` | `8cfee49f886a7f6705086868fe0ff595b29d63d6` |

The working tree was already substantially modified. This review neither changes nor attributes those existing changes. References below describe the inspected source, not a verified build of the screenshot's executable. Online `main`-branch sources were inspected on the review date and may subsequently change.

Work performed: screenshot inspection; source tracing across compilation, loading, preprocessing, model execution, class admission, postprocessing, evaluation, display, and relevant tests; comparison against both adjacent repositories; inspection of official COCO and benchmark repositories online. All 80 local COCO source-ID/name pairs were text-compared with upstream's catalog and matched.

Existing logs were queried through `./mmltk --logs`. The scoped query for the displayed scores and validation/evaluation events found no corresponding run record. No raw-log searches, product builds, tests, installations, checkpoint conversions, dataset recompilations, or new inference runs were performed. A lack of a matching log entry does not prove that the run never occurred.

## Findings ranked by relevance

| Priority | Finding | Status and likely reach |
|---|---|---|
| Critical | Standalone compiled validation omits ImageNet normalization | Confirmed execution-path defect; changes every model input |
| High | Compiled images use letterboxing instead of upstream square stretching | Confirmed input-distribution difference; particularly relevant to non-square images and small objects |
| High | Crowd annotations lose crowd semantics | Confirmed COCO protocol defect; can create false positives and false negatives |
| High | Ground-truth boxes are quantized; generic compilation can derive different boxes from masks | Confirmed target changes; particularly damaging for small boxes and high-IoU AP |
| Medium | Original annotation `area` is replaced by reconstructed bbox area | Confirmed; invalidates direct comparison of size-specific metrics, not by itself overall AP |
| Medium | Candidate and evaluation caps differ | Confirmed defaults; scores are not from the same protocol |
| Medium | Missing clipping relative to current upstream Python postprocessing | Confirmed, but the inspected published benchmark harness also leaves boxes unclipped |
| High, conditional | Native inference accepts partial weight loading and discards the load summary | Confirmed capability to hide incomplete loading; no evidence that it happened in this run |
| Unresolved | Numerical parity of native, ONNX, TensorRT, and official Python execution | No end-to-end oracle comparison available from the inspected tests or screenshot |
| Low, training-only | Matcher hardcodes focal alpha 0.25 | Confirmed divergence when a non-default alpha is selected; irrelevant to unchanged pretrained inference |

## 1. Standalone validation is missing input normalization

The complete path is:

1. [validate.cpp](src/backend/models/rfdetr/inference/validate.cpp), lines 49–74, constructs a `PredictRequest` with `CompiledDataset`.
2. [predict.cpp](src/backend/models/rfdetr/inference/predict.cpp), lines 928–955, loads compiled batches and calls `InferenceBatchPreprocessor::Run`.
3. [inference_preprocessor.h](src/backend/models/rfdetr/inference/inference_preprocessor.h), lines 8–29, wraps the loader's float buffer as NCHW. For float32 it returns that buffer unchanged. For other input dtypes it only copies/casts it.
4. [predict.cpp](src/backend/models/rfdetr/inference/predict.cpp), lines 234–287, forwards that tensor to the runtime or native model. The native backbone starts with patch projection; it does not apply ImageNet input normalization internally.

Compiled pixels really are raw unit-range RGB. [image_resize.cpp](src/backend/imaging/resample/image_resize.cpp), lines 119–142, converts byte channels using only `1/255`; [dataset_compiler_pixels.cpp](src/backend/data/dataset_compiler_pixels.cpp), lines 56–86, and [benchmark_writer.cpp](src/backend/data/benchmark_writer.cpp), lines 191–208, use these conversion helpers. Loading copies those floats; it does not supply the missing transform.

For channel `c`, upstream expects:

```text
u = byte / 255
input[c] = (u[c] - mean[c]) / std[c]
mean = (0.485, 0.456, 0.406)
std  = (0.229, 0.224, 0.225)
```

Our compiled standalone path supplies `u` instead. A gray input of 0.5 should become approximately `(0.0655, 0.1964, 0.4178)`, not `(0.5, 0.5, 0.5)`. Black should become approximately `(-2.1179, -2.0357, -1.8044)`, not zero. This changes patch embeddings and every downstream feature.

The expectation is independently confirmed by:

- Official [dataset transforms](../rf-upstream/src/rfdetr/datasets/transforms.py), lines 49 onward, and [square validation transforms](../rf-upstream/src/rfdetr/datasets/coco.py), lines 1004–1045.
- The actual [SAB RF-DETR preprocessing code](https://github.com/roboflow/single_artifact_benchmarking/blob/main/sab/models/benchmark_rfdetr.py), which normalizes RGB and resizes to the model shape.
- Our ordinary-image prediction path, [predict.cpp](src/backend/models/rfdetr/inference/predict.cpp), lines 648–676, which explicitly subtracts the mean and divides by the deviation.
- Our training evaluation path, [evaluation_run_owner.cpp](src/backend/models/rfdetr/training/evaluation_run_owner.cpp), lines 431–460, which uses `GpuBatchPreprocessor`; [gpu_augment.cpp](src/backend/models/rfdetr/training/gpu_augment.cpp), lines 64–84, calls `normalize_gpu_batch`.
- The C++ example's [CUDA preprocessing](../rf-detr-cpp/src/core/cuda_preprocess.cu), lines 13–99, and [default channel constants](../rf-detr-cpp/src/internal/cuda_preprocess.cuh), lines 60–61.

The existing [prediction-session test](src/backend/models/rfdetr/inference/tests/prediction_session.test.cpp), lines 34–54, explicitly requires compiled preprocessing to preserve unit-range values. It therefore protects the incorrect behavior for ordinary RF-DETR checkpoints instead of testing parity with their expected inputs.

**Consequence:** the same weights can produce different detections in ordinary-image Predict, training evaluation, and standalone compiled Validate. The omission also reaches exported backends when their graphs expect upstream-normalized tensors. An externally supplied graph that embeds preprocessing would need separate artifact-specific verification.

## 2. Letterboxing changes the model's input, even when coordinates align

Our [letterbox geometry](src/backend/imaging/resample/image_resize.cpp), lines 30–51, preserves aspect ratio, rounds the resized dimensions, and centers the result. Padding is black. Both benchmark and generic compilation use this geometry.

Official Nano's default is square resize: [config.py](../rf-upstream/src/rfdetr/config.py), lines 801–810 and 1069; [coco.py](../rf-upstream/src/rfdetr/datasets/coco.py), lines 1004–1045. The inspected SAB preprocessing also resizes directly to the model's height and width. The C++ example uses separate horizontal and vertical scale factors.

For a 640 × 480 image and a 384 × 384 model:

| Property | Upstream square stretch | Our compiled letterbox |
|---|---|---|
| Image content | 384 × 384 | 384 × 288 |
| Coordinate transform | `x' = 0.6x`, `y' = 0.8y` | `x' = 0.6x`, `y' = 0.6y + 48` |
| Padding | None | 48 rows above and below |
| A 32 × 32 source box | 19.2 × 25.6 model pixels | 19.2 × 19.2 model pixels |

At this aspect ratio, one quarter of the compiled canvas is padding. The pretrained model sees different shapes, less object signal along one axis, different positions, and different patch contents.

This is **not automatically a coordinate-frame bug**. Applying the same positive axis scaling and translation to both prediction and ground truth preserves continuous box IoU: intersection and union are multiplied by the same determinant. Our internal compiled predictions and GT are both in compiled-canvas coordinates. The problems are the changed inference image, rounding, and any later conversion that fails to undo the offsets.

The native inference mask is all-valid ([predict.cpp](src/backend/models/rfdetr/inference/predict.cpp), lines 285–287), so padding is treated as image content. Masking those bars alone would still not reproduce upstream's pretrained square-resize recipe.

There is a second pixel-level difference: [image_resize.cpp](src/backend/imaging/resample/image_resize.cpp), lines 171–219, uses AVIR, or the optional perceptual downscaler, and materializes resized bytes. Upstream uses torchvision resize. Different kernels, antialiasing, and intermediate rounding can change small-object predictions. These are secondary to the normalization and geometry discrepancies, but matter for an exact comparison.

## 3. Compiled ground truth is not the original COCO ground truth

### 3.1 Crowd information is lost

[ParsedAnnotation and its parser](src/backend/data/benchmark_annotations.cpp), lines 344–357 and 417–450, read image/category/bbox/segmentation but do not preserve `iscrowd`, `ignore`, or the original `area`. Crowd annotations are not deliberately removed by a crowd-aware policy; valid crowd boxes can flow through as ordinary objects.

The version-7 [compiled format](src/backend/data/compiled_format.h), lines 10–33, stores class, integer box, mask references, and image dimensions. It has no crowd field. The evaluator explicitly acknowledges this limitation at [evaluator.cpp](src/backend/models/rfdetr/core/evaluator.cpp), lines 213–218.

Official COCO crowd matching excludes crowd GT from the positive denominator, allows repeated matching to a crowd region, and uses intersection divided by detection area for that crowd overlap test. Matching detections are ignored rather than treated as ordinary true/false positives. Our matcher applies one-to-one ordinary IoU matching to every stored box. See the official [COCO matcher](https://github.com/cocodataset/cocoapi/blob/master/PythonAPI/pycocotools/cocoeval.py) and [crowd IoU implementation](https://github.com/cocodataset/cocoapi/blob/master/common/maskApi.c).

For example, several correct person detections inside a crowd region should be ignored under the crowd rules. Treating the region as one giant ordinary person box can make those detections false positives and leave the giant box as a false negative. This can lower **overall AP**, not just size-specific AP.

Dropping all crowd annotations would also differ from retaining their ignore regions. A faithful benchmark needs their original semantics.

### 3.2 Benchmark compilation rounds boxes outward

The built-in benchmark compiler **does use the supplied annotation bbox**. It does not generally derive that bbox from the resized segmentation mask. This distinction matters.

However, [benchmark_letterbox_box](src/backend/data/benchmark_writer.cpp), lines 231–255, computes:

```text
min corner = floor(normalized_min * resized_extent) + padding_offset
max corner = ceil (normalized_max * resized_extent) + padding_offset
```

The resulting values are stored as signed 16-bit integers. Original floating-point annotation coordinates cannot be recovered from this representation.

A mathematical counterexample, not an observation from the screenshot:

```text
Original transformed GT: [10.2, 10.2, 12.2, 12.2], area 4
Stored compiled GT:      [10.0, 10.0, 13.0, 13.0], area 9
Prediction: exactly the original transformed GT

IoU against original GT = 1
IoU against compiled GT = 4/9 ≈ 0.4444
```

A perfect original prediction now fails even AP50. For a 10 × 10 fractional box expanded to 11 × 11, the corresponding IoU is `100/121 ≈ 0.8264`, failing the 0.85, 0.90, and 0.95 thresholds. Quantization is therefore a credible contributor to both low small-object AP and lost high-IoU AP.

### 3.3 Generic PNG/JSONL compilation can change the bbox definition

The generic compiler has a different rule. [dataset_compiler_scan_labels.cpp](src/backend/data/dataset_compiler_scan_labels.cpp), lines 435–462, derives the final box from the materialized/resized mask, even when no resize is needed. A declared bbox is used for diagnostics rather than preserved as the authoritative detection target. Instances whose masks disappear during resize can be dropped.

Thus a COCO-to-generic-data conversion can introduce:

- A mask-derived box different from the original detection annotation.
- Rasterization and nearest-sample mask resize differences.
- Disappearance of tiny objects.
- Integer mask bounds replacing subpixel box coordinates.

These findings are conditional on which compilation route produced the screenshot's dataset.

### 3.4 Annotation deduplication changes the instance set

[benchmark_annotations.cpp](src/backend/data/benchmark_annotations.cpp), lines 691–707, deduplicates using class and box coordinates. [benchmark_compiler.cpp](src/backend/data/benchmark_compiler.cpp), lines 588–591, again sorts and deduplicates after integer box conversion. Distinct annotations can acquire the same class/box identity, particularly after downscaling; masks are not part of that identity.

This changes the GT population and its ordering. It can move scores in either direction. Official instance annotations should not be silently merged merely because their quantized boxes coincide.

### 3.5 Size categories use a substituted area

Our evaluator does compensate for image scale. It does **not** simply apply COCO thresholds to 384-pixel canvas areas. [evaluator.cpp](src/backend/models/rfdetr/core/evaluator.cpp), lines 452–458 and 486, calculates:

```text
area_scale = (original_width / resized_width)
           * (original_height / resized_height)
GT area = compiled_bbox_width * compiled_bbox_height * area_scale
```

The remaining discrepancy is that this reconstructs the area of the **rounded bbox**, not the annotation's original `area`. COCO uses the supplied annotation area for GT area filtering; in COCO instance annotations that can differ substantially from enclosing-box area. The [faster-coco evaluator](https://github.com/MiXaiLL76/faster_coco_eval/blob/main/faster_coco_eval/core/cocoeval.py) preserves the COCO area/crowd inputs rather than reconstructing this compiled surrogate.

A thin object can have a medium-size enclosing rectangle but a small annotation area. It will enter different size buckets here. This can materially distort the screenshot's AP small/medium/large and corresponding AR values. **Area substitution alone does not explain the 39.90 overall AP**, whose ordinary area range includes all these objects.

### 3.6 Image selection and identity

The built-in benchmark compiler explicitly requires 5,000 COCO validation images and retains empty validation images: [benchmark_compiler.cpp](src/backend/data/benchmark_compiler.cpp), lines 1004, 1306–1308, and 1470. Its validation plan is COCO-only; the extra training sources are not automatically mixed into that validation set. There is no source evidence here for blaming the built-in validation split on a deliberate sample of fewer images.

Still, Validate can request an image limit or be cancelled. Its loader disables shuffle and keeps the last partial batch ([inference_loader.cpp](src/backend/models/rfdetr/inference/inference_loader.cpp), lines 12–27); cancelled runs limit the evaluator to processed images ([validate.cpp](src/backend/models/rfdetr/inference/validate.cpp), line 173). Confirm the actual completed image count before comparing scores.

Internally, [evaluator.cpp](src/backend/models/rfdetr/core/evaluator.cpp), lines 409–411, assigns `image_index + 1` as the evaluation ID, and prediction uses the same mapping. That is internally consistent. Those IDs are **not automatically the original COCO image IDs**. External evaluation must restore original image identity, source category IDs, and original-image box coordinates. The benchmark manifest identifies the normalized source index and annotation digest ([benchmark_compiler.cpp](src/backend/data/benchmark_compiler.cpp), lines 1531–1542), which can help recover provenance.

## 4. Is the AP calculation mathematically wrong?

For its supported ordinary non-crowd inputs, the inspected reduction follows the intended COCO-style calculation:

```text
T = {0.50, 0.55, ..., 0.95}
R = {0.00, 0.01, ..., 1.00}

P_j = cumulative_TP_j / (cumulative_TP_j + cumulative_FP_j)
R_j = cumulative_TP_j / eligible_GT_count
p_interp(r) = max(P_j for j with R_j >= r), or 0 if none
AP(category, threshold) = sum(p_interp(r), r in R) / 101
mAP = mean over the 10 IoU thresholds and categories with eligible GT
```

Relevant source checks:

| Component | Observed implementation | Assessment |
|---|---|---|
| IoU | Continuous XYXY intersection/union, without pixel-inclusive `+1`; `evaluator.cpp:128–133` | Appropriate for these boxes |
| Axes | Exactly 10 IoU thresholds and 101 recall samples; `contract/evaluation_metrics.h:21–36` | Correct |
| Detection ordering | Descending score within image/category; `evaluator.cpp:190–211` | Correct basic ordering |
| Matching | Greedy highest eligible IoU, independently for every threshold and area; `evaluator.cpp:220–268` | Correct ordinary-GT structure; crowd missing |
| Area ignore | Nonignored GT takes precedence; unmatched detections outside the area range are ignored | Correct ordinary area behavior |
| Tie within GT matches | Last annotation in stable GT order wins at equal IoU | Deliberately follows COCO's replacement rule |
| Dataset aggregation | Sorts the category's detections across images, then cumulative TP/FP; `evaluator.cpp:279–307` | Does not incorrectly average per-image AP |
| Precision interpolation | Reverse cumulative maximum and recall-grid sampling; `evaluator.cpp:309–320` | Correct structure |
| Missing categories | Categories without eligible GT are excluded rather than counted as zero; `evaluator.cpp:798–850` | Correct structure |
| Summary values | AP averages thresholds; AP50 selects index 0 and AP75 index 5 | Correct |

The separate confidence/F1 sweep does not choose or truncate the AP curve. Standalone validation inherits prediction threshold **0.0** from [workflow_requests.h](src/backend/models/rfdetr/contract/workflow_requests.h), lines 68–80; a preview threshold of 0.5 does not replace it.

**Answer:** no basic AP integration, percentage, or class-average error was found. There are nevertheless real **evaluation correctness problems relative to the original COCO protocol**, especially crowd semantics and altered ground truth. Passing internally constructed AP tests does not establish equivalence with official COCO scores.

## 5. Do class IDs, head indices, and labels line up?

For the admitted official COCO weights, the inspected mapping is coherent.

COCO has 80 evaluated categories with sparse source IDs extending to 90. The official checkpoint has 91 classifier slots; it does not mean there are 91 foreground categories.

[coco_class_layout](src/backend/models/rfdetr/core/class_layout.cpp), lines 113–123, initially marks all 91 slots unused, then maps each actual COCO source-ID slot to its dense foreground index. It declares sigmoid logits and all-negative no-object encoding. Slot 0 and source-ID gaps are unused; **slot 90 is toothbrush, not background**.

| Class | Official source category / checkpoint slot | Our dense foreground index |
|---|---:|---:|
| person | 1 | 0 |
| stop sign | 13 | 11 |
| backpack | 27 | 24 |
| toothbrush | 90 | 79 |

The source-ID/name pairs in [coco_catalog.h](src/backend/data/catalog/coco_catalog.h) match all 80 pairs in [upstream coco_classes.py](../rf-upstream/src/rfdetr/assets/coco_classes.py).

The chain also remains consistent after admission:

- The benchmark adapter maps source IDs to the dense compiled catalog.
- [ClassPostprocessLane](src/backend/models/rfdetr/core/postprocess.cpp), lines 95–135, gathers physical foreground columns and maps selected positions to their declared references.
- [validate.cpp](src/backend/models/rfdetr/inference/validate.cpp), lines 85–114, requires semantic admission and permutes model catalog references into the dataset's catalog by class identity.
- Ground-truth packed class IDs index that dataset catalog.
- Native training deliberately creates a different head layout: dense foreground slots plus one unused slot ([class_layout.cpp](src/backend/models/rfdetr/core/class_layout.cpp), lines 96–105). Fresh pretrained transfer matches classifier rows by class name ([model.cpp](src/backend/models/rfdetr/core/model.cpp), lines 1372–1425); it does not blindly reinterpret sparse slot numbers as dense labels. Resume checks the saved layout against the ordered dataset catalog.

Known downloaded weights receive their COCO layout through verified asset evidence, not just a suggestive filename ([model_state.cpp](src/backend/models/rfdetr/core/model_state.cpp), lines 237–258). The local Nano registry and [upstream weight registry](../rf-upstream/src/rfdetr/assets/model_weights.py), lines 216–218, specify the same `nano_coco/checkpoint_best_regular.pth` asset and MD5 `fb6504cce7fbdc783f7a46991f07639f`.

This rules against a general static off-by-one defect in the current path. It does not verify the screenshot's actual artifact or a user-supplied class descriptor. A semantically incorrect descriptor can still attach plausible names to the wrong physical columns.

One smaller difference remains: our selection removes unused columns before top-K; upstream's generic decoder considers its complete logit tensor. These are not guaranteed to produce identical candidate lists if unused columns have appreciable scores.

## 6. Candidate limits and postprocessing

### Candidate count and evaluator cap are separate

| Path | Selected query/class pairs per image | Evaluator maxDets |
|---|---:|---|
| Inspected published Nano SAB harness | Up to 300 | `[1, 10, 100]` |
| Current upstream Python defaults | Nano `num_select=300` | Training `eval_max_dets=500`; legacy evaluator default 100 |
| Our standalone Validate defaults | Up to 500 | `[1, 10, 500]` |

The SAB values are established by its [RF-DETR decoder](https://github.com/roboflow/single_artifact_benchmarking/blob/main/sab/models/benchmark_rfdetr.py), [artifact request defaults](https://github.com/roboflow/single_artifact_benchmarking/blob/main/sab/models/utils.py), and [evaluation setup](https://github.com/roboflow/single_artifact_benchmarking/blob/main/sab/evaluation.py). The detection requests do not override the default 100 cap.

Our defaults come from [validate.cpp](src/backend/models/rfdetr/inference/validate.cpp), lines 61–62. The screenshot's AR @ 500 agrees with that evaluation cap. Upstream's current training default is explicitly 500 at [config.py](../rf-upstream/src/rfdetr/config.py), line 1138, so “upstream uses 100” is only correct for the benchmark/default legacy path being compared.

Both decoders rank the flattened **query × class** score tensor. Multiple candidates can come from one query; 500 candidates do not require a model with 500 query embeddings. COCO's category-aware evaluation cap is applied per image/category, whereas decoder top-K is across the image's candidate pairs. These limits must not be conflated.

Additional candidates can change AP in either direction: newly included false positives from one image can rank ahead of true positives from another. More retained detections need not imply higher AP, even though recall is nondecreasing when extending an otherwise unchanged per-image ranked prefix. Measure this difference rather than assuming its contribution.

### Bounding-box clipping differs across upstream paths

Our [postprocess.cpp](src/backend/models/rfdetr/core/postprocess.cpp), lines 47–92, converts `cxcywh` to `xyxy` and scales it, but does not clip coordinates to image bounds. The helper named `xyxy_clamped` in [evaluation.h](src/backend/models/rfdetr/core/evaluation.h), lines 77–87, only orders the corners; it does not clip them to an image.

Current upstream Python [PostProcess](../rf-upstream/src/rfdetr/models/postprocess.py), lines 150–169, does clip to `[0,width] × [0,height]`. The C++ example clips too.

For GT `[0,0,20,20]` and prediction `[-10,0,20,20]`, unclipped IoU is `400/600 ≈ 0.667`; clipped IoU is 1. This can affect high-IoU matches for edge objects.

**Benchmark qualification:** the inspected SAB RF-DETR decoder and its `cxcywh_to_xyxy` helper do not perform that clipping. Missing clipping is therefore a proven discrepancy against current upstream Python, but cannot simply be named as the reason we underperform the published SAB result. The comparison must select one exact postprocessing contract. For letterboxed inputs, source-image bounds also differ from padded-canvas bounds.

### Scores, suppression, and ties

Our sigmoid-per-class scoring and flattened selection follow the RF-DETR approach. There is no evidence that a softmax or NMS should be added for parity.

Current upstream Python uses stable descending `argsort` with ascending flattened index for score ties ([postprocess.py](../rf-upstream/src/rfdetr/models/postprocess.py), lines 130–143). Our code uses `topk`; the inspected SAB harness also uses `topk`. Equal scores can therefore produce a different candidate order from current Python, especially after reduced-precision saturation. This is a reproducibility concern, not a measured explanation for an 8.50-point loss.

## 7. Is the model algorithm itself executed incorrectly?

The source comparison found substantial agreement in the main Nano model equations. These are static findings, not measured tensor parity.

| Component | Comparison |
|---|---|
| Nano topology | Both use resolution 384, patch size 16, `num_windows=2` (a 2 × 2 window grid), positional grid 24, two decoder layers, 300 default queries, hidden decoder width 256, and 13 training groups |
| Backbone | Windowed DINOv2-small configuration: width 384, 12 blocks, six heads; feature stages 3, 6, 9, 12 |
| Attention schedule | Our apparently unusual zero-based full-attention positions follow upstream's explicit legacy schedule; this is not a discovered off-by-one bug |
| Proposal generation | Grid centers `(x+0.5)/valid_width`, `(y+0.5)/valid_height`, with initial width/height `0.05 * 2^level` |
| Two-stage selection | Rank encoder proposal class logits; gather selected references/features. Applying a pointwise box MLP before versus after selection is not inherently a mathematical difference |
| Box refinement | `center = base_center + delta_center * base_size`; `size = exp(delta_size) * base_size` when bbox reparameterization is enabled |
| Decoder | Self-attention, deformable cross-attention, residual/normalization, and feed-forward structure align |
| Deformable attention | Softmax over level/point weights and reference-box-relative offsets follow the upstream equations |
| Inference mode | Native prediction calls `eval()` and uses inference mode; no accidental training dropout was identified in this path |

Main references: local [preset_catalog.h](src/backend/models/rfdetr/contract/preset_catalog.h), lines 54–81; [model_config.h](src/backend/models/rfdetr/contract/model_config.h), lines 29–64; [model.cpp](src/backend/models/rfdetr/core/model.cpp), particularly 92–111, 288–459, 583–616, 653–767, and 790–960. Upstream references: [config.py](../rf-upstream/src/rfdetr/config.py), [dinov2.py](../rf-upstream/src/rfdetr/models/backbone/dinov2.py), lines 58–97; [transformer.py](../rf-upstream/src/rfdetr/models/transformer.py); and [lwdetr.py](../rf-upstream/src/rfdetr/models/lwdetr.py).

The bbox refinement equations intentionally allow corners outside the image. Adding a sigmoid simply because the outputs are described as normalized boxes would change the trained model's mathematics.

### Weight loading can conceal a model-execution problem

[predict.cpp](src/backend/models/rfdetr/inference/predict.cpp), line 171, calls `load_normalized_state(..., false)` and discards its summary. [model.cpp](src/backend/models/rfdetr/core/model.cpp), lines 1351–1456, supports partial loading: missing or incompatible non-class tensors can remain at initialization rather than rejecting the load. Class-dependent shape mismatches do have stronger checks.

This matters for a changed checkpoint topology, incomplete conversion, or unexpected key names. Predictions can remain finite and plausible while some learned tensors are absent. It is a **conditional risk**, not evidence that the registered Nano checkpoint actually fails to load. The next numerical comparison should include loaded/missing/unexpected/incompatible tensor inventories.

### Existing “parity” tests do not prove official-model parity

[checkpoint_parity.test.cpp](src/backend/models/rfdetr/training/tests/checkpoint_parity.test.cpp), lines 102–127, seeds a `NativeRfDetrModel`, writes an upstream-shaped checkpoint, normalizes it, and compares two `NativeRfDetrModel` instances. One is named `upstream_model`, but it is still the local C++ implementation. This proves a serialization round trip, not equivalence with Python RF-DETR's forward pass.

The inspected evaluator tests cover useful constructed cases, but do not establish parity against original COCO JSON plus an independent evaluator. The compiled preprocessing test actively asserts the normalization omission described above.

A wrong custom CUDA kernel, export interpretation, reduced-precision behavior, or tensor-layout handoff therefore remains possible. None was established as the source of this screenshot. Shared preprocessor and evaluator defects can also make several local backends agree with each other while all disagree with the official reference.

## 8. Training-only differences and other confounders

If these are freshly downloaded pretrained weights, optimizer, augmentation, and training-loss differences cannot explain the run. If these are locally trained/fine-tuned weights, they become important.

The default loss equations inspected are broadly aligned: IoU-aware BCE classification targets, L1 box loss, GIoU loss, auxiliary/encoder losses, and group normalization. See [detection_ops.cpp](src/backend/models/rfdetr/core/detection_ops.cpp), lines 751–819 and 879–934, versus upstream [criterion.py](../rf-upstream/src/rfdetr/models/criterion.py), lines 578–613 and the box-loss implementation.

One concrete non-default divergence exists: our dense matcher fixes `alpha=0.25` ([detection_ops.cpp](src/backend/models/rfdetr/core/detection_ops.cpp), line 541), and the CUDA matcher hardcodes weights 0.25/0.75 ([detr_matcher_cuda.cu](src/backend/models/rfdetr/core/detr_matcher_cuda.cu), lines 56–57). Upstream uses `self.focal_alpha` in the matching cost ([matcher.py](../rf-upstream/src/rfdetr/models/matcher.py), lines 201–207). Selecting a different focal alpha changes our loss but not the matcher in the same way as upstream. The default 0.25 agrees.

Local match-free assignment and denoising are additional training options; they are not enabled by the default [TrainingSupervisionConfig](src/backend/models/rfdetr/contract/training_supervision.h), lines 55–60. A run that enables them is not the unchanged upstream training recipe.

Other run facts to establish before attribution:

- Exact checkpoint hash, whether it was trained, and whether regular or EMA weights were evaluated. The registered pretrained Nano asset is explicitly the same regular checkpoint as upstream; there is no demonstrated registry-level EMA mixup.
- Actual resolution and dataset compilation settings, including perceptual downscaling. Matching only a preset name is insufficient.
- Backend, exported graph identity, effective input precision, autocast, and engine precision. `allow_fp16` can select the device's preferred native reduced precision; that does not by itself mean every backend reproduces the benchmark's FP16 execution.
- Completed image count, requested image limit, selected validation file, original annotation digest, and compile route.
- Correct graph output interpretation: raw logits versus already-sigmoid scores, normalized `cxcywh` versus already-decoded boxes. Known supported exports should follow the declared contract; arbitrary exports need confirmation.

A change in small-object AP alone cannot distinguish these causes, because input resolution, letterboxing, quantization, dropped tiny masks, and substituted area buckets all affect that row.

## 9. What `rf-detr-cpp` contributes

The example repository is useful as an independent implementation of square RGB preprocessing, ImageNet normalization, global candidate selection, and box decoding. Its CUDA resize is bilinear; it is not an exact oracle for torchvision's antialiased downsampling.

Its public class-ID convention must not be copied into our evaluator. [postprocess.hpp](../rf-detr-cpp/include/rfdetr/core/postprocess.hpp), lines 9–27, subtracts one from the raw class ID and describes this as dense. [coco_classes.hpp](../rf-detr-cpp/include/rfdetr/core/coco_classes.hpp), lines 5–12, retains sparse COCO placeholders and looks up the display name by adding one back. Thus its stop-sign ID is 12 and toothbrush ID is 89, whereas our dense-80 IDs are 11 and 79.

Its comment that current upstream slices `[...,1:]` does not match the inspected official `PostProcess`. It also defaults to confidence threshold 0.5, which would discard candidates needed for an AP comparison. Its label display can be consistent within its own convention without those integer IDs being compatible with ours.

Use it to cross-check inputs and raw exported-model outputs after accounting for these contracts. It is not an authoritative replacement for COCO metric evaluation.

## 10. Online COCO tooling and upstream-path distinctions

The researched sources establish three different reference routes:

1. **Published benchmark:** upstream's documentation describes COCO metrics over original val2017 annotations through SAB. The inspected [SAB evaluator](https://github.com/roboflow/single_artifact_benchmarking/blob/main/sab/evaluation.py) loads the original annotation JSON, restores predictions to original-image coordinates, and sets image IDs and maxDets explicitly. Its current `_load_coco_tools` invokes `faster_coco_eval.init_as_pycocotools()` before importing the familiar COCO APIs. The implementation is therefore currently the accelerated drop-in evaluator behind that interface.
2. **Legacy official evaluator:** [evaluation/coco_eval.py](../rf-upstream/src/rfdetr/evaluation/coco_eval.py), lines 343–378, copies the supplied COCO GT object and configures `[1,10,max_dets]`, defaulting to 100.
3. **Current training callback:** [training/callbacks/coco_eval.py](../rf-upstream/src/rfdetr/training/callbacks/coco_eval.py), lines 221–236, uses the configured metric backend and maxDets; the default backend is `hotcoco` and training cap is 500. Its tensor targets are not necessarily the original annotation JSON.

There is a significant qualification for route 3: [datasets/coco.py](../rf-upstream/src/rfdetr/datasets/coco.py), line 657, filters crowd annotations from the tensor target, and the callback's `_convert_targets`, lines 1377–1435, forwards boxes/labels and optional masks/crowd but not the original annotation `area`. Consequently, reproducing that convenience training path is not equivalent to proving the published benchmark protocol. This is an upstream distinction, not a reason to treat discarded COCO metadata as harmless.

The reference repositories inspected were [official COCO API](https://github.com/cocodataset/cocoapi), [faster-coco-eval](https://github.com/MiXaiLL76/faster_coco_eval), [Roboflow SAB](https://github.com/roboflow/single_artifact_benchmarking), and [official RF-DETR](https://github.com/roboflow/rf-detr). The relevant source links are attached to the findings above; no third-party blog was used as an algorithmic authority.

## 11. Controlled comparisons needed to attribute the gap

These are proposed follow-up measurements, **not actions performed by this review**. Any execution should use the repository's container wrapper.

1. **Pin the run.** Record executable revision, Nano weight hash, class descriptor, backend, precision, original val2017 annotation digest, compiled manifest, and 5,000 completed images. Save raw detections with correct source-image identity. This determines which confirmed source findings apply to the screenshot.
2. **Separate scoring from inference.** Feed one identical prediction set and one identical GT set to our evaluator and an independent COCO evaluator, using the same categories and caps. Start with non-crowd float-box fixtures, then crowd, area, ties, duplicates, empty images, and cap boundaries. Separately compare original COCO GT against the compiled surrogate. This isolates metric semantics and annotation loss without changing model predictions.
3. **Isolate normalization.** On the same compiled pixels, compare current standalone input with ImageNet-normalized input. Compare the tensor directly with our training evaluator's preprocessing. Hold weights, geometry, candidate limits, and evaluator fixed.
4. **Isolate image geometry and resampling.** Starting from original images, compare normalized square stretch against normalized letterbox, then match upstream's resize kernel. Evaluate continuous predictions against original COCO annotations with the correct inverse transform; do not reintroduce integer-box GT as a confounder.
5. **Establish actual model parity.** Feed the exact same normalized tensor and checkpoint to official Python, local native execution, and the relevant exported backend. Compare raw logits and normalized boxes before top-K; locate any discrepancy through patch embeddings, backbone stages, projector output, encoder proposal selection, and decoder layers. Begin with float32 and a complete weight-load inventory, then assess the actual reduced-precision configuration.
6. **Align the chosen benchmark contract.** Use the published harness's 300 candidate pairs and `[1,10,100]` evaluator caps for that reproduction. Match clipping and score-tie behavior to the selected reference. Report current-training-callback comparisons separately.
7. **Re-run full val2017 only after these checks.** Capture all 12 metrics plus per-class results and counts. Quantify the change from each isolated factor; do not infer a percentage contribution from source inspection.

The first investigation targets should be normalization, square-resize parity, and faithful original COCO annotations. A general class-index shift or a replacement AP integration formula is not supported by the inspected evidence.
