# Training-dashboard cleanup and ownership consolidation

## Goal

Consolidate verified repeated mechanisms and structural projections around the
validated training dashboard, Linux operation lifetimes, native persistence,
GPU custody and their tests. Reduce the number of implementation inventories
and resource-lifetime assemblies without changing product policy, formats,
physical completion requirements or independent acceptance oracles.

Source and documentation anchors are confirmed at
`f4f51d17fb48c46af4a574b86cbfcdc10dd1fca7`. The preceding dashboard work starts
at `70fe498472738ffa81d28e5bbef63b6d5b4f840f` and changes 73 files. Line
anchors below refer to the former commit; declarations and functions identify
the locations after earlier phases move lines.

## Summary

Six implementation phases group the findings by existing owner and complete
consumer cutover: Linux cancellation/counter/process lifetimes; persisted
records and publication; typed parsing/decoding and CLI consumers; GPU/Live
retirement mechanisms; reusable native test fixtures; and bounded frontend
history cleanup with functional evidence for the loss-only scale control.

The fresh C++ report contains zero textual groups and 88 structural groups.
The fresh frontend report contains **zero textual and zero structural hits**.
The two frontend proposals are source-derived. Detector candidates do not by
themselves establish a reusable abstraction or a product failure.

The source call chain suggests that changing Log loss scale also invalidates
non-loss charts. Preserve the documented loss-only requirement, add ordinary
functional coverage, and defer any corresponding behavior correction until
that case proves the failure during main-owned Final Validation. Removal of
unused frontend history bookkeeping is independently established and can be
implemented normally.

## Scope

The exact implementation files and expected test changes are listed per phase.
The work includes direct declaration dependencies, narrowly scoped ordinary
helpers/classes, one typed cancellation binding, canonical reflected record
projections, and test-only resource/fixture ownership. It does not change
training, inference, scheduling, model mathematics, capture policy, transport
admission, checkpoint interpretation or published data formats.

Preserve these completed product outcomes throughout every cutover:

- Train has six default charts: training loss, AP50, AP50:95, average recall,
  precision/recall/F1 and learning rates. Selection, reset, empty/waiting states,
  optional charts, epoch/optimizer-step axes, aspect-constrained layout and
  expansion use the existing center workspace. Retained camera/legend state
  survives expansion, hiding, remount and navigation. Wheel/trackpad input,
  including modifiers over plots, axes and legends, belongs to the page scroller.
- Live progress uses current native rank-local completed/usable-total images,
  measured finite rates and available losses, independently of selected saved
  history. The GUI does not infer totals from mutable settings. Sparse epoch
  evaluation, unavailable intervals, drops, attempts and final-test results
  retain their distinct meanings. No GUI validation-loss calculation or curve,
  extra evaluation trajectory, telemetry-thread coupling or throughput chart
  is introduced. EMA remains optional, with one selected validation weight set.
- Train/validation directory inference leaves optional test selection independent;
  absent test inputs are allowed and selected incompatible inputs fail admission.
  Output editing/browsing does not implicitly start training or load history.
  Saved history and checkpoint continuation remain explicit and current-format.
  Train retains no native image workspace; Validate retains no aspect selector;
  Train/Validate/Predict retain their hidden H2D/NUMA controls and existing support.
- Settings schema 8, normalized annotation-index version 2 with its 256-byte
  header, compiled dataset format 7, native checkpoint version 3, training-history
  format 2, class-descriptor format 1, application protocol 17 and graphics ABI
  14 retain their current representations and admission rules.
- Fatal operation/process failures remain concise and visible on stderr with
  disabled, uninitialized or failed diagnostic sinks. Preserve named file-log
  identities, exact mapped status versus OS cause, bounded single-line reporting,
  errno and pending-SIGPIPE behavior, single terminal reporting and quiet healthy
  shutdown. Optional diagnostics remain effect-only and inactive when disabled.
- Raw products, borrowed readers, imported backing/context custody, callback
  completion, browser draws, cancellation, failure and shutdown preserve every
  existing physical settlement requirement and independent test assertion.

Performance assessment is logical and source-based. No benchmark or profiling
campaign is included. Preserve bounded work/storage and useful retained capacity;
add no training pass, parameter scan, readback, synchronization, collective,
telemetry write, polling loop, worker or runtime registry.

## Architecture

`common/io` owns neutral filesystem and descriptor operations. Its consumers
retain cancellation, publication, counter-value and error policy.
`common/concurrency` owns the typed stop-token/event-cancellation lifetime,
parameterized by the existing source type and its destruction policy. Product
systems still own admission, effects, execution and result mapping.

Canonical ordinary declarations own record structure. Reflection materializes
structural facts at those declarations; serializers and settings visitors consume
them without a second handwritten member inventory. Distinct external shapes,
renamed fields and persisted format checks remain explicit. Reflection does not
own runtime resources or control flow. Exact RF-DETR preset lookup belongs to
the existing preset catalog; filename/path inference retains its separate policy.

GPU mechanisms stay in `frameworks/gpu`; Live slot-state mechanics stay in
`backend/media/live`. Neither owner acquires controller policy. Private runtime
helpers retain their enclosing class's state, locking and lifetime boundaries.
Test-only CUDA/filesystem/async support stays in `test_support`; dataset,
class-artifact and topology fixtures stay with their respective domain owners.
Consumers link those test facilities without adding dependencies to production
libraries. New ordinary headers include their complete dependencies and register
through owning CMake targets; no new module, facade or declaration inventory is
needed.

