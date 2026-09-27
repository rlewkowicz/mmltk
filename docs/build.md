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
repository-scoped reusable container, while build, test, and image-based
diagnostics own their execution containers. The
[process snapshot](commands.md#process-snapshots) inspects existing wrapper
containers without creating one. GUI/runtime and Wayland acceptance
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

The [frontend CMake inputs](../src/frontend/iced/CMakeLists.txt) include Iced,
iced_aw, iced_plot, and `iced-fluent-theme` Rust sources/manifests; plot WGSL
also invalidates the bundle. Configured host Rust tests compile with `--no-run`,
separately from the optimized `trunk build --release` Wasm bundle. Execution and
package selection belong to [validation](validation.md#native-and-browser-suites).

Desktop startup resolves the native CLI beside `mmltk-browser-host` and gives
that executable to the training process owner. Keep both installed siblings in
the package; a browser bundle alone cannot supply local training.

`./mmltk --build-gui` formats/checks the owned Iced frontend and rebuilds only
the browser bundle in the GUI graph. It can be followed by `--gui` in the same
invocation. Native host and Firefox changes still require the full
`./mmltk --build` package operation.

Firefox source comes from `third_party/firefox`; the wrapper does not fetch a
replacement source checkout. Its host toolchain and sysroot have separate
cached preparation. Validation uses the repository's first-party browser
suites and packaged application acceptance, as described in [validation](validation.md).

The required Firefox runtime inventory is
[firefox_runtime_files.txt](../src/entrypoints/desktop/firefox_runtime_files.txt).
CMake derives runtime byproducts from it, and
[build_firefox_runtime.sh](../src/entrypoints/desktop/build_firefox_runtime.sh)
uses it for completeness and its successful-build fingerprint. That script
passes the required `--runtime-manifest` to Firefox's
[`mmltk-stage-runtime`](../third_party/firefox/python/mozbuild/mozbuild/mach_commands.py).
The staging command reads the manifest once, checks both the current and
candidate runtime against that inventory, and includes its bytes in the inner
staging fingerprint before atomic publication. The wrapper's outer Firefox
input fingerprint also includes the manifest.

## Toolchain and image inputs

First-party ordinary C++ uses GCC 16.2 and C++26 reflection. CUDA uses NVCC
13.4, CUDA C++23, and GCC 16.2 as the host compiler with the explicit
unsupported-host override. GCC 14 is confined to the GCC 16.2 bootstrap.
Explicit ONNX, simdjson, and cppcheck source builds use GCC 16.2 while retaining
their configured language policies. Vendored dependencies keep their own
policies; Firefox retains its cached Clang toolchain and bootstrap sysroot.
The frontend Cargo workspace includes the owned `third_party/iced_plot` and
`third_party/iced_aw` crates. Plot declares its native test/renderer dependencies
in its own manifest, including package-selected tests and doctests.

[CMakeLists.txt](../CMakeLists.txt) requires exactly CMake 4.4.3.
[MmltkToolchain.cmake](../cmake/MmltkToolchain.cmake) enforces compiler paths
and versions; [CMakePresets.json](../CMakePresets.json) defines the graphs.
First-party native links and owned Firefox links use mold; the Iced browser
artifact targets WebAssembly.
Ordinary host and CUDA host code use function/data sections for the native
garbage-collecting link. Release C/C++ uses the configured IPO policy.
The [native link diagnostics](validation.md#native-symbol-and-link-diagnostics)
inspect existing objects and repeat one generated link without replacing the
Release artifact.
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

### Native Parquet dependency

[Dockerfile.dependencies](../docker/Dockerfile.dependencies) pins Apache
Arrow/Parquet **25.0.1** and verifies the source archive's SHA-512. It builds
static, position-independent libraries with GCC 16.2 and upstream C++20 policy,
without applying first-party C++26 reflection settings to this dependency.
Bundled dependencies follow the pins in that Arrow source release.

The selected closure enables Parquet/IPC, Snappy, Zstandard, and zlib, with
xsimd, Thrift, and RapidJSON. Boost is a transitive build dependency in that
closure. Cloud filesystems, Arrow Dataset/Compute/Flight, Python, utilities,
examples, and Arrow tests/benchmarks are disabled; the build installs no
Arrow runtime DSO or replacement allocator.

[The data target](../src/backend/data/CMakeLists.txt) requires that exact package
from `/opt/arrow` and links `Arrow::arrow_static` and
`Parquet::parquet_static` privately. The native
[COCONut importer](benchmark-datasets.md#native-import-and-provenance) owns their
format use. Existing libcurl, libarchive, and rapidgzip owners retain download
and extraction responsibilities.

Arrow's licenses/notices and the bundled/transitive source notices install
under `share/mmltk/licenses/arrow`, including the six selected dependency
directories and the captured Boost notices. The
[runtime package inventory](../tools/runtime_package.sh) includes this tree at
`/opt/mmltk/share/mmltk/licenses/arrow`; [NOTICE](../NOTICE) records the attribution.

### Native caption and video assets

The [raster target](../src/backend/imaging/raster/CMakeLists.txt) embeds the
existing `src/frontend/iced/assets/SourceSansPro-Regular.otf` into generated
`caption_font_data.h` in that component's CMake build directory. The font is a
configure dependency; native saved-caption rendering has no runtime font-path
lookup. Its OFL installs under `share/licenses/mmltk/caption-font`.
The shared `stb` target includes unmodified stb_truetype 1.26; its header and
MIT license install under `share/licenses/mmltk/stb-truetype`. See the
[vendored addition note](../third_party/stb/README.mmltk.md). Application caption
policy remains in the raster owner, while Iced keeps interactive layout.

The [video target](../src/backend/media/video/CMakeLists.txt) consumes
`libavformat`, `libavcodec`, `libavutil` and `libswscale` through pkg-config.
Development installs their headers; both the development and independent
runtime base install FFmpeg and `libx264-164`. Native prediction Full-video
output selects `h264_nvenc` when admitted, with `libx264` as the pre-write
software fallback. A built runtime therefore carries the software encoder as
well as the GPU-dependent path; [workflow behavior](rfdetr-workflows.md#prediction-samples-and-full-video)
owns selection and settlement.

## Target declarations and precompiled headers

Component `CMakeLists.txt` files register sources, ordinary declaration headers,
retained module units, and dependency visibility through
[MmltkSourceRegistration.cmake](../cmake/MmltkSourceRegistration.cmake).
Declaration-only projections use the owner's compile usage requirements
without linking its implementation. Registered header-isolation targets
compile each header in a generated one-include translation unit using the
owner's usage requirements. They run as dependencies of that owner, with
unity and PCH disabled and no forced includes; selected boundaries also
register both include orders.

The normal product graph uses target-local PCHs for these ordinary C++ sources:

| Owner | Shared header contents |
| --- | --- |
| `mmltk_backend_data`, `mmltk_controller_direct_services` | `src/pch_std.h`, `src/pch_linux.h`, `src/pch_json.h` |
| `mmltk_backend_models_rfdetr_core`, `mmltk_backend_models_rfdetr_training` | `src/pch_std.h`, `src/pch_linux.h`, `src/pch_torch.h` |
| `mmltk_backend_ml_cuda` | `src/pch_std.h`, `src/pch_torch.h` |

Implementations using a compiled PCH include its shared headers explicitly
instead of repeating their member includes. The standard and Linux/POSIX groups collect headers
found in at least three distinct first-party files in the initial inventory.
The JSON group contains nlohmann JSON; the Torch group contains only C10 CUDA
stream/guard declarations. C++26-only standard headers are language-gated;
reflection also requires the compiler's reflection feature macro.

Declaration headers, CUDA, module global fragments and targets without a compiled
PCH retain their direct library includes. A header's `pch_` name alone does not
justify replacing narrow includes with it. Each registered owner creates its own compiled artifact with
its own compiler, options, definitions, and include environment. C, CUDA,
retained module providers/implementations/importers, header-isolation units,
and sources with distinct per-source compile settings do not consume it.
For example, ordinary `native_optimizer.cpp` uses the training owner's PCH;
that target's module importers do not.

`./mmltk --audit-includes` inventories includes and previews or applies replacement
from the checked-in groups. Reports default to `output/include-audit.{json,md}`.
Group membership is reviewed; it is not automatically pruned when direct includes
disappear after consolidation. Use `--help` for options.

[MmltkComponent.cmake](../cmake/MmltkComponent.cmake) derives
`CMakeFiles/<target>.dir/mmltk-pch-policy.txt` from the target registration.
The generated `cmake_pch.hxx` and `.gch` live beside it under that component's
build directory.
[check_toolchain_invariants.py](../tools/check_toolchain_invariants.py) checks
the compilation database against this policy, including creation/use
environment equality, exclusions, and fatal invalid-PCH diagnostics.
The analysis graph retains these registrations and independent header-isolation
checks; [tidy](validation.md#formatting-and-static-analysis) selects GCC objects
or clang-tidy by reflection support. PCH use and declaration isolation are part
of the normal product build, without a benchmarking gate or measured speedup claim.

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
| `.cache/benchmark-dataset/v1` | Persistent benchmark source archives, JPEGs, indexes, and completion metadata; see [cache ownership](benchmark-datasets.md#persistent-cache-and-publication) |
| `output/` | Repository-root product artifacts; automatic workflow roots and explicit destinations follow [workflow output policy](rfdetr-workflows.md#run-output-directories) |
| `build/release`, `build/browser-app` | Staged package and canonical browser bundle |
| `build/validation`, `build/logs` | Acceptance evidence and [build/test/tidy transcripts](logging.md#wrapper-build-test-and-tidy-transcripts) |
| `build/diagnostics` | Wrapper-owned capability, standalone GPU, and native-link diagnostic artifacts |

`MMLTK_CACHE_ROOT` may select a subtree of `.cache`; `MMLTK_RELEASE_STAGE_ROOT`
must remain below `build`. The wrapper rejects overlapping checkout/cache-root
mutations. Retain caches for incremental builds.
The root artifact ignore rule is `/output/`. Build products, dependency caches,
test scratch, and diagnostics retain their independent locations; explicit
user destinations are not redirected into `output/`.

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

Both generation commands select `mmltk_protocol_v17_generation` and produce
the bindings, package marker, graphics ABI, and both cross-language fixtures.
`--generate-application-bindings` uses the dedicated
`.cache/cmake/application-bindings-release` graph with browser-host and
Firefox runtime builds disabled. `--generate-protocol` uses the shared
`.cache/cmake/release` graph.

Under the selected graph's `generated/frontend/iced/`, generation owns:

| Artifact | Purpose |
| --- | --- |
| `application_bindings.rs` | Typed native domain projection, codecs, schema fingerprint, interaction limits, and reflected metric scalar selectors |
| `browser_protocol.marker` | Application package marker for the [current typed boundary](gui-interaction.md#typed-application-boundary), `MMLTK_HOST_API_PROTOCOL_17` |
| `protocol_v17_client_records.hex` | Rust-to-native application fixture |
| `protocol_v17_server_records.cbor` | Native-to-Rust application fixture |
| `workspace_graphics_abi.rs` | Data-only native graphics ABI projection for Firefox; current ABI version 14 |

Generation alone neither repackages the host/bundle nor runs tests; the
[typed boundary](gui-interaction.md#typed-application-boundary) requires matching
packaged artifacts.

The graphics artifact derives records, field types, enum wire values, and
field-offset/size/alignment assertions from the native workspace import and
frame-signal declarations in [presentation/abi](../src/controller/presentation/abi).
Firefox includes it through `MMLTK_WORKSPACE_GRAPHICS_ABI`. CMake makes its
generation a direct Firefox build dependency and owns regeneration from the
native declarations and generator inputs.

The wrapper's `.cache/firefox/build-input.sha256` uses the version-3 outer
source key. It includes the dependency toolchain image identity,
`firefox_runtime_build.cmake`, `build_firefox_runtime.sh`, the runtime manifest,
the committed `third_party/firefox` tree, its tracked working-tree diff, and its
nonignored untracked files. Native declarations, the application generator, and
its CMake rules are handled by the generation graph rather than duplicated in
that outer source key.

`build_firefox_runtime.sh` combines that key with the runtime-manifest hash and
the emitted graphics ABI's content hash in
`.cache/firefox/obj-minimal-opt/.mmltk-build-input.sha256`. It skips Mach build
and staging only when this guard matches and every manifest entry exists.
Changed emitted ABI bytes therefore invalidate the guard even when Firefox's
source is unchanged; an unrelated generator edit producing identical ABI bytes
does not itself require Firefox compilation. Missing outputs or changed Firefox,
toolchain, build-script, manifest, or ABI inputs can still require work. These
guards retain the existing Firefox objdir, Clang/bootstrap sysroot, Cargo, and
sccache state.

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
