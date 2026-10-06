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
log-query fixtures. Resume failed tests and tidy checks with the focused
selections below; retain successful results and finish checks left unexecuted
by an interruption. Rebuild changed product code through `./mmltk --build`
before testing. Once focused retries pass, run the required full completion
pass. Retries do not restart or reorder validation stages.

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
`./mmltk --tidy --rust-only` formats just that Rust package through the same
containerized owner, without configuring or analyzing native code. It cannot
be combined with native file, start-at, or third-party selections.
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

## PCH include audit

```bash
./mmltk --audit-includes report
./mmltk --audit-includes preview
./mmltk --audit-includes fix
```

[audit_includes.mjs](../tools/audit_includes.mjs) is a lexical inventory and
conservative replacement tool for the checked-in PCH groups. It is not semantic
include-what-you-use analysis. `report` (the default) and `preview` write reports
without changing sources; `fix` applies admitted replacements. The tool reads
the existing release graph by default. Leading `--refresh` first reconfigures
that release graph with browser-host and Firefox runtime enabled.
`--build-dir PATH` selects the graph to inspect and `--output PREFIX` selects
report paths. Use the wrapper for all three modes;
the [build reference](build.md#target-declarations-and-precompiled-headers)
owns PCH registration and generated policy paths.

Default reports are `output/include-audit.json` and `.md`. They contain folder
and large-file inventories, standard/Linux include frequencies, repeated local
include bundles, per-source compiler contexts, proposed replacements, PCH
issues, and retained includes with concrete reasons. Counts include lexical
inactive branches and are inspection aids, not proof of redundant dependencies.

Every compilation-database entry is retained. Eligibility requires each
context's owner, generated policy and exact target-local forced PCH to agree;
replacement uses the groups common to all eligible contexts. Conflicting
use/skip registration, missing groups, wrong-target forced includes, mismatched
output ownership, and hidden response-file evidence prevent rewriting.
Declaration headers remain self-contained. CUDA, retained modules and their
importers, standalone/no-PCH sources, excluded source contexts, and headers
outside consumed groups retain direct includes. A retained include is not an
unresolved cleanup finding merely because another target uses that header in a PCH.

Before editing, `fix` runs
[check_toolchain_invariants.py](../tools/check_toolchain_invariants.py) through
the wrapper, including creation/use compiler-environment equality and exclusion
checks. It preserves conditional branches and comments, verifies that each
source still matches its inspected snapshot, and atomically replaces admitted
files with their modes retained. Neither audit output nor successful rewriting
establishes build/test success or a measured PCH speedup.

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
Its JavaScript selection includes
[browser_bundle.test.mjs](../src/frontend/iced/tests/browser_bundle.test.mjs),
which exercises the native bundle publisher's Wasm-reference admission and
the index's gated window observations. This runs with the existing integration,
graphics, and recovery adapter cases.
The browser test route explicitly requests NVIDIA graphics access, selects the
container's NVIDIA Vulkan ICD files, and sets the renderer backend to Vulkan.
It applies this setup whether called directly or from `all`; missing NVIDIA
Vulkan ICDs fail the route. The browser-app/iced_plot Cargo invocation sets
`RUST_TEST_THREADS=1` to serialize cases that own independent Vulkan devices.
The separate iced_aw library invocation retains its
own test scheduling. `iced_plot` owns its native test dependencies in
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
| `data-profile` | Explicit CPU Directory compile, benchmark pipeline/archive, compaction, pixels, retained-RGB resample, full Coconut, or CUDA loader profile; defaults to the Release graph |
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
execute `cuda-vulkan`, the compositor tooling suites, or either profile
runner. Full Coconut download/compilation is never implicit in `all`.
`gui` and `tsan` suite names are currently unavailable even though other
GUI/development build facilities exist.
Product-build `--no-run` checks prove Rust test compilation only. Execution
requires unfiltered `all` or standalone `browser-app`, including its JavaScript
cases.

The log-query tooling suite also exercises wrapper capture and process
ownership with isolated checkouts and a simulated Docker command. Its runtime
cases cover [GUI replacement](commands.md#gui-session-replacement) of shared
raw sessions, an existing GUI, and an owner still preparing its container;
retention of other checkout/test owners; and rejection of a delayed older GUI
after a newer invocation has started. These checks exercise wrapper admission
and cleanup without requiring a display or stopping real application sessions.

Some RF-DETR tests download model checkpoints and derive normalized weights,
ONNX, and TensorRT engines in `.cache/tests/rfdetr` on first use. Hardware-gated
tests may skip when their requirements are unavailable; a skip is not hardware
acceptance evidence.

### Data compilation and resampling profiles

`./mmltk --test data-profile -- --help` prepares the selected profile target and
its dependencies, then prints its reflected options. The
[runner](../src/backend/data/tests/profile_runner.cpp) calls production owners;
its [shared reflected options](../src/backend/data/tests/profile_support.h)
and [option admission](../src/backend/data/tests/profile_support.cpp) serve
every mode. All runner arguments follow `--` and
paths there are container-visible paths (`/workspace` is this checkout).
Select a mode explicitly:

| Mode | Timed work |
| --- | --- |
| `Compile` | Directory preparation, STB decode/resize, labels, and compiled-file finalization; synthetic `Png` or `Jpeg` bytes use Directory's numbered `.png` filenames |
| `Pixels` | Shared benchmark decode/resize/store into bounded memory destinations; benchmark JPEG uses TurboJPEG, PNG uses STB |
| `Resample` | Shared CPU resize/store from RGB decoded before timing; setup is reported separately |
| `Pipeline` | Bounded local benchmark publication, image/label processing, file services, mapped output, and finalization; Archive input selects the focused transport variant described below |
| `Compaction` | Shared same-inode mover, metadata writes, and sync on a bounded staged fixture; setup and verification are separate |
| `Coconut` | Complete Coconut recipe with Stock validation and dropped-mask recovery, Stretch, and the chosen perceptual setting, through final publication |
| `Loader` | Directory compilation followed by sequential, shuffled, and overlapped CUDA loading; this is the default mode |

For a bounded synthetic compilation profile:

```bash
./mmltk --test data-profile -- --mode Compile --encoding Jpeg \
  --test-dir /workspace/build/validation/data-profile-compile \
  --width 384 --height 384 --perceptual --mixed-geometry --shuffle-geometry \
  --num-images 256 --compile-workers 4 --warmup-runs 1 --repetitions 3
```

`--mixed-geometry` cycles 384×384, 640×480, 768×768, 1024×768, 2048×2048,
640×360, and 320×240 inputs. `--shuffle-geometry` uses seed 1729.
`--cpu-affinity` optionally fixes an eligible Linux CPU list inherited by the
production workers. A matching mixed-core mask does not guarantee matching
core-type distribution. Use the [read-only topology diagnostic](gpu-execution.md#read-only-capability-inspection)
to select a fixed CPU/core-type set and retain its SMT/frequency-policy facts
alongside paired measurements. Printed input dimensions and SHA-256 values let
paired runs establish identical encoded inputs. Synthetic Compile/Loader working
directories are removed afterward unless `--keep-artifacts` is set; use a
dedicated scratch directory.

Pixels/Resample can instead use `--input-manifest /workspace/PATH.txt` with one
JPEG/PNG path per nonempty line. Relative entries resolve beside the manifest.
Admission limits are 64 retained inputs and 256 MiB of encoded bytes;
Resample additionally caps retained decoded RGB at 512 MiB, checked from the
decoder's actual RGB extent for both JPEG and PNG before decoding. Per-worker
output destinations total at most 512 MiB. `--num-images` cycles those inputs for the
measured iteration. `cpu_profile` reports setup separately from work time,
images/second and logical source/output bytes/second, plus retained decoded
bytes and plan count. Those output rates are memory-store work, not disk or
network throughput.

Pipeline accepts 256–1,024 images, square output, at most 64 MiB of unique
synthetic encoded inputs and 1 GiB of logical cycled input, and a checked
staging/output cap of 1–2,048 MiB (default 2,048).
`--input-storage Mapped|Pooled` selects encoded-input custody.
`--task-records 0` uses production record sizing; 1–1,024 explicitly bounds
the record bank. Encoded-input hashes are printed separately; `pipeline_profile`
JSON includes output hashes, native-fixture annotation hashes when applicable,
setup, production, and finalization including sync. A selected sync event
additionally separates sync duration. Output is verified after timing;
`--keep-artifacts` retains the latest `pipeline-last.bin` under `--test-dir`.

```bash
./mmltk --test data-profile -- --mode Pipeline --encoding Jpeg \
  --test-dir /workspace/build/validation/data-profile-pipeline \
  --width 384 --height 384 --perceptual --mixed-geometry --shuffle-geometry \
  --num-images 256 --compile-workers 4 --input-storage Mapped \
  --warmup-runs 1 --repetitions 3
./mmltk --test data-profile -- --mode Compaction --compaction-case MixedLabels \
  --test-dir /workspace/build/validation/data-profile-compaction \
  --width 384 --height 384 --num-images 1024 --output-cap-mib 16384 \
  --warmup-runs 1 --repetitions 3
```

Compaction accepts 1–4,096 slots and a checked cap of at most 16,384 MiB;
the default cap remains 2,048 MiB. `Dense`, `Sparse`, and `MixedLabels` vary live
extents and label occupancy. Each iteration checks live-payload hashes,
physical slot identity, staged inode identity, and the final layout.
`compaction_profile` separates setup, movement, metadata, sync, and verification;
`compaction_sync_sec` includes movement through sync. `--compaction-cold` syncs
and advises only that fixture out of cache; it does not establish a cold host
or device. The temporary staged file is retired after verification.

Pipeline's focused causal variants reuse production owners:

- `--input-storage Archive` accepts 128–256 members in a generated gzip archive
  bounded to 64 MiB. It checks exact returned bytes and physical resource
  retirement, reports `archive_profile`, and does not run image conversion or
  publish a compiled split. `--archive-pressure None|Arena|Bytes|Exclusive`
  selects bounded admission pressure; non-None requires Archive input.
- `--annotations Ready|Gather|DeferredClean|DeferredRecovery` selects ordinary
  ready labels, normalized gather, unchanged native finalization, or deferred
  recovery. Non-Ready variants require Mapped/Pooled input and at least 256
  images. Native/gather preparation precedes image publication, so these fixtures
  establish work counts and custody, not full-recipe overlap.
- `--completion-gaps` records bounded image-completion intervals only when
  `profile.pipeline.completed` is selected. These traced intervals are not
  UI frame times.

For selected causal evidence, add `--trace-file /workspace/PATH.jsonl` and repeat
`--trace-prefix`, for example `compile.execution.retired`,
`benchmark.pipeline.retired`, or `profile.compaction.summary`. Other useful
aggregates are `benchmark.archive.completed`, `profile.pipeline.completed`,
and `profile.resize.completed`; [logging](logging.md#benchmark-compilation-traces)
defines their fields and collection demands. Setup, warmups, measured samples,
trace policy, hashes, affinity, and output checks all belong in a comparison.

Coconut mode requires `--cache-dir` and square target dimensions. It uses the
complete recipe: `--num-images` does not limit its population. Each warmup or
repetition is another complete compilation and can acquire/repair missing cache
sources. Its output remains under `--test-dir/compiled`. An explicitly requested
single production measurement therefore uses zero warmups and one repetition:

```bash
./mmltk --test data-profile -- --mode Coconut \
  --cache-dir /workspace/.cache/benchmark-dataset/v1 \
  --test-dir /workspace/build/validation/data-profile-coconut \
  --width 384 --height 384 --perceptual --compile-workers 24 \
  --warmup-runs 0 --repetitions 1
```

The example cache path is the default; supply the selected retained cache's
actual container path. Coconut prints phase changes and at most one ordinary
progress observation every five seconds, including canonical
`phase_completed`/`phase_total`, independent tracks, and JSON-escaped activity.
During compaction those phase quantities expose byte progress. Final
`coconut_profile` counts come from the published manifest; output bytes are
logical compiled lengths, not physical disk allocation or device throughput.

Tracing is explicit via `--trace-file`; repeat `--trace-prefix` to collect
selected event prefixes. A prefix requires a trace file. Selected aggregates
still incur their own collection and delivery costs; omit the file option for
untraced throughput comparisons. [Trace fields and query statistics](logging.md#benchmark-compilation-traces)
separate CPU time, worker wall-time sums, and elapsed time. Controlled fixtures
remain the routine compiler checks; full-cache acquisition and production-sized
Coconut profiles are separate opt-in work.

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

Repeat `--address HEX` with exactly one repository or installed `/opt` ELF
artifact to resolve instruction addresses through `addr2line`. Values accept
hexadecimal digits with an optional `0x`; their count cannot exceed `--limit`.
This route emits one JSONL `owner: "native-symbols"`, `event: "address"` record
per result, with `artifact`, `address`, and the demangled location in `message`.
It does not emit the symbol-list summary. Use addresses in that artifact's
address space and retain the matching binary/debug information; unresolved
locations or symbols alone do not establish an implementation defect.

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
./mmltk --diagnose-gpu-program --torch ./probe.cpp
```

This wrapper capability compiles one C++26 source against the CUDA driver and
Vulkan using the existing development image, then runs it in the existing
Wayland validation image. `--torch` must precede the source path and adds the
pinned PyTorch headers, its C++ ABI definition, Torch/C10 CPU and CUDA libraries,
and the CUDA runtime. The wrapper queries that installed Torch package for its
location and ABI; it does not install a diagnostic dependency. Extra arguments
after the source go directly to the program.
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
| `workspace_wayland_workflows` | Two H2D browsers: GPU selection and execution for Train/Validate/Predict/Export, actual Train start, resize during training, live progress pixels and hidden-tab progress, retained chart interaction, validation metrics and progressive sample/detail returns, confidence editing/filtering and responsive groups, direct Validate-to-Explore pixels, compiled/image/video prediction, Pause/Resume/EOF/Stop, export/cancellation, light/dark and minimum-width pixels; a second lifetime injects a resized canvas allocation failure and requires visible manual recovery plus completed native training; both end with SIGINT |
| `workspace_wayland_dpi` | One H2D browser at DPI 1.5: Dataset presentation fixtures, light/dark copy, and rapid changes |
| `workspace_wayland_terminal` | Two H2D browsers: a real window close and abrupt browser-peer loss after an Annotation edit, exact completed draw, and independent redraw |
| `workspace_wayland_probe_recovery` | Four H2D browsers with startup-latched allocation, reset, begin, or end probe failure; exact-content recovery and complete final pixel/semantic evidence |
| `workspace_wayland_quiet` | Two H2D browsers: blocked reads and a completed gesture with diagnostics/reporting/probes inactive |
| `workspace_wayland_gdr` | One optional GDR browser; unavailable hardware remains an explicit skip |

This is twelve required H2D lifetimes and one independently optional GDR lifetime.
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

Before cancelling and again before the successful generation settles, the
retained scenario requires four active resize steps: 480×900, 960×720,
fullscreen, and restored window. Each joins a later native revision, a later
Dataset root draw, the actual Iced root covering the canvas, matching CSS/backing
and configured/presented extents, and zero recorded pointer, wheel, or keydown
events between the resize request and completion.
Fullscreen entry/exit must change the real viewport through Firefox's native
window path. DOM fullscreen state or a redraw request alone is insufficient.
The [resize observations](logging.md#dataset-presentation-evidence) distinguish
unmet layout, progress, configuration, and presentation predicates.

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
| `mmltk_entrypoints_cli_tests` and `mmltk_entrypoints_tools_tests` | Reflected CLI names/aliases/negation, canonical member presence and width-only square compilation, GPU selection for bounded streaming and training-option exclusivity, scalar/item/fixed-capacity error precedence and unchanged rejected destinations; CLI/ONNX fatal stderr, logging overrides and named file identities, and command exit behavior |
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
fractional boxes, source metadata/order/duplicates, present-empty masks, format-10
arithmetic and admission, sample-local run offsets, inactive physical holes,
measured label maxima, in-place compaction, and old-version rejection. RF-DETR core
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
native adaptations. The [review index](reviews/rfdetr-math/README.md) preserves
the four baseline reviews, while the [current results](reviews/rfdetr-math/current-results.md)
record the completed bounded acceptance and its limits. Coverage is colocated
with these existing suites, selected
by `all` and the standalone `rfdetr` route. This inventory describes source
coverage, not a record that the current checkout passed these cases:

| Source | Required evidence |
| --- | --- |
| [layers/tests/ms_deform_attn.test.cpp](../src/backend/ml/layers/tests/ms_deform_attn.test.cpp) | Custom CUDA attention, exact support boundaries, nonfinite propagation, chunk tails, dispatcher capture and mixed-precision replay/VJPs |
| [core/tests/upstream_model_parity.test.cpp](../src/backend/models/rfdetr/core/tests/upstream_model_parity.test.cpp) | Actual pinned upstream model/resize artifacts, complete fixed-cotangent VJPs, discrete identities, and numerical-policy admission |
| [training/tests/upstream_optimizer_parity.test.cpp](../src/backend/models/rfdetr/training/tests/upstream_optimizer_parity.test.cpp) | Actual upstream criterion at matched weights, complete gradients/clipping, identical-gradient production AdamW state/updates, and separately measured independent trajectories |
| [core/tests/upstream_training_parity.test.cpp](../src/backend/models/rfdetr/core/tests/upstream_training_parity.test.cpp) | Independent geometry/epsilon/promotion equations and VJPs; categorical half-pixel sampling; representation-specific empty masks; learned encoder projection; grouped classification/box and dense/sparse mask objectives; full-domain nonfinite matcher sanitization; depthwise AMP derivatives and updates; direct DN mask equations |
| [training/tests/stock_training_parity.test.cpp](../src/backend/models/rfdetr/training/tests/stock_training_parity.test.cpp) | Literal two-stage K-squared scaling, per-parameter AdamW ages, managed warmup, device clipping, first/later/resumed EMA, recoverable AMP overflow cadence, backward outside autocast and distributed group/update equations |
| [core/tests/training_supervision.test.cpp](../src/backend/models/rfdetr/core/tests/training_supervision.test.cpp) | Match-Free correspondence/probes and mask costs, background-only images, DN layout/noise/leakage protection, invalid and empty boundaries, direct mask gradients and sampling behavior |
| [core/tests/selective_compilation.test.cpp](../src/backend/models/rfdetr/core/tests/selective_compilation.test.cpp) | First recording and retained live weights, signature/tail fallback, transactional replacement, dynamic query extents, direct grouped/DN SDPA and independent decoder-tail derivatives with outstanding backwards |
| [core/tests/full_compilation.test.cpp](../src/backend/models/rfdetr/core/tests/full_compilation.test.cpp) | First-real-forward full tracing with custom CUDA/depthwise operators, replay/VJPs, signature/mode/owner changes, output inventory and explicit supervision restrictions |
| [training/tests/training_supervision.test.cpp](../src/backend/models/rfdetr/training/tests/training_supervision.test.cpp) | Production Hungarian/Match-Free and optional DN, FP32/FP16/BF16, empty images, accumulated updates, eager/selective values/RNG/live state; target-ring reuse and delayed backward protection, partial DMA failure, selected donor uploads, next-batch preparation with exact continuation and parameter versions |
| [inference/tests/prediction_session.test.cpp](../src/backend/models/rfdetr/inference/tests/prediction_session.test.cpp) | Native weights compilation-request propagation, file-image resampling, incremental inference, and export snapshot isolation including failed publication |
| [training/tests/execution_policy.test.cpp](../src/backend/models/rfdetr/training/tests/execution_policy.test.cpp) | Literal SGD warmup exit/round-even behavior, native schedule reference clocks, final-epoch latches, sparse membership/repeats, semantic samples, and donor identity |
| [training/tests/training_gradient_reducer.test.cpp](../src/backend/models/rfdetr/training/tests/training_gradient_reducer.test.cpp) | Logical objective across physical capacities, early-bucket overlap, direct exclusive-gradient storage and alias isolation, exact count-wave/attempt admission, cross-stream count/metric completion, cancellation, partial failure and physical collective custody |
| [augmentation/tests/gpu_augment.test.cpp](../src/backend/models/rfdetr/augmentation/tests/gpu_augment.test.cpp) | Deterministic prepared identities/donors, RGB/output-domain paths, high-water reuse, host staging retirement before delayed pixels, exact visible support and erasure geometry, continuous boxes with empty raster masks, failure custody |
| [training/tests/distributed_training.test.cpp](../src/backend/models/rfdetr/training/tests/distributed_training.test.cpp) | Two selected physical GPUs: global objective/update equivalence, uneven/empty-rank contributions, early NCCL overlap, and cancellation/failure retirement |
| [training/tests/distributed_placement.test.cpp](../src/backend/models/rfdetr/training/tests/distributed_placement.test.cpp) | Independent known/unknown GPU locality resolution and explicit rank placement |
| [training/tests/model_merging.test.cpp](../src/backend/models/rfdetr/training/tests/model_merging.test.cpp) | Native averaging, cadence and zero intervals, final policies/ties/duplicates, finite mask metrics, immutable publication faults, whole-session leases and replaced/mixed artifact rejection |
| [training/tests/training_continuation.test.cpp](../src/backend/models/rfdetr/training/tests/training_continuation.test.cpp) | Exact continuation inventory, SGD held values, schedules/offsets/donor support, named optimizer/EMA state, and applied epoch policies |
| [core/tests/inference_lanes.test.cpp](../src/backend/models/rfdetr/core/tests/inference_lanes.test.cpp) | Bounded lanes, ordered delivery, source-copy release, backpressure, failure, and drain |

Two-GPU cases require two eligible CUDA devices and report a skip when that
capability is absent. A single-rank pass cannot establish physical NCCL overlap
or cross-rank cancellation. Controller/contract and frontend fixtures also
cover recipe projection, source-specific live/saved metrics, selected-output
metadata, and whole-session capability admission. These cases remain inside
the existing `all` gate; they add no plan-specific acceptance command.

The data suite's [roundtrip cases](../src/backend/data/tests/roundtrip.test.cpp)
exercise reordered/repeated source reads with preserved destinations, an upload
that completes while another slot's consumer is held, retained buffer reuse,
and independent read failure/cancellation under both admitted transfer routes.
They are included by `all`; their existence does not establish throughput or
hardware coverage for an unexecuted route.

The small complete-model compilation fixture compares four accumulated microbatches
using the production `TrainingStep`, including the K-squared divisor, frozen
and trainable parameter policy, then an optimizer update. It preserves exact
RNG state across matched eager/selective forwards and losses, and exercises
training/evaluation mode changes, padded evaluation, `2 → 1 → 2` batches,
explicit disable and changed-state copies. It exercises Selective with reference
and custom CUDA deformable attention, and FullTrace with custom CUDA attention
for stock supervision. Match-Free and DN retain their explicit FullTrace
restriction. Custom attention also has independent ML-layer evidence.

For that composed fixture, logits/boxes/losses use relative and absolute
tolerances of `0.004`; BF16 spatial features use `0.02` for both. FP32 gradients
use elementwise relative `0.01` and absolute `0.004`. AMP gradient comparisons
require finite L2 norms and bound both L2 error and maximum element error by
`0.02 * corresponding_reference_scale + 0.004` per tensor. Updated parameters
use relative `0.001` and absolute `0.0003`. Independent primitive/criterion
fixtures retain their tighter elementwise equation checks and explicit samples.
The source assertions own the exact bounds for each case; a composed AMP
tolerance is not a universal tolerance for arbitrary training inputs.
The one-query SDPA positional-gradient cancellation case permits the same
`0.003` absolute rounding residue as its dense gradient oracle. The separate
exact next-batch/Resume trajectory fixture enables deterministic cuDNN and
disables cuDNN benchmarking for its comparison, then restores both settings.
That fixture policy does not change ordinary training defaults.

A retained compilation capture from the validated NGC payload contained
1,163 executor-reuse records: 26 evaluation-backbone records each contained 33 TensorExpr groups;
the other 1,137 records, including reported training-region graphs, had no
counted fusion group. The bounded extracted evidence is
`build/validation/compilation-execution-evidence-attempt7.jsonl`, retaining
original log provenance. This historical observation establishes graph fusion in
that evaluation backbone only; it is not a fresh acceptance run of the current
implementation. Graph capture, executor reuse, available fuser flags
and fused ATen operations do not establish training graph fusion or throughput.
The [diagnostic fields](logging.md#native-selective-compilation) distinguish
those facts.

These are controlled functional, numerical, update and lifetime checks. The
independent producer below executes actual upstream model/criterion/optimizer
work on bounded saved inputs; it is not a full dataset training or measured
COCO accuracy/convergence study.
Native data membership and TF32 policy remain as documented in the reference.
GPU-gated cases explicitly skip when their required CUDA or physical
capabilities are unavailable; BF16 branches require compatible hardware.
GDRCopy mappings additionally need a usable `gdrdrv` or CUDA DMA-BUF mmap route.
A host lacking both cannot prove GDRCopy mapping even when CUDA/Wayland pass.

### Independent upstream diagnostic

```bash
./mmltk --diagnose-rfdetr-upstream --help
./mmltk --diagnose-rfdetr-upstream --upstream ../rf-upstream \
  --output build/validation/rfdetr-math/upstream
./mmltk --diagnose-rfdetr-upstream --upstream ../rf-upstream \
  --output build/validation/rfdetr-math/upstream-math --sdpa math
```

Choose a fresh output directory for each production run. Admission requires a
clean upstream checkout, including untracked files, at
`e9a138f70cc14e3fddca029a0683055e34ab8f35`, plus the cached official
`rf-detr-nano.pth` and `rf-detr-seg-nano.pt` under the repository cache's
`mmltk/weights`. Source-specific command-local Git trust and read-only Git
metadata mounts support linked worktrees and differently owned clean checkouts
without changing global Git configuration. The source and weights remain
read-only; the producer runs offline with a two-hour deadline. The
[validation-only image](build.md#upstream-reference-image) retains the pinned
donor Torch/CUDA environment.

[The producer](../tools/generate_rfdetr_upstream_oracle.py) imports the actual
upstream model, matcher, criterion, and PyTorch optimizer. It records original
official weight identities, normalized state, constructor facts, inputs/masks,
outputs, operator boundaries, fixed cotangents/VJPs, stochastic samples,
criterion terms, optimizer groups/moments/ages, and held-input outputs.
`manifest.json` uses `mmltk.rfdetr.upstream-oracle.v1`, with source/payload/
dependency/producer hashes, effective model and execution policy, and a checked
tensor inventory for every archive. Unsupported precisions are explicit.

The bounded stock matrix uses Nano 384 and SegNano 312, two Group-DETR groups,
FP32/FP16/BF16 where available, batches one/two, and train/eval. Nano FP32 batch
two training retains eight updates; the other cases retain one. The native
comparison uses production custom CUDA attention. Default SDPA retains backend
selection; `--sdpa math` explicitly selects upstream math SDPA and the native
model-local math route. Both sides disable matmul/cuDNN TF32 for this controlled
comparison; the manifest records float32 matmul precision `highest`.
AMP main references use original upstream matcher functions with FP32 operands;
the untouched upstream AMP matcher is also executed and saved under `stock/`.
Its differences remain explicit policy observations.

After production, the wrapper runs `[upstream_oracle]` in both existing core
and training executables. Only both successful stages publish
`verdict.json` with `status=passed_required_checks`. The manifest's
`references_complete_native_comparison_pending` status describes completed
reference production and is not rewritten into a native-pass claim.
References, appended `comparisons/*.jsonl`, `wrapper.log`, and executable logs
survive comparison failure; cancellation settles the producer or child test
owner without cancelling unrelated operations. Retry comparisons against
retained artifacts without regenerating them:

```bash
./mmltk --test rfdetr --executable mmltk_backend_models_rfdetr_core_tests \
  --env MMLTK_RFDETR_UPSTREAM_ORACLE_DIR=/workspace/build/validation/rfdetr-math/upstream \
  -- '[upstream_oracle]'
./mmltk --test rfdetr --executable mmltk_backend_models_rfdetr_training_tests \
  --env MMLTK_RFDETR_UPSTREAM_ORACLE_DIR=/workspace/build/validation/rfdetr-math/upstream \
  -- '[upstream_oracle]'
```

Absent optional artifacts produce a skip, not independent parity evidence.
Supplied invalid artifacts fail admission. The optional
`MMLTK_RFDETR_UPSTREAM_REPEAT_DIR` supplies a second independently
generated reference for upstream-versus-upstream comparisons. It is not needed
by the ordinary public diagnostic. [Logging](logging.md#upstream-numerical-comparisons)
explains record fields, retries, and bounded queries.

### Numerical gates and trajectory observations

[OracleComparison](../src/backend/models/rfdetr/core/tests/upstream_oracle_fixture.cpp)
owns these exact bounds. Value comparisons use elementwise
`abs(error) <= atol + rtol * abs(reference)`. Derivative comparisons require
both the maximum error and L2 error to satisfy that formula using the
corresponding reference maximum or L2 norm.

| Comparison | FP32 rtol / atol | FP16 rtol / atol | BF16 rtol / atol |
| --- | --- | --- | --- |
| Forward, criterion values, full parameter values | `0.0003 / 2e-5` | `0.015 / 0.002` | `0.03 / 0.002` |
| Complete derivative vectors and update deltas | `0.002 / 1e-7` | `0.015 / 1e-7` | `0.03 / 1e-7` |
| Optimizer second moments | `0.002 / 1e-12` | `0.015 / 1e-12` | `0.03 / 1e-12` |

Every per-tensor derivative discrepancy remains reported at its original bound,
including near-null attention key biases. The required derivative gate joins
the complete defined tensor inventory into one vector. At each matched training
step, the model and criterion start from the exact saved upstream weights.
Production `NativeOptimizer` then consumes identical saved upstream clipped
gradients while retaining its own continuous moments and ages; full parameters,
moments, ages, and groups have separate comparisons. Parameter deltas additionally
allow one representable FP32 parameter interval at
`max(abs(reference_before), abs(reference_after))` before the unchanged error
bound. Subtracting nearby stored weights cannot resolve smaller updates. Raw
errors, original normalized errors, and the explicit interval remain in the
report. This allowance does not apply to arbitrary forward or gradient errors.

Matched-state selection/assignment/sample identities and optimizer ages are
exact. Hyperparameters admit eight float64 epsilons for equivalent arithmetic,
with zero settings and group membership exact. Independent native/upstream
trajectories and independent Match-Free AdamW updates are observations, not
equality gates. Their measurement mode still fails invalid
inventories, shapes, dtypes, definedness, and nonfinite values. Match-Free is a
native objective extension: its compiled forward, objective, and complete
derivative gates compare against native eager execution, not stock upstream
objective parity. Representative coverage uses official pretrained Nano 384,
a frozen encoder, 13 groups, FP16/BF16, production `TrainingStep`, and fused AdamW.

Independent upstream reruns themselves exceed original strict per-tensor and
trajectory bounds. That measured fact motivated the separation above; it does
not identify every native discrepancy as upstream noise. PyTorch distinguishes
[finite-precision operation order](https://docs.pytorch.org/docs/2.14/notes/numerical_accuracy.html)
from [seeded randomness and deterministic algorithms](https://docs.pytorch.org/docs/2.14/notes/randomness.html).
The [current results](reviews/rfdetr-math/current-results.md#measured-trajectories)
retain the material remaining trajectory differences and the unmeasured AP limit.

### Training startup and monitoring validation record

The 2026-10-04 GUI capture for `output/train/run-0021` spent 63.066 seconds
in dataset preparation. The selected `compiled/train.bin` contains 587,017
eligible images, 5,069,379 labels, and 546,101,044 mask runs. Full admission
walked those payloads before sampling or training. GPU monitoring starts with
live training records, after this preparation stage.

The read-only diagnostic in
`build/validation/training-startup-measurement.log` measured the trusted
header/index open at **15.289 ms** and full payload validation at **115.258 s**.
Both ran serially in one invocation without requested cache eviction, while a
user training run was active. An earlier full validation of recently accessed
data took **1.109 s** (`training-startup-serial.jsonl` in the same directory).
These measurements show cache/load sensitivity and the work removed from
training admission; they do not measure complete training startup or establish
a controlled end-to-end speedup. Sampling still needs class membership.

Focused checks after `./mmltk --build` passed:

| Coverage | Result | Local artifact under `build/validation/` |
| --- | --- | --- |
| Compiled admission, trusted/full and serial/parallel | 3 cases, 266 assertions | `training-startup-data-tests.log` |
| Training telemetry and deterministic sampling plans | 20 cases, 2,599 assertions | `training-startup-training-tests.log` |
| Preparation status and fatal-cause publication | 2 cases, 48 assertions | `training-startup-process-tests.log` |
| Compiled diagnostic boundaries | 5 cases, 139 assertions | `training-startup-diagnostic-tests.log` |
| Rust chart projection, retention, gaps, and selection | 33 cases | `training-charts-browser-tests.log` |
| Browser pixel evidence, including flat chart boundaries | 9 cases across focused runs | `training-charts-pixel-proof-tests.log`, `training-charts-pixel-proof-tests-retry.log` |
| Physical GPU monitor and CUDA-context preservation | 2 cases, 22 assertions, no skips | `training-charts-gpu-tests.log` |
| Log queries, including bounded JSON documents | 183 cases | `training-json-log-tests.log` |

After the pixel-probe correction, `./mmltk --build` passed again
(`training-charts-pixel-proof-build.log`). The focused rendered gate
`./mmltk --test workspace-wayland --headless-compositor -- 'workspace_wayland_workflows'`
then passed, including normal training/chart interaction and the second
browser's injected graphics-failure recovery. Its transcript is
`training-charts-wayland-resumed.log`; `training-charts-rendered-evidence.jsonl`
retains all four performance-chart pixel observations. The probe includes the
full chart height so nearly flat used/total memory lines at opposite plot edges
remain observable. Earlier resource-limited attempts ran while the user's
training occupied the display GPU; the successful run followed its release.
Existing successful checks were retained through focused retries. No full
`all` suite, production dataset compilation, or Firefox-owned tests were run
for this change.

The earlier CLI/GUI throughput captures are not matched: CLI
`checkpoints/nano-match-free2` froze the encoder and disabled denoising; GUI
`output/train/run-0018` trained the encoder with denoising enabled. Their reader
and prefetch settings also differed. The comparison is retained in
`build/validation/training-gui-cli-comparison.jsonl`; its throughput values
cannot establish GUI overhead. GUI progress reads in the later capture averaged
about 0.3 ms across 239 observations (`training-gui-progress-read-cost.log`).
All these artifacts are ignored local evidence, absent from a fresh checkout.

## Benchmark compilation evidence

The existing native suites exercise bounded release fixtures through the
production compiler and local HTTP server. They establish cache identity,
mask/provenance conversion, actual transfer progress, publication, and failure
behavior independently of rendered UI acceptance:

| Owner | Evidence |
| --- | --- |
| [benchmark_dataset.test.cpp](../src/backend/data/tests/benchmark_dataset.test.cpp), `mmltk_backend_data_tests` | Cache-root/output overlap, retained generations and row-admission proofs, shared CPU/byte/descriptor/storage admission, bounded image services and continuations, mixed ordinary/range transport, Open Images continuation, independent pixels/labels/cache work, stable slot writes and compaction, cancellation/repair, native progress units, and lazy diagnostics |
| [coconut_dataset.test.cpp](../src/backend/data/tests/coconut_dataset.test.cpp), same target | Pinned recipe/validation choices, exact masks and physical/release joins, independent image/annotation acquisition, archive/Parquet continuation, native recovery, source ordinals and incomplete provenance, healthy subsets, Large/XL claim replacement, cache reuse/repair, and atomic publication |
| [roundtrip.test.cpp](../src/backend/data/tests/roundtrip.test.cpp), same target | Fixed layout/record admission, capacity and measured maxima, malformed/inactive-slot rejection, present-empty masks, Directory output equality across workers, prepared stream reuse, and preservation of published output on failure |
| [compile_progress.test.cpp](../src/backend/data/tests/compile_progress.test.cpp), same target | Exact counters behind a held observer, dirty deadlines and lifecycle/repair flushes, terminal/failure propagation, preparation budgets, Directory continuations and class floors, annotation/mask meaning, and complete capacity rejection |
| [benchmark_image_diagnostic.test.cpp](../src/entrypoints/tools/tests/benchmark_image_diagnostic.test.cpp), `mmltk_entrypoints_tools_tests` | CMake-owned native helper execution, format-10 physical slot admission, malformed/old-format rejection, normalized mask export, and raw PNG/Parquet inspection |
| [application_compute_services.test.cpp](../src/controller/subsystems/system/tests/application_compute_services.test.cpp), `mmltk_controller_data_compute_systems_tests` | Environment cache selection and staged/final output overlap through the artifact service, retained choices through materialization, and real resumed/restarted/unknown-total HTTP observations projected as typed transfer facts separately from bounded activity at an unchanged image fraction |
| [dataset_system.test.cpp](../src/controller/subsystems/system/tests/dataset_system.test.cpp), same target | Explicit compile captures settled choices, later settings edits preserve the admitted request, owned diagnostics survive runtime reconstruction and stop/join, a retry publishes real compiled output after failure leaves the destination absent, compilation/inspection/publication failure traces preserve their original cause, open-ended/recovered progress remains valid, and malformed progress including transfer/retained-byte bounds is rejected |
| [path_publication.test.cpp](../src/common/io/tests/path_publication.test.cpp), `mmltk_backend_data_tests` | New and replacement file/directory publication, occupied empty directories and dangling symlinks, parent-sync rollback, and best-effort old-generation cleanup after commit |
| [numa_memory.test.cpp](../src/common/system/tests/numa_memory.test.cpp), `mmltk_common_system_tests` | Page-locked ownership/move/reuse, locked-page discard refusal, per-page kernel error context, failed-candidate rollback, and preservation of existing storage |
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
source-local parser/archive rejection and fatal local I/O/publication failures.
Current counts come from
admitted recovery products, independently of historical failure-report lines.
Dataset-system cases capture the recovery choice with the other settled compile
settings, and later edits leave the admitted operation unchanged.

Pipeline cases hold a specific source, label, mask, or reader boundary and
require independent eligible work to complete before releasing it. Their
causal coverage includes:

| Boundary | Cases in the data suites |
| --- | --- |
| CPU admission | Waiting live-class floors, activation/retirement, fewer-core fairness, blocked-demand bypass/aging, shared Curl/file/external grants, class-aware file dispatch, returned CPU grants while compute callbacks still occupy their workers, and cancellation of queued/active borrowers |
| Image preparation | Dedicated bounded file capacity behind held cache/label output, stable service workers and pinned storage on small CPU plans, stage selection under a hard record bank, retained continuations ahead of later workspace waiters, checked inode preload, and physical cancellation/failure drain before another attempt |
| Memory and descriptors | Fixed pinning across class/lane reassignment, protected completion windows, one exceptional lineage and retained descendants, Arrow/encoded allocation windows, finite loans including zero-byte offers, nested descriptor commitments, low limits, and physical release before reacquisition |
| Filesystem growth | Shared destination-filesystem reservations, sparse occupied allocation outside source locks, physical output leases, immutable staging blocks, source-safe same-inode compaction, staged publication, and exception cleanup |
| Transport | Contended artifact locks, whole files alongside stored ranges, one-connection resume, rejected validators/coverage, durable out-of-order checkpoints, redirects, fair Curl admission, Open Images retries, and channel cancellation |
| Archives and Parquet | Requested members before unused tails, consumed checksum/truncation errors, raw/ZIP/sparse/gzip continuation, bounded gzip dictionaries/controls, retained Parquet rows/batches, independent groups, forward reader/window reuse through ordinary unrelated contention, bounded metadata readiness inspections, and exceptional reader retirement/replay |
| Immutable products | Mapped indexes/images across path replacement, exact row-admission proof reuse and geometry rejection, same-inode cache publication with independent destinations, independent original split publication/withdrawal, retained native support through repair, rebound view generations through component composition, rejection of stale composed labels, full-backing component dependencies, and cached groups that omit payload projections |
| Readiness and source custody | Per-row physical registrations, release-scoped namespace/generation original subscriptions, already-delivered generations and retired-row withdrawal, EOF descriptor/continuation release, cache preparation reusing a controller/sibling's live source lease, cold/retained late original and shared-physical repair, and fatal recovery settlement while originals/JPEG geometry remain held |
| Final labels | Labels before JPEG arrival and pixels before annotations, per-image geometry admission, canonical ordinal settlement/backpatches, explicit incomplete provenance across cache reuse and later repair, Large-over-XL claim restoration, present-empty masks, and validation progress during held training |
| Healthy subsets | Partially valid labels, true backgrounds, rejected/unlisted physical holes, unreadable annotation artifacts and pixels, bounded repair, contradictory geometry, empty required splits, and preservation of old publication on fatal failure |
| Fixed storage | 64-slot boundaries, checked extents, 500,000-byte payload overflow, eligible-only maxima including zero, cancellation/fault preservation, unified label/pixel read preparation, bounded prefault/gather continuations, repeated logical draws, and prepared capacities through epoch changes |

Additional compiler cases exercise exact slice exhaustion and non-FIFO
coalescing, alias/alignment custody, usable pinned tails, node-pooled normal
capacity, atomic continuation admission, resource-epoch/selective wake behavior,
and fairness without reclamation. Publication cases hold writes while later
members advance, preserve forward readers across ordinary backpressure, and
drain accepted input custody on cancellation. Archive replay checks bounded
header work across extended tar, GNU names, ZIP, and compressed tar; provenance
settlement checks batch values while preserving masks and identities. Storage
cases require one capacity sample through sibling settlement, independent
filesystem ledgers, and unchanged promises on failure. Range-batch cases cover
raw-written and preallocated extents, overlapping label captures, scratch
boundaries, partial mutation failures, and cancellation without publication loss.
Diagnostic cases prove disabled and throwing observers cannot change outcomes,
prefix demand crosses adapters before field construction, and selected retirement
retains complete costs without transient snapshots. Readiness cases cover
coalesced arrivals during callback delivery, stale completion handles after
record reuse, finite dispatch reaching a fitting tail, and ordinary waits that
do not enter the exceptional-drain index.

Archive cases require useful byte/descriptor reclamation across multiple victims,
retention under unrelated arena pressure, changed pressure demand delivery, and
bounded read turns while peers progress. Annotation cases cover ready-row
gathering across empty/oversized slices, held-peer cancellation, unchanged staged
native payload proof/lineage, and final-cache reuse beside modern parts or held
original snapshots.

Resampling cases check immutable algorithm selection, poisoned scratch across
mixed geometries, stale-plan rejection, overwrite sizing, every byte transfer
threshold/bucket boundary and adjacent float, exact SIMD final-row output, and
checked AVIR layout/preparation before destination writes. Retained-image cases
preflight actual decoded RGB extents independently of JPEG/PNG workspace.

These fixtures establish overlap, ownership, source identity/order, and the
specified capacity behavior. Benchmark arrival order can change physical slot
order and file bytes. They do not measure production-release throughput,
peak process memory, or live-download duration. Resource targets are described
in [shared compilation execution](compile-execution.md).
Progress cases retain one artifact/release contribution across interleaving,
repair, and preparation replacement, with explicit withdrawals instead of
counting repeated attempts as completed work. Mixed known B and unknown
Large/XL row extents remain unknown while independent pixels advance; component
completion leaves row counts unchanged. Settlement, invalidation, and known
empty extents preserve strict Dataset-system admission and generated unit
formatting.
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

### Compiled Explore opening measurements

The measurements below describe the former payload-verifying open. Current
[compiled loading](datasets.md#loading-a-compiled-file) trusts published
metadata for every reader and does not perform that scan. Use
`--diagnose-benchmark-image --profile-compiled WORKERS` to measure current
index loading; historical annotation-admission times measure different work.

The 2026-10-04 capture delayed `shutdown.explore_join` by 72.29 seconds while
the initial discrete Explore operation was active. An isolated read-only scan
of the selected `compiled/train.bin` reproduced the costly cold annotation
admission. The file contains 587,017 eligible images across 621,608 slots,
5,069,379 objects and 546,101,044 RLE pairs. Its validated maximum complete
label payload is 121,824 bytes; the maximum object count is 688 per image.
The current `compiled/val.bin` header reports 5,000 images, at most 63 objects
and a maximum payload of 40,624 bytes. The compiler default therefore covers
both existing outputs; it is not a bound inferred for other resolutions or
future datasets.

| Annotation admission | Observed wall time |
| --- | ---: |
| Existing serial open, cached file | 0.962 s |
| Existing serial open, file-local cache eviction requested | 124.528 s |
| Parallel open, same eviction request, eight persistent workers | 16.798 s |

The parallel workers used CPUs 0, 2, 4, 6, 8, 10, 12 and 14, NUMA node 0,
and nice −10. The original serial captures did not record placement, so these
are observed end-to-end opening times, not an isolated CPU-kernel comparison.
Both cold scans admitted identical object/run totals and exact maximum bytes.
No images were downloaded or compiled. Local records are
`build/validation/explore-open-baseline.jsonl`,
`explore-open-baseline-cold.jsonl` and `explore-open-parallel-cold.jsonl` in
that directory; the [diagnostic command](commands.md#wrapper-operations)
records placement in subsequent captures.

The focused validation built the complete product and checked changed native
units with tidy. Dataset admission, cancellation, pool reuse, compilation and
storage checks passed across the data suite and its focused retry: the initial
run had 566 passes, one fixture failure and one unavailable GDR branch; the
fixture then passed 75 assertions after explicitly selecting the persisted
format ceiling for its deliberately wide annotation. The new diagnostic cases
passed 137 assertions in five cases. Native Explore acceptance passed 157,085
assertions across eight cases; four cases were marked skipped after their
available paths completed because GDR was unavailable. Captures are
`explore-build-final.log`, `explore-data-tests.log`, `explore-data-retry.log`,
`explore-diagnostic-tests.log` and `explore-native-acceptance.log` under
`build/validation/`. Packaged acceptance through
`./mmltk --test workspace-wayland --headless-compositor` also exited zero:
88 cases passed, one GDR-capability case was skipped, and all 155,094 assertions
passed (`build/validation/explore-wayland-acceptance.log`). No full `all` suite
or production dataset compilation was part of this focused follow-up.

After removing reader verification, the same existing training bin opened in
8,115,359 ns (about 8.1 ms) with eight workers and file-local cache eviction
requested. Recorded index totals remained 5,069,379 labels and 546,101,044 runs
across 621,608 physical slots. This measures the trusted shared open, excluding
Explore's subsequent label-summary pass and rendering. Worker placement matched
the earlier eight-worker capture. The read-only command was:

```bash
./mmltk --diagnose-benchmark-image --compiled compiled/train.bin --sample 0 \
  --profile-compiled 8 --cold-compiled
```

The capture is
[explore-open-trusted-cold.jsonl](../build/validation/explore-open-trusted-cold.jsonl).
The product build and targeted tidy passed. Focused data checks passed 28,084
assertions across 34 cases (33 passed, one marked skipped after H2D coverage
because GDR was unavailable); diagnostic checks passed 137 assertions in five
cases; native Explore acceptance passed all 157,083 assertions across eight
cases (four passed, four marked skipped at their unavailable GDR branches).
The respective test transcripts are
[data](../build/logs/mmltk-test-20261004-171952-3066505.log),
[diagnostics](../build/logs/mmltk-test-20261004-172153-3072276.log), and
[Explore](../build/logs/mmltk-test-20261004-172213-3076038.log).

### Compiler throughput validation record

The aggregate-throughput implementation and validation finished at `cecde5c5`,
using production baseline `333032e7` preserved at documentation commit
`21bdcadc`. The [validation ledger](../build/validation/benchmark-throughput-validation.json)
records both clean full tidy stages, fresh cleanup passes, initial/final product
builds, focused remediation, retained successful checks, and bounded acceptance.
The checkpoints are `de7d0d5f` (`pre cleanup`) and `6942a35e` (`post cleanup`).

| Final gate | Result |
| --- | --- |
| [`./mmltk --test all`](../build/validation/benchmark-throughput-test-all-final.log) | Exit 0; the data executable passed 566 cases and 426,740 assertions, with one hardware-dependent skip |
| [`./mmltk --test workspace-wayland --headless-compositor`](../build/validation/benchmark-throughput-wayland-final.log) | Exit 0; 88 passed cases and 148,146 assertions, with one unavailable GDR-capability skip |

[Final gate results](../build/validation/benchmark-throughput-final-gates.json)
retain exact command lines, transcripts, statuses, and times. The current suite
pair followed the successful final build and focused fixes. Firefox coverage was first-party
packaged acceptance; no Firefox-owned suite or new complete Coconut run ran.

The [performance report](benchmark-performance.md#implemented-changes-and-bounded-measurements)
owns the matched matrix, core-controlled repeats, operation-count evidence,
output hashes, earlier small-image regression, and measured limits. Pipeline
and compaction compare output/live-payload hashes; Pixels/Resample retain
independent numerical coverage rather than claiming a profile hash check.
All referenced `build/validation` artifacts are ignored local evidence.

### Earlier compiler throughput validation record

The throughput and storage implementation spans `0d4e0f7a..333032e7`.
The local [performance ledger](../build/validation/benchmark-perceptual-performance.json)
retains the initial cancelled GUI capture, phase evidence, commands, profile
inputs, and validation scope. Cleanup checkpoints were `78ace8a2`
(`pre cleanup`) and `e43b17ec` (`post cleanup`); both configured tidy stages
and both cleanup profiles completed cleanly. Final follow-up validation used
the explicit benchmark-only scope, retaining earlier successful checks.

| Evidence | Result |
| --- | --- |
| [Final product build](../build/validation/benchmark-settlement-lifetime-build.log) | Exit 0; runtime `sha256:ba4609f2ffb21c5172ea5794c1b5e8e9123df3dbaca444b63518000e1539f98d` |
| [Batch settlement and Coconut cases](../build/validation/benchmark-settlement-batch-tests.log) | Exit 0; 170 cases, 68,164 assertions, before final batch-lifetime hardening |
| [Final affected cases](../build/validation/benchmark-final-targeted.log) | Exit 0; 3 cases, 11,217 assertions for batch settlement, dynamic slots, and fairness fixture synchronization |
| Earlier [complete ordinary suite](../build/logs/mmltk-test-20261003-021103-3285703.log) and subsequent [Wayland acceptance](../build/logs/mmltk-test-20261003-023536-3345034.log) | Both exit 0 before later compiler follow-ups; Wayland had 88 passes, one GDR skip, and 147,989 assertions |
| Later [broad suite](../build/validation/benchmark-final-all.log) | Stopped at user request, exit 143; resampling passed 47 cases/376,015 assertions, data reached 529 passes/one fairness-fixture failure/one skip. The fixture then passed in the final focused run above. This is not a passing full-suite result. |

Those follow-ups ended without a new passing full `all`/Wayland pair at
`333032e7`; the later completed pair is recorded above. The explicitly requested
complete Coconut observations and earlier `42c97d14` bounded comparisons are
retained in the [performance report's baseline evidence](benchmark-performance.md#baseline-measurement-boundary),
with their cache, mixed-core, and trace-cost limitations. These ignored local
artifacts are absent from a fresh checkout.

### Benchmark ownership and recovery validation record

This compilation ownership, recovery, and publication work completed at
`9da36b78`. The local
[validation ledger](../build/validation/benchmark-ownership.json) retains
phase/audit commits, controlled before/after evidence, focused remediation,
and the full validation stage history. Cleanup checkpoints are `14e261f8`
(`pre cleanup`) and `f8404cad` (`post cleanup`). Both full tidy stages ended
clean; the fresh C++ and frontend cleanup profiles reported zero textual and
structural hits. The final runtime image is
`sha256:55e52edfd4da54686f751e5332da9760e1e6c4e6613bb5244a46d05378b0148a`.

| Final required operation | Transcript and result |
| --- | --- |
| `./mmltk --build`, after remediation | [mmltk-build-20261002-142718-2352585.log](../build/logs/mmltk-build-20261002-142718-2352585.log), exit 0 |
| `./mmltk --test all` | [mmltk-test-20261002-143151-2373087.log](../build/logs/mmltk-test-20261002-143151-2373087.log), exit 0; 489 dataset cases passed, one GDR skip, 400,790 assertions; nine GDR-dependent native skips across the complete run, one existing ignored Rust doctest, and 177 log-query fixtures passed |
| Subsequent `./mmltk --test workspace-wayland --headless-compositor` | [mmltk-test-20261002-150148-2441664.log](../build/logs/mmltk-test-20261002-150148-2441664.log), exit 0; 88 cases passed, one GDR skip, 147,757 assertions |

These local ignored artifacts are absent from a fresh checkout. Compiler
coverage includes Coconut with Stock validation, dropped-mask recovery enabled,
Stretch, and perceptual downscaling. Directory and benchmark retain distinct
source front ends and codec policies while sharing the
[execution, storage, and publication owners](compile-execution.md).

Controlled NUMA evidence reproduced the reported residency error by discarding
an unlocked prefaulted page before verification. After locking moved before
verification, the discard was refused and the ownership case passed. The
original workload was unlogged: its actual per-page kernel status, including
whether a page was missing or on another node, remains unknown.

The 96-row Parquet fixture previously observed 540 metadata readiness visits
in each pressure variant. The final case bounds those visits to the row count
and requires two projection openings, zero replayed rows, retained payloads,
and settled storage with and without unrelated scratch contention. Late
physical/original repair cases cover cold and retained components, independent
unrelated sources, converged identities, and released publication leases.

Final Validation also reproduced recovery returning to a pending-original wait
after shared execution had already failed. The correction propagates the
existing interruption cause through recovery settlement. The final suites cover
both fatal label conversion and independent compiler failure while originals
and JPEG geometry remain held. Retry coverage creates and inspects a real
compiled artifact through fresh staging after the preceding failure leaves
the final destination absent. The historical Compile-again error
`open failed: compiled/train.bin: No such file or directory` has not been
reproduced through the current direct Compile path; successful retry and
operation-stage diagnostics do not establish its historical root cause.

The recipe became 17 lines smaller and the duplicated publication bodies
shrank from 58 to 39 lines; production changes from `5b0e775a` through
`9da36b78` totaled 234 additions and 174 deletions, a net increase of 60 lines.
Fixture counts establish removed scans and reused workers, not a production
speedup percentage.
That validation measured no production-sized throughput or peak-memory
benchmark. GDR skips
remain unverified hardware coverage. Firefox coverage used only first-party
suites and packaged application acceptance; no Firefox-owned tests were added
or run.

### Firefox and benchmark pipeline validation record

The preceding Firefox and benchmark pipeline validation is recorded at `87a0d2b1`.
The local [validation ledger](../build/validation/firefox-benchmark-validation.json)
retains both full tidy stages, fresh C++/frontend cleanup results, builds,
focused remediation, and final suite results. The cleanup checkpoints are
`464b4b6c` (`pre cleanup`) and `10f3cdd4` (`post cleanup`). The final runtime image
is `sha256:63660f31e205ff07335861d7922b3fb9fffe12d0b9b9d4c1b661b9e17c710308`.

| Final required operation | Transcript and result |
| --- | --- |
| `./mmltk --build`, after remediation | [mmltk-build-20261001-161418-1078382.log](../build/logs/mmltk-build-20261001-161418-1078382.log), exit 0 |
| `./mmltk --test all` | [mmltk-test-20261001-162542-1107674.log](../build/logs/mmltk-test-20261001-162542-1107674.log), exit 0; 480 dataset cases passed with 399,183 assertions, nine GDR-dependent native skips across the suites, and one existing ignored Rust case |
| Subsequent `./mmltk --test workspace-wayland --headless-compositor` | [mmltk-test-20261001-165121-1164919.log](../build/logs/mmltk-test-20261001-165121-1164919.log), exit 0; 88 cases passed, one GDR-dependent skip, 148,715 assertions |

These are local ignored artifacts, absent from a fresh checkout. The
[log query](logging.md#wrapper-build-test-and-tidy-transcripts) exposes terminal
and suite summaries without rerunning validation.

The reported production configuration was COCONut, Stock validation, mask
recovery enabled, Stretch, and perceptual downscaling. Its observation of
Labels/masks `241596/241605` and Pixels `16177/unknown` ended with
`dataset compiler returned invalid progress in its current state`. Source and
controlled cases established mixed row/component accounting and premature
known-total aggregation; the compiler now reports canonical units and exact row
ledgers while retaining strict progress rejection. Separate controlled source
custody evidence established cache preparation waiting on a still-live lease
held by its own controller/sibling; the correction reuses that exact lease.
The fixture with the same configuration also proves ready physical-archive
pixels can complete while a peer download remains held. No supplied production
trace establishes sustained throughput, a measured speedup, or every cause of
the production wait.

Firefox coverage remains in the existing first-party suites and packaged
application acceptance. The complete Wayland pass covers direct/copy workspace
integration, retained content, resizing, recovery, quiet execution, and shutdown.
It does not individually exercise every actor collision or binding ordering,
descriptor/capability rejection, Vulkan fatal branch, or idle semaphore-retirement
branch. GDR-dependent skips remain unverified hardware coverage. No Firefox-owned
test suite or dedicated Firefox harness was run for this validation.

### Adaptive compilation validation record

The earlier execution/progress cutover finished at implementation commit
`7f2c12b9`. Its local
[validation ledger](../build/validation/adaptive-execution-state.txt) records
the complete stage history: first full tidy and initial build, `pre cleanup`
at `dde77d44`, both fresh cleanup profiles and the second full tidy, and
`post cleanup` at `b1e98a1d`, followed by the required final build, tests, and
focused remediation. These are local ignored artifacts, absent from a fresh
checkout.

| Run | Transcript and result |
| --- | --- |
| Full `all` | [mmltk-test-20261001-054834-223635.log](../build/logs/mmltk-test-20261001-054834-223635.log), exit 42; exactly one failing Open Images progress-observation fixture, with all other native/browser/tool groups completed |
| Open Images focused retry | [mmltk-test-20261001-061354-308088.log](../build/logs/mmltk-test-20261001-061354-308088.log), exit 0; 22 assertions, one case, after checking throttle decisions through their existing trace and valid delivered snapshots |
| Full headless Wayland | [mmltk-test-20261001-061608-314991.log](../build/logs/mmltk-test-20261001-061608-314991.log), exit 42; 87 passed cases, one retained-state failure, one unavailable GDR case skipped |
| Product rebuild after fullscreen correction | [mmltk-build-20261001-063454-357066.log](../build/logs/mmltk-build-20261001-063454-357066.log), exit 0 |
| Focused browser-driver checks | [mmltk-test-20261001-064204-380016.log](../build/logs/mmltk-test-20261001-064204-380016.log), exit 0 |
| `workspace_wayland_retained` | [mmltk-test-20261001-064214-383849.log](../build/logs/mmltk-test-20261001-064214-383849.log), exit 0; 8,932 assertions, one case |
| Directly affected `workspace_wayland_dpi` | [mmltk-test-20261001-064613-397292.log](../build/logs/mmltk-test-20261001-064613-397292.log), exit 0; 7,239 assertions, one case |

The retained-state failure came from integration-only Firefox shortcuts that
changed document fullscreen without resizing the native Wayland widget.
Removing those shortcuts restored parent/widget fullscreen entry and exit.
The successful [retained Firefox capture](../build/validation/latest-wayland-test-firefox.log.history/69-863342452984565.log)
recorded a 1920×1080 fullscreen viewport and
1500×1100 restoration, all four steps in both native compile generations,
advancing progress without recorded pointer, wheel, or keydown events,
cancellation/restart, and artifact publication.

This run retained successful results under an explicit focused-continuation
instruction. Neither a later clean full `all` rerun nor a later clean full
Wayland rerun was performed. The passed retries resolve the recorded failures
within that scope; GDR hardware skips remain unverified coverage. They make no
full-release COCONut throughput, sustained CPU utilization, or peak-RSS claim.

### Fixed-storage measurements

The earlier format-10 validation of implementation commit `23974612` retained these
2026-09-29 wrapper transcripts:

| Operation | Transcript and observed result |
| --- | --- |
| `./mmltk --build` | [mmltk-build-20260929-224513-2452962.log](../build/logs/mmltk-build-20260929-224513-2452962.log), exit 0 |
| `./mmltk --test all` | [mmltk-test-20260929-232113-2554469.log](../build/logs/mmltk-test-20260929-232113-2554469.log), exit 0; nine native GDR-related cases skipped |
| `./mmltk --test workspace-wayland --headless-compositor` | [mmltk-test-20260929-234032-2601309.log](../build/logs/mmltk-test-20260929-234032-2601309.log), exit 0; 89,357 assertions, 87 passed cases, one unavailable GDR scenario skipped |

The data round-trip case completed its H2D checks before reporting its GDR
skip. These results establish the executed H2D/CUDA/Wayland paths, not GDR
mapping support on this host. The transcripts are local ignored build evidence,
not files shipped by a fresh checkout. Use the
[log-query examples](logging.md#benchmark-compilation-traces) to inspect complete
measurement records and terminal status.

The all-suite transcript contains bounded fixture observations:

| Record | Observed value and scope |
| --- | --- |
| `benchmark_label_fixture` | First projection 1,161 ns; 256 active label bytes; 1,000,012 retained workspace bytes; zero steady output-vector growths. The growth observer covers two output vectors and excludes mapped backing. |
| `benchmark_pipeline_fixture` | First label slot 81,691 ns; first pixel slot 1,026,153 ns; labels ready before JPEG arrival. Timings span ready publication to slot completion with fixture-owned normalized input. |
| `compiled_stream_fixture` (H2D) | First host pixels/labels 53,668 ns; 6,720 logical label bytes and 8,192 retained label bytes; zero steady growths. Observation covers owned buffers/read containers, not every allocation. |
| `native_png_fixture` | Exact independent scalar-ID/run equivalence across 18,456 pixels and 797 runs; combined PNG decode/support time 115,942 ns. This does not isolate SIMD performance. |

The used-byte copy fixture compares format 10 with an independent reproduction
of the packed algorithm at `3b1373b9`. Each reports sixteen repetitions totaling
16,224,000 pixel bytes and 35,840 label bytes:

| Warm order | Fixed format 10, bytes/s | Prior packed algorithm, bytes/s |
| --- | ---: | ---: |
| Sequential | 27,911,061,369 | 29,822,530,355 |
| Shuffled, seed 1729 | 27,716,707,236 | 27,211,891,680 |

The sequential fixed layout required 320 pixel copy calls versus 16 for packed
storage; both shuffled paths required 320. Page padding prevents coalescing in
that fixed fixture. These are warm in-memory used-byte copies, not cold storage
or GPU end-to-end throughput. The comparison does not establish either layout
as universally faster.

Compaction started with a 1,310,724,192-byte apparent staged file:

| Eligible maximum label bytes | Final apparent bytes |
| ---: | ---: |
| 0 | 1,210,060,896 |
| 72 | 1,216,352,352 |
| 466,944 | 1,304,432,736 |
| 500,000 | 1,310,724,192 |

The fixture verified unchanged inode identity and the 4 MiB scratch ceiling.
Apparent lengths include sparse holes and padding; they do not measure allocated
disk blocks or process RSS. None of these bounded observations is a production
baseline for COCONut B's 241,602 images, a cold-cache benchmark, a full-release
memory measurement, or an isolated SIMD speedup.

## Shared fixtures and evidence

Neutral fixtures and the shared Catch runner belong to
[src/test_support](../src/test_support). Domain fixture targets live with
their owning components and publish their own declaration dependencies.

| Fixture | Ownership and use |
| --- | --- |
| [ScopedTestStream](../src/test_support/cuda_test_utils.hpp) | Neutral nonblocking CUDA stream lifetime for media and inference tests |
| [CudaTestGate](../src/test_support/cuda_test_gate.h) | Holds one compute stream until a CPU release while independent DMA remains runnable; destruction releases and joins the held stream |
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
GPU overlap fixtures prepare first-use kernels and pinned/device slots before
placing deliberate holds. A hold is released on failure before joining its
pending work; the shared CUDA gate needs no further GPU submission to release.
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
