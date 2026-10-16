# Source mask recovery fixtures

Bounded fixtures retain exact COCONut PNG bytes/segment facts for COCO train2017
physical images **2212** (Parquet row 449: two absent dog segments, couch segment 7)
and **400** (absent dog segment 2, boat segment 3). Segment JSON retains artifact
path, row, image metadata, IDs, and support counts.

Original COCO normalized-v3 JSON retains annotation SHA-256 identity, exact float
boxes, ordinals, flags, area, and row-major runs. Dog IDs are 11021/16598 for 2212
and 9774 for 400; supporter IDs are 113052/176688. Export equivalent source facts
with the read-only diagnostic:

```bash
./mmltk --diagnose-benchmark-image --image-id 2212 --index .cache/benchmark-dataset/v1/indexes/coco/train2017.normalized.bin --export-mask-runs
./mmltk --diagnose-benchmark-image --image-id 400 --index .cache/benchmark-dataset/v1/indexes/coco/train2017.normalized.bin --export-mask-runs
```

Tests create temporary small Parquet/cache products; no production JPEG/compiled
file is required or modified.
