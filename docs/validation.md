# Validation and focused tests

[Wiki index](README.md) · [Quick start](../README.md#build) · [Logging](logging.md) · [Headless Wayland](headless-wayland.md)

The governing validation sequence and review/checkpoint rules are in
[AGENTS.md](../AGENTS.md). Follow those rules when executing a plan.
The commands below describe individual capabilities; they are not a substitute
for the required stage ordering.

## Formatting and static analysis

```bash
./mmltk --tidy
```

The full configured suite formats tracked first-party C/C++/CUDA files,
refreshes `.cache/cmake/analysis`, runs clang-tidy where supported, and
validates reflection translation units through GCC compiler objects.
CUDA clang-tidy covers host/device code at `sm_86`. Cppcheck is currently
disabled because its parser does not support the repository's reflection
syntax; `--cppcheck-only` is unavailable.

For follow-up work, select one or several tracked sources with one `--file`,
or resume at a translation unit with `--start-at`:

```bash
./mmltk --tidy --file src/common/system/cpu_affinity.cpp src/common/system/execution_policy.cpp
./mmltk --tidy --file src/backend/imaging/explore/explore_render_core.cu
./mmltk --tidy --start-at src/common/system/cpu_affinity.cpp
```

`--file` and `--start-at` cannot be combined. Focused runs do not replace the
full passes required by the workflow. Tidy writes a log below `build/logs`;
`MMLTK_TIDY_LOG_FILE` selects another report path. The implementation and
available analysis overrides are in
[run_static_analysis.sh](../tools/run_static_analysis.sh).

## Deduplication reports

```bash
./mmltk --cleanup-report cpp
./mmltk --cleanup-report frontend
./mmltk --cleanup-report all
```

These generate the C++ and/or frontend reports under `cleanup/`.
`cpp-code-deduplication.json` and `frontend-code-deduplication.json` contain
textual `hits` and structural `structural_hits`, ordered with cross-file
findings before within-file findings. Use the applicable full profiles and
resolution rules in `AGENTS.md`; reports do not themselves change product code.

## Native and browser suites

```bash
./mmltk --test list
./mmltk --test core
./mmltk --test application-systems --executable mmltk_controller_visual_systems_tests
./mmltk --test browser-app
```

Native selections configure the cached Release graph by default, explicitly
build selected test targets, and run their executables. Current targets can be
Ninja no-ops. `--config dev` selects the development graph where supported.
The browser-app suite owns the GUI graph, direct JavaScript-module tests, and
its Cargo test target.

| Suite | Selection |
| --- | --- |
| `application-systems` | Browser, service, shell, data/compute, visual, GPU, and Live suites |
| `application-contracts` | Browser, service, visual, and serialization suites |
| `transport` | Physical browser transport and attachment |
| `browser-runtime` | Desktop entrypoint and Firefox process-owner fixtures |
| `core` | Core acceptance, dataset, model catalog, system/concurrency, CLI, and tool tests |
| `rfdetr` | Native RF-DETR model, training, inference, export, CUDA, and layer suites |
| `rfdetr-profile` | Instrumented training profile runner; selects `dev` |
| `browser-app` | Rust/Iced protocol, state, transport, image-custody, and integration-driver tests, plus direct JavaScript adapter tests |
| `workspace-wayland` | Packaged Firefox/NVIDIA hardware acceptance |
| `headless-compositor` | Real NVIDIA Weston availability and protocol checks |
| `headless-compositor-tool` | Supervisor ownership/failure fixtures without GPU |
| `cleanup-tool` | Cleanup-report tooling fixtures |
| `log-query-tool` | Log parser/query/correlation/triage fixtures |
| `build` | Build the configured native test targets without running them |
| `all` | Build the native test targets; run the ordinary native executables |

`all` builds `mmltk_workspace_wayland_integration` but excludes it from its run
list. It also does not execute `browser-app`, the tooling suites, or the
profile runner. Run those explicitly. `gui` and `tsan` suite names are
currently unavailable even though other GUI/development build facilities exist.

Some RF-DETR tests download model checkpoints and derive normalized weights,
ONNX, and TensorRT engines in `.cache/tests/rfdetr` on first use. Hardware-gated
tests may skip when their requirements are unavailable; a skip is not hardware
acceptance evidence.

## Selection, environment, deadlines, and debugging

Put test-runner arguments after `--`:

```bash
./mmltk --test core -- --list-tests
./mmltk --test rfdetr -- '~[optin]'
./mmltk --test browser-app -- presentation
./mmltk --test application-systems --executable mmltk_frameworks_gpu_tests \
  --env MMLTK_GDR_TEST_DEVICE=0 -- '[gdr][hardware]'
./mmltk --test all --executable mmltk_backend_imaging_explore_tests
./mmltk --test all --executable mmltk_backend_imaging_upscale_tests -- '[capture],[probe]'
```

| Option | Behavior |
| --- | --- |
| `--executable TARGET` | Select one target owned by the suite |
| `--config release` or `--config dev` | Select the supported native graph |
| `--env NAME` or `--env NAME=VALUE` | Forward a native-test environment variable; repeatable |
| `--gdb` | Run one selected native executable in containerized GDB |
| `--gdb-command COMMAND` | Add an ordered debugger command; also enables GDB |
| `--headless-compositor` | Use private NVIDIA Weston with `workspace-wayland` |

For example:

```bash
./mmltk --test core --executable mmltk_common_system_tests --config dev \
  --gdb-command run --gdb-command bt
```

Noninteractive GDB uses batch mode; without supplied commands it runs the
executable and prints a backtrace. The wrapper applies no arbitrary
whole-executable or whole-suite test timeout. `MMLTK_TEST_TIMEOUT_SECONDS`
is no longer a supported timeout control.

Fixtures retain bounded startup, entered-boundary, progress, operation, and
shutdown waits. The packaged Wayland harness currently uses these deadlines:

| Boundary | Deadline |
| --- | --- |
| Browser startup | 20 seconds |
| Ordinary interaction progress | 6 seconds |
| Native work progress | 15 seconds |
| Failure settlement | 5 seconds |
| Application shutdown | 15 seconds |

Only actual progress in the relevant phase renews its deadline. A timeout is
a failure, never successful EOF or inferred completion. Source constants and
phase selection live in
[workspace_wayland_integration.test.cpp](../src/acceptance/tests/workspace_wayland_integration.test.cpp).
Compositor startup and process-group teardown have their own bounded waits in
[headless Wayland](headless-wayland.md).

`workspace-wayland` requires the packaged Release graph and rejects GDB.
`browser-app` accepts Cargo test arguments after `--` but does not support
native executable, environment, or debugger options. Its JavaScript suite
always runs in full, even when Cargo receives a test filter.
`headless-compositor` accepts a command after `--` and owns its runtime.
`headless-compositor-tool`, `log-query-tool`, and `cleanup-tool` own their
fixture invocations and reject extra arguments and native test options.

## Packaged Wayland acceptance

```bash
./mmltk --test workspace-wayland
./mmltk --test workspace-wayland --headless-compositor
./mmltk --test workspace-wayland --headless-compositor \
  -- 'workspace_wayland_retained,workspace_wayland_dpi'
```

Both use the packaged runtime and the native acceptance executable. The
visible path mounts the active host Wayland session. The private path adds a
GPU-rendered Weston output and retains the same product assertions.
Build the package first with `./mmltk --build`.

The wrapper enumerates and verifies the six registered hardware entrypoints
before executing the requested selection. Use one comma-separated Catch2
filter to select alternatives, as above. The executable also contains
standalone evidence-audit cases.

| Hardware entrypoint | Process lifetimes and required behavior |
| --- | --- |
| `workspace_wayland_retained` | One H2D browser: square/capacity growth, full controls, wide, tall, semantics, light copy, dark copy, rapid changes, then SIGINT |
| `workspace_wayland_dpi` | One H2D browser at DPI 1.5: light/dark copy and rapid changes |
| `workspace_wayland_terminal` | Two H2D browsers: a real window close and abrupt browser-peer loss after an Annotation edit, exact completed draw, and independent redraw |
| `workspace_wayland_probe_recovery` | Four H2D browsers with startup-latched allocation, reset, begin, or end probe failure; exact-content recovery and complete final pixel/semantic evidence |
| `workspace_wayland_quiet` | Two H2D browsers: blocked reads and a completed gesture with diagnostics/reporting/probes inactive |
| `workspace_wayland_gdr` | One optional GDR browser; unavailable hardware remains an explicit skip |

This is ten required H2D lifetimes and one independently optional GDR lifetime.
Compatible scenarios reuse a browser; startup-latched DPI, transport, and
fault settings and destructive exits retain separate lifetimes. Window-close
coverage enables lifecycle diagnostics with pixel probes off. Quiet coverage
keeps real typed control, input pressure, and settlement while leaving
diagnostic owners inactive.

`WaylandSession` owns the process, fixtures, artifact cursors, deadlines, and
lifetime-wide physical-custody evidence. Advance requires both typed frontend
settlement and the complete independent native/browser evidence. Scenario
entry closes a surviving detail overlay, restores scenario-specific settings
through normal UI messages, and waits for those operations before reopening.
It resets scenario-local expectations while retaining physical allocations,
claims, and release history. The primary compile workflow owns a private
output directory; other lifetimes reuse prepared source and compiled assets.

See [headless details](headless-wayland.md) for compositor readiness,
shutdown, and virtual-output limits. The [logging guide](logging.md#delivery-and-acceptance-ownership)
owns diagnostic delivery and artifact-family layout. Query one captured run
at a time.

## GUI behavior and evidence ownership

| Existing target | Evidence it owns |
| --- | --- |
| `mmltk_controller_annotation_tests` | Ordered reduction, document/history/save behavior, pressure, command barriers, rejection, and cancellation |
| `mmltk_controller_browser_tests` and `mmltk_frameworks_serialization_tests` | Reflected field/enum/schema facts, outer-record coverage, bounded compact codecs, owned/borrowed validation, and control receipts |
| `mmltk_frameworks_transport_tests` | Peer replacement, reconnect, output continuity, ring wrap, and transport custody |
| `mmltk_controller_visual_systems_tests` and `mmltk_frameworks_gpu_tests` | Native source progression, receiver-owned copies, borrowed-view lifetime, ready/release callbacks, resource failure, and retirement |
| `mmltk_acceptance` | Compiled-dataset Explore integration, projection, control-reader settlement, and independent prepared/released artifacts |
| `mmltk_backend_imaging_explore_tests` | Rendered-card geometry, semantic planes, filtered padding fringes, and exact two-sided copy evidence |
| `mmltk_backend_imaging_upscale_tests` | ONNX capture/replay, explicit allocation-counter ownership, and separate independent raster-oracle cases |
| `browser-app` | Retained input and credits, typed state reduction, canvas identity, frame/UI arrival order, submitted draws/completed fallback, quiet reporting, and JavaScript probe/callback settlement |
| `workspace-wayland` | Actual packaged interaction, retained sessions, native/browser physical identity, rendered pixels, recovery, and shutdown |

Use `--test all --executable TARGET` for targets not owned by a narrower suite.
The source/CMake registrations and wrapper inventory define executable
membership; `--test core` does not include the imaging test executables.

Native fixtures use causal entered receipts and release/stop-before-join
cleanup. Borrowed GPU locks are acquired and released on their owning thread;
contention is probed from another thread. Test-side failure must settle
pending readers before runtime teardown. Physical lifecycle, inventory,
frame identity, patch counts, and rendered samples keep independent assertions.
Missing, mismatched, duplicate, or reordered evidence is not replaced by
inference from another artifact. An expired deadline or incomplete final drain
cannot count as a passing scenario.

Record the executed target/filter, package/build identity, assertions,
terminal status, and hardware skips in the run's evidence under
`build/validation`. A successful focused ONNX capture/counter invocation does
not establish that the full Upscale raster-oracle executable ran; those cases
also require independent oracle files. Likewise, CPU assertions in a
masked-device run do not verify GPU behavior. Asset-cache metadata validation
must explicitly request ONNX diagnostics and distinguish reuse from export.
Build, test, architecture-review, and documentation-review results remain
separate evidence.
