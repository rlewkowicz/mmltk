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
| `./mmltk --gui` | Launch the packaged browser host with the canonical bundle |
| `./mmltk --prepare-gui-container` | Prepare the GUI runtime container without starting the app |
| `./mmltk --generate-application-bindings` | Generate typed native-to-Rust application bindings |
| `./mmltk --generate-protocol` | Generate protocol bindings and cross-language fixtures |
| `./mmltk --update-gui-lock` | Refresh the Cargo lock inputs for browser tests |
| `./mmltk --tidy` | Run configured formatting and static analysis |
| `./mmltk --cleanup-report cpp\|frontend\|all` | Generate the selected deduplication reports |
| `./mmltk --test list` | List supported suites and test options |
| `./mmltk --logs --help` | Show log-query grammar and options |
| `./mmltk --diagnose-io FILE` | Report a compiled file's storage/GPU capabilities |
| `./mmltk --diagnose-nvidia-payload donor\|development HEADER...` | Inspect public NVIDIA headers and payload selection |

The `|` entries above mean choose one value; they are not shell pipelines.
Build, test, tidy, cleanup, export, and diagnostics are separate operations.
Put `--logs`, `--diagnose-io`, `--diagnose-nvidia-payload`, or `--cleanup-report`
first when invoking that standalone operation.

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
```

The wrapper prevents raw native CLI invocation while its GUI is active,
including `--help`. Close that GUI before invoking the native CLI. Command
declarations remain available in the source links below.

Top-level commands compile source datasets, inspect compiled metadata,
benchmark loading, or dispatch RF-DETR work. RF-DETR exposes `compile`, `info`,
`build-engine`, `export-onnx`, `predict`, `evaluate`, `validate`, `train`, and
`normalize-weights`. Their options come from
[rfdetr_cli.cpp](../src/entrypoints/cli/rfdetr_cli.cpp) and the native contract;
use command help for required fields and current defaults.

For example, with existing input artifacts:

```bash
./mmltk rfdetr predict \
  --compiled ./compiled/val.bin \
  --weights ./checkpoints/checkpoint_best_regular.pt \
  --output ./predictions.json
```

See [datasets](datasets.md) for compilation and annotation requirements.
The native CLI also accepts `--log-level`, `--log-file`, and `--log-dir`.
See [logging activation](logging.md#activation-and-quiet-execution), especially
for ONNX metadata commands whose output requires explicit diagnostics.

## Desktop options and environment

```bash
./mmltk --gui --device-id 0 --numa-node -1
./mmltk --gui --gdrcopy
```

Desktop startup accepts `--device-id`, `--numa-node`, and `--gdrcopy`.
The [GPU execution guide](gpu-execution.md) explains locality and transport.
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
