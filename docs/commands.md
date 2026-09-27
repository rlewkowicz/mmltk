# Command reference

[Wiki index](README.md) · [Quick start](../README.md#build) · [Build details](build.md) · [Validation](validation.md)

Run commands from the repository root through `./mmltk`. The wrapper rewrites
supported host paths into container mounts; absolute host paths become
`/host/...`, with repository paths mapped into `/workspace`. Runtime operations
reuse a repository-scoped container and stream the application output.

## Wrapper operations

| Invocation | Purpose |
| --- | --- |
| `./mmltk --build` | Build and package the Release runtime |
| `./mmltk --build-gui` | Format/check Iced and rebuild the canonical browser bundle in the GUI graph |
| `./mmltk --gui` | Launch the packaged browser host with the canonical bundle |
| `./mmltk --prepare-gui-container` | Prepare the GUI runtime container without starting the app |
| `./mmltk --generate-application-bindings` | Generate bindings, marker, graphics ABI, and cross-language fixtures in the dedicated generation graph |
| `./mmltk --generate-protocol` | Generate the same artifacts in the shared Release graph |
| `./mmltk --update-gui-lock` | Refresh the Cargo lock inputs for browser tests |
| `./mmltk --update-firefox-lock` | Refresh Firefox's Cargo lock from its vendored sources, offline |
| `./mmltk --tidy` | Format native sources and Iced application Rust; run configured native analysis |
| `./mmltk --cleanup-report cpp\|frontend\|all` | Generate the selected deduplication reports |
| `./mmltk --raw-cpd [--review] [--min-tokens N] [--output PATH]` | Save exact-spelling C++ CPD evidence; `--review` adds an authored-file queue and lexical families in JSON/Markdown |
| `./mmltk --format-declarations check\|preview\|fix [--file PATH] [--report PATH]` | Report or safely shorten canonical reflection annotation syntax |
| `./mmltk --audit-includes [--refresh] [report\|preview\|fix] [--output PREFIX] [--build-dir PATH]` | Inventory all compiler contexts and include-retention reasons; preview/apply replacements only under verified target-local PCH policy |
| `./mmltk --test list` | List supported suites and test options |
| `./mmltk --test all` | Run ordinary native, browser JavaScript/Rust, cleanup/declaration, and log-query fixtures |
| `./mmltk --test cuda-vulkan -- --help` | Build/select the standalone CUDA/Vulkan diagnostic and show its positional options |
| `./mmltk --logs --help` | Show log-query grammar and options |
| `./mmltk --diagnose-processes [WRAPPER_MODE]` | Inspect processes in this repository's running wrapper containers |
| `./mmltk --diagnose-benchmark-image --image-id ID [OPTIONS]` | Compare retained image headers with normalized annotation geometry |
| `./mmltk --diagnose-io FILE` | Report a compiled file's storage/GPU capabilities |
| `./mmltk --diagnose-gpu-environment runtime\|wayland-validation\|development` | Inspect an existing image's GPU, driver, library, and ICD environment |
| `./mmltk --diagnose-gpu-program SOURCE.cpp ARGS...` | Compile and run a standalone CUDA-driver/Vulkan diagnostic using existing images |
| `./mmltk --diagnose-nvidia-payload donor\|development HEADER...` | Inspect public NVIDIA headers and payload selection |
| `./mmltk --diagnose-native-symbols [OPTIONS] ARTIFACT...` | Inspect cached native `.a`/`.o` symbols with the existing development image |
| `./mmltk --diagnose-native-link [OPTIONS] TARGET` | Repeat one generated Release link into separate diagnostic storage |

The `|` entries above mean choose one value; they are not shell pipelines.
Build, test, tidy, cleanup, export, and diagnostics are separate operations.
Put `--logs`, `--diagnose-io`, `--diagnose-nvidia-payload`,
`--diagnose-gpu-environment`, `--diagnose-gpu-program`, `--diagnose-native-symbols`,
`--diagnose-native-link`, `--diagnose-processes`, `--diagnose-benchmark-image`,
`--cleanup-report`, `--raw-cpd`, `--format-declarations`, or `--audit-includes`
first when invoking that standalone operation.

Detailed references: [CUDA/Vulkan diagnostics](validation.md#standalone-cudavulkan-diagnostic),
[native symbols/links](validation.md#native-symbol-and-link-diagnostics),
[test selectors](validation.md#selection-environment-deadlines-and-debugging),
[declaration tooling](validation.md#raw-cpd-and-declaration-formatting),
[PCH include audits](validation.md#pch-include-audit), and
[log queries/provenance](logging.md).

Build, test, and tidy automatically retain both output streams in
[wrapper transcripts](logging.md#wrapper-build-test-and-tidy-transcripts).
`MMLTK_BUILD_LOG_FILE`, `MMLTK_TEST_LOG_FILE`, and `MMLTK_TIDY_LOG_FILE`
override their destinations; relative paths use the invocation directory.
Test help/list routes skip capture. A new `--test all` replaces earlier tests
owned by this checkout; [replacement scope and failures](validation.md#replacing-an-active-test-run)
also apply when `all` has filters.

### Process snapshots

```bash
./mmltk --diagnose-processes --help
./mmltk --diagnose-processes
./mmltk --diagnose-processes build
./mmltk --diagnose-processes test
```

The default selects all running containers labeled as wrapper-owned by this
repository. The optional argument matches the exact wrapper mode label; `test`
includes both test compilation and execution. Output identifies each container
and mode, then reports PID, parent PID, elapsed time, CPU time, CPU/memory
percentages, process state, wait channel, and process name. It omits command
arguments and environment variables.

Snapshots require a running daemon and are read-only: no daemon start,
container creation/mutation, process attachment, or image build/pull. Each daemon
query has a 15-second deadline. A selection with no matching container succeeds
with empty output. A snapshot alone establishes neither a stall, completion,
nor performance.

Explicit process actions accept `mmltk` or a native executable basename of the
form `mmltk[_-][A-Za-z0-9_-]+` and require exactly one matching wrapper container:

```bash
./mmltk --diagnose-processes test --backtrace mmltk_backend_models_rfdetr_training_tests
./mmltk --diagnose-processes test --read-file mmltk_backend_models_rfdetr_training_tests --file /workspace/build/example.json
./mmltk --diagnose-processes test --terminate mmltk_backend_models_rfdetr_training_tests --pid 123
```

The examples require that executable to be running. `--pid` is a container PID
selector for an action, not a host PID. Selection verifies `/proc/PID/exe`,
rather than command text. Backtrace attaches GDB as container root, collects
all thread stacks, and detaches under a 35-second debugger deadline. Read-file
uses the selected process's root namespace, accepts an absolute regular-file
path, and prints escaped bytes up to 4 MiB. Terminate sends SIGTERM through a
pidfd to the selected process and can fail the interrupted operation. These
explicit actions are distinct from the read-only snapshot; none creates or
builds a container. Wrapper action execution is bounded to 45 seconds.

### Benchmark image geometry

`./mmltk --diagnose-benchmark-image --help` describes the numeric image ID and
repository-relative inputs. Alternatively, `--compiled PATH --sample N`
resolves a zero-based Explore sample to its source identity and compiled objects.
Repeat `--index PATH` to inspect version-3 normalized annotation rows; add
`--objects` for their object records. Use `--image PATH` for a cached image or
`--archive PATH` to find the image in a retained tar archive. JSONL output includes
dimensions, archive member, image identity, encoded SHA-256, and JPEG EXIF
orientation when present. Headers establish geometry, not full decodability.
`--parquet PATH` inspects the matching COCONut row's original embedded PNG and
segment metadata; `--panoptic PATH` inspects an extracted panoptic PNG. These
count exact RGB segment IDs and report each declared segment's pixel support
when metadata is available. They compile a bounded diagnostic helper with the
existing development compiler, native Arrow libraries, and vendored PNG decoder;
no Python Parquet package or new dependency is installed.

`--export-mask-runs` requires exactly one `--index` and writes the selected
image's normalized objects to
`build/validation/benchmark-image/ID.originals.json`. It retains the annotation
identity, dimensions, boxes, flags, source IDs/ordinals, area, and exact row-major
runs. Export is bounded to 65,535 objects, 65,535 runs per object, and 1,048,576
runs per image. For example:

```bash
./mmltk --diagnose-benchmark-image --image-id 2212 \
  --index .cache/benchmark-dataset/v1/indexes/coco/train2017.normalized.bin \
  --export-mask-runs
```

This inspects the selected retained index; it does not compile or enable
[mask recovery](benchmark-datasets.md#optional-dropped-mask-recovery).

The command runs in the existing development image with networking disabled,
read-only source mounts, no image build or pull, and a ten-minute deadline. Optional
`--export` copies the matched encoded image to
`build/validation/benchmark-image/ID.jpg` or `ID.png` for inspection, replacing
an earlier diagnostic copy. Mask inspection additionally exports the exact
`ID.mask.png` and a segment-color visualization `ID.segments.png`. It does not
change the source cache. Tar scans are sequential, and each inspected image is
bounded to 64 MiB.

## Native CLI

`./mmltk --help` is the packaged native CLI's help, not a complete listing of
wrapper operations. Commands expose their own reflected argument help:

```bash
./mmltk compile --help
./mmltk info --help
./mmltk bench --help
./mmltk rfdetr --help
./mmltk rfdetr train --help
./mmltk rfdetr predict --help
./mmltk rfdetr evaluate --help
./mmltk rfdetr validate --help
./mmltk rfdetr export-onnx --help
./mmltk rfdetr build-engine --help
```

The wrapper prevents raw native CLI invocation while its GUI is active,
including `--help`. Close that GUI before invoking the native CLI. Command
declarations remain available in the source links below.

Top-level commands compile source datasets, inspect compiled metadata,
benchmark loading, or dispatch RF-DETR work. RF-DETR exposes `compile`, `info`,
`build-engine`, `export-onnx`, `predict`, `evaluate`, `validate`, `train`, and
`normalize-weights`. Their options come from
[rfdetr_cli_options.h](../src/entrypoints/cli/rfdetr_cli_options.h) and the native contract;
use command help for required fields and current defaults.
Root compile/info/bench declarations live in
[cli_options.h](../src/entrypoints/cli/cli_options.h); command execution remains
in the corresponding `.cpp` owners. The
[authoring reference](reflection.md#compact-cli-declarations) explains derived
names, explicit spelling exceptions, aliases, negation, and exposure audits.

For example, with existing input artifacts:

```bash
./mmltk rfdetr predict \
  --compiled ./compiled/val.bin \
  --weights ./checkpoints/model.pt \
  --output ./predictions.json
```

Key option groups (required fields and defaults remain in command help):

| Commands | Options and reference |
| --- | --- |
| `compile`, `rfdetr compile` | `--resize-mode Stretch` (default) or `Letterbox`; independent `--perceptual-downscale`; [source/geometry rules](datasets.md#compile-and-inspect) |
| `rfdetr validate` | `--resize-mode` affects source compilation only; existing bins retain stored geometry |
| `rfdetr train` | Independent `--aug-perceptual-downscale`, `--use-ema`/`--no-ema`, `--resume`, `--output-dir`; `--test-compiled` is optional, train/validation required |
| Model-input commands | `--class-layout` supplies a [digest-bound descriptor](rfdetr-workflows.md#class-identity-and-model-admission) |
| `rfdetr predict` | Repeatable `--image` or `--compiled`, CLI `--batch-size`; local video is GUI-only |
| `rfdetr evaluate`, `validate` | One backend or ordered multi-backend report, respectively; independent `--candidate-count` and `--eval-max-dets` |
| Train / Predict counts | Train exposes `--eval-max-dets`; Predict uses `--max-dets-per-image`; [count semantics and zero defaults](rfdetr-workflows.md#model-input-and-detection-selection) |

[RF-DETR workflows](rfdetr-workflows.md) owns checkpoint, metric, and history behavior.
Train, predict and evaluate accept `--compile-mode none|selective|full`.
The [native selective contract](rfdetr-training.md#native-selective-compilation)
describes the default selective path, its guards and inference propagation;
the separate full-trace route is outside that evidence. CLI `export-onnx` and
`build-engine` keep explicit destinations. GUI Export combines independent
ONNX/TensorRT selections through one conversion and reserves its output beside
the other [workflow runs](rfdetr-workflows.md#run-output-directories).

The native CLI also accepts `--log-level`, `--log-file`, and `--log-dir`.
See [logging activation](logging.md#activation-and-quiet-execution), especially
for ONNX metadata commands whose output requires explicit diagnostics.
Fatal operation failures still produce a concise
[stderr report](logging.md#fatal-stderr-reports) with diagnostics disabled.

### Training request selection

`rfdetr train --batch-size` counts **global microbatch** images;
`--grad-accum-steps` and logical `--lanes` determine effective batch.
`--validation-lanes` is independent. See [batch equations/admission](rfdetr-training.md#logical-lanes-and-global-batch).
Recipe flags are `--optimizer adamw|muon|sgd`,
`--lr-scheduler step|cosine|ultralytics-linear` (last choice requires SGD),
`--nesterov`/`--no-nesterov`, and `--warmup-bias-lr`. Final-epoch flags are
`--unfreeze-encoder-last-epochs` and `--disable-augmentation-last-epochs`.

`--request-json JSON` accepts one complete canonical `TrainRequest`, bounded to
64 KiB. The argument is JSON text, not a filename. It is mutually exclusive with
every scalar train option. Use this route for the complete lane configuration,
stable model IDs and per-model recipes, merge/final policy, and sparse sampling
policy. The canonical
[request](../src/backend/models/rfdetr/contract/workflow_requests.h),
[execution plan](../src/backend/models/rfdetr/contract/execution_plan.h), and
[parser](../src/entrypoints/cli/rfdetr_cli.cpp) own its field names and admission;
there is no second CLI-specific schema. Paths inside JSON must already identify
the inputs as visible to the runtime container.

`--resume PATH` supplies a [whole-session manifest](model-merging.md#whole-session-resume).
The scalar CLI does not implicitly restore all request settings; supply matching
compiled inputs, recipes, and policies. Inspection and GUI Prepare Resume retain
their distinct setting-restoration workflow. The selected deployment `.pt`
artifact remains the ordinary input to prediction/export.

### Benchmark cache selection

GUI and CLI benchmark compilation use the same compiler-owned precedence:

1. A nonempty explicit `BenchmarkCompilerConfig::cache_dir`.
2. A nonempty `MMLTK_BENCHMARK_DATASET_CACHE_ROOT` environment value.
3. The relative fallback `./.cache/benchmark-dataset/v1`.

The wrapper supplies `MMLTK_BENCHMARK_DATASET_CACHE_ROOT` as the container path
for `<MMLTK_CACHE_ROOT>/benchmark-dataset/v1`. `MMLTK_CACHE_ROOT` defaults to
the repository's `.cache` and may select a subtree there; relative values are
repository-relative. Configure that wrapper root to change the shared GUI/CLI
location. The supplied absolute path keeps subdirectory invocation from
accidentally selecting another cache. An empty environment value falls through
to the compiler's relative default when it is called without that wrapper value.

The CLI forwards its explicit `--cache-dir` to the compiler:

```bash
./mmltk rfdetr compile --compile-benchmark-dataset 432 \
  --output-dir ./compiled --cache-dir ./.cache/benchmark-dataset/v1
```

`--compile-benchmark-dataset` takes the square resolution. This CLI route uses
Coco custom; the GUI's [Dataset controls](gui-interaction.md#dataset-compilation-controls)
expose Coconut, its validation choices, and optional dropped-mask recovery.
There is no CLI recovery switch. `--cache-dir` and `--overwrite`
require benchmark mode. `--overwrite` replaces compiled output while preserving
the separate persistent source cache. The
[benchmark reference](benchmark-datasets.md#persistent-cache-and-publication)
owns cache reuse, output/cache overlap rejection, and publication behavior.

## Desktop options and environment

```bash
./mmltk --gui --device-id 0 --numa-node -1
./mmltk --gui --gdrcopy
```

Desktop startup accepts `--device-id`, `--numa-node`, and `--gdrcopy`.
Its device option selects native visual execution. The Train, Validate,
Export, and Predict cards retain their own compute selections; Firefox's
graphics device follows the Wayland session. The
[GPU execution guide](gpu-execution.md) explains selection, locality, and transport.
The wrapper discovers the active Wayland socket and forwards the matching
runtime/session paths. `XDG_RUNTIME_DIR`, `WAYLAND_DISPLAY`, and, when needed,
`__NV_PRIME_RENDER_OFFLOAD` can be supplied from the same working host session.
It runs display clients as the resolved host UID/GID; missing Wayland state
fails launch.

An ordinary launch leaves diagnostics and probes inactive. Use the explicit
environment settings in [logging](logging.md) to capture native or Firefox
output. GUI logging is environment-configured; the desktop execution parser
accepts only the device, NUMA, and GDRCopy options listed above.
The normal runtime image defaults to `mmltk`; `MMLTK_IMAGE` and
`MMLTK_BUILD_IMAGE` override runtime and build image names.

## ShiftLUT model tooling

```bash
./mmltk --export-shiftlut --source ../ShiftLUT --export-only
./mmltk --export-shiftlut --source ../ShiftLUT --verify-only
```

The source must contain `LUT_test/LUTs/ShiftLUT_sr_s7_int`. Export mode builds
the native generator in `.cache/cmake/shiftlut-export` and writes the
application's `ShiftLUT_fp32.onnx` asset. Verify mode uses the generator from
the existing Release graph and writes evidence under
`build/validation/atlas-viewer-residency/shiftlut`. This is model-generation
and verification tooling; it is separate from the ordinary application CLI.
