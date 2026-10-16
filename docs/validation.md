# Validation and focused tests

[Wiki index](README.md) · [Quick start](../README.md#build) · [Logging](logging.md) · [Headless Wayland](headless-wayland.md)

[AGENTS.md](../AGENTS.md#final-validation-workflow) owns validation scope, stage
order, fix/review ownership, checkpoints, and the final documentation pass.
Outside action-plan execution, use focused checks for the changed behavior and
affected boundaries; full test suites are optional. During action-plan execution,
after the required final full build, both gate commands must pass in this order:

```bash
./mmltk --test all
./mmltk --test workspace-wayland --headless-compositor
```

Unfiltered `all` runs native, browser JavaScript/Rust, cleanup/declaration,
log-query, and compositor fixtures. External RF-DETR upstream comparisons and
historical-session acceptance are opt-in: ordinary `all` and `rfdetr` runs do
not prepare their data and report those cases as skipped when no explicit
evidence is supplied. `--with-upstream` explicitly enables preparation;
`--env MMLTK_RFDETR_UPSTREAM_ORACLE_DIR=PATH` and
`--env MMLTK_RFDETR_BASELINE_SESSION=PATH` reuse existing evidence without
generating either missing fixture. Full upstream references can exceed 1 TiB;
they are tensor snapshots, separate from diagnostic logs. New-model numerical
acceptance still requires the explicitly selected comparisons to pass.
Resume failed tests and tidy checks with
the focused selections below; retain successful results and finish checks left
unexecuted by an interruption. Rebuild changed product code through `./mmltk --build`
before testing. During action-plan execution, once focused retries pass, run the
required full completion pass. Retries do not restart or reorder validation stages.

## Formatting and static analysis

```bash
./mmltk --tidy
./mmltk --tidy --rust-only
./mmltk --tidy --file src/common/system/cpu_affinity.cpp src/common/system/execution_policy.cpp
./mmltk --tidy --file src/backend/imaging/explore/explore_render_core.cu
./mmltk --tidy --start-at src/common/system/cpu_affinity.cpp
```

Full tidy runs [tools analysis](#native-tool-analysis), formats tracked
first-party C/C++/CUDA and the `mmltk-browser-app` Rust package, refreshes
`.cache/cmake/analysis`, then runs supported clang-tidy checks and exact GCC
objects for reflection units. CUDA clang-tidy covers host/device at `sm_86`.
[PCH/header isolation](build.md#target-declarations-and-precompiled-headers)
remain enabled. Cppcheck and `--cppcheck-only` are disabled because reflection
syntax is unsupported.

| Selection | Scope |
| --- | --- |
| `--file PATH...` | One/grouped tracked sources; skips package-wide Rust formatting |
| `--start-at PATH` | Resume native units; retains Rust formatting |
| `--rust-only` | Containerized Rust formatting only; no native configuration/analysis |

File/resume selections are mutually exclusive. Rust-only rejects native
selection and third-party options. Focused retries do not replace action-plan
full passes. Rust formatting is not a Rust static-analysis suite.

[.clang-format](../.clang-format): Google base, 200 columns, one-column indent
and continuation, no tabs, Clang 22 long braced-list opening/closing breaks,
`LambdaBodyIndentation: OuterScope`. Local aggregates can anchor callback tables;
formatting alone is not structural reduction. [.clang-tidy](../.clang-tidy)
sets `modernize-use-auto.MinTypeNameLength=5`, a spelling threshold that does
not replace ownership clarity.

[run_static_analysis.cpp](../tools/toolchain/run_static_analysis.cpp) owns
analysis overrides. [Transcripts](logging.md#wrapper-build-test-and-tidy-transcripts)
use `MMLTK_TIDY_LOG_FILE` for an explicit destination.

### Native tool analysis

```bash
./mmltk --tidy-tools
./mmltk --tidy-tools --file tools/mmltk.cpp tools/driver/tool_command.cpp
./mmltk --tidy-tools --start-at tools/logging/log_query.cpp
```

Tools tidy uses `.cache/cmake/tools`, the tools image, and registered units
only. C++26/modules are checked by building exact GCC objects/dependencies;
clang-tidy cannot consume GCC BMIs. Grouped file/resume rules match full tidy;
resume uses registration order. Focused selections require the database from
`--build-tools`.

Full `--tidy` routes mixed file selections to their tools/product graphs.
Product-dependent diagnostics belong to product analysis. Full formatting also
covers isolated oracle/Weston sources, whose compilation stays in their own
images; oracle preparation is `--diagnose-rfdetr-upstream --build-only`.
Tools tidy rejects foreign-graph, Rust, and third-party selections. It retains
the tidy transcript without publishing tools; rebuild them before exercising
changed behavior.

## PCH include audit

```bash
./mmltk --audit-includes report
./mmltk --audit-includes preview
./mmltk --audit-includes fix
```

[audit_includes.cpp](../tools/cleanup/audit_includes.cpp) lexically inventories
checked-in PCH groups and conservatively replaces includes; it is not semantic
include-what-you-use. Default `report` and `preview` leave source unchanged;
`fix` applies admitted edits. The existing Release graph is default;
`--refresh` first reconfigures it with browser-host/Firefox enabled.
`--build-dir PATH` selects a graph; `--output PREFIX` replaces default
`output/include-audit.{json,md}`. Use the wrapper for every mode.

Reports retain folder/large-file inventories, standard/Linux frequencies,
repeated local bundles, every source compiler context, proposals, PCH issues,
and concrete retention reasons. Inactive branches count lexically; frequency
alone proves no redundancy.

Rewriting requires every context's owner, generated policy, output, and exact
target-local forced PCH to agree; only common eligible groups apply. Conflicting
use/skip, missing groups, wrong forced includes, mismatched owners, or hidden
response-file evidence block edits. Declaration headers remain self-contained.
CUDA, modules/importers, no-PCH/standalone/excluded contexts, and unconsumed groups
retain direct includes; that retention is not a cleanup failure.

`fix` first runs [toolchain invariants](../tools/toolchain/check_toolchain_invariants.cpp),
including creation/use environment equality and exclusions. It preserves
comments/branches, checks original snapshots, and atomically replaces files
with modes retained. [Build policy](build.md#target-declarations-and-precompiled-headers)
owns registration/generated paths. Audit success proves neither tests nor speedup.

## Deduplication reports

```bash
./mmltk --cleanup-report cpp
./mmltk --cleanup-report frontend
./mmltk --cleanup-report tools
./mmltk --cleanup-report all
```

[generate_cleanup_json.cpp](../tools/cleanup/generate_cleanup_json.cpp) writes
`cleanup/{cpp,frontend,tools}-code-deduplication.json`; `all` runs all profiles.
`hits`/`structural_hits` contain match sizes, paths/ranges, consolidation patterns,
with cross-file hits first. Reports change no source. [AGENTS rules](../AGENTS.md#code-deduplication)
own applicable profiles and resolution.

`cleanup/rejected.json` separately retains profile configuration, inventory,
producer, rejected candidates, suppressions, and Duplo `raw_hits`. Updating one
profile preserves others. Every output is replaced atomically. Producer records
native module/source, executing ELF path/SHA-256, and adjacent tools input hash;
raw reports use the same identity.

| Profile | Inventory / minimum |
| --- | --- |
| C++ | First-party native; Duplo 6 lines, CPD 39 tokens, identifiers anonymized/literal sequences ignored |
| Frontend | Duplo 9 lines, CPD 75 tokens |
| Tools | Recursive authored `tools/` modules/sources/headers, driver/fixtures, untracked-unignored files; excludes build/generated/vendor/deleted paths; C++ settings |

C++ context filtering applies at every CPD size and code-bearing Duplo hit. It
compares complete statements with operation/type/member/constants/assertions
and local-variable relationships restored; independent dimension/storage reads
retain role names. It rejects fragments hiding different callees/arguments,
signature boilerplate, aliases, adjacent getters, isolated calls, lock-forward
bodies, and bodyless loop headers. Receiver/local-value forms can match through
one method API while repeated-input relationships remain distinct. Arithmetic,
CUDA setup, and scalar/admission work have no blanket exclusion.

Candidates include repeated executable sequences, identical complete records in
distinct declarations, and same-name overloads sharing complete first parameter
and meaningful opening setup; a guard/alias alone is insufficient. Repeated
parts of one statement/declaration are not independent occurrences. Cross-function
fragments are compared as balanced complete statements; default arguments do
not interrupt indexing, compound returns keep constants, and unbalanced syntax
stays visible. Equivalent CPD patterns consolidate without dropping targets.
Duplo preserves multiline literals/embedded scripts for manual inspection;
non-code rejections retain reasons.

The lexical index loads/indexes one affected file at a time, caches boundaries/
owners, and groups spans without all-pairs function comparison. No filename
exclusions or suppression registry apply. Review containing operations, existing
APIs, ownership, and lifetime before extraction; sizes/covered lines are not
removable-line estimates.

Only inspected false positives permit narrow `CLEANUP-IGNORE` or bounded
`CLEANUP-OFF`/`CLEANUP-ON`; rejected coordinates/reasons/counts remain in output.
[Detector integrity](#raw-cpd-and-declaration-formatting) applies throughout.
Clean ordinary reports prove neither zero raw repetition nor complete human review.

## Raw CPD and declaration formatting

```bash
./mmltk --raw-cpd
./mmltk --raw-cpd --min-tokens 12 --output cleanup/declarations-raw.json
./mmltk --raw-cpd --review
./mmltk --raw-cpd --review --output cleanup/declarations-review.json
./mmltk --raw-cpd --tools
./mmltk --raw-cpd --tools --review
./mmltk --format-declarations check
./mmltk --format-declarations preview --file src/backend/models/rfdetr/contract/train_recipe.h
./mmltk --format-declarations fix --file src/backend/models/rfdetr/contract/train_recipe.h
```

[generate_cleanup_json.cpp](../tools/cleanup/generate_cleanup_json.cpp) owns both
ordinary cleanup and raw evidence. Raw CPD defaults to 12 tokens (minimum 2),
preserves exact identifier/literal spelling, disables block skipping,
and applies no source-context filter or inline suppression. Its first-party
C/C++/CUDA inventory includes ordinary and template headers, modules, and
untracked unignored files, excludes `third_party`, and omits deleted paths.
This is declaration triage, not a verdict that every lexical match should be
extracted into a shared algorithm. `--tools` selects the standalone native tool
inventory and defaults to `cleanup/tools-raw.json`, or
`cleanup/tools-review.json` and `.md` with `--review`.

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
native wrapper sources. It uses tracked and untracked-unignored Git inputs and omits
deleted paths, third-party code, and generated/output trees. Each entry records
language, byte and physical-line counts, SHA-256, dedicated/embedded test
evidence, family IDs, and `status: "unreviewed"`. Non-C++ languages receive size
triage only. The queue includes every file **over 500 physical lines** and any
file with a candidate family. A file with exactly 500 lines needs a candidate
family to enter the queue.

[review_patterns.cpp](../tools/cleanup/review_patterns.cpp) groups bounded lexical
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

[format_declarations.cpp](../tools/cleanup/format_declarations.cpp) shares the lexical
index and classifiers in
[declaration_patterns.cpp](../tools/cleanup/declaration_patterns.cpp). It is not
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
| `cleanup-tool` | Cleanup/raw-CPD evidence, declaration formatter, review-candidate, and include-audit fixtures |
| `log-query-tool` | Log parser/query/correlation/triage fixtures |
| `build` | Build the configured native test targets without running them |
| `all` | Build native test targets; run ordinary native executables, browser JavaScript/Rust tests, cleanup/declaration, log-query, and compositor fixtures |

`all` builds `mmltk_workspace_wayland_integration` but excludes it from its run
list. With no native executable or runner-argument filter, it then executes
`browser-app`, `cleanup-tool`, `log-query-tool`, and `headless-compositor-tool`,
retaining the first nonzero status while running the remaining groups.
`all --executable TARGET` and `all -- FILTER`
remain native selections and do not append those fixture groups. `all` does not
execute `cuda-vulkan`, the real `headless-compositor` check, or either profile
runner. Full Coconut download/compilation is never implicit in `all`.
`gui` and `tsan` suite names are currently unavailable even though other
GUI/development build facilities exist.
Product-build `--no-run` checks prove Rust test compilation only. Execution
requires unfiltered `all` or standalone `browser-app`, including its JavaScript
cases.

Some RF-DETR tests download model checkpoints and derive normalized weights,
ONNX, and TensorRT engines in `.cache/tests/rfdetr` on first use. Hardware-gated
tests may skip when their requirements are unavailable; a skip is not hardware
acceptance evidence.

### Native tool fixtures

The [separate tools runner](build.md#composition-and-fixture-isolation),
`mmltk-tests`, owns these fixtures. The production driver starts that runner
for internal fixture requests and contains only production code. Public suite
routing stays containerized:

| Public suite | Native fixture commands |
| --- | --- |
| `cleanup-tool` | `test-generate-cleanup-json`, `test-format-declarations`, `test-review-candidates`, `test-audit-includes` |
| `log-query-tool` | `test-log-query` |
| `headless-compositor-tool` | `test-headless-compositor` |

The native commands support `--list-tests`; the public log-query and compositor
suites forward it after `--`. The cleanup suite instead supports the shared
`--test-name-pattern` selection described [below](#selection-environment-deadlines-and-debugging).
Rebuild changed fixtures with `./mmltk --build-tools` before running them.
Standalone log-query and compositor fixture suites require the existing build
image, use read-only repository mounts without GPU/network access, and build
or pull no image. The cleanup suite uses the ordinary build-image preparation.

The log-query tooling suite also exercises wrapper capture and process
ownership with isolated checkouts and a simulated Docker command. Its runtime
cases cover [GUI replacement](commands.md#gui-session-replacement) of shared
raw sessions, an existing GUI, and an owner still preparing its container;
retention of other checkout/test owners; and rejection of a delayed older GUI
after a newer invocation has started. These checks exercise wrapper admission
and cleanup without requiring a display or stopping real application sessions.

The explicit `./mmltk --test log-query-tool --tools-ownership` selection runs
the cold-bootstrap ownership/handoff fixture as container root with the
required UID/GID and ownership capabilities. Ordinary fixture discovery and
`all` omit that case and retain restricted container permissions.

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
./mmltk --test log-query-tool -- --list-tests
./mmltk --test headless-compositor-tool -- --list-tests
./mmltk --test headless-compositor-tool -- test_normal_completion_and_command_status
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
| `--with-upstream` | With `all` or `rfdetr`, explicitly prepare upstream references and the historical session; full references can exceed 1 TiB |
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
with none it runs ordinary discovery. `-- --list-tests` lists its native cases;
the [tools ownership case](#native-tool-fixtures) has its explicit option.
`headless-compositor-tool` accepts one exact `test_name` or `--list-tests`
after `--`. Both reject native test options.
`cleanup-tool` accepts only `-- --test-name-pattern PATTERN` for filtering and
rejects native test options. The same regular expression goes to all four
cleanup fixture commands; each prints its selected count, including zero when
only another group matches.

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
[diagnose_native_symbols.cpp](../tools/diagnostics/diagnose_native_symbols.cpp) and
[diagnose_native_link.cpp](../tools/diagnostics/diagnose_native_link.cpp).

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

Build the package with `./mmltk --build`. Visible runs mount host Wayland;
headless runs add private NVIDIA Weston with identical assertions. The wrapper
verifies all seven [registered hardware entrypoints](../src/acceptance/tests/wayland/scenarios.cpp)
before the selected comma-separated Catch union. Standalone evidence-audit cases
also live in this executable.

| Entrypoint | Browser lifetimes / required behavior |
| --- | --- |
| `workspace_wayland_retained` | One H2D: Dataset disclosures/input, real Cancel/restart, capacity growth, integer typing/paste, cached/held-miss gallery/detail returns, Detail-open resize both orientations, augmentation retention, fractional/wrapped/partial rows, wide/tall layouts, native labels/semantics, light/dark copy, Annotate layout/long lists, FPS, rapid changes, SIGINT |
| `workspace_wayland_workflows` | Two H2D: Train/Validate/Predict/Export GPU execution, real Train start/resize/live+hidden progress, retained charts, progressive Validation/detail/confidence/layout, direct Explore return, compiled/image/video Predict and Pause/Resume/EOF/Stop, export/cancellation, theme/minimum width. Second lifetime injects resized-canvas allocation failure and requires manual recovery plus completed training; both SIGINT |
| `workspace_wayland_dpi` | One H2D, scale 1.5: Dataset fixtures, theme/copy, rapid changes |
| `workspace_wayland_terminal` | Two H2D: real window close and abrupt peer loss after Annotation edit, completed draw, independent redraw |
| `workspace_wayland_probe_recovery` | Four H2D: startup-latched allocation/reset/begin/end probe failures, exact-content recovery, final pixels/semantics |
| `workspace_wayland_quiet` | Two H2D: blocked reads and completed gesture with diagnostics/reporting/probes inactive |
| `workspace_wayland_gdr` | One optional GDR lifetime; absent hardware explicitly skips |

Twelve H2D lifetimes are required; GDR is independent. Compatible scenarios
reuse browsers; startup-latched settings/destructive exits require isolation.
Window-close uses lifecycle diagnostics without probes. Quiet scenarios retain
real input pressure, typed control, settlement, and drawn readiness.

[WaylandSession](../src/acceptance/tests/wayland/session.h) owns process/fixtures,
artifact cursors, deadlines, and lifetime custody. Advancement requires typed
frontend settlement **and** independent native/browser evidence. Scenario entry
closes overlays, restores settings through ordinary messages, and settles before
reopening; physical allocation/claim/release history survives scenario-local resets.
Primary compilation owns a private destination; other lifetimes reuse fixtures.
Padding-sensitive retention selects Letterbox in the real Dataset radio;
workflow models keep default Stretch. Geometry evidence is specific to that choice.

| Harness owner | Responsibility |
| --- | --- |
| [session.cpp](../src/acceptance/tests/wayland/session.cpp) | Process/fixtures, advancement, deadlines, final drain |
| [artifact_cursor.h](../src/acceptance/tests/wayland/artifact_cursor.h) | Incremental extent reads, 16 KiB chunks, ≤64 KiB retained line; appends wait for notification |
| [native_audit.cpp](../src/acceptance/tests/wayland/native_audit.cpp) | Command/operation/input/product facts |
| [browser_audit.cpp](../src/acceptance/tests/wayland/browser_audit.cpp) | Interaction, displayed geometry, rendered UI |
| [surface_audit.cpp](../src/acceptance/tests/wayland/surface_audit.cpp) | Allocation/acquisition/draw/settlement ledger |
| [pixel_audit.cpp](../src/acceptance/tests/wayland/pixel_audit.cpp) | Independent native/browser pixel joins |

[audit](../src/acceptance/tests/wayland/audits.test.cpp) and [cursor](../src/acceptance/tests/wayland/artifact_cursor.test.cpp)
cases exercise these owners. Malformed native JSON, overflow, truncation, or
incomplete final records fail. Probe reconciliation uses ordered generation/frame
ranges and matching slot identity before geometry scan; evidence ordinals,
predecessor/suffix rules, independent drains, and late conflicts remain binding.

The [Rust driver](../src/frontend/iced/src/integration_control.rs) separates
lifecycle, retained, annotation_product, workflows, and Dataset presentation;
reporting is effect-only. It uses [native model/video inputs](../src/acceptance/tests/workflow_wayland_inputs.cpp),
ordinary actions/viewers, sibling CLI, and product systems:

| Behavior | Required independent observations |
| --- | --- |
| Train / GPU cards | Typed terminals plus curve/progress/theme/narrow pixels; bar capture paired to native image count, hidden-tab later metric sequences; [admitted generation ↔ actual device/rank](logging.md#workflow-gpu-evidence), including pre-progress cancellation and four Output/GPU light layouts plus dark/narrow Train |
| Validation | Detail opens during capture and survives successful settlement; closing returns six same-generation samples without reevaluation; canvas may lag metrics. All saved PNGs verified |
| Confidence / captions | Exact `0.437`, invalid/blank retention, inert arrows/wheel, settled `0→1→0`; [pixel oracle](logging.md#rendered-ui-acceptance-evidence) retains raw detections/clean identity/evaluation generation. Groundtruth/Detections below preview, 20 px spacing, narrow wrap, Advanced text. Seven atlas/detail caption cases cycle GT/Det; actual glyphs and Det-over-GT pixels required |
| Workflow fixture | Real six-query detector calibrated against selected proposals to overlap captions; Validation FP32 matches calibration, Train still updates actual model. Native completion/caption cache alone proves no drawing |
| Predict / outputs | Saving-control state, compiled/image samples, completed video, playable cancelled partial; disabled media/report leaves only empty `.mmltk-run-claim` with unchanged processing. [Failure records](logging.md#prediction-media-acceptance) distinguish invalid from missing evidence; native media tests own timestamps/audio/write faults/shortfall/crash-prefix recovery |
| Explore return / layout | Direct Validate→Explore without extra Open, requiring paired ready-tile pixels; all Output cards atop right columns, left cards before actions, Predict saving controls retained |
| Charts | Browser pan/legend settles through expansion/grid/hide/reveal/navigation; wheel over plot/axes/legend with modifiers leaves camera unchanged. Train aspect selector present, Train native workspace/Validate aspect selector absent; sparse gaps stay gaps, axes/legend cannot replace curve pixels |
| Gallery | Measured N/N+1/N rows, forward/reverse demand, exact cached cells, held visible miss, known pending selection/hover, drawn compiled identity before/after release. Native controlled fixtures separately establish disk/GPU priority |
| Detail resize | Repeated layout changes each orientation while retaining gallery measurements; ordinary close requires reconciled viewport, paired atlas draw, ready/new lower rows. Temporary inline canvas sizes invoke normal observer and are restored |
| Retained semantics | Exact original/derived detail draws and overlay labels; holds initial automatic Basic during original stages, then releases and uses ordinary derived actions. Square/wide/tall retain automatic Basic |
| Integers | Five Explore controls: min/max instances, seed, first/last compiled index. Spinner edges, wheel suppression, invalid unsigned text, native persistence/restoration; seed typing/paste uses exact value above `2^53`. Digits advance across render frames; rapid resize does not repeat audit |
| Annotate | Wide/narrow shared layout, Timeline in Advanced, page/horizontal scroll reachability, 32 objects/classes and wrapped final entries. Remeasure after scroll with whole-pixel tolerance; gestures retain full-image/visible-region geometry |
| Status | Real overlay, bounded fixture notices: add/update/dismiss/overflow, long wrapping/selection/exact Copy, hover/Escape/reentry, keyboard/touch, themes/narrow/modal, shortcut isolation, pulse/reduced motion, hidden quietness. [Records](logging.md#rendered-ui-acceptance-evidence) join identity; fixtures are not native failures. Rust covers reconnect/dedup/ack/stale delivery/clipboard |

The [physical ledger](logging.md#physical-presentation-evidence) requires inventories,
exact transfers/acquisitions, mode-specific settlement, encoded/submitted draws,
final release, and pixels; exact draw IDs permit out-of-order callbacks. Capacity
retry delivers arena availability before a held completion receipt. A typed
undersized-candidate hold proves completed fallback before candidate release.
Empty notice consumption waits for paired zero-match atlas draw; submission and
settlement remain separately required. FPS combines real submissions and async
canvas read, not numerical performance claims.

Independent card/frame probes, clipped atlas patches, Annotation geometry/background,
and [Vulkan output](logging.md#vulkan-diagnostics-and-descriptor-provenance) complement
custody. Window-close and intentional loss have distinct terminals; exit alone
proves neither balanced destruction nor GPU completion. [Headless operation](headless-wayland.md)
owns compositor limits; [logging](logging.md#delivery-and-acceptance-ownership)
owns delivery/artifact families. Query one captured lifetime at a time.

### Dataset presentation and lifecycle

[lifecycle.rs](../src/frontend/iced/src/integration_control/lifecycle.rs) clicks
both recipes/all three Coconut validation choices with native settlement,
visibility, disabled Browse behavior, and restoration. Actual ordinary/narrow
draws verify indentation, aligned descriptions, small recovery type, clip, and
label pixels; narrow uses 480 px canvas with normal horizontal page scroll.

Nested-choice opening/closing/reversal, manual split/Compile-size disclosures,
and non-Dataset Diagnostics exercise [shared transitions](gui-interaction.md#shared-form-expansion-and-dividers):
absolute viewport/scroll stability, outgoing-input rejection, drag-selected split
editing under both offsets, and held-checkbox offscreen release without mutation.
Settings and restored canvas must settle before navigation.

[dataset_presentation.rs](../src/frontend/iced/src/integration_control/dataset_presentation.rs)
renders production progress in 36 local cases: nine states × widths 224/360 ×
themes, repeated at DPI 1.5. Cases include unknown/grouped totals, cached/resumed
sources, shorter active updates, completed tracks while Syncing/Publishing,
cancellation request/Cancelled/Failed/Completed, and late post-settlement progress.
They never enter native model/work counts. Progress/divider pixels and complete
expected caption draws are required; clipped intermediate frames establish
reflow only.

Retained acceptance starts real Directory compilation, observes active work/all
three tracks, clicks Cancel, requires inactive Cancelled state without live rows,
then starts a fresh successful generation. Independent audits reconstruct exact
captions for **both** generations; fast completion cannot substitute. [Caption
policy](benchmark-datasets.md#reading-compilation-progress) owns Directory acquisition.

Both active generations require 480×900, 960×720, real fullscreen, and restored
window steps. Each joins current native revision, fresh Dataset root draw,
CSS/backing/configured/presented extents, full-root coverage, and zero pointer/
wheel/keydown events during resize. Native progress may stay unchanged during
Syncing; draw/presentation must advance. DOM fullscreen/request alone is
insufficient. [Resize records](logging.md#dataset-presentation-evidence) identify
unmet predicates without extending deadlines.

The asynchronous canvas probe exercises borrowed-input reuse, same-key supersession,
geometry restoration, owner replacement, and retired-callback drain; replacement
scratch/evidence must remain untouched. [Record definitions](logging.md#dataset-presentation-evidence)
separate draw/caption/pixel/custody. These cases acquire no benchmark release and
claim no production download throughput or full-release capacity.

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
native adaptations. Numerical gates and measurement limits are defined below.
Coverage is colocated
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

Graph capture, executor reuse, fuser availability, fused ATen operators, graph
fusion, and throughput are separate claims. Inspect the actual optimized graph
with [compilation diagnostics](logging.md#native-selective-compilation); functional
compilation tests alone establish no training fusion or speedup.

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
./mmltk --diagnose-rfdetr-upstream --case CASE_ID --output FRESH_PATH
```

This diagnostic explicitly generates references and compares native code.
Ordinary `all`/`rfdetr` never prepare them. Test `--with-upstream` opts into shared
references and historical-session preparation; explicit `--env` evidence paths
reuse only supplied data. Missing optional evidence skips; supplied invalid
evidence fails admission.

| Mode | Result |
| --- | --- |
| Full numerical (default) | All catalog references, then core/training/inference/export `[upstream_oracle]` cases |
| `--case ID` | Exactly one named matrix case; same source/checkpoint/precision/archive checks, no native comparisons, manifest `selected_reference_native_comparison_pending`; cannot certify full matrix |
| `--case ID --backward-operators` | Bounded intermediate gradient diagnostics; disabled by default |
| `--case ID --export-reference` | Prepared CPU/CUDA controls and an actual pinned upstream ONNX artifact for one controlled FP32 batch-one case; disabled by default |
| `--inventory-only` | Verify/download official assets; topology/class/joint/tensor metadata, no product build/forward/training/comparisons |
| `--build-only` | Prepare isolated image/environment only, no weights/source checkout/numerical execution |

Admission requires clean core/Plus sources (including untracked files) at
[`rfdetr_sources.json`](../tools/oracle/rfdetr_sources.json) revisions/trees.
Omitted paths use managed pins; omitted output uses capability-addressed cache.
Inventory verifies all sixteen official assets in cache `mmltk/weights`.
Command-local Git trust/read-only metadata supports linked/differently owned
checkouts without global changes. Numerical work binds source/weight identities,
uses the [pinned donor image](build.md#upstream-reference-image), and has a
48-hour deadline with bounded cancellation.

[Native producer](../tools/oracle/generate_rfdetr_upstream_oracle.cpp) embeds donor
CPython and executes actual upstream model/matcher/criterion/PyTorch optimizer.
`manifest.json` format `mmltk.rfdetr.upstream-oracle.v2` binds source/payload/
dependency/producer hashes, model/execution policy, and checked tensor inventories.
`producer_sha256` identifies the executable; unsupported precision is explicit.
Archives retain official/normalized weights, constructors, inputs/masks/outputs,
operator boundaries, cotangents/VJPs, stochastic samples, criterion terms,
optimizer groups/moments/ages, and held-input results.

Controlled FP32 batch-one cases also retain `export-math/`: the pinned source's
prepared CUDA export graph with math SDPA, matching native export execution.
Training cases capture this copy after their ordinary-policy update on the held
input. Export comparisons use these outputs with the same value bounds and row
order; native forward, gradient and optimizer comparisons retain the ordinary
case policy. The manifest records the separate export policy. The optional
`--export-reference` adds CPU and ordinary-policy CUDA controls plus a hash-bound
artifact from the pinned upstream `OnnxExporter`. The export test's opt-in
`MMLTK_RFDETR_EXPORT_SOURCE_RUNTIME=1` measures that independent artifact through
the same runtime with matching precision options. These extra measurements do
not replace required comparisons. `MMLTK_RFDETR_EXPORT_BACKEND=onnx|tensorrt`
scopes a focused retry to the failed backend; default acceptance retains both.

The sixteen-preset stock-resolution matrix includes full-stock FP32 training
and controlled two-group train/eval, batches 1/2, FP32/FP16/supported BF16.
Controlled batch-two training keeps eight updates/precision, others one:
with BF16, **208 cases / 544 step archives**. Full tensor/gradient/update/moment
payloads can exceed **1 TiB**; JSONL logs are separate and small.

The native comparison uses production custom CUDA attention. Default SDPA keeps
backend selection; `--sdpa math` explicitly selects both upstream math and native
model-local math. Both disable matmul/cuDNN TF32; manifest precision is `highest`.
Main AMP matcher references use original functions with FP32 operands; untouched
upstream AMP matcher also runs under `stock/`, with explicit policy differences.

Only all four successful native stages publish `verdict.json`,
`status=passed_required_checks`. Full production status
`references_complete_native_comparison_pending` is never rewritten as native
success. Selected references never publish a full verdict. Per-attempt receipts,
appended comparison JSONL, wrapper/executable logs, and references survive failure;
cancellation retires only owned producer/test processes.

Retry against explicit retained evidence without regeneration:

```bash
./mmltk --test rfdetr --executable mmltk_backend_models_rfdetr_core_tests \
  --env MMLTK_RFDETR_UPSTREAM_ORACLE_DIR=/workspace/build/validation/rfdetr-math/upstream \
  -- '[upstream_oracle]'
./mmltk --test rfdetr --executable mmltk_backend_models_rfdetr_training_tests \
  --env MMLTK_RFDETR_UPSTREAM_ORACLE_DIR=/workspace/build/validation/rfdetr-math/upstream \
  -- '[upstream_oracle]'
```

Prepare unfinished cases individually, consume, then retire their payloads after
**every required consumer** passes. Keep training inputs through optimizer and
trained-export checks; preserve authenticated indexes, manifests, and reports.
Omitted combinations stay unrun; deleting inputs does not change required scope.
Official export reads explicitly selected inputs/task outputs, authenticating the
full index and every selected physical shard; complete archive admission requires
all payloads. Partially retained generations cannot serve fresh full checks and
never regenerate implicitly. Preparation rejects `retention.json`-marked
directories; use a fresh destination for missing cases.

`MMLTK_RFDETR_UPSTREAM_REPEAT_DIR` optionally supplies a second independent
reference for upstream-to-upstream observation; normal diagnostics do not need
it. [Comparison logging](logging.md#upstream-numerical-comparisons) owns fields,
retries, and queries.

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

Independent upstream reruns can differ under finite precision; that does not
classify a native discrepancy as upstream noise. PyTorch distinguishes
[finite-precision operation order](https://docs.pytorch.org/docs/2.14/notes/numerical_accuracy.html)
from [seeded randomness and deterministic algorithms](https://docs.pytorch.org/docs/2.14/notes/randomness.html).
Trajectory measurements do not establish AP, convergence or throughput equivalence.

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

Measure the current trusted index open through the read-only diagnostic:

```bash
./mmltk --diagnose-benchmark-image --compiled compiled/train.bin --sample 0 \
  --profile-compiled 8 --cold-compiled
```

Retain recorded counts, worker affinity/NUMA/nice, input identity, and cache
condition. `--cold-compiled` requests eviction only for that file. The timed
open copies/aggregates published metadata; it excludes payload verification,
Explore's later label-summary pass, and rendering. [Compiled loading](datasets.md#loading-a-compiled-file)
defines that boundary. Compare equivalent work and placement; file-local eviction
does not establish cold host/device state.

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
Keep command results and reviews separate. Review reports belong only under
ignored `build/validation/`; never add or commit them as documentation.
