# Validation and focused tests

[Wiki index](README.md) · [Quick start](../README.md#build) · [Logging](logging.md) · [Headless Wayland](headless-wayland.md)

The governing validation sequence and review/checkpoint rules are in
[AGENTS.md](../AGENTS.md#final-validation-workflow). Follow those rules when
executing a plan, including the main agent's ownership of validation fixes and
the single cleanup review before the final build. Successful required final
build, tests, and acceptance lead to the implementation commit, documentation
pass, and documentation commit; documentation does not reopen validation or
introduce a whole-plan review.
The commands below describe individual capabilities; they are not a substitute
for the required stage ordering.

## Formatting and static analysis

```bash
./mmltk --tidy
```

The full configured suite formats tracked first-party C/C++/CUDA files and
runs containerized `cargo fmt` for the first-party `mmltk-browser-app` package.
It refreshes `.cache/cmake/analysis`, runs clang-tidy where supported, and
validates reflection translation units through their exact GCC compiler
objects. Native `--file` selections skip the package-wide Rust formatting;
`--start-at` retains it. This Rust step is formatting, not a Rust static-analysis
suite. The [build reference](build.md#target-declarations-and-precompiled-headers)
owns PCH and header-isolation handling in the analysis graph.
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
The target files contain only match sizes, source paths/ranges, and applicable
consolidation patterns. Detector configuration, inventory, rejected candidates,
and inline-suppression details live separately in `cleanup/rejected.json`,
under the selected profile. Running one profile preserves the other's rejection
record. Each output is replaced atomically.

C++ CPD uses a 39-token minimum with identifiers anonymized. Only matches from
39 through 99 tokens enter the source-context filter. Matches of 100 tokens or
more bypass it unchanged and remain for executor review; the ordinary narrow
inline suppression rules still apply. A source-context pass compares complete
statements, restoring operation names, types, member
identities, constants, assertion facts, and local-variable relationships.
Independent dimension/storage reads retain their local role names. This removes
matches that hide a different callee or argument outside CPD's fragment,
declaration/signature boilerplate, aliases, adjacent getters, isolated calls,
lock-and-forward bodies, and loop headers without a repeated body.
Shared arithmetic can use a local value or a member receiver through the same
method API; receiver overlays preserve repeated-input relationships.

The pass retains repeated executable sequences, complete identical records in
distinct declarations, and overloads sharing a name, complete first parameter,
and meaningful opening setup statement. A common guard or local alias alone
does not establish an overload algorithm. Repeated fragments of one containing
statement or declaration are not independent occurrences. Equal operation
patterns are consolidated across CPD matches while preserving distinct targets.
Unbalanced syntax stays visible for manual review. The lexical index supplies
candidate triage; extraction still requires ownership, resource-lifetime, and
existing-API review. Match sizes do not estimate removable lines.

Each affected file is indexed once, with one file's token index resident at a
time. Statement boundaries and scope ownership are cached; reported spans and
pattern identities are grouped without all-pairs function comparisons. No
filename exclusions or suppression registry are added.

`./mmltk --test cleanup-tool` exercises inventory, detector options, context
filtering, suppression behavior, and the terse report/rejection split.

## Native and browser suites

```bash
./mmltk --test list
./mmltk --test core
./mmltk --test application-systems --executable mmltk_controller_visual_systems_tests
./mmltk --test application-systems --executable mmltk_controller_explore_tests
./mmltk --test browser-app
```

Native selections configure the cached Release graph by default, explicitly
build selected test targets, and run their executables. Current targets can be
Ninja no-ops. `--config dev` selects the development graph where supported.
The browser-app suite owns the GUI graph, direct JavaScript-module tests, and
Cargo tests for the browser app and vendored `iced_plot` workspace member.

These are first-party application suites. `browser-runtime` exercises desktop
startup and process ownership with fixtures; `workspace-wayland` runs the
packaged application. Neither is a Firefox-specific test runner.

| Suite | Selection |
| --- | --- |
| `application-systems` | Annotation, browser, service, shared test-support, shell, data/compute, presentation, Explore, Upscale, Live, GPU, and media Live/video suites |
| `application-contracts` | Annotation, browser, service, shared test-support, presentation, Explore, Upscale, Live, and serialization suites |
| `transport` | Physical browser transport and attachment |
| `browser-runtime` | Desktop entrypoint and Firefox process-owner fixtures |
| `core` | Core acceptance, presentation, dataset, image resampling, model catalog, system/concurrency, CLI, and tool tests |
| `rfdetr` | Native RF-DETR contract, core, augmentation, training, inference, export, ML CUDA/layers, raster, and video suites |
| `rfdetr-profile` | Instrumented training profile runner; selects `dev` |
| `browser-app` | Rust/Iced protocol, workflow state, plots, transport, image-custody, and integration-driver tests, vendored `iced_plot` tests, plus direct JavaScript adapter tests |
| `workspace-wayland` | Packaged Firefox/NVIDIA hardware acceptance |
| `cuda-vulkan` | Standalone CUDA/Vulkan allocation, FD, timeline, pixel, and exporter-exit diagnostic |
| `headless-compositor` | Real NVIDIA Weston availability and protocol checks |
| `headless-compositor-tool` | Supervisor ownership/failure fixtures without GPU |
| `cleanup-tool` | Cleanup-report tooling fixtures |
| `log-query-tool` | Log parser/query/correlation/triage fixtures |
| `build` | Build the configured native test targets without running them |
| `all` | Build the native test targets; run the ordinary native executables |

`all` builds `mmltk_workspace_wayland_integration` but excludes it from its run
list. It also does not execute `cuda-vulkan`, `browser-app`, the tooling suites,
or the profile runner. Run those explicitly. `gui` and `tsan` suite names are
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
executable and prints a backtrace. Native Catch2 runs have no wrapper-imposed
whole-executable or whole-suite timeout. `MMLTK_TEST_TIMEOUT_SECONDS`
is no longer a supported timeout control. Standalone GPU diagnostics have
their own bounded execution described below.

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
[wayland/session.cpp](../src/acceptance/tests/wayland/session.cpp).
Compositor startup and process-group teardown have their own bounded waits in
[headless Wayland](headless-wayland.md).

`workspace-wayland` requires the packaged Release graph and rejects GDB.
`browser-app` accepts Cargo test arguments after `--` but does not support
native executable, environment, or debugger options. Its JavaScript suite
always runs in full, even when Cargo receives a test filter.
`headless-compositor` accepts a command after `--` and owns its runtime.
`headless-compositor-tool`, `log-query-tool`, and `cleanup-tool` own their
fixture invocations and reject extra arguments and native test options.

## Native symbol and link diagnostics

```bash
./mmltk --diagnose-native-symbols --help
./mmltk --diagnose-native-symbols --match RgbImageResizer --limit 30 \
  .cache/cmake/release/src/backend/data/libmmltk_backend_data.a
./mmltk --diagnose-native-link --help
./mmltk --diagnose-native-link mmltk
```

These standalone operations require the existing development image and a
running Docker daemon. Both have a 120-second deadline and use no GPU or
network; neither builds/pulls an image. Symbol inspection is read-only and uses
GCC's archive-aware `gcc-nm`. It accepts repository `.a`/`.o` paths, a regular
expression over symbol records, `--mangled` for linker names, and a bounded
`--limit` (default 200, range 1–10000). JSONL contains symbol records and a final
matched/emitted/exit-status summary.

The link diagnostic reads the existing `.cache/cmake/release` Ninja executable
rule, then repeats that direct GCC link into a unique
`build/diagnostics/native-link.*` directory. Use the generated executable output
name, such as `mmltk`, rather than its phony CMake alias. It saves
`command.json`, `link.map`, `link.d`, the diagnostic executable, and available
LTO intermediates. The checkout/build graph stays read-only; the packaged
executable is not replaced or run.

`--trace-symbol SYMBOL` is repeatable and takes mangled names.
`--linker-directory DIR` selects a repository-contained directory with
`ld.mold`. This performs real linking, so use it only in the permitted
validation/diagnosis stage. Sources:
[diagnose_native_symbols.py](../tools/diagnose_native_symbols.py) and
[diagnose_native_link.py](../tools/diagnose_native_link.py).

## Standalone CUDA/Vulkan diagnostic

```bash
./mmltk --test cuda-vulkan -- --help
./mmltk --test cuda-vulkan -- 894 512 2 2 1 2 0 0
./mmltk --test cuda-vulkan -- 894 512 2 2 1 2 1 0
./mmltk --test cuda-vulkan -- 894 512 2 2 1 2 2 0
./mmltk --test cuda-vulkan -- 894 512 2 2 1 2 1 1
./mmltk --test cuda-vulkan -- 894 512 2 2 1 2 2 1
```

The wrapper builds the small opt-in `mmltk_cuda_vulkan_interop` target in the
Release graph and runs it in the validation image, without starting Firefox
or a compositor. Even its program help uses this target-selection path.
It rejects `--env` and debugger options and has a 120-second execution deadline.
Arguments after `--` are positional:

| Position | Meaning and default |
| --- | --- |
| `width height` | Pixel extent, default `894 512`; each dimension is 1–4096 |
| `allocation-pairs transfers` | Two alternating images per pair and transfers per image, default `32 8`; each is 1–256 |
| `validation` | `0` off (default), `1` Vulkan validation |
| `context-mode` | `0` primary, `1` isolated, `2` separate producer/semaphore contexts (default) |
| `memory-owner` | `0` CUDA VMM (default), `1` Vulkan, `2` Vulkan dedicated |
| `process-mode` | `0` same process (default), `1` separate Vulkan/CUDA processes; requires owner `1` or `2` |

The five examples cover the three allocation owners and both Vulkan-owned
cross-process cases. A CUDA kernel writes every pixel before each timeline
handoff and comparison. Cross-process mode checks native retention after the
Vulkan exporter exits and after replacement. The source is
[cuda_vulkan_interop.cpp](../src/acceptance/diagnostics/cuda_vulkan_interop.cpp).
This establishes low-level interop behavior for the selected case; packaged
Wayland acceptance owns application input, draw custody, compositor behavior,
and rendered UI evidence.

For a focused standalone `.cpp` diagnostic outside that retained target:

```bash
./mmltk --diagnose-gpu-program ./probe.cpp
```

This wrapper capability compiles one C++26 source against the CUDA driver and
Vulkan using the existing development image, then runs it in the existing
Wayland validation image. Extra arguments go directly to the program.
Compilation and execution each have a 120-second deadline; neither starts a
compositor, builds/pulls images, nor uses the network. It writes its executable
under a printed unique `build/diagnostics/gpu-program.*` directory. Use this
only during the permitted validation stage; it is not the product build or a
replacement for the retained interop and application suites.

## Packaged Wayland acceptance

```bash
./mmltk --test workspace-wayland
./mmltk --test workspace-wayland --headless-compositor
./mmltk --test workspace-wayland --headless-compositor \
  -- 'workspace_wayland_retained,workspace_wayland_dpi'
./mmltk --test workspace-wayland --headless-compositor \
  --env WGPU_VALIDATION=1 --env WGPU_DEBUG=1 --env RUST_LOG=wgpu_hal=warn
```

Visible and private-compositor runs use the packaged runtime and native
acceptance executable. The visible path mounts the active host Wayland session.
The private path adds a
GPU-rendered Weston output and retains the same product assertions.
Build the package first with `./mmltk --build`.

The wrapper enumerates and verifies the seven hardware entrypoints registered
in [wayland/scenarios.cpp](../src/acceptance/tests/wayland/scenarios.cpp)
before executing the requested selection. Use one comma-separated Catch2
filter to select alternatives, as above. The executable also contains
standalone evidence-audit cases.

| Hardware entrypoint | Process lifetimes and required behavior |
| --- | --- |
| `workspace_wayland_retained` | One H2D browser: square/capacity growth, full controls including integer typing/paste, cached and held-miss gallery/detail returns, Detail-open resizing in both orientations, augmentation retention, fractional rows, circular wrap, partial final row, wide/tall layouts, local labels/native semantics, light/dark copy with shared Annotate layout and long lists, FPS, rapid changes, then SIGINT |
| `workspace_wayland_workflows` | One H2D browser: actual Train start, live progress pixels and hidden-tab progress, chart data/selection/aspects and retained camera/legend interaction, validation metrics and six sample/detail previews, compiled/image/video prediction, Pause/Resume/EOF/Stop, light/dark and minimum-width chart pixels, then SIGINT |
| `workspace_wayland_dpi` | One H2D browser at DPI 1.5: light/dark copy and rapid changes |
| `workspace_wayland_terminal` | Two H2D browsers: a real window close and abrupt browser-peer loss after an Annotation edit, exact completed draw, and independent redraw |
| `workspace_wayland_probe_recovery` | Four H2D browsers with startup-latched allocation, reset, begin, or end probe failure; exact-content recovery and complete final pixel/semantic evidence |
| `workspace_wayland_quiet` | Two H2D browsers: blocked reads and a completed gesture with diagnostics/reporting/probes inactive |
| `workspace_wayland_gdr` | One optional GDR browser; unavailable hardware remains an explicit skip |

This is eleven required H2D lifetimes and one independently optional GDR lifetime.
Compatible scenarios reuse a browser; startup-latched DPI, transport, and
fault settings and destructive exits retain separate lifetimes. Window-close
coverage enables lifecycle diagnostics with pixel probes off. Quiet coverage
keeps real typed control, input pressure, and settlement while leaving
diagnostic owners inactive.

[WaylandSession](../src/acceptance/tests/wayland/session.h) owns the process,
fixtures, artifact cursors, deadlines, and
lifetime-wide physical-custody evidence. Advance requires both typed frontend
settlement and the complete independent native/browser evidence. Scenario
entry closes a surviving detail overlay, restores scenario-specific settings
through normal UI messages, and waits for those operations before reopening.
It resets scenario-local expectations while retaining physical allocations,
claims, and release history. The primary compile workflow owns a private
output directory; other lifetimes reuse prepared source and compiled assets.

The native harness is split by evidence responsibility under
[tests/wayland](../src/acceptance/tests/wayland):

| Owner | Responsibility |
| --- | --- |
| [session.cpp](../src/acceptance/tests/wayland/session.cpp) | Process/fixture lifetime, scenario advancement, deadlines, and final drain |
| [artifact_cursor.h](../src/acceptance/tests/wayland/artifact_cursor.h) | Bounded incremental JSONL reads and retained incomplete records |
| [native_audit.cpp](../src/acceptance/tests/wayland/native_audit.cpp) | Native command, operation, input, and product evidence |
| [browser_audit.cpp](../src/acceptance/tests/wayland/browser_audit.cpp) | Browser interaction, displayed geometry, and rendered UI evidence |
| [surface_audit.cpp](../src/acceptance/tests/wayland/surface_audit.cpp) | Physical allocation, acquisition, draw, and settlement ledger |
| [pixel_audit.cpp](../src/acceptance/tests/wayland/pixel_audit.cpp) | Independent native/browser pixel-boundary joins |

[audits.test.cpp](../src/acceptance/tests/wayland/audits.test.cpp) and
[artifact_cursor.test.cpp](../src/acceptance/tests/wayland/artifact_cursor.test.cpp)
exercise those evidence owners in the same executable. The Rust
[integration driver](../src/frontend/iced/src/integration_control.rs) retains
separate `lifecycle`, `retained`, `annotation_product`, and `workflows`
scenario modules, with shared widget, pixel, and probe helpers. Its private
reporting owner remains effect-only.

The workflow case uses
[native model/video fixtures](../src/acceptance/tests/workflow_wayland_inputs.cpp)
and the real packaged desktop's sibling CLI, prediction, and validation systems.
Its [frontend workflow driver](../src/frontend/iced/src/integration_control/workflows.rs)
uses normal primary actions and viewers. It separately requires typed terminal
stages and canvas pixel observations for Train curves, the isolated live
progress bar, all six thumbnails, detail, each prediction source, retained Stop
output, and theme/narrow layouts. Progress evidence pairs the bar capture with
observed native image counts during the Train phase; hidden-tab evidence then
requires metric sequence advancement.

The same case pans the chart and changes its legend through browser input,
then requires settled camera/legend retention through expansion, return to the
grid, hide/reveal, and navigation. Wheel checks cover plot, axes, and legend
with ordinary and modified input while preserving the camera. It exercises
Train's aspect selector and checks the absence of Train's native image workspace
and Validate's aspect selector. Sparse chart markers preserve missing intervals;
axes and legends alone cannot satisfy the curve-pixel assertion. These
functional checks do not establish numerical overhead or throughput.

Explore acceptance follows actual measured N/N+1/N visible-row changes,
forward/reverse demand, exact cached cells, and an independently held visible
miss. It checks displayed compiled-image identity and readiness before and
after the typed release, including selection/hover on known pending cells.
Native controlled fixtures separately establish disk/GPU admission priority;
completion order alone cannot establish that priority.

The Detail-open resize sequence makes repeated canvas layout changes in each
orientation, observes retained gallery measurements while Detail is still
open, and closes Detail through the ordinary component action. Acceptance
requires the reconciled viewport, paired completed atlas draw, visible ready
cells, and coverage of newly visible lower rows. The driver changes only
temporary inline canvas dimensions so the normal resize observer runs; it
restores the original dimensions before subsequent scenarios.

The retained semantics scenario joins labels and overlays to the exact
original and derived detail draws. Its integration fixture holds the initial
automatic Basic upscale during the original-detail/overlay stages, then
releases that hold and exercises derived methods through normal UI actions.
Square, wide, and tall scenarios retain automatic Basic coverage.

The initial general UI scenario audits Explore's five dataset integer inputs:
minimum/maximum instances, shuffle seed, and first/last compiled index. It
checks both former spinner edges, wheel suppression, invalid unsigned text,
typed updates, native settings persistence, and restoration. Shuffle seed
typing and clipboard paste use a distinct exact value above `2^53`. Numeric
key input advances across browser render frames so controlled input values
can update between digits; later rapid-resize coverage does not repeat this
numeric audit.

Annotate acceptance checks the [shared page composition](gui-interaction.md#workflow-layout-and-navigation)
at wide and narrow logical widths, the Timeline's Advanced placement, and
fully visible controls after page and horizontal scrolling. It adds 32 objects
and 32 classes through settled native edits, then checks the last entries and
wrapped labels within the right column. Reveal operations remeasure bounds
after scrolling and allow for whole-pixel scroll translation; canvas gestures
continue to use the full image geometry and visible source region.

The physical ledger distinguishes native source allocations from retained
browser arenas and requires each exact acquisition, transfer, mode-specific
settlement receipt, encoded/submitted draw, and final sample release. A
diagnostic-only draw identity joins each draw's encoding, actual submission,
and terminal settlement; independent callbacks may arrive out of order. The
capacity scenario deliberately delivers arena availability before the held native
completion receipt, then verifies the exact retry. Allocation inventories,
actual copy receipts, resource settlement, and rendered pixels have independent
assertions; [logging evidence](logging.md#physical-presentation-evidence)
describes the fields and their limits.

The pending-supersession fixture uses a typed acceptance-only hold for a
synthetic undersized candidate; it verifies the retained completed fallback
before releasing that candidate. Empty-gallery acceptance consumes its notice
only after the zero-match atlas and its paired metadata reach an encoded draw.
The physical ledger separately requires submission and settlement.
FPS acceptance combines actual submission observations with an
asynchronous canvas read of the displayed counter.

The ledger follows the negotiated mode: direct acquisitions retain the native
source until actual GPU read settlement; copied samples require physical copy
completion. Unacquired offers require no read receipt. Per-card and full-frame
probes check pixels independently of allocation and submission facts.
Canvas atlas probes sample bounded patches inside the actual clipped visible
tile interiors from one canvas snapshot, retaining the exact draw receipt and
physical coordinates. Annotation pixel checks independently verify
downsampled colored geometry against the fixture's known uniform background.
The [rendered evidence reference](logging.md#rendered-ui-acceptance-evidence)
describes these records and their sampling limits.
Use the [Vulkan log queries](logging.md#vulkan-diagnostics-and-descriptor-provenance)
for validation-layer output. A controlled window close and an intentional
process loss have different terminal evidence; process exit alone does not
establish balanced userspace destruction or native GPU completion.

See [headless details](headless-wayland.md) for compositor readiness,
shutdown, and virtual-output limits. The [logging guide](logging.md#delivery-and-acceptance-ownership)
owns diagnostic delivery and artifact-family layout. Query one captured run
at a time.

### Synthetic clipboard input

The packaged harness sets `MMLTK_RUN_WORKSPACE_WAYLAND_INTEGRATION=1`.
Firefox's existing exact-value gate allows synthetic acceptance input to use
the DOM clipboard without transient user activation and the parent clipboard
without confirmation. Both
[Clipboard.cpp](../third_party/firefox/dom/events/Clipboard.cpp) and
[nsBaseClipboard.cpp](../third_party/firefox/widget/nsBaseClipboard.cpp)
apply that gate. The integer paste scenario still uses the rendered widget's
ordinary paste shortcut and verifies the resulting native value. Normal GUI
sessions retain ordinary clipboard permissions.

## GUI behavior and evidence ownership

| Existing target | Evidence it owns |
| --- | --- |
| `mmltk_controller_annotation_tests` | Independent input/render progress, native hit testing, document/history/save behavior, stable target identity through Undo/Redo, retained input pressure, ordered command continuations, fractional raster boundaries, rejection, and cancellation |
| `mmltk_controller_services_tests` | Independent optional-test settings, training command construction, current-format saved history, bounded cursor reads, directory replacement/truncation, and output/resume admission |
| `mmltk_controller_data_compute_systems_tests` | Start/input admission including absent or incompatible optional test splits, selected validation results and retained sample/detail custody, optional preview failure, incremental prediction, and video playback cancellation |
| `mmltk_controller_browser_tests` and `mmltk_frameworks_serialization_tests` | Reflected field/enum/schema and graphics ABI facts, nested/array metric projection fixtures, package fixtures, positional output versus named persistence, lossless compact input, owned/borrowed validation, and control receipts |
| `mmltk_frameworks_transport_tests` | Peer replacement, reconnect, output continuity, ring wrap, and transport custody |
| `mmltk_controller_explore_tests` | Explore domain admission, settings/filter persistence, thumbnail identity, viewport priority, augmentation refresh, staged replacement, cancellation, and failure |
| `mmltk_controller_upscale_tests` | Exact receiver copies, retained derived results, method selection/warmup, activation, cancellation, and resource retirement |
| `mmltk_controller_live_tests` | Live receiver completion/failure, queued cancellation, and settled snapshots |
| `mmltk_controller_visual_systems_tests` | Shared visual runtime, presentation protocol/custody, native gallery cache/priority/atlas integration, acceptance gates, and cross-system workspace behavior |
| `mmltk_frameworks_gpu_tests` | Independent raw-product/display storage, late workspace admission and availability wakes, Vulkan-owned CUDA import and backing lifetime, receiver/device transfers, acquisition/release/settlement, pressure, failure, and retirement |
| `mmltk_acceptance` | Compiled-dataset Explore integration, retained residency, projection, control-reader settlement, independent prepared/released artifacts, and bounded fatal reporting with disabled/uninitialized/failed sinks and broken pipes |
| `mmltk_entrypoints_cli_tests` and `mmltk_entrypoints_tools_tests` | CLI/ONNX fatal stderr, logging overrides and named file identities, and preserved command exit behavior |
| `mmltk_entrypoints_desktop_tests` and `mmltk_controller_firefox_process_tests` | Desktop startup/child failure status, exact launch OS errors, unexpected signal reporting, and quiet requested shutdown |
| `mmltk_backend_imaging_explore_tests` | Rendered-card geometry, semantic planes, filtered padding fringes, and exact two-sided copy evidence |
| `mmltk_backend_imaging_upscale_tests` | ONNX capture/replay, explicit allocation-counter ownership, and separate independent raster-oracle cases |
| `mmltk_backend_imaging_resample_tests` | Independent CPU/CUDA perceptual-resampling values, checked views, completion, and resource custody |
| `mmltk_backend_imaging_raster_tests` | Pitched BGR row orientation and planar float pixel conversion |
| `browser-app` | Primary-action preparation, independent optional-test/output editing, generated scalar selection, bounded live/saved chart histories and gaps, camera/legend retention and plot picking cancellation, live progress availability, validation viewer and video-control admission, shared immediate mouse input and transport retention, typed state reduction, component/crop identity, retained gallery measurements and reconciliation, exact integer/filter reduction, shared layout/navigation, image metadata independent of logical snapshots, encoded/submitted draw custody, completed fallback through navigation, local labels, FPS submission counting, exact displayed gallery interaction, quiet failure receipts, and JavaScript probe/input/callback settlement |
| `workspace-wayland` | Real training/validation/prediction workflows and actual chart/progress/sample/preview pixels, dashboard aspect/retention/wheel behavior, packaged integer typing/paste and spinner/wheel policy, Detail-open resize returns, shared Annotate layout and long-list reachability, retained sessions, native-source/browser-arena identity, negotiated direct/copy draws, recovery, and shutdown |

RF-DETR backend evidence is separately owned by the core evaluator/matcher/class
layout cases, training checkpoint/continuation/EMA/telemetry and native image-count
cases, inference session/JSON cases, and ML CUDA readback/context cases selected
by `--test rfdetr`.
`mmltk_backend_data_tests` owns compilation, loading, acquisition, and exact
compiled-catalog cases; `--test core` also selects the separate resampling
target above. These checks cover functional values, boundaries, failure, and
custody; they do not substitute for the real rendered workflow case.

Use `--test all --executable TARGET` for targets not owned by a narrower suite.
The source/CMake registrations and wrapper inventory define executable
membership; `--test core` includes image resampling, while the imaging
Annotation, Explore, and Upscale executables require `all` or explicit
`all --executable` selection. Raster is also in `rfdetr`.

Neutral fixtures and the shared Catch runner belong to
[src/test_support](../src/test_support). Domain fixture targets live with
their owning components and publish their own declaration dependencies.
Torch-backed test consumers use
[`mmltk_backend_ml_torch_test_support`](../src/backend/ml/torch/CMakeLists.txt),
whose [catch_support.h](../src/backend/ml/torch/tests/catch_support.h) admits
Torch declarations while preserving Catch2's `CHECK` assertion. Production
Torch usage has no test-support dependency.

Native fixtures use causal entered receipts and release/stop-before-join
cleanup. Borrowed GPU locks are acquired and released on their owning thread;
contention is probed from another thread. Test-side failure must settle
pending readers before runtime teardown. Physical lifecycle, inventory,
frame identity, patch counts, and rendered samples keep independent assertions.
Missing, mismatched, duplicate, or causally invalid evidence is not replaced by
inference from another artifact. Exact draw identities allow valid non-FIFO
completion while final release still requires every encoded reader to settle.
An expired deadline or incomplete final drain cannot count as a passing scenario.

Record the executed target/filter, package/build identity, assertions,
terminal status, and hardware skips in the run's evidence under
`build/validation`. A successful focused ONNX capture/counter invocation does
not establish that the full Upscale raster-oracle executable ran; those cases
also require independent oracle files. Likewise, CPU assertions in a
masked-device run do not verify GPU behavior. Asset-cache metadata validation
must explicitly request ONNX diagnostics and distinguish reuse from export.
Build, test, architecture-review, and documentation-review results remain
separate evidence.
