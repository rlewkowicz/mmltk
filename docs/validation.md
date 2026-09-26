# Validation and focused tests

[Wiki index](README.md) · [Quick start](../README.md#build) · [Logging](logging.md) · [Headless Wayland](headless-wayland.md)

[AGENTS.md](../AGENTS.md#final-validation-workflow) owns validation stage order,
fix/review ownership, checkpoints, and the final documentation pass. After the
required final full build, both gate commands must pass in this order:

```bash
./mmltk --test all
./mmltk --test workspace-wayland --headless-compositor
```

Unfiltered `all` runs native, browser JavaScript/Rust, cleanup/declaration, and
log-query fixtures. The standalone capabilities below neither replace nor
supplement this gate with focused filters, executables, or extra suites.

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
[.clang-format](../.clang-format) keeps the Google base, a 200-column limit,
one-column indentation and continuation, and no tabs. Its Clang 22 braced-list
settings use one-column indentation with opening/closing breaks for long lists;
`LambdaBodyIndentation: OuterScope` keeps callbacks near their enclosing scope.
An ordinary local aggregate can give a callback table a short anchor, as the
`WebSocketBehavior` value in
[browser_server.cpp](../src/frameworks/transport/browser_server.cpp) does.
Layout changes do not count as structural code reduction.
The repository's `modernize-use-auto.MinTypeNameLength` is `5` in
[.clang-tidy](../.clang-tidy); it is a type-spelling threshold, not a permission
to hide meaningful ownership or introduce indirection.
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
full passes required by the workflow. Tidy retains a
[wrapper transcript](logging.md#wrapper-build-test-and-tidy-transcripts), with
`MMLTK_TIDY_LOG_FILE` selecting its destination. The implementation and
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
record. Its `duplo.raw_hits` retains the detector's original hits before source
filtering. Each output is replaced atomically.

C++ Duplo uses a six-line minimum; CPD uses a 39-token minimum with identifiers
anonymized and literal sequences ignored. Frontend minima are nine lines and
75 tokens. The C++ source-context pass applies at every CPD token size and to
Duplo's code-bearing textual candidates. It compares complete statements,
restoring operation names, types, member
identities, constants, assertion facts, and local-variable relationships.
Independent dimension/storage reads retain their local role names. This removes
matches that hide a different callee or argument outside CPD's fragment,
declaration/signature boilerplate, aliases, adjacent getters, isolated calls,
lock-and-forward bodies, and loop headers without a repeated body.
Shared arithmetic can use a local value or a member receiver through the same
method API; receiver overlays preserve repeated-input relationships.
Multiplication-only arithmetic, useful CUDA setup, and repeated scalar/admission
operations remain candidates. There is no blanket CUDA, role-name, or
admission-name exclusion. A partial match must be assessed against its complete
containing operations, regardless of match size.

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

[Inline suppression policy](../AGENTS.md#deduplication-rules) applies only to
ordinary reports; raw declaration evidence below is separate.
The [detector-integrity checks below](#raw-cpd-and-declaration-formatting) also
apply to ordinary reports. Inspected false positives use one narrow
`CLEANUP-IGNORE` or bounded `CLEANUP-OFF`/`CLEANUP-ON` span. The rejection report
retains the rejected coordinates/reasons and suppressed occurrence counts.
A clean ordinary report is not evidence of zero raw repetition or a complete
human review of every file.

## Raw CPD and declaration formatting

```bash
./mmltk --raw-cpd
./mmltk --raw-cpd --min-tokens 12 --output cleanup/declarations-raw.json
./mmltk --raw-cpd --review
./mmltk --raw-cpd --review --output cleanup/declarations-review.json
./mmltk --format-declarations check
./mmltk --format-declarations preview --file src/backend/models/rfdetr/contract/train_recipe.h
./mmltk --format-declarations fix --file src/backend/models/rfdetr/contract/train_recipe.h
```

[generate_cleanup_json.mjs](../tools/generate_cleanup_json.mjs) owns both
ordinary cleanup and raw evidence. Raw CPD defaults to 12 tokens (minimum 2),
preserves exact identifier/literal spelling, disables block skipping,
and applies no source-context filter or inline suppression. Its first-party
C/C++/CUDA inventory includes ordinary and template headers, modules, and
untracked unignored files, excludes `third_party`, and omits deleted paths.
This is declaration triage, not a verdict that every lexical match should be
extracted into a shared algorithm.

Ordinary and raw CPD both scan immutable temporary copies with unique supported
`.cpp` or `.rs` suffixes, so CUDA, module, and template-header inputs actually
reach the selected lexer. A one-to-one map restores the original source paths.
PMD `CPD-OFF`/`CPD-ON` markers are neutralized only inside comments, preserving
their length; strings containing marker-like text remain unchanged. Original
line endings, offsets, ranges, and reported text remain intact. End columns are
exclusive, and coordinate validation rejects spans outside the original source
or splitting a Unicode surrogate pair. Both modes require complete per-file
lexer coverage and valid match records, and compare source SHA-256 identities
before publishing. An error cannot become an apparently clean partial report.

The format-1 raw report defaults to
`cleanup/declarations-raw.json`: `inventory`, detector configuration/status,
`raw_matches` with original occurrence coordinates/text, independent
`classified_context`, and a summary. Classification supplies context without
adding or deleting detector matches. Raw mode rejects nonzero detector status,
error/processing-error XML, diagnostic exceptions, or incomplete lexer coverage;
ordinary mode uses the same detector-integrity checks.

### Review candidates and the large-file queue

`--review` adds `review` to the raw report and defaults to
`cleanup/declarations-review.json` plus `cleanup/declarations-review.md`.
`--output` chooses the JSON path and the same-stem Markdown sibling. Both
renderings are prepared before publication and each replacement is atomic;
the pair is not a filesystem transaction. Candidate presence is successful
analysis, not a failing check.

The review inventory includes first-party authored native, Rust, JavaScript/
TypeScript, Python, shell, CMake, HTML/CSS, and Dockerfile code, including the
root wrapper. It uses tracked and untracked-unignored Git inputs and omits
deleted paths, third-party code, and generated/output trees. Each entry records
language, byte and physical-line counts, SHA-256, dedicated/embedded test
evidence, family IDs, and `status: "unreviewed"`. Non-C++ languages receive size
triage only. The queue includes every file **over 500 physical lines** and any
file with a candidate family. A file with exactly 500 lines needs a candidate
family to enter the queue.

[review_patterns.mjs](../tools/cleanup/review_patterns.mjs) groups bounded lexical
CLI bindings, CLI exclusions, trait declarations, repeated qualifications, and
same-name mappings with at least three distinct occurrences. Families retain
original spans and owner evidence, invariant tokens, varying slots and values,
file/owner/occurrence counts, and overlapping raw-match IDs. Raw matches,
independent declaration classifications, and review families stay separate;
manual/rejected classification context remains in the JSON. Covered-line counts
merge overlapping spans and do not estimate removable or production lines.

Markdown lists the queue and shows up to three excerpts per family, each at
most eight lines and 800 characters. Full anchors and values remain in JSON.
Every entry remains unreviewed until a person inspects it; generating the queue
does not establish a complete visual inspection, semantic equivalence, or safe
extraction. File size is an inspection trigger, not a refactoring requirement.

### Annotation formatter

[format_declarations.mjs](../tools/format_declarations.mjs) shares the lexical
index and classifiers in
[declaration_patterns.mjs](../tools/cleanup/declaration_patterns.mjs). It is not
a semantic C++ parser. By default it examines the complete first-party C++
inventory; repeat `--file PATH` to narrow it. Files must belong to that
inventory. Macro-conflict inspection still covers the full inventory.
`--report PATH` selects the format-1 JSON report, default
`cleanup/declarations-format.json`; both tools require a `.json` output path.

Every formatter mode writes original spans, reasons, proposed edits, retained
candidates, manual findings, and summary counts atomically. **Check** exits 1
when safe edits remain, 0 otherwise (manual findings can still remain), and 2
on an error. **Preview** leaves sources unchanged and reports before/after
text. **Fix** applies only proven annotation spelling/direct-include edits,
checks that each original file is unchanged, preserves its mode, and replaces
it atomically. Repeated fix is idempotent.

Automatic policy rewriting requires an absolute canonical name or an absolute
global alias to the absolute canonical namespace. Relative lookup remains
manual, as do conditional dependencies, annotation comments, conflicting
macros, ambiguous syntax, and unsupported comma-containing type arguments.
Template module inputs such as `.cppm.in` retain manual include placement.
The [authoring guide](reflection.md) owns macro semantics and examples. This
formatter does not consolidate repeated records or infer application owners.

The `cleanup-tool` fixtures cover inventory, ordinary filters and suppressions,
raw exact-spelling evidence and incomplete-scan rejection, original ranges,
source hashes, supported detector suffixes, review-family/queue accounting,
classification, conservative policy lookup, and formatter idempotence. They
run within unfiltered `all`. A standalone filtered retry is:

```bash
./mmltk --test cleanup-tool -- --test-name-pattern PATTERN
```

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
Cargo tests for the browser app and vendored `iced_plot` workspace member,
plus an explicit `iced_aw --lib` invocation with `--no-default-features` and
`--features number_input,selection_list`. The iced_aw selection covers its
library tests; dependency-generated illustrative icon doctests are outside that
selection. The [frontend CMake registration](../src/frontend/iced/CMakeLists.txt)
owns both execution and the corresponding `--no-run` check commands.
The browser test route explicitly requests NVIDIA graphics access, selects the
container's NVIDIA Vulkan ICD files, and sets the renderer backend to Vulkan.
It applies this setup whether called directly or from `all`; missing NVIDIA
Vulkan ICDs fail the route. `iced_plot` owns its native test dependencies in
its [manifest](../third_party/iced_plot/Cargo.toml), including package-selected
tests and doctests.

These are first-party application suites. `browser-runtime` exercises desktop
startup and process ownership with fixtures; `workspace-wayland` runs the
packaged application. Firefox coverage remains indirect through these routes,
under the [repository test policy](../AGENTS.md#tests); Firefox-owned suites are
outside the permitted test set.

| Suite | Selection |
| --- | --- |
| `application-systems` | Annotation, browser, service, shared test-support, shell, data/compute, presentation, Explore, Upscale, Live, GPU, and media Live/video suites |
| `application-contracts` | Annotation, browser, service, shared test-support, presentation, Explore, Upscale, Live, and serialization suites |
| `transport` | Physical browser transport and attachment |
| `browser-runtime` | Desktop entrypoint and Firefox process-owner fixtures |
| `core` | Core acceptance, presentation, dataset, image resampling, model catalog, system/concurrency, CLI, and tool tests |
| `rfdetr` | Native RF-DETR contract, core, augmentation, training, inference, export, ML CUDA/layers, raster, and video suites |
| `rfdetr-profile` | Instrumented training profile runner; selects `dev` |
| `browser-app` | Rust/Iced protocol, workflow state, plots, transport, image-custody, and integration-driver tests; vendored `iced_plot` and selected `iced_aw` library tests; direct JavaScript adapter tests |
| `workspace-wayland` | Packaged Firefox/NVIDIA hardware acceptance |
| `cuda-vulkan` | Standalone CUDA/Vulkan allocation, FD, timeline, pixel, and exporter-exit diagnostic |
| `headless-compositor` | Real NVIDIA Weston availability and protocol checks |
| `headless-compositor-tool` | Supervisor ownership/failure fixtures without GPU |
| `cleanup-tool` | Cleanup/raw-CPD evidence and declaration-formatter fixtures |
| `log-query-tool` | Log parser/query/correlation/triage fixtures |
| `build` | Build the configured native test targets without running them |
| `all` | Build native test targets; run ordinary native executables, browser JavaScript/Rust tests, cleanup/declaration fixtures, and log-query fixtures |

`all` builds `mmltk_workspace_wayland_integration` but excludes it from its run
list. With no native executable or runner-argument filter, it then executes
`browser-app`, `cleanup-tool`, and `log-query-tool`, retaining the first nonzero status while
running the remaining groups. `all --executable TARGET` and `all -- FILTER`
remain native selections and do not append those fixture groups. `all` does not
execute `cuda-vulkan`, the compositor tooling suites, or the profile
runner. `gui` and `tsan` suite names are currently unavailable even though other
GUI/development build facilities exist.
Product-build `--no-run` checks prove Rust test compilation only. Execution
requires unfiltered `all` or standalone `browser-app`, including its JavaScript
cases.

Some RF-DETR tests download model checkpoints and derive normalized weights,
ONNX, and TensorRT engines in `.cache/tests/rfdetr` on first use. Hardware-gated
tests may skip when their requirements are unavailable; a skip is not hardware
acceptance evidence.

### Replacing an active test run

A valid new `./mmltk --test all` first stops earlier test runs owned by this
checkout. It does this before waiting
for build/cache mutation locks. A short admission lock protects registration
and replacement; it is released before the new suite runs. Registrations under
the repository cache's `locks/test-run-*.owner` cover tooling suites and gaps
between containers. Container labels and registrations identify the owner by
PID and process-start identity, avoiding an unrelated process that reused a PID.

Replacement requests graceful wrapper shutdown and stops repository-labeled
test containers, then uses bounded waits and forced cleanup when needed. It
verifies retirement before admitting the new run. If owners or containers
cannot be retired, or enumeration/verification fails, the new run is not
admitted. Normal exit removes the run's registration. Other repositories,
non-test containers, and unrelated processes are outside this replacement scope.

Focused suite names preserve active tests. An `all` invocation still replaces
earlier runs when it carries `--executable` or runner filters; use the owning
focused suite when replacement is unwanted. Test help/list routes neither
register nor cancel a run. Use [process snapshots](commands.md#process-snapshots)
for activity and [wrapper transcripts](logging.md#wrapper-build-test-and-tidy-transcripts)
for command status.

## Selection, environment, deadlines, and debugging

Put test-runner arguments after `--`:

```bash
./mmltk --test core -- --list-tests
./mmltk --test rfdetr -- '~[optin]'
./mmltk --test browser-app -- presentation
./mmltk --test browser-app -- --js-test-name-pattern 'probe'
./mmltk --test browser-app -- --rust-package mmltk-browser-app view::workflow::gpu::tests
./mmltk --test browser-app -- --rust-package iced_plot --doc
./mmltk --test log-query-tool -- FileQueryTests.test_training_failure_is_an_explicit_triage_anchor
./mmltk --test application-systems --executable mmltk_frameworks_gpu_tests \
  --env MMLTK_GDR_TEST_DEVICE=0 -- '[gdr][hardware]'
./mmltk --test all --executable mmltk_backend_imaging_explore_tests
./mmltk --test all --executable mmltk_backend_imaging_upscale_tests -- '[capture],[probe]'
```

For several Catch case names, pass one quoted comma-separated union filter,
for example `-- 'first case name,second case name'`. Separate positional names
do not express that union. An unmatched selection or **No tests ran** is failed
selection evidence, not a passing run; [log parsing](logging.md#catch-selection-and-terminal-evidence)
exposes both under `--errors`.

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
For current activity during a quiet command, use the read-only
[process snapshot](commands.md#process-snapshots); it does not interrupt the
running build or test.

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
native executable, environment, or debugger options. Optional prefix selectors
must precede ordinary Cargo arguments:

| Browser selector | Behavior |
| --- | --- |
| No prefix | Run all JavaScript cases, then the ordinary Cargo selections; Cargo filters apply to both Rust invocations |
| `--js-test-name-pattern PATTERN` | Filter JavaScript case names while retaining both ordinary Rust invocations and any following Cargo arguments |
| `--rust-package PACKAGE` | Omit JavaScript and select `mmltk-browser-app`, `iced_plot`, or `iced_aw` |

The two prefixes are mutually exclusive. `iced_aw` always retains its existing
`--lib --no-default-features --features number_input,selection_list` selection.
For example, the JavaScript `probe` command above still runs the ordinary Rust
suites; the `iced_plot --doc` command selects only that package's doctests.
`headless-compositor` accepts a command after `--` and owns its runtime.
`log-query-tool` accepts one or more `Class.test_method` selectors after `--`;
with none it runs full discovery. It rejects native test options.
`headless-compositor-tool` rejects extra arguments and native test options.
`cleanup-tool` accepts only `-- --test-name-pattern PATTERN` for filtering and
rejects native test options.

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
GCC's archive-aware `gcc-nm`. It accepts repository `.a`/`.o` paths or installed
artifacts below `/opt` in the development image, a regular
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
| `workspace_wayland_retained` | One H2D browser: Dataset presentation/disclosure/input and real cancel/restart, square/capacity growth, full controls including integer typing/paste, cached and held-miss gallery/detail returns, Detail-open resizing in both orientations, augmentation retention, fractional rows, circular wrap, partial final row, wide/tall layouts, local labels/native semantics, light/dark copy with shared Annotate layout and long lists, FPS, rapid changes, then SIGINT |
| `workspace_wayland_workflows` | One H2D browser: GPU selection and execution for Train/Validate/Predict/Export, actual Train start, live progress pixels and hidden-tab progress, retained chart interaction, validation metrics and progressive sample/detail returns, confidence editing/filtering and responsive groups, direct Validate-to-Explore pixels, compiled/image/video prediction, Pause/Resume/EOF/Stop, export/cancellation, light/dark and minimum-width pixels, then SIGINT |
| `workspace_wayland_dpi` | One H2D browser at DPI 1.5: Dataset presentation fixtures, light/dark copy, and rapid changes |
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
The padding-sensitive retained fixture explicitly chooses Letterbox through
the Dataset radio and checks the stored compile mode. The model-workflow
fixture uses the ordinary Stretch default. Padding observations therefore
remain evidence for the selected geometry rather than assumptions about every
compiled dataset.

The initial [Dataset scenario](#dataset-presentation-and-lifecycle) combines
real control input, bounded presentation fixtures, and native Directory
compilation. Benchmark release acquisition remains owned by the independent
[native fixture cases](#benchmark-compilation-evidence).

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
scenario modules. `dataset_presentation` observes Dataset drawing and owns
bounded presentation fixtures alongside the shared widget, pixel, and probe
helpers. Its private reporting owner remains effect-only.

The JSONL cursor reads one captured file extent in 16 KiB chunks, appending
newline-delimited segments into a retained buffer capped at 64 KiB per line.
Later appends wait for the next notification. Malformed native JSON, overflow,
truncation, and an incomplete final native record remain failures. Native
probe reconciliation searches the selected generation's ordered frame range;
when a source revision is fixed, browser checks establish matching slot identity
before one geometry scan. Evidence ordinals, predecessor/suffix rules,
independent stream drains, and late-conflict rejection still govern those joins.

The [workflow driver](../src/frontend/iced/src/integration_control/workflows.rs)
uses [native model/video fixtures](../src/acceptance/tests/workflow_wayland_inputs.cpp),
normal primary actions/viewers, and the packaged sibling CLI and native systems.
Typed terminals and canvas pixels are independently required for Train curves,
progress, all six Validation thumbnails/detail, each prediction source, retained
Stop output, and theme/narrow layouts. Bar capture pairs native image counts
during Train; hidden-tab checks require later metric sequences. Validation opens
detail during capture, settles successfully with detail retained, then closes it and requires all
six same-generation samples without reevaluation. Canvas checks wait for sample
rendering, which can lag native metric completion.

Each workflow selects a native inventory device through the GPU card and settles
settings. [GPU evidence](logging.md#workflow-gpu-evidence) requires bidirectional
admitted-generation/execution joins, device/rank facts, cancellation before
progress, and Output/GPU layout in all four light cards plus dark/narrow Train.
Selection labels alone do not pass.

The [Status driver](../src/frontend/iced/src/integration_control/status.rs)
exercises the real overlay using bounded in-memory notices: add/update/dismiss,
overflow, wrapping/selection of long text, exact Copy, hover/Escape/reentry,
keyboard/touch, wide/narrow/theme/modal layouts, annotation shortcut isolation,
pulse/reduced motion, and
hidden quietness. JavaScript input/canvas and independent audits join
[rendered records](logging.md#rendered-ui-acceptance-evidence) to notice identity;
fixtures are not native failures. Rust tests cover bootstrap/reconnect,
deduplication/acknowledgment, stale delivery, and clipboard custody under the
[Status contract](gui-interaction.md#session-status).

The workflow scenario also verifies the individual saved Validation PNGs,
Predict saving-control state, compiled/image samples, completed video, and
playable cancelled partial video. Media-disabled fixtures independently disable
the image/compiled report and require only the empty `.mmltk-run-claim`
reservation marker in the directory, with unchanged processing coverage.
Native media cases separately exercise timestamps/audio, write failures,
shortfall and crash-prefix recovery. The retained scenario measures all four
Output cards at the top of the right column and checks the left-column cards
preceding each primary action; Predict keeps its saving controls. Exact
[failure records](logging.md#prediction-media-acceptance) distinguish invalid
observations from missing terminal evidence.

The same browser opens Explore, runs Validate, and returns directly with no
intermediate page or extra Open. It requires [paired ready-tile pixels](logging.md#rendered-ui-acceptance-evidence),
independently of dataset readiness.

Confidence checks cover the default, exact `0.437`, blank/out-of-range retention,
inert arrow/wheel input, and settled `0 → 1 → 0` edits. The independent
[confidence pixel oracle](logging.md#rendered-ui-acceptance-evidence) verifies
filtering with unchanged metrics/evaluation generation, raw detections, and
clean identity. Layout checks require Groundtruth/Detections beneath the preview,
20-pixel spacing, narrow wrapping, and Advanced confidence text.

The workflow fixture keeps a real six-query native detector. It calibrates its
checkpoint against the model's actual selected proposals so detection captions
overlap the synthetic ground-truth boxes; its Validation request uses FP32 to
match that calibration. Training still updates the real model. The independent
[caption canvas oracle](logging.md#rendered-ui-acceptance-evidence) cycles GT/Det
visibility across atlas and detail, requires visible glyph evidence, and checks
that Det captions cover GT captions. Native completion or caption-cache contents
cannot substitute for those pixels.

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

The [physical ledger](logging.md#physical-presentation-evidence) independently
checks allocation inventories, exact acquisitions/transfers, mode-specific
settlement, encoded/submitted draws, final release, and rendered pixels, admitting
out-of-order callbacks by draw identity. The capacity fixture delivers arena
availability before a held native completion receipt, then checks the exact retry.

The pending-supersession fixture uses a typed acceptance-only hold for a
synthetic undersized candidate; it verifies the retained completed fallback
before releasing that candidate. Empty-gallery acceptance consumes its notice
only after the zero-match atlas and its paired metadata reach an encoded draw.
The physical ledger separately requires submission and settlement.
FPS acceptance combines actual submission observations with an
asynchronous canvas read of the displayed counter.

[Mode-specific physical evidence](logging.md#physical-presentation-evidence)
distinguishes direct read settlement, copy completion, and unacquired offers.
Independent per-card/full-frame probes, clipped atlas patches, and Annotation
geometry/background checks follow the [rendered evidence rules](logging.md#rendered-ui-acceptance-evidence).
[Vulkan queries](logging.md#vulkan-diagnostics-and-descriptor-provenance) inspect
validation-layer output. Controlled window close and intentional process loss
have distinct terminals; exit alone proves neither balanced userspace destruction
nor native GPU completion.

See [headless details](headless-wayland.md) for compositor readiness,
shutdown, and virtual-output limits. The [logging guide](logging.md#delivery-and-acceptance-ownership)
owns diagnostic delivery and artifact-family layout. Query one captured run
at a time.

### Dataset presentation and lifecycle

The [lifecycle driver](../src/frontend/iced/src/integration_control/lifecycle.rs)
clicks both benchmark recipes and all three Coconut validation choices through
ordinary browser input. It requires native settings settlement, expected
current-tree visibility, an inert disabled source Browse control, and restoration.
Actual draws additionally establish Coconut indentation, aligned descriptions,
small recovery typography, the revealed
clip, and label pixels at ordinary and narrow widths. The narrow input case
uses a 480-pixel canvas with the normal horizontal page scroll.

The same driver checks opening, closing, and interrupted reversal of nested
choices, plus the manual split paths and Compile size disclosures. Diagnostics
expansion supplies a non-Dataset consumer of the
[shared transition](gui-interaction.md#shared-form-expansion-and-dividers).
It observes stable absolute viewport/scroll geometry across disclosure changes,
rejects clicks into outgoing controls, edits a split path through drag selection
under simultaneous vertical/horizontal offsets, and releases a held checkbox
after scrolling it offscreen without changing its native value. Baseline
settings and the actual restored canvas layout must settle before navigation.

[dataset_presentation.rs](../src/frontend/iced/src/integration_control/dataset_presentation.rs)
renders the production progress component in 36 bounded cases: nine successive
states at widths 224 and 360 in both themes. They cover unknown totals, grouped
work counts, cached/resumed sources, shorter updates during reserved active
height, completed tracks during Syncing/Publishing, cancellation requested,
Cancelled, Failed, Completed, and late progress after settlement. These local
values never enter the application model or count as native compile work.
Canvas observations check the progress area and both shared dividers. Fixture
completion must join a settled draw containing the complete expected captions;
intermediate clipped frames establish reflow without replacing that proof.
The DPI scenario repeats these presentation fixtures at scale 1.5.

The retained scenario separately starts the real local Directory compilation,
observes its active work and all three tracks, and clicks Cancel. It waits until
native compilation is inactive and reports Cancelled, with a settled result
without live rows, then starts a new generation and requires success. The
independent browser audit reconstructs exact work and track captions from each
generation's native facts. Both the cancelled generation and the successful
restart must supply fully exposed
matching captions. A fast successful compile cannot substitute for cancellation
or active-progress evidence. Directory's acquisition caption follows the
[progress formatting policy](benchmark-datasets.md#reading-compilation-progress).

The opt-in asynchronous canvas probe also exercises reuse of borrowed inputs,
superseding a request with the same fixture key, changing/restoring canvas
geometry, and reporting owner replacement. Retired callbacks must leave the
replacement owner's canvas scratch and evidence unchanged. The
[record reference](logging.md#dataset-presentation-evidence)
defines the separate draw, caption, pixel, and probe-custody observations.
These packaged cases acquire no live benchmark release and make no production
download-throughput or full-release-capacity claim.

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
| `mmltk_controller_annotation_tests` | Independent input/render progress, normalized-run hit testing, disk cleanup against scalar support, document/history/save behavior, immutable scene reuse, complete journal moves, packed upload reuse and allocation-local damage, stable target identity through Undo/Redo, retained input pressure, ordered command continuations, fractional raster boundaries, Original crop/aspect materialization, masks beyond boxes and present-empty masks, rejection, and cancellation |
| `mmltk_controller_services_tests` | Counter-read interruption/size/error policies, reflected named settings including complete Explore fields and label visibility, benchmark choices and preview-confidence defaults/repair/persistence, immutable CUDA inventory versus saved device IDs, independent optional-test settings, training command construction and bounded child failure causes, current-format saved history, bounded cursor reads, directory replacement/truncation, and output/resume admission |
| `mmltk_controller_data_compute_systems_tests` | Start/input admission including absent or incompatible optional test splits, selected-GPU admission and missing devices, TensorRT reinspection, captured NUMA placement, complete session/stream retirement and sealed unsafe custody, selected validation results and progressive/detail retention through settlement/refusal/retry, preview-confidence recomposition without another evaluation, compact RGB8 preview transfers/reuse and failure, incremental prediction, and video playback cancellation |
| `mmltk_controller_browser_tests` and `mmltk_frameworks_serialization_tests` | Reflected field/enum/schema and graphics ABI facts, nested/array metric projection fixtures, package fixtures, positional output versus named persistence, named-field lookup/error precedence, exact CBOR bytes and borrowed map keys, split owned/borrowed payloads, UTF-8 block/page tails, lossless compact input, and control receipts |
| `mmltk_frameworks_transport_tests` | Peer replacement, reconnect, output continuity, ring wrap, and transport custody |
| `mmltk_controller_explore_tests` | Explore domain admission, settings/filter persistence, thumbnail identity, viewport priority, augmentation refresh, staged replacement, cancellation, and failure |
| `mmltk_controller_upscale_tests` | Exact receiver copies, complete retained writes, semantic-only clean preservation, checked output geometry, paired source extent, retained derived results, once-only warm admission, provider readiness/fallback, activation, cancellation, and resource retirement |
| `mmltk_controller_live_tests` | Complete retained capture writes, receiver completion/failure, held readers, queued cancellation, and settled snapshots |
| `mmltk_controller_visual_systems_tests` | Shared visual runtime, presentation protocol/custody, native gallery cache/priority/atlas integration, acceptance gates, and cross-system workspace behavior |
| `mmltk_frameworks_gpu_tests` | Independent raw-product/display storage, late workspace admission and availability wakes, Vulkan-owned CUDA import and backing lifetime, complete receiver/device transfers, sparse display coverage/coarsening/history recovery and scratch custody, acquisition/release/settlement, pressure, failure, and retirement |
| `mmltk_acceptance` | Compiled-dataset Explore integration, retained residency, projection, control-reader settlement, independent prepared/released artifacts, and bounded fatal reporting with disabled/uninitialized/failed sinks and broken pipes |
| `mmltk_entrypoints_cli_tests` and `mmltk_entrypoints_tools_tests` | Reflected CLI names/aliases/negation, canonical member presence and width-only square compilation, scalar/item/fixed-capacity error precedence and unchanged rejected destinations; CLI/ONNX fatal stderr, logging overrides and named file identities, and command exit behavior |
| `mmltk_common_concurrency_tests` | Borrowed cancellation, scoped stop-token bridging, pre-requested/concurrent cancellation, unwind, and source destruction policy |
| `mmltk_entrypoints_desktop_tests` and `mmltk_controller_firefox_process_tests` | Desktop startup/child failure status, exact launch OS errors, unexpected signal reporting, and quiet requested shutdown |
| `mmltk_backend_imaging_explore_tests` | Rendered-card geometry, semantic planes, filtered padding fringes, and exact two-sided copy evidence |
| `mmltk_backend_imaging_annotation_tests` | Resolved-mask foreground support, tight bounds, empty masks, and HSV filtering against independent scalar expectations |
| `mmltk_backend_imaging_upscale_tests` | Exact Basic sharpening bytes and guards, bitwise neural tile preparation, tile stitching, ONNX capture/replay, explicit allocation-counter ownership, and separate independent raster-oracle cases |
| `mmltk_backend_imaging_resample_tests` | Independent CPU/CUDA perceptual values including fractional SIMD lanes/tails, AVIR float4/scalar comparisons and signed rounding boundaries, bitwise quantized planar projection and CUDA accumulator traversal, worker-local reuse, checked views, completion, and resource custody |
| `mmltk_backend_imaging_raster_tests` | Pitched BGR row orientation, exact RGB8/planar-float conversion with odd extents and pitch guards, inclusive detection-confidence filtering, overwrite painter order and independent blended/additive behavior, clipped flat-mask runs, native named captions, and owned PNG staging/publication/failure settlement |
| `mmltk_backend_media_video_tests` | Packaged encoder availability, odd geometry, variable timestamps, multiple audio tracks/dispositions, write/rename failures, cancelled partial settlement, and a decodable flushed MKV prefix after SIGKILL without a trailer |
| `browser-app` | Primary-action preparation, independent optional-test/output editing, generated scalar selection, bounded live/saved chart histories and gaps, camera/legend retention and plot picking cancellation, live progress availability, validation viewer and video-control admission, shared immediate mouse input and transport retention, typed state reduction, component/crop identity, retained gallery measurements and reconciliation, exact integer/filter reduction, shared layout/navigation, image metadata independent of logical snapshots, encoded/submitted draw custody, completed fallback through navigation, local labels, FPS submission counting, exact displayed gallery interaction, quiet failure receipts, and JavaScript probe/input/callback settlement |
| `workspace-wayland` | Real training/validation/prediction workflows and actual chart/progress/sample/preview pixels, confidence editing/pixel filtering, Validation group placement/wrapping, direct Validate-to-Explore atlas pixels, dashboard aspect/retention/wheel behavior, packaged integer typing/paste and spinner/wheel policy, Detail-open resize returns, shared Annotate layout and long-list reachability, retained sessions, native-source/browser-arena identity, negotiated direct/copy draws, recovery, and shutdown |

RF-DETR backend evidence is separately owned by the core evaluator/matcher/class
layout cases, training checkpoint/continuation/EMA/telemetry and native image-count
cases, inference session/JSON cases, and ML CUDA readback/context cases selected
by `--test rfdetr`.
`mmltk_backend_data_tests` owns compilation, loading, acquisition, and exact
compiled-catalog cases; `--test core` also selects the separate resampling
target above. These checks cover functional values, boundaries, failure, and
custody; they do not substitute for the real rendered workflow case.

The data/compute target additionally owns the captured validation sample payload,
required PNG failures with retained metrics, independent export combinations,
prediction subset/reservoir selection, unchanged inference coverage, shortfall,
failed replacement preservation and browser-independent saved composition.
The service target owns exclusive run claims, numbered-directory races and
the separation between current selections and admitted output facts.

The data cases also cover locally scheduled batch capacity, empty shards,
categorical RLE sampling, parser scratch reuse, both resize modes, authoritative
fractional boxes, source metadata/order/duplicates, present-empty masks, format-9
admission, mask offsets beyond 4 GiB, and old-version rejection. RF-DETR core
cases cover full physical-slot ranking, post-selection class filtering,
query/mask alignment, focal-alpha assignment,
crowd matching/ignore precedence, original-area ranges, per-category maxDets,
and inclusive COCO thresholds. Training and inference cases exercise shared
normalization and the separate candidate/evaluation budgets. These are focused
numerical and integration checks, not a full COCO accuracy or external parity
run; [compiled-mask limits](rfdetr-workflows.md#evaluation-metrics-and-retained-samples)
still apply.

RF-DETR core cases also compare packed-mask encoding with scalar runs and
exception-time state, retained evaluation prefixes with complete ordering, and
compact matcher matrices with independent CUDA pair values and target lookup
ranges. Inference cases cover packed-readback budget/fallback decisions,
odd-byte strides, retained device/pinned capacity, and CLI image identity.
Export cases retain deterministic dependency ordering and cycle rejection;
ML layer cases check fully overwritten deformable-attention output and its
existing gradients. The common I/O cases check empty/multi-chunk file digests,
cancellation, mutation, and replacement.

The `browser-app` cases additionally cover generated resize settings, paired
source/aspect geometry and inverse input mapping, Original changes without
restarting Upscale, and rapid-navigation requests using the current selected
source while Annotation import retains the exact drawn source, crop, and target
extent. They verify shared paired caption colors and referenced-category
membership, separated GT/Det collections, actual texture-view binding reuse and
retirement, borrowed pending filter presentation, and redraw input cancellation.
They also exercise destination-owned foreground routing from every page with
and without retained Validation results, a first gallery measurement waking
pending Open, bounded pending Next/Previous movement, paired preview-confidence
caption filtering, exact debounced decimal edits, and responsive control groups.
The iced_plot shader case compares every RGBA pixel against the prior three-pass
sequence for fractional and opaque alpha, with separate fixed opaque-color,
painter-order, and clip-guard checks. Plot cases also cover caption reconciliation
after camera changes and quiet unchanged redraws. The selected iced_aw library
cases exercise retained numeric-input trees, exact editing, focus/selection,
partial decimal spellings across rebuilds, button/wheel/typed-only arrow policy,
and selection-list behavior.

Use `--test all --executable TARGET` for targets not owned by a narrower suite.
The source/CMake registrations and wrapper inventory define executable
membership; `--test core` includes image resampling, while the imaging
Annotation, Explore, and Upscale executables require `all` or explicit
`all --executable` selection. Raster is also in `rfdetr`.

## Training mathematics and compilation evidence

The [training reference](rfdetr-training.md) owns the pinned equations and
native adaptations. Coverage is colocated with these existing suites, selected
by `all` and the standalone `rfdetr` route:

| Source | Required evidence |
| --- | --- |
| [core/tests/upstream_training_parity.test.cpp](../src/backend/models/rfdetr/core/tests/upstream_training_parity.test.cpp) | Independent geometry/epsilon/promotion equations and VJPs; categorical half-pixel sampling; representation-specific empty masks; learned encoder projection; grouped classification/box and dense/sparse mask objectives; full-domain nonfinite matcher sanitization; depthwise AMP derivatives and updates; direct DN mask equations |
| [training/tests/stock_training_parity.test.cpp](../src/backend/models/rfdetr/training/tests/stock_training_parity.test.cpp) | Literal two-stage K-squared scaling, per-parameter AdamW ages, managed warmup, device clipping, first/later/resumed EMA, recoverable AMP overflow cadence, backward outside autocast and distributed group/update equations |
| [core/tests/training_supervision.test.cpp](../src/backend/models/rfdetr/core/tests/training_supervision.test.cpp) | Match-Free correspondence/probes and mask costs, background-only images, DN layout/noise/leakage protection, invalid and empty boundaries, direct mask gradients and sampling behavior |
| [core/tests/selective_compilation.test.cpp](../src/backend/models/rfdetr/core/tests/selective_compilation.test.cpp) | First recording and retained live weights, signature/tail fallback, transactional replacement, dynamic query extents, direct grouped/DN SDPA and independent decoder-tail derivatives with outstanding backwards |
| [training/tests/training_supervision.test.cpp](../src/backend/models/rfdetr/training/tests/training_supervision.test.cpp) | Production routed Hungarian/Match-Free and optional DN for boxes/masks; FP32/FP16/BF16; empty images, accumulated gradients and actual optimizer updates; current-format continuation; eager/selective values, recording/reuse/optimized execution, RNG and live state copies |
| [inference/tests/prediction_session.test.cpp](../src/backend/models/rfdetr/inference/tests/prediction_session.test.cpp) | Native weights compilation-request propagation and incremental inference behavior |
| [training/tests/execution_policy.test.cpp](../src/backend/models/rfdetr/training/tests/execution_policy.test.cpp) | Literal SGD warmup exit/round-even behavior, native schedule reference clocks, final-epoch latches, sparse membership/repeats, semantic samples, and donor identity |
| [training/tests/training_gradient_reducer.test.cpp](../src/backend/models/rfdetr/training/tests/training_gradient_reducer.test.cpp) | Logical objective under physical capacity changes, early-bucket overlap, cancellation, partial failure, and physical collective custody |
| [training/tests/distributed_training.test.cpp](../src/backend/models/rfdetr/training/tests/distributed_training.test.cpp) | Two selected physical GPUs: global objective/update equivalence, uneven/empty-rank contributions, early NCCL overlap, and cancellation/failure retirement |
| [training/tests/model_merging.test.cpp](../src/backend/models/rfdetr/training/tests/model_merging.test.cpp) | Native averaging, cadence and zero intervals, final policies/ties/duplicates, finite mask metrics, immutable publication faults, whole-session leases and replaced/mixed artifact rejection |
| [training/tests/training_continuation.test.cpp](../src/backend/models/rfdetr/training/tests/training_continuation.test.cpp) | Exact continuation inventory, SGD held values, schedules/offsets/donor support, named optimizer/EMA state, and applied epoch policies |
| [core/tests/inference_lanes.test.cpp](../src/backend/models/rfdetr/core/tests/inference_lanes.test.cpp) | Bounded lanes, ordered delivery, source-copy release, backpressure, failure, and drain |

Two-GPU cases require two eligible CUDA devices and report a skip when that
capability is absent. A single-rank pass cannot establish physical NCCL overlap
or cross-rank cancellation. Controller/contract and frontend fixtures also
cover recipe projection, source-specific live/saved metrics, selected-output
metadata, and whole-session capability admission. These cases remain inside
the existing `all` gate; they add no plan-specific acceptance command.

The complete-model selective fixture compares four accumulated microbatches
using the production `TrainingStep`, including the K-squared divisor, frozen
and trainable parameter policy, then an optimizer update. It preserves exact
RNG state across matched eager/selective forwards and losses, and exercises
training/evaluation mode changes, padded evaluation, `2 → 1 → 2` batches,
explicit disable and changed-state copies. It forces the reference deformable
attention implementation to isolate the bounded tensor regions; custom CUDA
attention has separate ML-layer evidence.

For that composed fixture, logits/boxes/losses use relative and absolute
tolerances of `0.004`; BF16 spatial features use `0.02` for both. FP32 gradients
use elementwise relative `0.01` and absolute `0.004`. AMP gradient comparisons
require finite L2 norms and bound both L2 error and maximum element error by
`0.02 * corresponding_reference_scale + 0.004` per tensor. Updated parameters
use relative `0.001` and absolute `0.0003`. Independent primitive/criterion
fixtures retain their tighter elementwise equation checks and explicit samples.
The source assertions own the exact bounds for each case; a composed AMP
tolerance is not a universal tolerance for arbitrary training inputs.

The validated NGC payload's compilation capture contained 1,163 executor-reuse
records: 26 evaluation-backbone records each contained 33 TensorExpr groups;
the other 1,137 records, including reported training-region graphs, had no
counted fusion group. The bounded extracted evidence is
`build/validation/compilation-execution-evidence-attempt7.jsonl`, retaining
original log provenance. This observation establishes graph fusion in that
evaluation backbone only. Graph capture, executor reuse, available fuser flags
and fused ATen operations do not establish training graph fusion or throughput.
The [diagnostic fields](logging.md#native-selective-compilation) distinguish
those facts.

These are controlled functional, numerical, update and lifetime checks, not a
full upstream Python training run or measured COCO accuracy/convergence study.
Native data membership and TF32 policy remain as documented in the reference.
GPU-gated cases explicitly skip when their required CUDA or physical
capabilities are unavailable; BF16 branches require compatible hardware.
GDRCopy mappings additionally need a usable `gdrdrv` or CUDA DMA-BUF mmap route.
A host lacking both cannot prove GDRCopy mapping even when CUDA/Wayland pass.

## Benchmark compilation evidence

The existing native suites exercise bounded release fixtures through the
production compiler and local HTTP server. They establish cache identity,
mask/provenance conversion, actual transfer progress, publication, and failure
behavior independently of rendered UI acceptance:

| Owner | Evidence |
| --- | --- |
| [benchmark_dataset.test.cpp](../src/backend/data/tests/benchmark_dataset.test.cpp), `mmltk_backend_data_tests` | Cache-root precedence and staging/publication overlap, completed-group and individual-JPEG reuse, typed artifact/image readiness, pixels during held labels/acquisition, single-worker execution, source-lease retirement, startup/failure unwinding, independent progress and retry withdrawal, durable segmented resume, unknown-total downloads, lazy diagnostics, and typed storage failure |
| [coconut_dataset.test.cpp](../src/backend/data/tests/coconut_dataset.test.cpp), same target | Pinned recipe selection, all validation choices, exact masks and physical/release joins, empty images, canonical paths/inventory bytes, cross-recipe reuse, immutable cold/partial/warm admissions, ready metadata during held masks/leases, aggregate indexing through repair, stock cache reuse, and unchanged prior publication on failure/cancellation |
| [application_compute_services.test.cpp](../src/controller/subsystems/system/tests/application_compute_services.test.cpp), `mmltk_controller_data_compute_systems_tests` | Environment cache selection and staged/final output overlap through the artifact service, retained choices through materialization, and real resumed/restarted/unknown-total HTTP observations projected as typed transfer facts separately from bounded activity at an unchanged image fraction |
| [dataset_system.test.cpp](../src/controller/subsystems/system/tests/dataset_system.test.cpp), same target | Explicit compile captures settled choices, later settings edits preserve the admitted request, owned diagnostics survive runtime reconstruction and stop/join, open-ended/recovered progress remains valid, and malformed progress including transfer/retained-byte bounds is rejected |
| [dataset_wiring.test.cpp](../src/controller/shell/tests/dataset_wiring.test.cpp), `mmltk_controller_shell_tests` | Production shell/factory propagation of enabled and disabled diagnostics through the staged compiler path |
| [settings.test.cpp](../src/controller/services/tests/settings.test.cpp), `mmltk_controller_services_tests` | Native defaults, missing/invalid setting repair, default-off recovery for settings missing that field, and recipe/hidden validation/recovery persistence |
| [Dataset component](../src/frontend/iced/src/view/train/dataset.rs), [progress cases](../src/frontend/iced/src/view/train/dataset/progress/tests.rs), and [integration reporting](../src/frontend/iced/src/integration_control/reporting.rs), `browser-app` | Generated selection edits, visibility/active-control policy, exact grouped/IEC quantities, independent transfer/track semantics, native terminal visibility, measured height reservation, and disabled-reporting quietness |

The existing [COCONut cases](../src/backend/data/tests/coconut_dataset.test.cpp)
also cover conservative dropped-mask matching, independent source IDs, surviving
mask subtraction, authoritative boxes, fully carved masks, and recovery-off
behavior. [Bounded source fixtures](../src/backend/data/tests/fixtures/coconut_recovery/README.md)
retain the exact two-dog masks for physical COCO image 2212 and the dog mask for
image 400. They check source support and disjoint couch/boat support after both
Stretch and Letterbox mask projection. They do not compile a production dataset
or rewrite production JPEGs/bins.

Cache cases cover recovery on/off across every validation choice, unchanged
base/physical products, original-identity changes, independently unavailable
original splits, later retry, reordered image provenance, cancellation, and
fatal local parser/archive/publication failures. Current counts come from
admitted recovery products, independently of historical failure-report lines.
Dataset-system cases capture the recovery choice with the other settled compile
settings, and later edits leave the admitted operation unchanged.

Pipeline cases hold a specific source, label, mask, or reader boundary and
require independent eligible work to complete before releasing it. They cover
duplicate readiness under queue pressure, retained pixels after quarantine,
compatible pixel reuse through repair, release-reader retirement, and staged
output capacity. These causal fixtures establish overlap and bounded custody;
they do not measure production-release throughput or live-download duration.
Progress cases retain one artifact/release contribution across interleaving,
repair, and preparation replacement, with explicit withdrawals instead of
counting repeated attempts as completed work.
They also preserve the latest artifact transfer independently of source totals,
clear it on changed activity/completion, and retain the CLI's detailed status.

The `browser-app` fixture group additionally owns
[shared transition cases](../src/frontend/iced/src/view/shared/transition/tests.rs)
for reversal, intrinsic reflow, retained widget identity, redraw quietness,
focus/overlay clipping, mouse/touch cancellation, and composed-scroller input.
Integration-driver/JavaScript cases cover asynchronous array ownership,
request retirement, and replacement-owner isolation. [Suite selection](#native-and-browser-suites)
defines build checks versus fixture execution.

For standalone focused selection outside the Final Validation gate:

```bash
./mmltk --test core --executable mmltk_backend_data_tests -- '[benchmark],[coconut]'
./mmltk --test application-systems --executable mmltk_controller_data_compute_systems_tests -- '[dataset],[benchmark],[progress]'
./mmltk --test application-systems --executable mmltk_controller_shell_tests -- '[dataset]'
```

[Packaged Dataset acceptance](#dataset-presentation-and-lifecycle) separately
proves rendering, input, cancellation/restart, and native caption agreement;
[capacity/download limits](benchmark-datasets.md#cache-formats-and-capacity) bound
these claims. Trace/serialization suites cover timestamps, effect-only delivery
failure, Unicode scalars, and malformed wire-text rejection.

## Shared fixtures and evidence

Neutral fixtures and the shared Catch runner belong to
[src/test_support](../src/test_support). Domain fixture targets live with
their owning components and publish their own declaration dependencies.

| Fixture | Ownership and use |
| --- | --- |
| [ScopedTestStream](../src/test_support/cuda_test_utils.hpp) | Neutral nonblocking CUDA stream lifetime for media and inference tests |
| [Dataset fixture](../src/backend/data/tests/test_fixture.h) | Synthetic inputs, one canonical compiler configuration, and compilation of existing deliberately modified inputs |
| [NUMA topology support](../src/common/system/tests/numa_topology_test_support.h) | Value-only selection from a caller-captured topology; each scenario retains its own snapshot and worker budget |
| [ClassArtifactFixture](../src/backend/models/rfdetr/core/tests/class_artifact_fixture.h) | RF-DETR artifact/descriptor seeding and independent SHA-256 preservation checks, including absence of staged directories |
| [Application data support](../src/controller/subsystems/system/tests/application_data_test_support.h) | Data/compute inspection fixtures and their domain facts |

Torch-backed test consumers use
[`mmltk_backend_ml_torch_test_support`](../src/backend/ml/torch/CMakeLists.txt),
whose [catch_support.h](../src/backend/ml/torch/tests/catch_support.h) admits
Torch declarations while preserving Catch2's `CHECK` assertion. Production
Torch usage has no test-support dependency.

The [metrics component cases](../src/frontend/iced/src/view/metrics.rs) drive
ordinary plot shader redraw and drag events, reduce returned messages, and
inspect settled camera/legend observations after remount. They cover loss-scale
changes for every non-loss chart, visible and hidden charts, and independent
live/saved history; repeated selection does not invalidate the view. Separate
cases verify actual loss-axis scaling and autoscale after epoch, history, or
data changes. Those component checks complement the packaged browser's
canvas-pixel and retained-interaction evidence above.

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
