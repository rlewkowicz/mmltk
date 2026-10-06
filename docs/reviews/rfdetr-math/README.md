# RF-DETR numerical reviews and acceptance

[Wiki index](../../README.md) · [Training reference](../../rfdetr-training.md) · [Validation procedure](../../validation.md#independent-upstream-diagnostic)

The four reviews below examine native baseline
`52586e98f535205cc242dfe74ef5144eef0373bb` against read-only RF-DETR
`e9a138f70cc14e3fddca029a0683055e34ab8f35`. They are historical source findings,
with the execution limits stated in each report. Their line anchors belong to
those revisions; descriptions of missing behavior are not current product claims.

| Report | Reviewed boundary |
| --- | --- |
| [01 — CUDA operations](01-cuda-ops.md) | Deformable attention, support derivatives, exceptional values, chunking, matcher precision, and compiler flags |
| [02 — Compilation and tracing](02-compilation-tracing.md) | Opaque operations, custom VJPs, trace specialization/lifetime, export isolation, and compiler alternatives |
| [03 — Model and forward](03-model-forward.md) | Checkpoint envelopes/admission, initialization, output publication, activation and preprocessing |
| [04 — Training and gradients](04-training-gradients.md) | IA-BCE, scale growth, global normalization, optimizer/Resume policy, Match-Free and DN scope |

[Current results and closure](current-results.md) records implementation through
`90e82d54`, actual upstream evidence, focused acceptance, and remaining numerical
and quality limits. The ordinary guides own current behavior and operations:

- [Training and native compilation](../../rfdetr-training.md), including
  [compiler choices](../../rfdetr-training.md#compiler-choices-and-native-feasibility).
- [Checkpoint and export workflows](../../rfdetr-workflows.md).
- [RGB resampling](../../datasets.md#rgb-resampling-methods).
- [Independent reference production and numerical policy](../../validation.md#independent-upstream-diagnostic).
- [Comparison records and queries](../../logging.md#upstream-numerical-comparisons).

Neither the reviews nor their acceptance attributes a measured AP change to a
repair. Match-Free remains a native objective extension, and autonomous
trajectories remain distinct from matched-state equation checks.