Rust/Iced continues to own visual selection, interaction, presentation and the
bounded live/saved histories. The existing metrics component, retained chart
and vendored plot program remain their respective owners. No vendor or generated
schema edit is expected for the frontend work.

## Execution dependencies

Use the implementation/reviewer/framework-audit workflow in
[AGENTS.md](AGENTS.md#executing-an-action-plan), including its
[post-main-phase audit](AGENTS.md#post-main-phase-framework-audit). Phases 1–4
establish ordinary owners; Phase 5 cuts over their shared test consumers; Phase 6
is independent frontend work. Complete all implementation phases before
[Final Validation](#final-validation). The phase evidence lists define required
cases for that stage, not permission for early product builds or tests.

## Phase 1 — Linux cancellation, counter consumption and process failure ownership

**Findings:** COMMON-02/03, CTRL-02/03/04/05 and the diagnostic-pipe part of CTRL-08.

**Actions and confirmed anchors**

1. At `src/common/concurrency/event_cancellation.h:67`, add a noncopyable,
   nonmovable `ScopedEventCancellation<Source>` beside `EventCancellationSource`.
   It privately owns one minted source/token pair and one `std::stop_callback`,
   exposes direct token borrowing and explicit consumption, and unregisters the
   callback before destroying the source. Include `<stop_token>` directly.
   Preserve tag identity and both `CancelOnDestruction` policies without new
   descriptors, allocation or synchronization beyond the existing callback.
   Replace all eight assemblies in `dataset_system.cpp:12,24`,
   `model_system.cpp:27`, `file_dialog_system.cpp:12` and
   `training_system.cpp:31,50,59,84`; delete Train's callback-only helper at `:21`.
   Keep registration timing, especially Train's launch-before-mint order, and
   leave progress/provider effects and result translation at each call site.
   Add the direct private common-concurrency dependency to the data/compute target.
2. Extend `src/common/io/event_fd.h:7` / `event_fd.cpp:14` with one ordinary
   EINTR-retrying counter read. Return the `uint64_t` counter, actual byte result
   and captured failure errno with self-contained Linux type includes. The
   operation performs one successful/terminal read and imposes no value policy.
   Reuse it inside `wait_event_fd:19` and `drain_event_fd:26` without changing
   their readiness checks or drain loops. Migrate `presentation_system.cpp:364`,
   `native_presentation_writer.cpp:417,510`, `diagnostics_client.cpp:181,365`,
   `firefox_process_owner.cpp:389,406`, `visual_system_fixture.h:177` and
   `linux_process_test_utils.hpp:53`.
3. Delete capture's complete local signal/drain copies at
   `capture_session_core.cpp:29,45` and `capture_session_preview.cpp:33,42`;
   migrate every call to the existing common operations. Add capture's private
   common-I/O link. Retain negative-fd handling, EINTR/EAGAIN semantics and its
   exact restart/terminal wake order.
4. In `firefox_process_owner.cpp`, add one private mutex-held operation for the
   first paired infrastructure-status/OS-cause update. Use it at `:338,413,446,485`.
   Remove the unreachable second error-recording branch at `:490` while retaining
   the SIGKILL call. Signals, timer operations, reaping, observations and logging
   stay at their current sites.
5. Consolidate the union of exit-policy assertions from
   `application_services.test.cpp:135` and `firefox_process_owner.test.cpp:141`
   into an explicit case table in the latter. Retain normal/requested zero and
   nonzero exits, healthy/unhealthy presentation, requested/unrequested SIGTERM,
   escalation with SIGTERM/SIGKILL, ENOENT status and status 1 with independent
   EACCES cause under both presentation outcomes. Physical startup/custody tests
   remain separate.
6. Finish using `make_diagnostic_pipe` at `application_services.test.cpp:52`
   throughout that file: raw setups at `:472,494,507,610,852,855` use the existing
   helper. Keep capacity adjustment and its independent checks in the stalled
   writer test, descriptor ownership through moves/close, and all raw terminal
   read assertions. This is one local fixture cutover, not a new scenario runner.

**Preservation and evidence**

Presentation accepts a nonnegative read or EAGAIN; diagnostics requires a full
read or EAGAIN; Firefox's timer requires a full read and distinguishes actual
errno from synthetic EIO; terminal/stop consumption ignores the result.
`consume_timerfd` still requires exactly one expiration. Preserve these separate
policies, ambient errno behavior, poll ordering and single-read versus drain
behavior. Firefox's stop write and the fatal stderr writer retain their distinct
error/SIGPIPE policies. Keep first-error precedence, one terminal observation,
pidfd/retained-PID custody, escalation, reaping and profile removal.

Extend existing common cancellation cases for pre-requested stop, concurrent
stop/destruction, borrowed/consumed tokens, exception unwind and both destruction
policies. Extend controlled-descriptor cases in services tests for full,
interrupted, would-block, short and failed counter reads with causal coordination.
Required suites cover common concurrency; file-dialog, dataset/model and provider
cancellation; diagnostic close/backpressure; Firefox graceful/escalated stop;
presentation completion/retirement; and Live receiver/stop paths. The existing
Firefox readiness-pipe and raw terminal-read assertions remain independent oracles.

**Expected files**

```text
src/common/concurrency/event_cancellation.h
src/common/concurrency/tests/cancellation_observation.test.cpp
src/common/io/event_fd.h
src/common/io/event_fd.cpp
src/controller/services/file_dialog_system.cpp
src/controller/services/diagnostics_client.cpp
src/controller/services/firefox_process_owner.cpp
src/controller/services/tests/application_services.test.cpp
src/controller/services/tests/firefox_process_owner.test.cpp
src/controller/subsystems/system/dataset_system.cpp
src/controller/subsystems/system/model_system.cpp
src/controller/subsystems/train/training_system.cpp
src/controller/subsystems/system/CMakeLists.txt
src/controller/presentation/presentation_system.cpp
src/controller/presentation/native_presentation_writer.cpp
src/controller/presentation/tests/support/visual_system_fixture.h
src/test_support/linux_process_test_utils.hpp
src/backend/media/capture/capture_session_core.cpp
src/backend/media/capture/capture_session_preview.cpp
src/backend/media/capture/CMakeLists.txt
```

**Handoff/risk:** the neutral primitives must not absorb any caller's failure or
physical-custody decision. Phase 5 adds test-only links to the data/compute CMake
file without changing this phase's production dependencies.

## Phase 2 — Canonical persisted records and destination preparation

**Findings:** COMMON-01/BACKEND-01, BACKEND-08 and CTRL-01.

**Actions and confirmed anchors**

1. Add ordinary `ensure_parent_directory(path)` beside publication operations
   in `src/common/io/file_memory.h:12` / `file_memory.cpp:29`. Select `.` for an
   empty parent, call the existing throwing `create_directories`, and return the
   selected parent. Replace the seven complete operations at
   `staging_directory.cpp:9`, `benchmark_annotations.cpp:1209`,
   `benchmark_cache.cpp:62,86`, `benchmark_download.cpp:265,796` and
   `benchmark_writer.cpp:287`. Do not alter the compiler's separate path
   selection and creation at `benchmark_compiler.cpp:772,774`: a progress callback
   lies between them. Logging's empty-parent skip and settings' error-code policy
   are also distinct.
2. Materialize `AnnotationRejectCounts` at its canonical ordinary declaration
   `src/backend/data/detail/benchmark_annotations.h:36`, using the existing
   reflected-field machinery. Derive binary array conversions at
   `benchmark_annotations.cpp:57,62` and the named manifest projection at
   `benchmark_compiler.cpp:688,1522,1547` from that materialization. Keep concrete
   conversion functions with backend data; remove the parallel member/key
   inventories, with no runtime table or generic new serialization framework.
   Lock the six `uint64_t` slots in this exact order: `raw_records`,
   `unmapped_categories`, `unknown_images`, `malformed_records`,
   `degenerate_boxes`, `duplicate_boxes`. Compile-time checks protect count,
   types, order, version 2 and the 256-byte normalized header. Preserve JSON key
   spelling/numeric values and the current aggregation, including duplicate-box
   counting after worker aggregation and abbreviated gated traces.
3. Reuse/generalize `visit_option_fields` locally at
   `src/controller/contracts/gui_settings.cpp:406` for the complete same-name
   records currently listed at `:265,298,587`. Project `SourceSelectionState`,
   `TrainExecutionPaneState` and `UiSettingsState` from the materialized
   declarations in `view_state.h:34,138,100` / `:355,361,359`, including the Train
   base projection into derived state. Leave recipe/workflow subsets, renamed
   keys and enum normalization as explicit distinct external shapes. No native
   declaration or generated binding change is needed for these settings records.

**Preservation and evidence**

Filesystem consumers retain their lock, resume/segmentation, cancellation,
unique sibling naming, open modes/flags, fsync/rename and rollback order. No new
probe, canonicalization or retry is added. Test bare/relative/absolute and
obstructed parents through the existing publication/cache/download cases.
StagingDirectory retains complete and partial-construction cleanup for dataset
compilation, class publication and controller ArtifactStore.

In `benchmark_dataset.test.cpp:367`, use distinct values for all six counters
and independent persisted expectations for binary round-trip and named manifest
projection, not a second projection as the oracle. Retain invalid-cache,
annotation and cancellation cases. In `settings.test.cpp:550,814,830,1537`,
retain independent nonuniform values/key-shape checks, missing-member retention,
normalization, malformed-input rejection and atomic mutation with schema 8.
Existing data, inference-publication and controller cancelled-artifact cases
must continue to preserve exact previous bundle bytes.

**Expected files**

```text
src/common/io/file_memory.h
src/common/io/file_memory.cpp
src/common/io/staging_directory.cpp
src/backend/data/detail/benchmark_annotations.h
src/backend/data/benchmark_annotations.cpp
src/backend/data/benchmark_cache.cpp
src/backend/data/benchmark_download.cpp
src/backend/data/benchmark_writer.cpp
src/backend/data/benchmark_compiler.cpp
src/backend/data/tests/benchmark_dataset.test.cpp
src/controller/contracts/gui_settings.cpp
src/controller/services/tests/settings.test.cpp
```

**Handoff/risk:** common I/O remains below data policy; backend data already links
reflection and JSON. Field materialization must not silently redefine a persisted
slot order, add runtime lookup, or broaden settings validation behavior. Phase 5
reuses fixture ownership without replacing this phase's independent persistence
expectations.

## Phase 3 — Typed parser, decoder and CLI catalog consumers

**Findings:** FW-04/05/06, BACKEND-03, ACCEPTANCE-03/ENTRYPOINTS-01,
ENTRYPOINTS-02 and the browser fixture part of CTRL-08.

**Actions and confirmed anchors**

1. In `src/frameworks/reflection/reflected_descriptors.h:216`, consolidate the
   nonboolean parse/error/constraint pipeline in `assign_accessed`. Select the
   parsed type at compile time, unwrapping repeatable sequences only. Parse
   optionals as the optional type to preserve empty-input reset. Branch after
   successful parsing/validation for checked append or assignment; remove the
   redundant optional assignment branch. Preserve boolean handling and error
   precedence: parse, scalar policy, maximum items, fixed capacity.
2. Add one private nonconsuming byte operation beside `wire::Reader::byte` at
   `src/frameworks/serialization/cbor_wire.h:231`; centralize the repeated EOF
   and segment lookup at `cbor_wire.cpp:325,330`. `byte` alone advances after
   success. `next_is_null` keeps its document-size check before EOF; do not add
   that check to every byte read. Reuse `contextual_key_error` at
   `reflected_cbor_detail.h:1396` in `require_projected_members:1455` and
   `require_variant_key:1525`, preserving their separate decoder policies.
3. Delete only `find_model_preset` at
   `src/backend/models/rfdetr/contract/model_config.h:94` / `model_config.cpp:58`.
   Migrate every exact-name caller to `find_preset_catalog_entry` in
   `preset_catalog.h:246`, including using-declarations, with direct ordinary
   declaration includes. Preserve filename/alias/path inference and
   `native_config_from_preset`. Update `weight_catalog.test.cpp:54` to assert
   canonical catalog identity and all concrete architecture expectations instead
   of comparing two names for the same lookup.
4. Reuse `runtime_paths::current_executable_path()` from
   `src/common/system/runtime_paths.h:5` / `runtime_paths.cpp:16`. Delete
   acceptance's helper at `reporter_logging.test.cpp:42` and migrate `:51,157`.
   Replace CLI support's fixed-buffer read at `tests/support/cli_path.cpp:15`.
   Keep its priority: existing configured path, existing discovered sibling
   `mmltk`, existing `/opt/mmltk/bin/mmltk`, then original configured path.
   Preserve `exists(..., error_code)` handling. Catch only the resolver's
   `std::runtime_error` in a narrow discovery scope, then continue fallbacks;
   skip sibling lookup on an empty path. Acceptance self-discovery still fails
   instead of choosing another executable. Add private common-system links to
   both test-consuming targets; the common resolver itself does not change.
5. In `cli_runtime.test.cpp`, replace all seven manual directory lifetimes
   starting at `:514,542,554,577,647,676,729` with the already-used `ScopedTempDir`,
   retain each prefix and a reference to `path()`, and delete local helpers
   at `:48,55`. Keep owners alive through subprocess completion and assertions;
   preserve in-test removals, `.mmltk-data` resets, fake Docker scripts,
   executable permissions and exact path/UID/GID/environment assertions.
6. Use the existing `decode_intent_fixture` at
   `src/controller/browser/tests/client_record.test.cpp:127` in its annotation
   alternative loop at `:338`. Preserve independent correlation, endpoint,
   dispatch and exhaustive-alternative assertions.

**Preservation and evidence**

Parser cases retain exact messages, failed-assignment destination state,
optional reset (`cli_runtime.test.cpp:438`), path/enum/environment policies and
scalar zero-allocation parsing (`:443`). Add invalid input at a full repeatable
destination to establish error precedence. CLI, desktop-options and Train-command
consumers compile against the same declarations.

CBOR remains byte-for-byte unchanged with exact offsets, dot-separated paths,
base-before-derived failure order, item accounting, borrowed lifetimes and O(1)
allocation-free successful byte access. Extend `cbor_wire.test.cpp:196,448,521`
for nested missing members, wrong variant keys, empty/segmented input, repeated
peek, segment transitions, no advance on failure and limit-before-EOF behavior.
Missing required members still report UnknownKey and wrong variant keys report
TypeMismatch with the expected key. Ordinary and opaque decoders remain distinct.

Preset lookup retains case-sensitive membership, stable catalog pointers, null
for empty/unknown/retired names, bounded linear cost and existing alias ambiguity.
Required targets include RF-DETR contract, core/native integration, training
checkpoint parity, inference artifact resolution, CLI, serialization and browser
protocol. Review the complete CLI fallback decision table, including failed/empty
self-discovery; successful configured-path tests alone do not prove other branches.
Existing reporter compact/TAP and seven fatal modes retain owned command bytes,
child custody, pending-SIGPIPE, exact stderr and exit assertions.

**Expected files**

```text
src/frameworks/reflection/reflected_descriptors.h
src/frameworks/serialization/cbor_wire.h
src/frameworks/serialization/cbor_wire.cpp
src/frameworks/serialization/reflected_cbor_detail.h
src/frameworks/serialization/tests/cbor_wire.test.cpp
src/controller/browser/tests/client_record.test.cpp
src/backend/models/rfdetr/contract/model_config.h
src/backend/models/rfdetr/contract/model_config.cpp
src/backend/models/rfdetr/contract/workflow_requests.cpp
src/backend/models/rfdetr/contract/tests/training_supervision.test.cpp
src/backend/models/rfdetr/contract/tests/weight_catalog.test.cpp
src/backend/models/rfdetr/inference/rfdetr_runtime_backend.cpp
src/backend/models/rfdetr/core/model_state.cpp
src/backend/models/rfdetr/core/tests/asset_cache_support.h
src/backend/models/rfdetr/training/tests/checkpoint_parity.test.cpp
src/entrypoints/cli/tests/support/cli_path.cpp
src/entrypoints/cli/tests/cli_runtime.test.cpp
src/entrypoints/cli/CMakeLists.txt
src/acceptance/tests/reporter_logging.test.cpp
src/acceptance/CMakeLists.txt
```

**Handoff/risk:** dependencies stay consumer → common system/reflection/
serialization/native catalog. Do not unwrap optional values with
`descriptor_scalar_t`, broaden a discovery catch, replace branch evidence with
configured-path success, or alter protocol/schema identity while consolidating
mechanics. The affected module importers and declaration owners must be built
together in Final Validation.

## Phase 4 — GPU and Live failure, wake and retirement mechanisms

**Findings:** FW-01/BACKEND-02, FW-02/03 and BACKEND-04/05.

**Actions and confirmed anchors**

1. Declare one CUDA-driver status translator beside `ensure_cuda_ok` in
   `src/frameworks/gpu/cuda_error.h:65`; define it in `cuda_runtime.cpp:12`.
   Replace all uses of the three identical local translators at
   `device_execution.cpp:11`, `detail/gdr_buffer_backend.cpp:10` and
   `src/backend/data/compiled_image_stream.cpp:30`, deleting their definitions.
   Preserve `std::runtime_error`, each operation string, `cuGetErrorString` and
   the exact `CUDA driver failure` fallback. Success does no query/allocation.
   Leave runtime `CudaError`, pinned-host numeric formatting, GDR error
   classification and other nonthrowing policies with their current owners.
2. Add one State-local wake-invocation operation in
   `ImageWorkspace::State` at `image_workspace.cpp:149`. It accepts the selected
   atomic sink and performs the existing acquire-load, retained invocation and
   exception swallowing. Use it at `:204,472,489,510,538,677`, keeping sink
   selection and display admission checks at the existing call sites.
3. In `SystemImageRuntime::Retire` at `system_image_runtime.cpp:164`, put the
   bound model-release/destruction sequence under one failure-aggregation catch
   instead of the two repeated bind catches at `:190,203`. Keep both context
   binds, the incomplete-release early return, initiating/secondary failure
   order and retention on either bind failure. `ReleaseResources` may change
   the current context; successful rebinding still precedes model destruction.
4. Add one ordinary retirement-claim operation beside `claim_live_slot` in
   `src/backend/media/live/detail/live_slot_state.h:25`. It accepts an already
   Completing slot, refuses Free/Terminal, and otherwise performs the existing
   single compare-exchange. Use it at `detail/live_device_types.h:70`,
   `live_analyzer_worker.cpp:112` and `live_raw_frame_cache.cpp:146`.
   Leave `LiveCompositor::stop` at `live_compositor.cpp:75` separate: it also
   excludes Acquired slots. Do not substitute a second state load/claim there
   or weaken reader custody.
5. Have `live_session_controller.cpp:320` construct its current CUDA failure
   status and call `record_failure:332`. Keep `record_failure_locked`, first
   failure, wake-pending/phase transition and notification order unchanged.
6. Add one private ordinary event-release operation on
   `TrainingSupervisionImpl::TimingState` at
   `src/backend/models/rfdetr/core/training_supervision.cpp:138`, used by the
   partial-construction catch at `:149` and destructor at `:158`. Retain each
   caller's device guard, destruction of every created event, ignored destroy
   status and original exception. Timing events remain distinct from completion
   event-pool leases.

**Preservation and evidence**

Driver translation changes no context push/pop, restoration-failure precedence,
worker propagation or retirement command order. Workspace notifications remain
outside access locks with the same acquire/release semantics and callable
lifetime. Preserve CancelWrite's pending-source early return and publication
before wake; CancelDisplayWrite's existing product wakes before and after
clearing display-held state; Detach's unconditional display wake; and
CompleteRead's product-only wake. A GPU callback only marks completion and wakes
its owner; it does not settle/publish/release storage.

Model retirement still settles streams/workspaces before release, preserves the
fallback failure for incomplete release, retains the model on either bind
failure, and then retires pools/products with unchanged deferred classification.
Live retains synchronization before claiming, one raw store-stream settlement,
analyzer result-event/provider release, scrub-before-publication and exact
Free/Terminal choice. Add no per-slot raw-stream synchronization or new wait.

Extend `workspace_interop.test.cpp:1319` for absent/throwing sinks and cancellation
wake order; retain zero display notifications before owner settlement and exactly
one afterward. Extend `image_execution_retirement.test.cpp:107,199,220,237,252,330,343`
with post-release bind-failure custody coverage. Extend `live_output_lease.test.cpp:98`
with retirement-state boundaries and failed single-attempt claims. Required cases
cover GPU placement/GDR/retirement and visual/presentation custody, backend data
and Explore streams, Live stop/receiver failure, and core
`test_timing_leases_are_explicit_bounded_and_harvested_once` with static inspection
of partial-construction release.

**Expected files**

```text
src/frameworks/gpu/cuda_error.h
src/frameworks/gpu/cuda_runtime.cpp
src/frameworks/gpu/device_execution.cpp
src/frameworks/gpu/detail/gdr_buffer_backend.cpp
src/backend/data/compiled_image_stream.cpp
src/frameworks/gpu/image_workspace.cpp
src/frameworks/gpu/tests/workspace_interop.test.cpp
src/frameworks/gpu/system_image_runtime.cpp
src/frameworks/gpu/tests/image_execution_retirement.test.cpp
src/backend/media/live/detail/live_slot_state.h
src/backend/media/live/detail/live_device_types.h
src/backend/media/live/live_analyzer_worker.cpp
src/backend/media/live/live_raw_frame_cache.cpp
src/backend/media/live/live_session_controller.cpp
src/backend/media/live/tests/live_output_lease.test.cpp
src/backend/models/rfdetr/core/training_supervision.cpp
```

**Handoff/risk:** the GPU target already owns the driver link and header-isolation
checks; backend data already links GPU. Similar-looking settlement code is not
permission to merge resource owners, remove the second bind, collapse two wake
opportunities or replace physical completion with diagnostic evidence.

## Phase 5 — Reusable domain fixtures and test resource custody

**Findings:** ACCEPTANCE-01/02, BACKEND-06/07, COMMON-04/CTRL-07 and CTRL-06/09.

**Actions and confirmed anchors**

1. Use the existing `NativeExploreFixture` at
   `src/acceptance/tests/compiled_dataset_explore.test.cpp:287` for default
   constructions at `:446,826`, with local audit/system references. Keep custom
   gated configurations at `:1159,1360` local. Replace only their destructor-only
   StopGate structs at `:1168,1365` with existing `ScopedTestCleanup`, at the same
   declaration positions. The first cleanup calls gate.Stop, stale-lane Release,
   then prefetch-lane Release; the second stops its gate. Keep successful-path
   stops and the cancellation entered-receipt/release-before-assertion at `:1411`.
2. Add one noncopyable scoped test stream in
   `src/test_support/cuda_test_utils.hpp`, requiring successful nonblocking CUDA
   construction, exposing its handle and ignoring destruction status as before.
   Replace the five stream lifetimes in
   `video_file_source.test.cpp:93,321,384,501,596` and the five in
   `prediction_session.test.cpp:173,475,510,701,780`. Device/NUMA selection,
   causal waits, Torch guards and domain lifetimes remain at each call site.
   Do not introduce pooling or extra synchronization.
3. Extend existing backend-data fixture support at
   `tests/test_fixture.h:5,15` / `test_fixture.cpp:67` with an ordinary
   compile-existing-fixture operation. Map its paths/split/dimensions, select
   one worker, prepare, and compile split zero. Use it at
   `prediction_session.test.cpp:691,942`. Keep source creation separate: the
   second case edits categories/annotations before compilation. This operation
   must never clear/recreate the fixture root or override other compiler defaults.
4. Add self-contained `src/common/system/tests/numa_topology_test_support.h`
   on `mmltk_common_system_test_support` (`system/CMakeLists.txt:33`). A single
   selection function takes `const NumaTopology&` and returns the first permitted
   `CpuTopology` value, failing clearly for empty/unmapped input. Migrate
   `execution_policy.test.cpp:42,67,82`, `runtime_placement.test.cpp:136,162`,
   `native_ops.test.cpp:595` and `worker_pool.test.cpp:190,207`. Each caller keeps
   its fresh Capture, including denied-syscall children; workers reuse that same
   snapshot for resolve_placement and budget checks. Add only test-support links
   to common concurrency and RF-DETR core and register header isolation on the
   extended topology support target.
5. Add self-contained
   `src/backend/models/rfdetr/core/tests/class_artifact_fixture.h` beside the
   class-artifact owner. The fixture seeds a caller-selected artifact and its
   digest-bound `.classes.json`, stores both independent digests and explicitly
   checks unchanged bytes and absence of leaked staging directories after the
   relevant producer/publication scope. Reuse it in
   `class_layout_roundtrip.test.cpp:185`, `prediction_session.test.cpp:1046` and
   `application_data_compute_systems.test.cpp:514`. Keep root RAII, test-selected
   bytes/paths, stop timing, producer calls and distinct domain assertions local.
   Register the header-only
   `mmltk_backend_models_rfdetr_class_artifact_test_support` target beside core's
   test targets in `core/CMakeLists.txt:153`, outside the Python-checkpoint-loader
   conditional, with direct declaration/link requirements and header isolation.
   Link only inference and controller data/compute tests to it. Do not expand
   the checkpoint fixture target or link test support into production core.
6. Add one local successful-inspection builder beside `split` at
   `application_data_test_support.h:29`; use it in `FakeDatasetRuntime::Inspect:59`
   and `BlockingInspectRuntime::Inspect:172`. Retain path order, skipped empty
   paths, bounded split capacity and existing split values. Blocking Enter,
   started receipt, stop-aware wait, Leave and concurrency observations stay
   at the existing call sites in their current order.

**Preservation and evidence**

Inspect declaration/destruction order through assertion and exception unwind,
not only successful completion. Explore audit outlives its system; gates release
before joins; test streams outlive every session/borrowed pixel and retain current
Catch failure behavior. Keep all H2D/GDR variants, exact geometry/semantic checks,
independent training-batch lease, receiver copies, retained capacity, transaction
rollback, weak-artifact expiry and all four transaction outcomes.

Class-bundle tests retain pre-cancel versus live post-staging cancellation,
ArtifactPublicationCancelled, controller Cancelled mapping, zero processed images,
empty backend results, both exact digests and directory checks; do not use the
production admission reader as its own preservation oracle. Topology support
never captures/caches topology, changes affinity, substitutes node zero or computes
expected policy. Preserve EPERM rollback, placement-before-CUDA, worker budgets
and LSAP NUMA/capacity assertions. Logical costs remain one topology lookup and
existing fixture-sized I/O; no product work is added.

Required targets: acceptance; media video; RF-DETR inference and core; backend
data; common system/concurrency; controller data/compute. Shared header changes
and all directly changed consumers are built/validated together.

**Expected files**

```text
src/acceptance/tests/compiled_dataset_explore.test.cpp
src/test_support/cuda_test_utils.hpp
src/backend/media/video/tests/video_file_source.test.cpp
src/backend/data/tests/test_fixture.h
src/backend/data/tests/test_fixture.cpp
src/backend/models/rfdetr/inference/tests/prediction_session.test.cpp
src/backend/models/rfdetr/inference/tests/class_layout_roundtrip.test.cpp
src/backend/models/rfdetr/core/tests/class_artifact_fixture.h (new)
src/backend/models/rfdetr/core/tests/native_ops.test.cpp
src/backend/models/rfdetr/core/CMakeLists.txt
src/backend/models/rfdetr/inference/CMakeLists.txt
src/common/system/tests/numa_topology_test_support.h (new)
src/common/system/tests/execution_policy.test.cpp
src/common/system/CMakeLists.txt
src/common/concurrency/tests/worker_pool.test.cpp
src/common/concurrency/CMakeLists.txt
src/controller/subsystems/system/tests/runtime_placement.test.cpp
src/controller/subsystems/system/tests/application_data_compute_systems.test.cpp
src/controller/subsystems/system/tests/application_data_test_support.h
src/controller/subsystems/system/CMakeLists.txt
```

**Handoff/risk:** retain test-only dependency direction and explicit causal
assertions. Each caller owns topology capture; the selector owns no topology
state or execution policy. The class fixture owns bundle setup and preservation;
producer policy and inference remain with their domain tests.

## Phase 6 — Bounded history cleanup and loss-scale functional evidence

**Findings:** FE-02 and evidence-only implementation of FE-01. Neither is a
frontend detector finding.

**Actions and confirmed anchors**

1. Remove unused `Curve::omitted_extrema` and `Curve::available` from
   `src/frontend/iced/src/view/metrics/history.rs:33,35`, including initialization,
   clearing, retirement accumulation at `:95`, push bookkeeping at `:67` and
   ingestion assignment at `:252`. Production uses bucket presence and omission
   counts (`metrics.rs:275,187`); native `MetricSummary.available` is separate
   and remains intact. Replace the three history-test availability assertions
   at `history.rs:337,343,357` with independent retained-bucket and pending-gap
   assertions. Preserve endpoints/extrema in retained buckets, missing/segment
   state, duplicate suppression, coalescing, the 128-bucket bound and omissions.
2. Extend the existing metrics test module at `metrics.rs:325` with a functional
   loss-scale case using `iced::widget::shader::Program::update` for PlotWidget,
   its associated State, real redraw/drag events and returned render-publication
   messages reduced through the component. The established pattern is
   `third_party/iced_plot/src/plot_widget.rs:2188,2197`; do not fabricate private
   camera/publication fields or add a vendor test facade.
3. With settled data, pan a non-loss chart, toggle Log loss scale, remount and
   require identical camera/legend state and no unrelated dirty preparation.
   Cover visible and hidden/revealed non-loss charts, both live and explicitly
   selected saved histories, an unchanged log selection, and loss/component
   charts still changing effective scale. Keep epoch-axis and real data/history
   changes exercising their existing autoscale behavior. Use existing state
   access/observations and ordinary program publications, not product telemetry.
4. Do not change production invalidation in this phase. The source concern is
   `metrics.rs:160` invalidating every chart, `chart.rs:52` keying all charts by
   the global log flag and `:61,94` rewriting scale/positions; plot setters
   advance data version at `plot_widget.rs:394,974` before projection/autoscale
   at `:1647,1663`. The new case must establish whether that path violates the
   required behavior at Final Validation before a fix is applied.

**Expected files**

```text
src/frontend/iced/src/view/metrics/history.rs
src/frontend/iced/src/view/metrics.rs
```

The only anticipated conditional production correction, owned by Final
Validation after failing evidence, also touches
`src/frontend/iced/src/view/metrics/chart.rs`. No native/schema/vendor file is
expected to change.

**Handoff/risk:** preserve shape identities, independent live/saved ingestion,
gaps, sparse selected evaluation, remount/picking settlement, page scrolling,
the progress card and every packaged dashboard outcome. Removal of obsolete
state must not remove retained-bucket extrema or native metric availability.
The test is an acceptance requirement; its possible failure is not permission
to change behavior before observation or to add a separate review phase.

## Final Validation

Use [AGENTS.md's Final Validation workflow](AGENTS.md#final-validation-workflow)
and [the command/evidence reference](docs/validation.md) as the single authority
for ownership, ordered tidy/build/cleanup/tidy/review/final-build stages and
failure remediation. Both full builds use `./mmltk --build`; both full tidy
passes use `./mmltk --tidy`. Both cleanup profiles apply because handwritten
Rust changes: obtain fresh full reports with `./mmltk --cleanup-report all` at
the prescribed stage, resolve genuine hits, and obtain the final clean full
passes. Historical reports are inputs to planning, not substituted clean reports.

After the required final full build, execute:

```bash
./mmltk --test browser-app
./mmltk --test all
./mmltk --test browser-runtime
./mmltk --test workspace-wayland --headless-compositor
```

`all` covers the changed native owners and shared-fixture consumers registered
in `mmltk:3257–3293`; browser-app additionally covers the owned plot workspace,
and browser-runtime retains explicit process-exit evidence. Full packaged
Wayland acceptance retains required H2D lifetimes, dashboard/progress pixels,
aspects, camera/legend retention, wheel ownership, ordered input, physical
custody and quiet/terminal shutdown. Optional GDR or multi-device unavailability
remains an explicit skip, not successful hardware evidence.

If the new loss-scale functional case fails through the identified invalidation
path, retain that failing evidence before editing. The main agent then makes the
narrow correction in the metrics owner: a changed log selection dirties only
`Chart::loss()` charts and each retained chart keys preparation by its effective
scale, so non-loss charts do not observe the global loss setting. Reuse current
dirty/preparation state; retain all-chart invalidation for X-axis or selected
history changes and existing autoscale on real data. Rebuild with `./mmltk --build`
and rerun the affected functional and required packaged acceptance cases under
the normal validation-fix rules. If the case passes, do not manufacture a
production edit. A distinct failure requires its own evidence-driven diagnosis.

Record per-phase required boundary cases, target/filter, build/source identity,
terminal result and hardware skips under `build/validation`; distinguish static
branch/lifetime review from executed evidence. Use the log-query tooling for
failures. Complete the logical performance review of the whole implementation:
filesystem/read syscall counts, loop complexity, allocation/capacity, callback
ordering, owner-thread completion and absence of additional training/GPU work.
No timing measurements are required or implied.

## Documentation completion

After successful Final Validation, perform the single documentation pass and
main-agent documentation review defined in [AGENTS.md](AGENTS.md#documentation).
Reconcile commands, paths, ownership, formats and behavior against the resulting
code while preserving the committed dashboard and failure-reporting facts.
The documentation set is `README.md`, `CONTRACT.md`, `AGENTS.md` and the current
wiki: `docs/README.md`, `docs/architecture.md`, `docs/build.md`, `docs/commands.md`,
`docs/datasets.md`, `docs/gpu-execution.md`, `docs/gui-interaction.md`,
`docs/headless-wayland.md`, `docs/logging.md`, `docs/rfdetr-workflows.md`,
`docs/roadmap.md` and `docs/validation.md`. Edit only materially
changed facts in their authoritative homes; preserve README's protected prefix.

## Risks and post-cleanup concerns

- Cancellation callback registration/destruction order is physical lifetime,
  not vocabulary. Moving a binding or exposing its source would enlarge its
  contract and can invalidate callback captures.
- Counter-read success, short-read policy, actual errno and mapped synthetic
  status differ by caller. Consolidating the syscall must not standardize those
  outcomes or swallow a fatal boundary.
- Persisted reflection must retain exact binary order and named keys. Independent
  format/key assertions remain deliberate duplication of expectations, not a
  second implementation inventory.
- GPU wake invocation is not display admission or completion. Live compositor
  acquired-reader exclusions and model teardown's second context bind remain
  essential even where nearby code looks similar.
- Fixture consolidation can hide missing coverage or change destruction order.
  Preserve every scenario, transport variant, byte/custody assertion and causal
  entered/released boundary. No generic test runner replaces these oracles.
- Reject shared abstractions for the independent integration-environment gates,
  atomic observation getters, admission/wait assertions, dimension-specific
  arithmetic, tensor/storage operations and distinct readiness protocols.
  Reject capture-owning topology helpers, generalized CUDA exception policies,
  runtime schema registries and generic retirement/event-loop coordinators.
- Frontend invalidation remains a conditional, evidence-gated correction to an
  existing documented requirement. Source inspection alone is not rendered or
  functional failure acceptance; zero frontend detector hits stays explicit.
- Required final execution evidence must come from the changed source. Historical
  results, source-based performance reasoning and hardware skips cannot replace
  it. Documentation completion does not reopen implementation or validation.
