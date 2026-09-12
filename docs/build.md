# Building and reusable state

[Wiki index](README.md) · [Quick start](../README.md#build) · [Commands](commands.md) · [Validation](validation.md)

## Host and container boundary

Build and run through [the repository wrapper](../mmltk). The supported target
is Linux with NVIDIA CUDA; the desktop additionally requires Wayland. Docker
must be usable by the invoking account, with Buildx available for image builds
and NVIDIA GPU access available to runtime/test containers. The wrapper probes
Docker and attempts a noninteractive daemon start for ordinary build/runtime
operations. Read-only diagnostic commands require an already-running daemon.

The wrapper builds the toolchains in containers. Its normal runtime uses a
repository-scoped reusable container, while build, test, and diagnostic
operations own their execution containers. GUI/runtime and Wayland acceptance
use privileged containers and host resource mounts. Log and I/O diagnostic
containers use restricted, read-only mounts; see [logging](logging.md) and
[GPU execution](gpu-execution.md).

## Release builds

```bash
./mmltk --build
```

`--build` configures the Release CMake/Ninja graph, builds and installs native
deliverables, publishes the Iced bundle, builds the owned Firefox runtime, and
assembles the runtime image. The toolchain/Release branch and the independently
fingerprinted runtime-base branch can run concurrently before packaging.
Within Ninja, native compilation, the browser bundle, and the coarse Firefox
edge can overlap. Firefox runs its own Mach/RecursiveMake/Cargo build.

The build publishes the canonical bundle at `build/browser-app/dist`; every
`--gui` launch mounts that bundle read-only at the packaged browser-app
location. A missing or incompatible bundle fails launch, so rebuild after a
native contract change.

`./mmltk --build-gui` formats/checks the owned Iced frontend and rebuilds only
the browser bundle in the GUI graph. It can be followed by `--gui` in the same
invocation. Native host and Firefox changes still require the full
`./mmltk --build` package operation.

Firefox source comes from `third_party/firefox`; the wrapper does not fetch a
replacement source checkout. Its host toolchain and sysroot have separate
cached preparation. Validation uses the repository's first-party browser
suites and packaged application acceptance, as described in [validation](validation.md).

## Toolchain and image inputs

First-party ordinary C++ uses GCC 16.2 and C++26 reflection. CUDA uses NVCC
13.4, CUDA C++23, and GCC 16.2 as the host compiler with the explicit
unsupported-host override. GCC 14 is confined to the GCC 16.2 bootstrap.
Explicit ONNX, simdjson, and cppcheck source builds use GCC 16.2 while retaining
their configured language policies. Vendored dependencies keep their own
policies; Firefox retains its cached Clang toolchain and bootstrap sysroot.

[CMakeLists.txt](../CMakeLists.txt) requires exactly CMake 4.4.3.
[MmltkToolchain.cmake](../cmake/MmltkToolchain.cmake) enforces compiler paths
and versions; [CMakePresets.json](../CMakePresets.json) defines the graphs.
First-party native links and owned Firefox links use mold; the Iced browser
artifact targets WebAssembly.
Core compilation uses ccache; Firefox retains its canonical objdir, Cargo
dependency information, and sccache.
The container supplies Rust, `wasm32-unknown-unknown`, Trunk, wasm-bindgen,
wasm-opt, and the other configured build tools.

Development and runtime bases are independent Ubuntu 24.04 stages.
[docker/nvidia-payload.json](../docker/nvidia-payload.json) is authoritative for
the digest-pinned NGC donor, selected CUDA/Torch/TensorRT/Python libraries,
normalized paths, dependencies, and notices. Its selected closure includes
NCCL and Torch's CPU MKL libraries; ONNX Runtime is packaged separately.
Image and staged-output fingerprints govern reuse of the final runtime.

ONNX Runtime 1.27.1 is built by
[Dockerfile.onnxruntime](../docker/Dockerfile.onnxruntime). Its
[CUDA graph capture patch](../docker/patches/onnxruntime-1.27.1-cuda-graph-capture.patch)
and GCC compatibility patch are inputs to the wrapper's dependency
fingerprint. The capture patch owns both thread-local capture and capture-end
RAII cleanup; [GPU execution](gpu-execution.md#onnx-capture-and-verification-storage)
explains the independent-worker behavior. The dependency retains its own
C++/CUDA language policies.

Inspect public header selection with:

```bash
./mmltk --diagnose-nvidia-payload donor cudalibxt.h cufftXt.h
./mmltk --diagnose-nvidia-payload development cudalibxt.h cufftXt.h
```

`donor` uses BuildKit and writes its report under
`build/diagnostics/nvidia-payload`; `development` inspects an existing build
image without rebuilding it. Supply public filenames, not directory paths.
Reports include locations, package ownership, direct includes, and selection.

## Cache and output locations

These are the default repository-local locations:

| Path | Contents |
| --- | --- |
| `.cache/cmake/release` | Shared Release build/test graph |
| `.cache/cmake/gui`, `.cache/cmake/dev`, `.cache/cmake/analysis` | Other wrapper-selected graphs |
| `.cache/cargo/home`, `.cache/cargo/target/browser-app` | Cargo downloads and browser artifacts |
| `.cache/ccache` | Core compiler results |
| `.cache/firefox/obj-minimal-opt`, `mozbuild`, `sccache` | Firefox objects, toolchain/sysroot state, compiler cache |
| `.cache/browser-app` | Shared bundle fingerprints and publication state |
| `.cache/buildkit` | Exported image-build caches |
| `.cache/image-fingerprints`, `.cache/locks` | Verified identities and mutation locks |
| `.cache/tests/rfdetr` | RF-DETR test assets and derived artifacts |
| `build/release`, `build/browser-app` | Staged package and canonical browser bundle |
| `build/validation`, `build/logs` | Acceptance evidence and build/analysis logs |

`MMLTK_CACHE_ROOT` may select a subtree of `.cache`; `MMLTK_RELEASE_STAGE_ROOT`
must remain below `build`. The wrapper rejects overlapping checkout/cache-root
mutations. Retain caches for incremental builds.

`release` configures package and test targets; native tests are excluded from
the default build. `gui` selects browser work, while `dev` and `analysis`
provide non-package development graphs. A `tsan` configure preset exists, but
`./mmltk --test tsan` is currently rejected. Use the supported wrapper
operations in [validation](validation.md).

## Generated bindings and dependency maintenance

```bash
./mmltk --generate-application-bindings
./mmltk --generate-protocol
./mmltk --update-gui-lock
./mmltk --update-firefox-lock
```

The first builds typed application bindings in its dedicated
`.cache/cmake/application-bindings-release` graph. The second selects the
Release protocol-generation target and its cross-language fixtures.

Under the selected graph's `generated/frontend/iced/`, generation owns:

| Artifact | Purpose |
| --- | --- |
| `application_bindings.rs` | Typed native domain projection, codecs, schema fingerprint, and interaction limits |
| `browser_protocol.marker` | Application package marker for the [current typed boundary](gui-interaction.md#typed-application-boundary), `MMLTK_HOST_API_PROTOCOL_16` |
| `protocol_v16_client_records.hex` | Rust-to-native application fixture |
| `protocol_v16_server_records.cbor` | Native-to-Rust application fixture |
| `workspace_graphics_abi.rs` | Data-only native graphics ABI projection for Firefox; current ABI version 13 |

Application-binding generation produces the bindings, marker, and graphics
artifact; protocol generation also produces the two application fixtures.
The current [typed boundary](gui-interaction.md#typed-application-boundary) must be
packaged together with the native host and browser bundle. Generating
bindings alone does not update the complete runtime package.

The graphics artifact derives records, enum wire values, field offsets, sizes,
and alignments from the native workspace import and frame-signal declarations.
Firefox includes it through `MMLTK_WORKSPACE_GRAPHICS_ABI`. CMake makes its
generation a direct Firefox build dependency. The wrapper's
`.cache/firefox/build-input.sha256` includes the canonical graphics declarations,
emitter, generator, and generation build rules as well as the Firefox source
and toolchain identity. The successful build stamp at
`.cache/firefox/obj-minimal-opt/.mmltk-build-input.sha256` also includes the
generated artifact's content hash. A native graphics change therefore
invalidates Firefox reuse even when `third_party/firefox` itself is unchanged.
This boundary does not change Firefox's cached Clang/bootstrap-sysroot policy.

Generated Rust stays in build output: change canonical native declarations
or generators, then regenerate. `--update-gui-lock` performs the containerized
Cargo fetch used to refresh the native browser-test dependency lock; ordinary
builds use the locked dependency set.
`--update-firefox-lock` runs offline Cargo metadata against Firefox's vendored
sources and `.cargo/config.toml.in`; it updates that dependency lock without
replacing the source checkout or running Firefox tests.

The private [Wayland validation image](headless-wayland.md) is separately
fingerprinted from the packaged runtime and build images. It adds Weston and
a validation-only input-seat module without replacing the application or
Firefox. Its builder and runtime use the same pinned Weston package version.

## Isolated build timing

[build_time_trace.sh](../tools/build_time_trace.sh) invokes `./mmltk --build`
with isolated cache, image, builder, and staging identities:

```bash
./tools/build_time_trace.sh --cache-key comparison --label cold
./tools/build_time_trace.sh --cache-key comparison --warm-rebuild --label warm
./tools/build_time_trace.sh --cache-key comparison --no-op --label noop
```

Omit `--cache-key` for a fresh cold cache. Warm/no-op modes require a completed
prior run with that key and are mutually exclusive. `--warm-rebuild` removes
only that key's CMake tree, Firefox objdir, and staged products, retaining its
compiler/Cargo/mozbuild/browser/BuildKit caches. The normal caches are unchanged.

Reports live under `build/time-trace/<key>/runs`; isolated reusable state lives
under `.cache/build-time-trace/<key>`. Reports include phase/stage events,
configure reuse, job allocation, build overlap, compiler-cache statistics,
browser/Rust work, package-cache outcomes, and runtime layer sizes. Each key
has its own overlap lock.
