# Repository Guidelines

## Project Goal
To build the fastest dataloader for machine learning ever made.

## Project Structure & Module Organization
`include/` contains public headers and binary-format structs. `src/` holds the core C++/CUDA implementation for compilation, mmap loading, streaming, the CLI, and the native RF-DETR runtime. `tests/` contains the roundtrip, worker-pool, RF-DETR parity/integration, and profile harnesses. `utilities/` contains local build/profile/validation helpers, and `third_party/` vendors header-only dependencies (`stb`, `nlohmann/json`). Keep generated artifacts under `build/` or dataset output folders such as `compiled/`; do not commit them.

## Build, Test, and Development Commands
Use CMake with Ninja for native builds when your host compiler is supported by the installed CUDA toolkit:

```bash
./utilities/install_local_rfdetr_deps.sh
cmake --preset release --fresh
cmake --build --preset release -j"$(nproc)"

cmake --preset dev --fresh
cmake --build --preset dev -j"$(nproc)"
ctest --preset dev
```

`BUILD_RFDETR_NATIVE` is the native RF-DETR toggle. Upstream `.pth` parsing is controlled separately by `-DBUILD_RFDETR_PYTHON_CHECKPOINT_LOADER=ON`. Keep repo-local builds under `build/release` and `build/dev`; do not create ad hoc root-level build trees.
`release` is the optimized runtime build. `dev` is the only development build and carries tests plus profiling helpers.

For native RF-DETR validation and the real validation profile flow, the public knob is `batch_size`. ONNX/TensorRT interpret it as synthetic width backed by internal lane parallelism; the checkpoint worker uses it as the real PyTorch batch size.

## Coding Style & Naming Conventions
Follow the existing style: 4-space indentation, braces on the same line, and concise comments only where behavior is non-obvious. Keep types and classes in `PascalCase` (`DatasetLoader`), methods in `snake_case` (`begin_epoch`), and config structs nested near their owners. Match current file naming: implementation files in `src/` use lower_snake_case, and tests use `test_*.cpp`. Keep files modular, highly classed, keep code DRY. Break files into logical functional boundaries. Stay terse, write high quality, production ready code. Minimize LOC without sacrificing functionality.

## Testing Guidelines
Add or extend end-to-end tests in `tests/` when changing binary layout, mmap behavior, batching, CLI behavior, embedded checkpoint parsing, or the RF-DETR runtime. Prefer assertions that validate observable behavior: file generation, metadata, batch contents, zero-copy memory sharing, CLI output, value ranges, and profile metric presence. Run `ctest --preset dev --output-on-failure` for the native suite, `./utilities/run_static_analysis.sh build/dev` for the aggressive `clang-tidy`/`cppcheck`/`valgrind`/`vulture` pass, and keep `tests/test_worker_pool.cpp`, `tests/test_rfdetr_checkpoint.cpp`, `tests/test_rfdetr_native_integration.cpp`, and the native RF-DETR coverage green when touching the corresponding surfaces. The Valgrind pass runs on `test_roundtrip`, so use the existing shared harness rather than adding one-off memory-check binaries unless the main harness cannot cover the code path. Linux-only is intentional; do not preserve portability warnings that do not matter on Linux. Build and test locally. Ensure tests stay DRY. Build re-usable repeatable harnesses that reduce specialty test cases and construction.

## Optimization
**IMPORTANT**: Always optimize (O)n. We always want to reduce loops, reduce cpu/gpu/memory churn. Realistically, mostly all application memory should be pinned. Most buffers should exist for the duration of the application and reused aggressively. When it makes sense (only when it makes sense), optimize datatypes as well. Generally we want small well defined buffers designed to hold the minimal memory size needed to accomplish the goal. Parallelism is super important. When it makes sense (only when it makes sense), ensure minimal blocking and maximize parallelism.

## Portability
This is a linux only data system. You never need to worry about compatibility with MACOS or Windows. You always want to optimize maximally for linux systems.

## Legacy Code
If a request is made to change a functional outcome, remove legacy code. Always fail loud. Do not add silent or quiet fallbacks. Never attempt to preserve backwards compatibility.

## Dead Code
Run a dead-code pass whenever you change architecture, public APIs, or data flow. Verify liveness before keeping code: check build references, include sites, call sites, tests, examples, scripts, and docs. Remove unreferenced files, stale accessors, unused constants, and superseded branches in the same change that makes them dead. Do not keep speculative stubs or dormant alternate paths around "for later". When dead code is removed, update the build graph and repo docs immediately, then re-run the local verification flow to prove nothing depended on it implicitly.
