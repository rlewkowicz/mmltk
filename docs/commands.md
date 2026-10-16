# Command reference

[Wiki index](README.md) · [Quick start](../README.md#build) · [Build details](build.md) · [Validation](validation.md)

Run from the repository root through native `./mmltk`; cold checkouts need
[bootstrap](build.md#native-tools-and-checkout-bootstrap). The checkout mounts at
`/workspace`. Supported host path arguments are rewritten to
`/host/<absolute-host-path>`, resolving relative paths from the invocation directory.
Paths inside canonical JSON or explicit test `--env` values must already be
container-visible. Runtime containers are repository-labeled and stream app output.

## Wrapper operations

| Invocation | Purpose |
| --- | --- |
| `./mmltk --build-tools` | Build and atomically publish the standalone native driver, container copy, fixture runner, and library closure |
| `./mmltk --tidy-tools [--file PATH...] [--start-at PATH]` | Format and validate the registered standalone tools graph; file and resume selections are mutually exclusive |
| `./mmltk --build` | Ensure current tools, then build and package the Release runtime |
| `./mmltk --build-gui` | Format/check Iced and rebuild the canonical browser bundle in the GUI graph |
| `./mmltk --gui` | Replace the current runtime session and launch the packaged browser host |
| `./mmltk --prepare-gui-container` | Prepare the GUI runtime container without starting the app |
| `./mmltk --generate-application-bindings` | Generate bindings, marker, graphics ABI, and cross-language fixtures in the dedicated generation graph |
| `./mmltk --generate-protocol` | Generate the same artifacts in the shared Release graph |
| `./mmltk --update-gui-lock` | Refresh the Cargo lock inputs for browser tests |
| `./mmltk --update-firefox-lock` | Refresh Firefox's Cargo lock from its vendored sources, offline |
| `./mmltk --tidy` | Run the tools stage, format native sources and Iced application Rust, and run configured product analysis |
| `./mmltk --cleanup-report cpp\|frontend\|tools\|all` | Generate selected deduplication reports; `all` includes all three profiles |
| `./mmltk --raw-cpd [--tools] [--review] [--min-tokens N] [--output PATH]` | Save exact-spelling C++ CPD evidence; `--tools` scopes it to native tools, and `--review` adds an authored-file queue and lexical families in JSON/Markdown |
| `./mmltk --format-declarations check\|preview\|fix [--file PATH] [--report PATH]` | Report or safely shorten canonical reflection annotation syntax |
| `./mmltk --audit-includes [--refresh] [report\|preview\|fix] [--output PREFIX] [--build-dir PATH]` | Inventory all compiler contexts and include-retention reasons; preview/apply replacements only under verified target-local PCH policy |
| `./mmltk --test list` | List supported suites and test options |
| `./mmltk --test all` | Run ordinary native, browser JavaScript/Rust, cleanup/declaration, log-query, and compositor fixtures |
| `./mmltk --test rfdetr --with-upstream` | Explicitly prepare and run external upstream and historical-session validation; full reference tensors can exceed 1 TiB |
| `./mmltk --test rfdetr --with-upstream=encoder-pretraining` | Prepare only original encoder/reference evidence and run the selected suite; no full detector or historical-session preparation |
| `./mmltk --test data-profile -- --help` | Prepare the opt-in target and show CPU compile/pipeline/archive/compaction/resample, full Coconut, and CUDA loader options |
| `./mmltk --test cuda-vulkan -- --help` | Build/select the standalone CUDA/Vulkan diagnostic and show its positional options |
| `./mmltk --logs --help` | Show log-query grammar and options |
| `./mmltk --diagnose-processes [WRAPPER_MODE]` | Inspect processes in this repository's running wrapper containers |
| `./mmltk --diagnose-benchmark-image --image-id ID [OPTIONS]` | Compare retained image headers with normalized annotation geometry |
| `./mmltk --diagnose-io FILE` | Report eligible CPU/core/SMT/frequency-policy facts and a compiled file's storage/GPU capabilities |
| `./mmltk --diagnose-gpu-environment runtime\|wayland-validation\|development` | Inspect an existing image's GPU, driver, library, and ICD environment |
| `./mmltk --diagnose-gpu-program [--torch] SOURCE.cpp ARGS...` | Compile and run a standalone CUDA-driver/Vulkan diagnostic, optionally linked to pinned PyTorch, using existing images |
| `./mmltk --diagnose-rfdetr-upstream [--upstream PATH] [--plus-upstream PATH] [--output PATH] [--sdpa default\|math]` | Explicit pinned references plus core/training/inference/export comparisons; selected `--case ID` prepares one case without comparisons |
| `./mmltk --diagnose-rfdetr-upstream --encoder-pretraining-only [--output PATH]` | Prepare three original encoder assets, bounded references for all 16 presets and independent controls; native comparisons run separately |
| `./mmltk --diagnose-nvidia-payload donor\|development HEADER...` | Inspect public NVIDIA headers and payload selection |
| `./mmltk --diagnose-native-symbols [OPTIONS] ARTIFACT...` | Inspect cached native `.a`/`.o` symbols or resolve `--address HEX` values in one ELF artifact with the existing development image |
| `./mmltk --diagnose-native-link [OPTIONS] TARGET` | Repeat one generated Release link into separate diagnostic storage |
| `./mmltk --tool build-time-trace -- [OPTIONS]` | Run an [isolated build measurement](build.md#isolated-build-timing) through the native driver |

`|` means choose a value, not a shell pipeline. Build, test, tidy, cleanup,
export, and diagnostics are separate operations. Standalone `--logs`,
`--diagnose-*`, `--cleanup-report`, `--raw-cpd`, `--format-declarations`, and
`--audit-includes` must be the first operation.

| Detail | Reference |
| --- | --- |
| Selectors, profiles, CUDA/Vulkan, native symbols/links | [Validation](validation.md) |
| Upstream opt-in, single-case preparation, retention/verdicts | [Upstream validation](validation.md#independent-upstream-diagnostic), [encoder evidence](validation.md#encoder-pretraining-evidence) |
| Cleanup/declaration/PCH tools | [Validation tooling](validation.md#deduplication-reports) |
| Log queries and capture provenance | [Logging](logging.md) |

Build/tools-build/test/tidy automatically retain [transcripts](logging.md#wrapper-build-test-and-tidy-transcripts).
Overrides: `MMLTK_BUILD_LOG_FILE`, `MMLTK_TEST_LOG_FILE`, `MMLTK_TIDY_LOG_FILE`;
relative paths use the invocation directory. Test help/list skips capture. New
`--test all`, including filtered runs, [replaces earlier checkout-owned tests](validation.md#replacing-an-active-test-run).

### Native tool entrypoints

The wrapper/CMake invoke compiled modules as
`mmltk --tool COMMAND [--argv0 LOGICAL] -- ARGS...` in their owning process or
container. [Reflected vocabulary/dispatch](build.md#composition-and-fixture-isolation)
own names and exhaustive entrypoint selection; logical names may retain script
suffixes without identifying script files. Public operations supply image
admission, paths, mounts, limits, ownership, and transcripts. Build timing is an
orchestrator calling `./mmltk --build`.

[Build ownership](build.md#standalone-tools-module-graph) covers process/plugin
boundaries; [fixtures](validation.md#native-tool-fixtures) map six commands to
public suites and the separate runner.

### GUI session replacement

`--gui` retires earlier GUI/raw app invocations from this checkout before
launching. Admission is serialized; the GUI holds the runtime lifetime lock.
An older delayed invocation cannot replace a newer admitted runtime.

`.cache/locks/runtime-run-*.owner` covers pre-container startup; labels also
identify earlier container sessions. Matching uses repository, mode, PID, and
process-start identity. Replacement requests graceful stop, waits with bounded
force if needed, then verifies both containers and lock retired. Exit removes
registration.

Builds, tests, other checkouts, and unrelated processes are excluded. Raw apps
may coexist; new raw execution and `--prepare-gui-container` are refused while
the GUI occupies runtime.

### Process snapshots

```bash
./mmltk --diagnose-processes --help
./mmltk --diagnose-processes
./mmltk --diagnose-processes build
./mmltk --diagnose-processes test
```

Default selects this checkout's running wrapper containers; an optional mode
matches its exact label (`test` covers compilation and execution). Each prints
container/mode, PID/PPID, elapsed/CPU time, CPU/memory percentages, state, wait
channel, and process name; arguments/environment are omitted. No match succeeds
silently. A snapshot alone proves neither stall, completion, nor performance.

Read-only snapshots require a running daemon, with 15 seconds per query: no
daemon start, container creation/mutation/attachment, or image build/pull.
Explicit actions require exactly one container and a running `mmltk` or
`mmltk[_-][A-Za-z0-9_-]+` executable, verified through `/proc/PID/exe`:

```bash
./mmltk --diagnose-processes test --backtrace mmltk_backend_models_rfdetr_training_tests
./mmltk --diagnose-processes test --read-file mmltk_backend_models_rfdetr_training_tests --file /workspace/build/example.json
./mmltk --diagnose-processes test --terminate mmltk_backend_models_rfdetr_training_tests --pid 123
```

| Action | Behavior |
| --- | --- |
| `--backtrace NAME` | Root GDB attach, all threads, detach; 35-second debugger limit |
| `--read-file NAME --file PATH` | Absolute regular file through selected process root; escaped bytes, ≤4 MiB |
| `--terminate NAME` | pidfd SIGTERM; can fail the interrupted operation |

Optional `--pid` is a **container PID** selector. Actions take ≤45 seconds,
build/create no container, and are distinct from read-only snapshots.

### Benchmark image geometry

```bash
./mmltk --diagnose-benchmark-image --help
./mmltk --diagnose-benchmark-image --image-id 2212 \
  --index .cache/benchmark-dataset/v1/indexes/coco/train2017.normalized.bin \
  --export-mask-runs
```

Inputs are repository-relative; select a numeric `--image-id`, or
`--compiled PATH --sample N`. Compiled `N` is the zero-based physical/displayed
ID, not filtered position or eligible-image ordinal. Inactive/out-of-range
slots and unsupported formats fail. Records retain `image_count`, physical
`slot_count`, used `label_bytes`, and header `max_label_bytes`; `./mmltk info
--compiled PATH` also prints the maximum complete label payload.

| Option | Inspection |
| --- | --- |
| Repeat `--index PATH` | Version-3 normalized rows; `--objects` adds objects |
| `--image PATH`, `--archive PATH` | Cached image or retained tar member; dimensions, identity, SHA-256, member, JPEG EXIF orientation. Header geometry does not prove decodability |
| `--parquet PATH`, `--panoptic PATH` | Original COCONut PNG/segment metadata or extracted panoptic PNG; exact RGB IDs and declared segment pixel support |
| `--profile-compiled WORKERS` | Trusted shared open, 1–64 workers (one runs on caller); nanoseconds, recorded totals, actual affinity/NUMA/nice. Copies/aggregates index without payload/per-sample verification |
| `--cold-compiled` | Request eviction for this file only, without global cache drop or data changes |
| `--export-mask-runs` | Exactly one index; `build/validation/benchmark-image/ID.originals.json` with identity, geometry, flags, source IDs/ordinals, area, exact row-major runs; ≤65,535 objects/runs per object and ≤1,048,576 runs per image |
| `--export` | Replace diagnostic `ID.jpg`/`.png`; mask inspection also writes exact `ID.mask.png` and colored `ID.segments.png` |

The tool never changes source cache or enables [mask recovery](benchmark-datasets.md#optional-dropped-mask-recovery).
Tar scans are sequential; each image is ≤64 MiB. Existing-development-image
inspection uses read-only mounts, no network/build/pull, and a ten-minute limit.
Profiling permits memory-policy inspection/worker priority. Without profiling,
only the selected sample is read.

Only compiled/Parquet/panoptic requests prepare [CMake-owned helpers](build.md#benchmark-diagnostic-builds)
in a reusable separate graph, logging preparation to stderr and JSONL to stdout.
Compiled helpers use checked Dataset layout/trusted annotations; masks use native
Arrow/Parquet and vendored PNG. Help/index/image/archive need no product graph,
Python package, or new dependency.

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

Root compile/info/bench options come from [cli_options.h](../src/entrypoints/cli/cli_options.h).
RF-DETR exposes `compile`, `info`, `build-engine`, `export-onnx`, `predict`,
`evaluate`, `validate`, `train`, and `normalize-weights`, declared in
[rfdetr_cli_options.h](../src/entrypoints/cli/rfdetr_cli_options.h). Execution
stays with corresponding `.cpp` owners. Command help owns required fields and
defaults; [declaration authoring](reflection.md#compact-cli-declarations) covers
names, exceptions, aliases, negation, and exposure audits.

For example, with existing input artifacts:

```bash
./mmltk rfdetr predict \
  --gpu 0 \
  --compiled ./compiled/val.bin \
  --weights ./checkpoints/model.pt \
  --output ./predictions.json
```

Use `--gpu N` on `bench` and RF-DETR device-selecting operations to select the
CUDA-visible device index (default `0`). It accepts `--gpu=N` too, and shares
the existing `--device-id` selection for model operations. Both `compile`
commands accept it as an alias of `--cuda-device-id`; current image and mask
compilation remains CPU work. Multi-GPU training retains `--device-ids 0,1`,
which is mutually exclusive with `--gpu`. The
[GPU guide](gpu-execution.md#workflow-device-selection) explains device numbering.

Key option groups (required fields and defaults remain in command help):

| Commands | Options and reference |
| --- | --- |
| `compile`, `rfdetr compile` | `--resize-mode Stretch` (default) or `Letterbox`; `--resize-method Native\|TorchBilinear\|TorchBilinearAntialias`; independent `--perceptual-downscale` applies to Native; [source/geometry rules](datasets.md#compile-and-inspect) |
| `rfdetr validate` | `--resize-mode` and `--resize-method` affect source compilation only; existing bins retain stored pixels and geometry |
| `rfdetr train` | Independent `--aug-perceptual-downscale`, `--use-ema`/`--no-ema`, `--resume`, `--output-dir`; `--test-compiled` is optional, train/validation required |
| Model-input commands | `--class-layout` supplies a [digest-bound descriptor](rfdetr-workflows.md#class-identity-and-model-admission) |
| `rfdetr predict` | Repeatable `--image` or `--compiled`, CLI `--batch-size`; `--resize-method Native\|TorchBilinear\|TorchBilinearAntialias` applies to image files; local video is GUI-only |
| `rfdetr evaluate`, `validate` | One backend or ordered multi-backend report, respectively; independent `--candidate-count` and `--eval-max-dets` |
| Train / Predict counts | Train exposes `--eval-max-dets`; Predict uses `--max-dets-per-image`; [count semantics and zero defaults](rfdetr-workflows.md#model-input-and-detection-selection) |

Both compile routes share `--workers` across live work classes and file/Curl
services within eligible CPUs; it counts neither per-source pools nor a memory
cap. [Compilation](compile-execution.md) owns scheduling, checked pinning, and
exceptional inputs. `bench` measures compiled loading; [data-profile](validation.md#data-compilation-and-resampling-profiles)
measures compilation. Full Coconut requires explicit mode/cache selection and
may acquire sources; ordinary `all` omits it.

[RF-DETR workflows](rfdetr-workflows.md) owns checkpoint, metric, and history behavior.
Train, predict and evaluate accept `--compile-mode none|selective|full`.
The [native compilation contract](rfdetr-training.md#native-selective-compilation)
describes the default selective path, shared guards, FullTrace restrictions,
and inference propagation. CLI `export-onnx` and
`build-engine` keep explicit destinations. GUI Export combines independent
ONNX/TensorRT selections through one conversion and reserves its output beside
the other [workflow runs](rfdetr-workflows.md#run-output-directories).

The native CLI also accepts `--log-level`, `--log-file`, and `--log-dir`.
See [logging activation](logging.md#activation-and-quiet-execution), especially
for ONNX metadata commands whose output requires explicit diagnostics.
Fatal operation failures still produce a concise
[stderr report](logging.md#fatal-stderr-reports) with diagnostics disabled.

### Training request selection

Choose exactly one source: `--from-scratch --preset NAME`, `--weights PATH`, or
`--resume PATH`. Scratch requires a known catalog preset and rejects detector
weights, Resume and `--class-layout` in the same request. For example:

```bash
./mmltk rfdetr train --from-scratch --preset rf-detr-atto \
  --train-compiled ./compiled/train.bin --val-compiled ./compiled/val.bin \
  --output-dir ./output/train/scratch
```

This acquires the preset's verified base encoder and initializes new detector
weights. The [workflow guide](rfdetr-workflows.md#training-initialization) owns
mode semantics. There is no public scalar encoder-path flag: acquisition resolves
`TrainRequest::encoder_weights_path` for launch, and the native JSON boundary
can carry an already resolved path that must pass the same asset integrity checks.
Invalid or conflicting source choices fail before acquisition. The internal
path is not saved in reusable requests.

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
./mmltk --gui --gpu 0 --numa-node -1
./mmltk --gui --gdrcopy
```

Desktop startup accepts `--gpu` (also `--device-id`), `--numa-node`, and `--gdrcopy`.
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
