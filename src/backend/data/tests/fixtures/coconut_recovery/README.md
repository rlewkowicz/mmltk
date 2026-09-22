# Source mask recovery fixtures

These bounded fixtures retain exact COCONut source PNG bytes and segment facts
for COCO train2017 physical images 2212 and 400. Image 2212 is source Parquet
row 449 (two absent dog segments, couch segment 7); image 400 has absent dog
segment 2 and boat segment 3. The JSON segment files retain the source artifact
path, row, image metadata, segment IDs, and observed support counts.

Original instance masks and identities were exported from the admitted COCO
train2017 normalized-v3 cache using the read-only wrapper diagnostic:

```
./mmltk --diagnose-benchmark-image --image-id 2212 --index .cache/benchmark-dataset/v1/indexes/coco/train2017.normalized.bin --export-mask-runs
./mmltk --diagnose-benchmark-image --image-id 400 --index .cache/benchmark-dataset/v1/indexes/coco/train2017.normalized.bin --export-mask-runs
```

The originals JSON retains its annotation SHA-256, exact normalized float boxes,
source ordinals, flags, areas, and row-major mask runs. Dog original IDs are
11021 and 16598 for image 2212, and 9774 for image 400. Supporter IDs are 113052
and 176688. Tests materialize only temporary small Parquet files and cache data;
no production JPEG or compiled file is required or modified.
