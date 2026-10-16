# Capturing and querying diagnostics

[Wiki index](README.md) · [Commands](commands.md) · [Validation](validation.md) · [Headless Wayland](headless-wayland.md)

## Activation and quiet execution

Ordinary `--gui` has no diagnostic sink, integration/reporting state, or pixel
probe. Disabled diagnostics do no collection, formatting, clock reads, counter
updates, or I/O. Product ordering/custody stays independent of diagnostic IDs.
[Fatal stderr](#fatal-stderr-reports) remains active. [Training history](rfdetr-workflows.md#saved-history-and-plots)
(`run.json`, `metrics.jsonl`, progress, incomplete-history notices) is product
output, independent of diagnostics.

| Opt-in | Effect |
| --- | --- |
| `MMLTK_LOG_LEVEL`, `MMLTK_LOG_FILE`, `MMLTK_LOG_DIR` | Native stderr and rotating file |
| CLI `--log-level`, `--log-file`, `--log-dir` | Override matching environment settings |
| `MMLTK_GUI_TRACE_FILE` | Native JSONL, benchmark compilation, browser surface lifecycle |
| `MMLTK_GUI_PIXEL_TRACE=1` plus GUI trace path | Pixel probes |
| `MMLTK_FIREFOX_LOG_FILE` | Separate Firefox stdout/stderr capture; enables no native/pixel collection |

A non-`off` level or destination enables native logging at the build default;
explicit `off` wins over destinations without disabling separately requested
GUI tracing. Desktop uses environment settings; native CLI/ONNX tools accept
flags too. With only a level, the wrapper prepares `.mmltk-data/logs` for the
runtime UID/GID. Explicit destinations retain host-path rewriting. Native files
use application names, 10 MiB rotation, and five archives.

```bash
MMLTK_LOG_LEVEL=info ./mmltk --gui
./mmltk info --compiled ./compiled/train.bin --log-level info
./mmltk rfdetr info --onnx ./model.onnx --log-level info
```

Routine ONNX inspection/simplification metadata needs diagnostics; `rfdetr info
--onnx` forwards overrides to its sibling tool. Fatal stderr/status and ordinary
CLI result stdout remain independent.

## Wrapper build, test, and tidy transcripts

The native [driver](../tools/mmltk.cpp) automatically tees explicit build/test/
tidy stdout+stderr to the printed repository-local path:

| Operation | Default `build/logs/` filename | Override |
| --- | --- | --- |
| `--build` | `mmltk-build-YYYYMMDD-HHMMSS-PID.log` | `MMLTK_BUILD_LOG_FILE` |
| `--build-tools` | `mmltk-build-tools-YYYYMMDD-HHMMSS-PID.log` | `MMLTK_BUILD_LOG_FILE` |
| `--diagnose-rfdetr-upstream --build-only` | `mmltk-build-rfdetr-oracle-YYYYMMDD-HHMMSS-PID.log` | `MMLTK_BUILD_LOG_FILE` |
| `--test` | `mmltk-test-YYYYMMDD-HHMMSS-PID.log` | `MMLTK_TEST_LOG_FILE` |
| `--tidy`, `--tidy-tools` | `mmltk-tidy-YYYYMMDD-HHMMSS-PID.log` | `MMLTK_TIDY_LOG_FILE` |

Relative overrides use the invocation directory and append if present; use new
filenames per run. Test list/help/`-h`/`--help` and tools-build help skip capture.
Startup text before operation selection is excluded. Transcripts enable no GUI
tracing, probes, or routine diagnostics. [Cold bootstrap](build.md#native-tools-and-checkout-bootstrap)
uses `mmltk-tools-bootstrap-YYYYMMDD-HHMMSS-PID.log`; its launched operation
captures separately. Bootstrap nested inside build/tidy uses the parent capture.

The caller keeps separate stdout/stderr; merging tees establishes no total
order. Ordinary exit drains both pipes before appending/printing:

```text
mmltk: test exited with status 0
```

Other labels are `build`, `build-tools`, `tidy` (both tidy routes), and
`build-rfdetr-oracle`. Capture/cleanup failure makes otherwise successful work
nonzero. Abrupt termination can leave no terminal; EOF proves no success.
Observed terminal lines normalize to `process.terminal`, `@exit_code`, `@terminal`.

```bash
MMLTK_TEST_LOG_FILE=build/logs/test-investigation.log ./mmltk --test core
./mmltk --logs build/logs/test-investigation.log --errors --format timeline
./mmltk --logs build/logs/test-investigation.log \
  -q '@event=process.terminal' --fields @event,@exit_code,@terminal --format timeline
```

Default log-query input is `build/validation`; select transcripts explicitly.
[Test replacement](validation.md#replacing-an-active-test-run) can explain a
cancelled run, whose terminal remains independent of its replacement.

## Fatal stderr reports

Native CLI, RF-DETR, ONNX, and desktop terminal failures use the common reporter:

```text
fatal: <component>: <detail> (status=<available status>)
```

Status is omitted when unavailable. Component/detail caps are 160/800 bytes
with truncation markers; control bytes become spaces in a fixed 1024-byte
buffer. Reporting preserves `errno` and attempts stderr with disabled,
uninitialized, or failed sinks. Broken pipes neither terminate through SIGPIPE
nor consume a previously pending signal. Enabled file sinks receive a best-effort
critical copy without duplicate stderr; RF-DETR/ONNX retain named identities.
Healthy execution/requested shutdown is quiet.

Owners preserve original errors: Firefox launch/infrastructure retains OS error
separately from mapped status; unsuccessful exit retains its code; unexpected
signal reports its number and maps desktop status to `128 + signal`. Requested
healthy SIGINT/SIGTERM is not failure.

[Logging interface](../src/common/logging/mmltk_logging.cppm) and
[implementation](../src/common/logging/mmltk_logging.cpp) own `report_fatal`.
It serves ordinary terminal boundaries and **must not run in signal handlers**.

Packaged Wayland diagnostics can additionally set
`--env MMLTK_RUN_WORKSPACE_WAYLAND_TERMINATE_TRACE=1`. The
[desktop entrypoint](../src/entrypoints/desktop/browser_runtime_entry.cpp) installs
the ordinary `std::terminate` handler only with integration enabled and native
logging at `trace`. It reports `native termination`, the active exception when
available, and at most 40 stack frames to stderr before aborting. This opt-in
handler is separate from signal reporting; frames alone establish no cause.

### Firefox fatal causes and process identity

Unrecoverable Firefox WebGPU/dispatcher/queue/completion boundaries report the
original OS/wgpu/Vulkan cause before aborting, independently of diagnostics:

```text
fatal: firefox.webgpu: operation=<name> pid=<pid> ppid=<ppid> pgid=<pgid> <available context>status=<original error>
```

Ordinary reporting uses a 2048-byte stack buffer, UTF-8-safe truncation,
control-byte replacement, ≤8 writes, preserved `errno`, and available
actor/device/queue/semaphore/submission. It is not a signal handler or trace opt-in.

Surviving `ContentParent` classifies unexpected channel loss using shutdown
intent: bounded 1024-byte `firefox.ipc` report, operation `content_channel_error`,
`child_pid`, `actor`, `channel=PContentParent`, `side=parent`,
`status=AbnormalShutdown`, and the PID/PPID/PGID envelope.
[FirefoxProcessOwner](../src/controller/services/firefox/firefox_process_owner.cpp)
classifies root exit/signal/intent. Child EOF during requested root SIGTERM
alone establishes no fatal cause.

Parser event `fatal.report` has level `fatal`, original text, opaque status,
and structured context only **before** `status=`. Error text cannot forge IDs.
`@process`, `@parent_process`, `@process_group` require exact positive Linux IDs;
process-owner child/stop uses observed child PID and spawn-established PGID.
Unknown zero IDs stay unknown. Automatic joins stay within one capture and
label exact-process, parent/child, and group relationships separately. Shared
parents do not join siblings; group membership attributes no crash cause or
parent terminal to a particular child.

```bash
./mmltk --logs --family latest-wayland-test --run ARCHIVE_ID \
  -q '@event=fatal.report OR @event=firefox.workspace.channel_terminal OR @owner=firefox_process' \
  --fields @event,operation,component,@process,@parent_process,@process_group,child_pid,status,@signal,@exit_code \
  --auto-correlate --format timeline --limit 40
```

## Local training failures

[TrainProcessClient](../src/controller/services/training/train_process_client.cpp)
scans consumed child output for line-start `fatal: ` beyond its 64 KiB console
retention. Current-line and retained-cause buffers are each 3072 bytes. It
finishes chunked/unterminated lines before terminal publication and preserves
specific worker causes over generic distributed-parent failures; progress and
allocator warnings do not select a cause.

A complete fatal publishes failed state immediately while the group retires;
Train/Status show it and Start remains unavailable. The first cause arms a
five-second cleanup deadline before surviving peers receive SIGKILL. Children
unblock inherited SIGINT/SIGTERM for ordinary peer termination. Cancellation
and retirement remain independent of diagnostics.

[Training runtime](../src/controller/subsystems/train/training_system.cpp)
projects decoded exit/signal and UTF-8-safe cause into `ComputeTerminal.detail`;
explicit CUDA OOM adds [batch/lane guidance](rfdetr-workflows.md#local-training-failures).
TrainingSystem owns one settled fatal stderr, including preparation failure;
standalone runtime retains its reporting responsibility.

| Enabled trace | Fields/boundary |
| --- | --- |
| `child.exited`, `child.signaled`, `training.failed` | Bounded batch/lane, output, device, available run/attempt |
| `training.worker_failed` | Each newly selected cause before retirement; `value`=process group, `document_resource`=output, ≤4096-byte cause/guidance |
| `training.progress.read.begin`, `.end` | Progress-file read/decode/validation/projection span; end duration and output identity; no disabled-trace clocks |
| `training.inputs` | Before dataset inspection: `sequence`=operation generation, `value`=captured settings revision, requested/prepared preset/resolution, split paths |
| `training.operation.failed`, `.settled` | Typed outcome, including pre-process failures; `sequence`=generation, `value`=snapshot revision |
| `training.preparation` | Startup stage before metrics, operation generation; native `preparation.json` changes only on transitions and is watched alongside metrics |
| `iced.notice.added`, `.updated`, `.dismissed` | Frontend notice ID, origin, content version, title/detail; independent of native settlement |

Input/operation messages are bounded to 1024 UTF-8 bytes; UI retains full bounded
terminal detail. Resolution errors identify file, actual geometry/channels,
and requirements. Preparation covers GPU connection, dataset admission,
checkpoint/model construction, GPU weight loading, synchronization, optimizer
setup. [Dataset identity](model-merging.md#whole-session-resume) uses metadata,
without payload scans or startup console parsing.

Distributed CLI forwards logging overrides. Worker filenames insert `.rank-N`
before the extension; absent explicit file, use `mmltk.rank-N.log` in selected
log/output directory. Disabled logging creates no worker files.

TrainingRecord format 3 projects lowercase `training.<role>.<phase>`.
Every `Terminal` is terminal evidence; `progress.phase="Error"` anchors failure.
Counters such as `class_error` do not. Triage prioritizes run/attempt, role,
sequence, phase, session/model, and first cause over configuration, follows exact
run/attempt, and excludes training records from generic suffix balancing.

```bash
./mmltk --logs output/train/RUN/metrics.jsonl --errors --format timeline
./mmltk --logs output/train/RUN/metrics.jsonl --triage
./mmltk --logs output/train/RUN/metrics.jsonl \
  -q '@model=2 AND (@phase=Merge OR @role=Terminal)' --format timeline
./mmltk --logs .mmltk-data/logs/gui-trace.jsonl .mmltk-data/logs/firefox.log \
  -q '@event=training.inputs OR @event=training.operation.failed OR @event=iced.notice.added' \
  --format timeline --fields @event,sequence,value,notice_id,origin,message
./mmltk --logs checkpoints/CLI_RUN/metrics.jsonl output/train/GUI_RUN/metrics.jsonl \
  -q '@event=training.boundary.starting' --fields attempt_configuration --format jsonl
./mmltk --logs output/train/GUI_RUN/metrics.jsonl \
  -q '@event=training.live.train' \
  --fields progress.completed_images,progress.images_per_second,progress.scalars \
  --tail --limit 10 --format jsonl
```

Replace `RUN` with the selected Output directory. Terminal/Error proves failed
training; owning terminal/fatal stderr establishes cause. GPU counters are
[saved metrics](rfdetr-workflows.md#saved-history-and-plots), not tracing.
Compare encoder freezing, DN, batch/GPU, loading, and augmentation before
attributing throughput to rendering. `images_per_second` is an epoch average;
different startup/work makes isolated values incomparable.

### Training session comparison

For the [full-session fixtures](validation.md#training-mathematics-and-compilation-evidence),
`--env MMLTK_TEST_SESSION_CONTINUATION_TRACE=1` enables stderr JSONL with
`event: "test.training_session_difference"` for differing compatible tensors.
Records contain `tensor`, `maximum_difference`, `reference_maximum`,
`l2_difference`, `reference_l2`, and `maximum_bound_ratio`. The last field divides
elementwise error by the family comparison's FP32 bound. Collection is opt-in;
the validation reference owns comparison policies.

## Capture one reproduction

```bash
mkdir -p .mmltk-data/logs
MMLTK_GUI_TRACE_FILE=.mmltk-data/logs/gui-trace.jsonl \
MMLTK_FIREFOX_LOG_FILE=.mmltk-data/logs/firefox.log \
./mmltk --gui
```

Native tracing covers command/transport/worker/compiler/presentation/cleanup/
failure JSONL and **truncates** the chosen path. Preserve prior captures.
Firefox output needs a separate path; both parents must exist and be runtime-user
writable. Add `MMLTK_GUI_PIXEL_TRACE=1` with the native trace path for probes;
lifecycle traces alone prove no pixels. [Normalized IDs](#fields-and-correlation)
join native high/low and browser 32-hex identities within distinct source/arena
namespaces.

Enabled Iced probes reuse small compute/readback storage, hold the exact sample
slot, preserve shader-read layout, and create no full-image capture texture.
Ordinary display has no probe owner. Frame probes sample the same 5×5 Cartesian
points bounded by content; Explore cards use 5%, 35%, 50%, 65%, 95% on each axis
plus semantic/padding checks, retaining clean/semantic/reference RGBA and actual
integer coordinates. These explicit readbacks are separate from GPU display;
native kernel probes and direct CUDA samples are independent. Optional Firefox
direct-image probes require completion and valid markers; undefined alias
samples prove no contents. [GPU tracing](gpu-execution.md) owns other variables.

## Native selective compilation

Native `debug` logging enables [tensor-trace diagnostics](../src/backend/models/rfdetr/core/tensor_trace.cpp)
for Selective/FullTrace, as `rfdetr compilation` messages separate from GUI JSONL:

```bash
./mmltk --logs .mmltk-data/logs/rfdetr.log \
  -q '"rfdetr compilation"' \
  --fields region,owner,reason,training,signature,grad,amp,amp_cache,amp_dtype,torch,graph_available,operators,fusion_groups,tensor_expr_groups \
  --format jsonl --limit 40
```

| Fields | Meaning |
| --- | --- |
| `region`, `owner` | Backbone, decoder tail, segmentation spatial/query, or full-model region; process-local instance |
| `preparation`, `reason` | Armed/disabled/identical request; record/reuse, signature/prepared-batch fallback, owner/attention-policy invalidation |
| `signature`, `training`, `grad`, `amp`, `amp_cache`, `amp_dtype` | Actual invocation, including autocast cache and numeric dtype |
| `torch` | Compiled version macro; [payload manifest](../docker/nvidia-payload.json) owns full build identity |
| `executor_optimize`, `fuse_cpu`, `fuse_gpu`, `tf32_matmul`, `tf32_cudnn` | Runtime policy |
| `graph_available`, `operators` | Inspected graph |
| `fusion_groups`, `tensor_expr_groups` | Recursive FusionGroup/CudaFusionGroup/TensorExprGroup total; last form separately |

Reuse inspects the last optimized executed graph when available. Disabled debug
performs no graph inspection/formatting. Capture/reuse, fuser availability,
graph fusion, and fused ATen (e.g. SDPA) differ. None alone proves a training
speedup over upstream Dynamo/Inductor; [validation](validation.md#training-mathematics-and-compilation-evidence)
defines evidence boundaries.

## ONNX session release

Native `trace` logging enables `event=onnx.runtime.release` messages from the
[ONNX backend](../src/backend/ml/runtime/backend_onnx.cpp), separately from GUI
JSONL. `phase=before_session` precedes binding/session release;
`phase=after_session` follows successful stream settlement and pinned-allocator
release. Fields are `stream`, CUDA `status` from `cudaStreamQuery`, and
`last_error` from `cudaPeekAtLastError`. Disabled trace performs neither query.
These observations diagnose the [release order](gpu-execution.md#onnx-capture-and-verification-storage)
and do not replace the returned close status.

## Upstream numerical comparisons

[Upstream validation](validation.md#independent-upstream-diagnostic) retains
`wrapper.log`, native executable logs, `manifest.json`, authenticated archives,
and `comparisons/*.jsonl`. Only successful combined required comparisons publish
`verdict.json`; reference preparation alone proves no native parity.

| Field | Meaning |
| --- | --- |
| `boundary` | Tensor/identity/inventory scope |
| `policy` record | Precision, backend, executable SHA-256 |
| `run_unix_ns` | Appended retry identity; filter/group to separate attempts |
| `max_absolute`, `l2_error`, `expected_l2`, `normalized_error` | Raw error and reference scale, alongside declared rtol/atol |
| `parameter_quantization_max`, `unadjusted_normalized_error` | Update-delta interval and original error |
| `measurement_only`, `policy_difference`, `first_difference` | Interpret `passed=false` observations under [required gates](validation.md#numerical-gates-and-trajectory-observations) |

Inventory/dtype/shape/definedness/nonfinite failures fail even measurement-only
trajectories. Complete-vector derivative gates retain individual tensor facts
and establish no trajectory equality. Keep reports with execution ledger and
terminal; preserve failed attempts separately from completed checks.

```bash
./mmltk --logs 'build/validation/rfdetr-math/upstream/comparisons/*.jsonl' \
  -q 'boundary="vjp/complete-vector"' \
  --fields @file,run_unix_ns,boundary,max_absolute,l2_error,expected_l2,passed \
  --format jsonl --limit 100
./mmltk --logs 'build/validation/rfdetr-math/upstream/comparisons/*.jsonl' \
  -q 'first_difference=true' \
  --fields @file,run_unix_ns,boundary,measurement_only,policy_difference,normalized_error \
  --format jsonl --limit 40
```

Use substring `:` or regex `~`, e.g. `boundary:"held/loss"`; `contains` is invalid.

## Benchmark compilation traces

The [dataset runtime's owned target](architecture.md#native-domain-work) connects
GUI compilation to the shared diagnostic sink. No pixel-trace flag is needed for
benchmark records. CLI benchmark compilation supplies its trace callback
when native logging is enabled at `trace`, for example with `--log-level trace`.

GUI records use `kind: "benchmark_dataset"`, a top-level `name`, nested `fields`,
and `steady_ns` from the shared runtime sink. The query tool normalizes `name`
to `@event`; fields can be queried directly. Select one capture so repeated
artifact names and monotonic timestamps do not join different runs.
The opt-in [data profiler](validation.md#data-compilation-and-resampling-profiles)
uses `event`, `steady_ns`, `run_id`, and nested `fields` in its separate
append-only JSONL file. Its repeatable `--trace-prefix` selects event prefixes
and flushes selected records. One immutable
[trace selection](../src/backend/data/compile_trace_selection.h) follows both
native JSON and serialized public delivery. Producers check selection before
collecting event-only clocks, snapshots, hashes, or fields. A selected aggregate
still collects the counters and timings needed for that result: selecting only
`compile.execution.retired` retains cumulative costs without transient execution
snapshots or rejected-reservation ancestry. Selecting a completed span retains
its start clock even when its start event is excluded. With no prefixes, the
sink selects the full trace; with no sink, diagnostics are inactive. Omit
`--trace-file` for untraced throughput, and compare off, selected, and full
capture as separate conditions.

| Events or fields | Observed work |
| --- | --- |
| `artifact.compile.failed` | Artifact-service `operation` (`compilation`, `staged_inspection`, or `final_publication`), `staged_path`, `final_path`, and original `error`; staging is empty when source/output checks or staging creation fail |
| `benchmark.compile.paths` | Resolved `cache_root` and compiler `output_root`, with separate `*_truncated` and `*_utf8_replaced` diagnostic flags |
| `benchmark.progress.activity` | Current `activity`, numeric `phase`, and optional numeric `source` |
| `benchmark.progress.delivery` | Opt-in `produced`, `materialized`, `coalesced`, `delivered`, `pending`, and `high_water` counts from the bounded progress observer |
| `compile.execution`, `.retired` | Shared class/CPU admission, parked/ready/running records, normal and exceptional pinned storage, descriptor promises, completed work, nested `arena`/`costs`, and blocked `demands`; retirement follows worker join |
| `compile.pixel` | Shared Directory/benchmark pixel operation: codec/path, source/target extents, encoded/requested/opaque/output bytes, validation/preparation/decode/destination/resize-store wall time, thread CPU time, and failure stage |
| `file.service.started`, `.stopped`, `file.admitted`, `.wait_records`, `.wait_cpu`, `.running`, `.physical_completed`, `.completed` | Bounded file-service records, class grants, physical calls, and callback settlement; `service` distinguishes `image` from `output` |
| `benchmark.image.stage_started`, `.stage_completed` | `root`, `image_id`, `generation`, `stage` (`header`, `workspace`, `input`, `output`, `pixels`), `workspace_bytes`, `encoded_bytes`, `duration_ns`, and `failed` |
| `benchmark.pipeline.ready` | Per-stage ready counts, `pending_images`, `image_head_probes`, `image_admitted`, `workspace_resource_denied`, `workspace_probe_limit`, `label_outputs`, `stage_records_blocked`, `label_file_records_blocked`, `empty_dispatch`, `lock_wait_ns`, and `body_ns` |
| `benchmark.pipeline.retired` | Aggregate dispatch turns/probes/yields, dispatch wall/CPU time, and stage-ordered call, wall/CPU, fault, residence, and retained-byte observations |
| `benchmark.download.cache_hit`, `.preseeded` | Retained archive `artifact`, `bytes`, and admission `integrity` |
| `benchmark.download.progress` | `artifact`, `completed_bytes`, `total_bytes`, `attempt`, `resumed`, `retained_bytes`, `durable_bytes`, and `redownload` |
| `benchmark.download.segmented_resume_state`, `.partial_checkpoint` | Retained range state or ordinary-transfer checkpoint and resume eligibility |
| `benchmark.download.attempt_failed`, `.segment_retry`, `.segmented_fallback` | HTTP/CURL failure, discarded partial state, or fallback context where available |
| `benchmark.images.cache_scan`, `.cache_reuse`, `.progress` | `source`/`shard`, inspected/reused/resolved images, and selection counts |
| `benchmark.images.validation_failed` | Rejected archive `member`, `image_id`, `source`/`shard`, encoded `bytes`, and `reason` |
| `benchmark.images.buffer_wait`, `.input_wait`, `.file_wait`, `.pixel_wait`, `.input_released`, `.decoder_admission`, `.input_pressure`, `.pressure` | Encoded-publication capacity, decoder/input custody, and physical pressure context; a pressure event can carry a complete execution snapshot |
| `benchmark.archive.scan`, `.extracted`, `.extract_cache_hit` | Archive traversal or annotation-member extraction/reuse |
| `benchmark.archive.suspended`, `.completed` | Decoder opens/suspensions/restores, forward/replayed cursor bytes, copied bytes, skipped headers, bounded read turns/blocks, and CPU time |
| `benchmark.archive.admission_wait` | Source admission snapshot: `bytes`, `descriptors`, `committed_descriptors`, `descriptor_capacity`, `producer_ceiling`, and `active_cpus`; `reason` is `source_admission` |
| `benchmark.resource.wait` | First blocked reservation snapshot: requested/held bytes and descriptors, continuation commitments, limits, source location, parent chain, and live owner locations |
| `benchmark.annotations.originals_wait`, `.originals_begin` | Entry to stock-original lifecycle admission, then admitted work with `recover_dropped_masks` and numeric `validation` |
| `benchmark.annotations.inputs_wait`, `.inputs_acquired`, `.inputs_reused` | COCONut `release`/`part` input custody: waiting for a new physical lease, acquiring it, or reusing the same still-live lease |
| `benchmark.coconut.original_subscribed`, `.original_notified` | One physical `source`/original `generation` subscription and its delivery to currently live `rows` |
| `benchmark.coconut.projection_opened`, `.projected_batch`, `.projection_parked`, `.projection_replay` | Parquet projection openings and physical `replayed_rows`, bounded batch/staging custody, window admission waits, and exceptional contiguous-fit replay; `future_window_bytes` reports retained capacity for forward reads |
| `benchmark.coconut.metadata_readiness` | One retained metadata inspection per Parquet `artifact`/`group` initialization, with `inspected_rows` and `ready` |
| `benchmark.coconut.recovery_observed`, `.recovery_parked`, `.recovery_ready`, `.recovery_settled`, `.recovery_retired` | Row-owned recovery lifecycle with available `image_id`, source ordinal, namespace, and generation context |
| `benchmark.annotations.release_metadata`, `.release_complete` | COCONut release `edition`, with `cache_hit` when available, at metadata and settled component completion; source-local omissions can leave a healthy subset |
| `benchmark.annotations.preparation` | COCONut `edition`, preparation `stage`, `completed`, `total` (zero means unknown), and `unit` (`rows` or `archive entries`), independently of normalized-row completion |
| `benchmark.annotations.progress` | COCONut normalized `completed_rows` and `total_rows` by `edition`, including the first completed row |
| `benchmark.archive.image_ready` | Independent physical source `artifact`, encountered `image_id`, and `entries` containing its completed-image observation; the zero-image record also marks source invalidation |
| `benchmark.annotations.mask_discovery` | XL annotation-archive `edition` and actual inspected `entries` |
| `benchmark.annotations.cache_row` | A reused component row published independently: `edition`, physical `source`, `image_id`, and component `position` |
| `benchmark.annotations.component_admitted` | Reused COCONut component `edition`, physical `source`, and `images` |
| `benchmark.annotations.release_cache_selected` | Immediately admitted final release-cache reuse, with `edition` |
| `benchmark.coconut.native_staged`, `.native_finalized` | Native staging/copy bytes and final unchanged-payload reuse or recovered-object counts, with image identity |
| `benchmark.annotations.assembled`, `.workspace` | Assembled image/box/run counts and retained vector capacities, or a parser lane's `retained_sparse_bytes` |
| `benchmark.annotations.gather_started`, `.gather_batch`, `.gathered` | Gathered `images`, `boxes`, `runs`, `validated_runs`, and `copied_bytes`; final `tasks`, `max_images`, `max_boxes`, and `max_runs` describe bounded grouping; proven immutable rows need no second run-admission scan |
| `benchmark.annotations.cache_prepare`, `.cache_synced` | Cache destination `path`, `bytes`, and `same_backing`; preparation also reports `payload_copy_bytes` |
| `benchmark.labels.projection_started`, `.slot_written` | Projection entry with `root`, `image_id`, and retained lane workspace bytes, or successful direct label-slot write with `root` and `image_id` |
| `benchmark.labels.placed` | COCONut's successful per-image label-slot placement: `split` and `image_id`; this is not a final split seal |
| `benchmark.images.archive_retry`, `benchmark.pixel_compile.cache_repair` | Bounded image/archive repair context |
| `benchmark.download.failure_sha256`, `benchmark.images.failure_sha256` | Failure digest and cause for an artifact or image; COCONut annotation repair additionally reports `matches_expected` |
| `benchmark.pixel_compile.throughput`, `.complete` | Completed/total images where available, `split`, elapsed seconds, images/second, and ETA observations |
| `benchmark.storage.preflight`, `.reserved`, `.capacity_failed`, `.projection` | Capacity transaction or projected output bounds; transactions carry `required_bytes`, `reserved_bytes`, `available_bytes`, `capacity_samples`, `retries`, `duration_ns`, and `failed` |
| `benchmark.cost.started`, `.completed` | Final/control work spans: numeric `kind`, `span_id`, `duration_ns`, `failed`, and scope fields such as release, input identity, rows revisited, selected images, settlement batches, split, and bytes |
| `benchmark.compaction.started`, `.completed`, `benchmark.split_sync.started`, `.completed` | Split compaction or physical sync boundaries with `split` and `bytes` |
| `benchmark.compaction.summary`, `compile.compaction.summary`, `profile.compaction.summary` | The shared compactor's live/moved/stationary bytes, copy requests and syscalls, mutation batches/ranges/syscalls, durations, scratch high-water, capacity fallbacks, and reclaim support; event prefix identifies benchmark, Directory profile, or standalone compaction profile |
| `profile.pipeline.completed`, `profile.archive.completed`, `profile.resize.completed` | Bounded fixture results, optional image-completion gaps and annotation work, archive custody/byte checks, or resize preparation/allocation requests; [profile modes](validation.md#data-compilation-and-resampling-profiles) define scope |
| `benchmark.directory_sync.started`, `.completed`, `benchmark.publication.started`, `.complete` | Directory sync and final atomic publication boundaries with `path` or `output` |
| `benchmark.publication.old_generation_cleanup` | Best-effort removal of the exchanged old generation: `path`, `removed`, and `error`; cleanup failure does not revoke committed publication |

`artifact.compile.failed` uses the same optional runtime transport for Directory
and benchmark requests. The artifact service inspects the staged split files,
then publishes and returns final split paths. Its failure record preserves the
original cause and both locations. Paths and JSON are formatted only when the
observer is installed. Diagnostic delivery cannot change the operation result.

For a [GUI capture](#capture-one-reproduction):

```bash
./mmltk --logs .mmltk-data/logs/gui-trace.jsonl \
  -q '@event=artifact.compile.failed OR @event=benchmark.compile.paths' --format jsonl --limit 20
./mmltk --logs .mmltk-data/logs/gui-trace.jsonl \
  -q '@event:benchmark.download' --where 'artifact="objects365-v2-train-patch-17"' \
  --fields @event,artifact,completed_bytes,total_bytes,attempt,resumed,retained_bytes,durable_bytes,redownload \
  --format jsonl --limit 60
./mmltk --logs .mmltk-data/logs/gui-trace.jsonl \
  -q '@event=compile.execution OR @event=benchmark.resource.wait OR @event=benchmark.progress.delivery' \
  --format jsonl --limit 20
```

Use exact catalog event names, `--where` source/shard/image identity, and
`--fields` for bounded projections; query stage/input/file/annotation events
together to follow independent progress.

[Compilation progress](benchmark-datasets.md#reading-compilation-progress) defines
actual-write bytes, unknown totals, retry withdrawals, independent tracks,
source aggregation, and compaction's live-byte progress. Release/pixel/acquisition
records can interleave; phase names are not exclusive execution states.
These observations prove neither performance
gains nor, from unchanged fractions or absent best-effort records, deadlock or
network liveness.

Admission snapshots describe the shared ledger at an observation point, not
process RSS, filesystem-wide reservations, or a deadlock diagnosis. The
[resource reference](compile-execution.md#fixed-pinned-arenas)
distinguishes transient workspace from retained canonical products.
`benchmark.resource.wait` is emitted once per blocked reservation with tracing
enabled. Its `parents` chain records available and borrowed continuation
capacity; `owners` records live allowances and their admission locations.
Owner `id` and `parent` values are process-local addresses for that snapshot,
and may be reused after retirement. The record is not a heartbeat: subsequent
ledger changes during the same wait need not produce another snapshot. Disabled
tracing does not collect this owner inventory.

`compile.execution` uses canonical class order Acquisition, Extraction,
Annotations, Pixels, Gather, Output for `live`, `baseline`, `ready`, and
`running`; `parked` uses None, Dependency, Bytes, Descriptors, Cpu, Withdrawal,
Continuation, Fairness. Task-indexed arrays use Acquisition, Metadata, Header, Projection,
Archive, Normalize, Recovery, Labels, Pixels, Gather, CacheWrite, Output.
`external` and `services` describe grants within the same CPU budget.
`normal_locked` is fixed normal backing; `resident` adds physically materialized
exceptional backing, while `leased` counts normal slices currently borrowed.
These are arena quantities, not measured process RSS. `exceptional_reserved`
can precede materialization; `exceptional_peak` records the observed normal-plus-
exceptional high-water commitment. `opaque_library` is the separate library
charge. `protected_workspace`, `shared_workspace`, and `compaction_workspace`
expose protected preparation; the compiler's separate preparation event includes
`workspace_contexts` and `context_capacity`, which are planner quantities rather
than a limit on live task-sized allocations.
`workspace_reuses` and `physical_retirements` are enabled-observer counters.

The nested `arena` projection comes from
[CompileArenaObservation](../src/backend/data/detail/compile_arena_observation.h).
It distinguishes requested and alignment-rounded occupied bytes, alignment
loss, live allocations, ordinary capacity/free bytes/largest free extent,
protected-window occupancy, physical backing regions, unregistered bytes, and
allocation/occupied high-water marks. `largest_ordinary_capacity` describes
normal shape fit; `largest_ordinary_extent` describes current contiguous room.
A small free extent does not establish an oversized input.

`costs` contains eligibility probes, notifier batches/deliveries, eventfd writes,
snapshots built, submission attempts, record denials, external drain candidates,
resource epoch, admission-lock wait time, readiness/completion callback time,
worker notifications, selection attempts/empty selections, and worker
creation/retirement. A wake or empty selection is not a newly created worker.
`demands` projects the canonical request and its denial, observed resource epoch,
and largest eligible extent.
Denial values follow
[CompileResourceDenial](../src/backend/data/detail/compile_resources.h);
`AgedDemand` is fairness, not a reclamation request. The
[readiness rules](compile-execution.md#selective-readiness-and-resource-pressure)
govern retries with diagnostics disabled too.

`task_duration_ns` aggregates the physical task call through frame settlement,
before completion callbacks; `task_body_ns` measures the work body and
`task_cpu_ns` uses that thread's CPU clock. `ready_to_start_ns` accumulates
queued-to-start delay. Pixel `cpu_ns` covers its validation through final
store, while `duration_ns` and its stage fields measure wall time. CPU clock
failure sets `cpu_incomplete` or `costs.incomplete`. Pixel
`resize_preparations`, `resize_allocation_requests`, and
`resize_allocation_bytes` count execution work/requests, not simultaneously live
storage. The profiler reports separate `layout_*` preparation/allocation counts.
Pixel observations distinguish requested RGB `method` from the selected
execution `path`; `opaque_bytes` is the admitted library-storage charge,
including Torch resize temporaries, rather than a measured allocator event.
Concurrent tasks, nested callbacks, and spans overlap: summed worker wall time
or CPU time is not elapsed compile time and must not be added to it.

`benchmark.pipeline.ready` also reports `probes`, `probe_quantum`, `yielded`,
and `body_cpu_ns`. The retired pipeline's stage arrays follow Header, Workspace,
Input, Output, Pixels. Stage residence and retained-byte peaks describe this
pipeline's admitted custody, not process RSS. Archive forward/replay source and
decoded bytes are decoder cursor observations, not physical disk-read traffic.
Compaction `*_calls`/`*_ranges` count wrapper requests; `*_syscalls` count actual
I/O attempts, including retries and excluding ledger capacity probes. The
compaction summary excludes the writer's later metadata and sync boundaries.

`BenchmarkCostKind` order is PhysicalWait, RecoveryDrain, ComponentGather,
InventorySeal, Selection, Compaction. Selection reports `settled_images` and
`settlement_batches`; compaction reports its split and logical extent.
`span_id` is an effect-only address that can be reused, so pair starts and ends
within the same run and interval. Storage `capacity_samples` is one for an
initial/growing transaction and zero for a shrinking promise; current capacity
transactions do not retry. With no sample, `available_bytes=0` is not a
measurement of full storage. Growth compares against sibling outstanding
promises; successful initial/shrink observations report the committed aggregate.

File records expose class-ordered `queued`, `running`, and `completing` arrays,
plus `records`, `capacity`, `services`, `busy`, `admitted`, `completed`,
`operation`, `bytes`, `duration_ns`, `class`, and `failed`. Physical/completed
events and service-stop aggregates include queue delay, thread CPU, callback
time, minor/major faults, and cumulative `total_*` quantities. The physical call
returns its shared CPU grant before its completion callback releases record
and borrowed-input custody: `file.physical_completed` and `file.completed`
therefore describe different boundaries. Service capacity and retained
[continuation ownership](compile-execution.md#owners-and-ready-work) remain
product facts with tracing disabled.

Source progress facts remain exact while ordinary observations coalesce on the
[dirty-state drainer](compile-execution.md#progress-delivery). Delivery counts
measure observation work, not completed images. Tracing a source activity can
retain a decision that no visible snapshot sampled; do not require one UI update
per producer callback or infer scheduling from these effect-only identities.

Metadata, originals, normalization, labels, and pixels can progress separately;
`release_complete` is not a prerequisite for every image's label conversion.
Preparation and physical-source observations also update visible source activity.
Their row counts, completed-image counts, and archive cursors describe different
work; they are not interchangeable normalized-row or final eligible-image
counts. Slot placement can precede JPEG arrival, canonical ordinal settlement,
and final membership. Compaction, sync, and `benchmark.publication.complete`
remain separate from those per-image records. Work-based updates use bounded
observation points, and the UI can coalesce intermediate observations.

Failure hashing follows [cache repair policy](benchmark-datasets.md#persistent-cache-and-publication).
Pinned annotation checksums used to decide replacement remain required without
tracing. Optional diagnostic hashes are collected only when their event is selected;
enabling logs does not authorize cache reuse or repair. `failed.txt` is a
separate append-only [omission/provenance report](benchmark-datasets.md#native-import-and-provenance),
not a trace or current recovery-count ledger. Healthy images with incomplete
source-ordinal provenance can appear in that report without being omitted;
inspect its reason and the component manifest's `incomplete_ordinal_images`.

For fixture output, select its actual wrapper transcript and query
`benchmark_label_fixture`, `benchmark_pipeline_fixture`, `compiled_stream_fixture`,
`compiled_used_read_fixture`, `compiled_compaction_fixture`, or `native_png_fixture`
with `--fields @text --format jsonl`. Output-vector/prepared-buffer growth counts
cover only those owners, not all allocation, mapped bytes, or process RSS.

Transfer observers exist only for progress/tracing; unobserved segmented downloads
skip progress locking and clock reads. Trace construction/serialization/callback
failures are contained, with one delivery attempt. Invalid payloads go to
diagnostic failure handling, never successful empty records or compilation
failure. Complete-delivery acceptance may independently fail its evidence gate.

## Compiled Explore opening

GUI tracing includes `explore.dataset.open.started`/`.completed` and
`explore.dataset.summaries.started`/`.completed`, using ordinary span duration/
outcome. `explore.dataset.admission` reports summarized slots in `value`, total
in `detail`; callbacks may be out of order. Opening cancellation covers index
copy/count aggregation before summaries. No record implies payload/per-sample
verification. Disabled tracing installs no observer/clocks.

```bash
./mmltk --logs .mmltk-data/logs/gui-trace.jsonl \
  -q '@event:explore.dataset OR @event:shutdown.explore_join' --format timeline --limit 80
```

For isolated open timings use [compiled diagnostics](commands.md#benchmark-image-geometry)
`--profile-compiled WORKERS`, retaining cache and placement conditions.

## Delivery and acceptance ownership

Ordinary enabled diagnostics use a best-effort background writer: 256 queued
records, ≤16 KiB each; contention/capacity can drop records. Disabled execution
creates no active state. Explicit Wayland integration with a lifecycle sink
selects complete delivery on the same bounded writer, can wait for capacity,
fails acceptance on encoding/delivery failure, and drains at shutdown. Sources:
[client](../src/controller/services/diagnostics/diagnostics_client.h),
[runtime](../src/controller/services/diagnostics/runtime_diagnostics.cpp),
[startup](../src/entrypoints/desktop/browser_runtime_entry.cpp).

| Enabled transport record | Meaning |
| --- | --- |
| `browser.record.serialized` | `value`=encoded bytes, `detail`=priority |
| `browser.server.record_coalesced`, `.stale_state_discarded`, `.queue_high_water` | Replacement/discard accumulation and peak pending occupancy since last diagnostic flush |
| `iced.transport.produced`, `.coalesced`, `.stale`, `.enqueued`, `.delivered` | Surface-trace source/retention/delivery; queued occupancy and 64-record capacity |

Coalescing preserves discrete events; [Dataset delivery](application-wire.md#dataset-operation-snapshots)
owns product behavior. Materialization, encoding, retention, and delivery differ.

Quiet acceptance retains real control receipts/input pressure/settlement/exact
drawn readiness, independent of [Rust](../src/frontend/iced/src/integration_control/reporting.rs)
and [JavaScript](../src/frontend/iced/src/integration_control/browser.mjs) reporting.
Disabled reporting performs no payload callbacks, phase/revision/style dedup,
passive viewport/gallery scans, widget measurement, or sink work. Failed receipts
still retain static source context and ≤4096 UTF-8-safe detail bytes with native
kind/title/body. Successful receipts have no failure payload; inactive drivers
collect none.

Each `build/validation` artifact has one writer:

| Default family member | Writer/content |
| --- | --- |
| `latest-wayland-test.jsonl` | Native lifecycle |
| `latest-wayland-test-acceptance.jsonl` | Harness decisions, blockers, terminal |
| `latest-wayland-test-native.log` | Host stdout/stderr |
| `latest-wayland-test-application.log` | Native logger |
| `latest-wayland-test-firefox.log` | Firefox/browser evidence |
| `latest-wayland-test-mozilla-…moz_log` | Explicit Mozilla modules, process/child/rotation identities |

Aliased destinations are rejected; preceding families rotate to `.history`.
[Wrapper transcript](#wrapper-build-test-and-tidy-transcripts) and [headless supervisor](headless-wayland.md)
are separately owned evidence. [Select the scenario archive](#select-captures-and-histories)
after rotation. Enabled acceptance requires complete physical/rendered chains;
missing-record exceptions or generic query hypotheses cannot prove handoffs.

## Physical presentation evidence

```bash
./mmltk --logs --family latest-wayland-test -q '@event=presentation.frame.edge' \
  --correlate @workspace_source+transfer_sequence --format timeline --limit 80
./mmltk --logs --family latest-wayland-test -q '@event=acceptance.physical_inventory' --format jsonl
./mmltk --logs --family latest-wayland-test -q '@event=iced.surface.draw_submitted' \
  --correlate @surface+draw_identity --format timeline --limit 60
```

The harness independently joins native source admission/allocation, acquisition,
ready/release, Firefox forwarding/child dispatch, and physical settlement. Source
and arena differ. Retry of one publication can have a new `transfer_sequence`;
source+transfer identifies physical attempt. Unacquired offers own no read.
Acquired direct mode requires `firefox.workspace.read_settled`; copy mode requires
`firefox.workspace.copy_completed`, each after exact forwarding/dispatch.

Diagnostic `draw_identity` joins selection/acquisition → `draw_encoded` →
`draw_submitted` (after actual queue call) → `draw_settled`/`draw_abandoned`.
Exact IDs allow out-of-order settlement. Product holds/timelines govern release
with diagnostics disabled; drawing additionally needs paired image/pixels.
Independent drains retain incomplete joins through retirement; final missing
evidence fails acceptance.

Canvas signal events are separate from image reads: `firefox.canvas.signal_created`,
`.signal_cancelled`, `.signal_handle_released`, `.signal_batch_submitted`,
`.signal_submitted`, `.signal_completed`, `.signal_retired`. Available fields:
`actor`, `queue`, `vk_device`, `vk_queue`, `vk_semaphore`, `submission`, `count`,
`signal_count`, `batch_count`, `queue_submit_calls`, `age_ns`. Handle release is
not completion; completed follows exact fence. Timeline `batch_count=2` may have
`queue_submit_calls=1`; [submission custody](gpu-execution.md#browser-canvas-descriptors-and-submission-custody)
owns that path.

| `acceptance.physical_inventory` | Meaning |
| --- | --- |
| `live_workspaces`, `workspace_bytes` | Nonretired source allocations/bytes |
| `live_sample_arenas`, `live_sample_slots` | Copy arenas/physical capacity; direct wrappers separate |
| `completed_browser_copies`, `settled_direct_reads` | Fully joined mode-specific completion |
| `settled_release_only_reads` | Acquired reads returned without display |
| `unacquired_offers` | Offered without acquired/releasing transfer |
| `encoded_draws`, `settled_draws`, `abandoned_draws` | Encoding and terminal custody |
| `final_reader_releases` | Samples returned after final hold |
| `explore_peak_pinned_bytes` | Observed Explore staging high-water |

These lifetime/inventory counts measure neither latency/throughput nor every
internal transfer. Workspace/raw pools/sample storage differ. A complete chain
proves its path; absent best-effort events prove no zero-copy claim.

Typed acceptance-only undersized-candidate holds prove retained fallback before
release, independent of product acknowledgment/cadence. Empty-gallery acceptance
waits for paired zero-match atlas draw before consuming notice. [FPS](gui-interaction.md#browser-redraws-and-fps)
is a separate opt-in product meter; its submissions do not replace custody or
the separately activated async canvas probe.

## Workflow GPU evidence

Enabled native `workflow.gpu_execution` records describe the actual
Validate/Predict/Export runtime device, or each launched Train rank. `participant`
names the workflow, `sequence` carries its admitted generation, `device` is the
CUDA ordinal, `value` is rank, and `detail` is rank count. These are effect-only
observations; neither logging nor a diagnostic identity owns execution or
resource lifetime.

The packaged workflow driver emits `integration.workflow_gpu_selected` after
ordinary settings settlement, `integration.workflow_gpu_layout` for the shared
Output/GPU bounds, and `integration.workflow.operation_admitted` from
native-backed operation snapshots. The independent audit joins admitted
generations and actual execution in both directions, including cancellation
before progress. All four cards have light-layout evidence; Train additionally
has dark and narrow layout evidence. Native/controller fixtures separately
cover ordered multi-rank selection and missing devices.

```bash
./mmltk --logs --family latest-wayland-test --run ARCHIVE_ID \
  -q '@event=workflow.gpu_execution OR @event:integration.workflow_gpu_ OR @event=integration.workflow.operation_admitted' \
  --format timeline --limit 60
```

Select the workflow capture with [archive discovery](#select-captures-and-histories).
CUDA ordinals and graphics/NVML indices have distinct meaning; the
[GPU reference](gpu-execution.md#workflow-device-selection) owns that boundary.

## Rendered UI acceptance evidence

The opt-in [browser driver](../src/frontend/iced/src/integration_control.rs),
its [JavaScript adapter](../src/frontend/iced/src/integration_control/browser.mjs),
and the independent [browser-evidence audit](../src/acceptance/tests/wayland/browser_audit.cpp)
record UI interaction and pixels separately from physical resource custody:

| Records | Evidence |
| --- | --- |
| `integration.rgb_resampling` | Rendered Dataset/Predict resampling selections and settled native settings; pixel equations are covered by the independent resize fixtures |
| Dataset selection, draw, caption, pixel, and cancellation records | [Dataset presentation evidence](#dataset-presentation-evidence) below |
| `integration.status.notice`, `integration.status.stage` | Stable notice add/remove identity, bounded population, and completed fixture stages |
| `integration.status.hover`, `integration.status.interaction`, `integration.status.latch` | Rendered hover boundary, explicit activation, and Escape/reentry behavior |
| `integration.status.modal`, `integration.status.focus`, `integration.status.clipboard` | Modal availability, focus/input ownership, and exact clipboard result |
| `integration.status.selection`, `integration.status.selection_readback` | Selectable complete text and actual selection/readback evidence |
| `integration.status.exercise`, `integration.status.pulse`, `integration.status.visibility` | Rendered input exercise, visible pulse/reduced motion, and hidden-window quietness |
| `integration.atlas_resize_measured`, `integration.atlas_resize` | Gallery measurements retained beneath Detail, then required/actual rows after each completed resized return |
| `integration.atlas_ready_cell`, `integration.atlas_canvas_sample` | Exact drawn gallery identity, compiled image, selected canvas coordinates, patch counts, and sampled color |
| `integration.explore_integer`, `integration.explore_integer_paste_baseline`, `integration.explore_integer_paste`, `integration.explore_integer_paste_restored` | Exact decimal integer values, native revision progression, typing/paste persistence, and restoration |
| `integration.number_replace`, `integration.number_paste`, `integration.number_key_stage` | Synthetic focus, selection, modifier, and key-delivery stages; delivery alone does not prove native persistence |
| `integration.annotation_layout`, `integration.annotation_reachable`, `integration.annotation_tail` | Shared columns, wide/narrow viewport behavior, fully revealed controls, and long-list final entries |
| `integration.annotation_pixel` | Native geometry/palette expectation and actual canvas pixel at the current image scale |
| `integration.workflow.completed` | Typed Train, dashboard interaction/aspect, Validate, compiled/image/video Predict, Stop, theme, and narrow-layout stage completion |
| `integration.workflow.pixels` | Actual sampled/visible canvas pixel counts for charts, the live progress bar, validation atlas/detail, direct Validate-to-Explore return, and prediction stages; image captures retain the expected source/presentation revisions |
| `integration.workflow.progress` | Native completed/total image counts and epoch facts at the live progress capture |
| `integration.keypoint.catalog` | Preset in `control`, prepared/completed stage in `detail`; `a`/`b` are model/operation generations, `c` is resolution, `d` is catalog row |
| `integration.keypoint.training-artifact`, `.training-output` | Checkpoint/output path in `control`, run ID in `detail`; paired `a`/`b` model/operation generations, `c` optimizer step, `d` OKS AP |
| `integration.keypoint.display_policy` | Current sample stage, raw/visible detection counts, confidence threshold, and sampled joint count for paired Predict/Live skeletons |
| `integration.validation_confidence_edit` | Nine settled decimal/invalid-input/arrow/wheel/endpoint stages, with the value, native settings revision, and unchanged evaluation generation |
| `integration.validation_confidence_pixels` | Paired threshold, retained raw detection count/score range, clean identity, evaluation generation, settings revision, and canvas difference counts for `0 → 1 → 0` |
| `integration.validation_layout`, `integration.validation_text` | Measured atlas and overlay-group bounds in ordinary/narrow layouts, plus Groundtruth/Detections and confidence-control text |
| `integration.validation_progressive` | Selected generation and available population when detail opens during capture, then the complete retained population after closing detail on successful settlement |
| `integration.validation_restored_tile` | Generation, tile index, current image revision, and actual colored pixels for each returned Validation tile |
| `integration.workflow.caption_pixels` | Caption case/stage, patch count, observed background/glyph pixels, compared pixels, and currently verified overlap patches from the actual canvas |
| `integration.workflow.caption_geometry` | Opt-in bounded clip and candidate GT/Det label rectangles when the workflow finds no eligible overlapping caption patch |
| `integration.metric_projection` | Finite sample count, connected-segment count, and total projected entries for a Train curve |
| `integration.metric_values` | First and last finite projected points for the named curve |
| `integration.chart_view` | Settled camera ranges at retained-view interaction stages |
| `integration.workflow.plot_evidence`, `integration.workflow.progress_evidence` | On failed curve/bar visibility, a bounded 64×36 RGBA overview from the same captured canvas snapshot |

```bash
./mmltk --logs --family latest-wayland-test \
  -q '@event:integration.explore_integer OR @event:integration.number_' \
  --format timeline --limit 60
./mmltk --logs --family latest-wayland-test \
  -q '@event:integration.atlas_resize OR @event=integration.atlas_ready_cell' \
  --format jsonl --limit 40
./mmltk --logs --family latest-wayland-test \
  -q '@event:integration.workflow OR @event:integration.metric_ OR @event=integration.chart_view' \
  --format jsonl --limit 60
./mmltk --logs --family latest-wayland-test --run ARCHIVE_ID \
  -q '@event:integration.status.' --format timeline --limit 80
```

Chart probes count saturated curve pixels in a middle strip clear of legend/Y
axis. Progress probes sample the isolated bar's full extent and require visible
saturated fill, including partial progress. Widget samples wait for post-layout
presentation opportunities. Image samples require current draw identity, paired
metadata, and visible geometry, retrying stale asynchronous captures within the
driver bound. Semantic completion, finite metrics, or buffers alone cannot prove
drawing. Missing records may leave finite chart markers without connected
segments; the chart preserves those gaps.

For `integration.workflow.pixels` with `detail: "validate-to-explore"`, the
browser samples a ready tile's interior after direct navigation. The record
retains the compiled index, source/presentation revisions, physical canvas
coordinates, and a patch at most 8×8 pixels. The independent audit requires
nonzero identities, `ready_tile`, `matched`, and at least 12 colored pixels
occupying at least half the patch. Dataset readiness or a status label alone
cannot satisfy this check.

The confidence oracle hides ground truth while comparing the retained atlas
at thresholds 0, 1, and 0. Its fixture's raw scores are below 1, so the middle
capture must remove detection pixels. A pixel counts as different when any RGB
channel changes by more than two levels; at least 12 must differ at threshold 1
and none may differ after returning to 0. Raw detection facts, clean identity,
evaluation generation, and the paired settings revisions must also agree.
This bounded fixture checks preview filtering, not model accuracy.

Validation caption evidence uses seven cases: atlas cells 0–5 and detail case
6. Stages 0–8 start with both layers, then repeat Det-only, hidden, GT-only, and
both twice. Thus a complete workflow observes 63 distinct case/stage pairs;
repeated captures can add records without adding cases. Each case holds at most
eight patches selected from paired native bounds and explicit RGB, separately
from the rendering caption cache. The browser oracle requires actual layer
backgrounds, checks hidden pixels, and compares the restored combined patch
byte-for-byte with Det-only pixels. Final completion also requires a patch with
observed GT glyphs covered by Det captions. Empty patch sets or geometry records
alone cannot prove that overlap.

`verified` on caption-pixel records counts currently verified patches across
cases. It can decrease when a new visibility cycle or changed geometry resets
a case's evidence. `backgrounds`, `glyphs`, and `compared` describe the current
stage's work. Geometry records carry
`control`, a `detail` describing the clip or layer/text/scale, and rectangle
`x`, `y`, `width`, `height` as strings in fields `a`, `b`, `c`, `d`. At most
16 candidate labels per layer are reported per selection attempt, only while
integration reporting is enabled. The
[workflow fixture](validation.md#packaged-wayland-acceptance) establishes the
overlap using a real native checkpoint; these records describe its rendered
observations rather than production model accuracy.

Atlas canvas sampling uses one snapshot of the current canvas. For each ready
tile, its interior is intersected with the actual draw clip; at most nine
patches of up to 8×8 pixels are inspected there. The selected patch retains
physical `canvas_x`/`canvas_y`, `sampled_pixels`, `colored_pixels`, and
`cell_sample_rgba`. `cell_sample_x` and `cell_sample_y` express its center in
thousandths of a native card pixel. Offscreen pixels cannot satisfy the check,
and sparse patches do not establish every pixel of an image. Draw receipt,
paired metadata, compiled-image identity, and native pixel evidence remain
separate required facts.

The annotation pixel fixture has uniform RGB `(48, 80, 112)` background.
For downsampled colored geometry, the browser sampler and native audit each
accept their color comparison or a bounded blend of the expected color over
that background, with consistent coverage across all three channels.
Solid controls and class swatches retain direct RGB comparisons with a
tolerance of three intensity levels per channel.

The native acceptance audit bounds retained Explore diagnostic snapshots at
256 to accommodate admission and completion observations during numeric
entry. This is evidence storage, not application ordering or graphics state.
Clipboard acceptance and its opt-in permission gate are documented in
[validation](validation.md#synthetic-clipboard-input). Enabled harness captures
include Mozilla `Clipboard` and `WidgetClipboard` module logs alongside the
existing graphics modules.

### Canvas allocation and recovery

Canvas allocation failures emit gated `firefox.canvas.allocation_failed` with
the requested width, height, allocation bytes, and Vulkan status. Packaged
resize recovery coverage additionally records `firefox.canvas.allocation_fault`
and `integration.graphics.recovery`; the latter checks that the recovery overlay
and manual reload button are visible and retains the displayed cause. The fault
setting `MMLTK_TEST_CANVAS_OOM_WIDTH` is read only with packaged acceptance
diagnostics enabled and fails one canvas allocation without consuming VRAM.

```bash
./mmltk --logs --family latest-wayland-test \
  -q '@event:firefox.canvas.allocation OR @event:integration.graphics.recovery' \
  --format timeline --limit 20
```

### Prediction media acceptance

The existing [BrowserAudit](../src/acceptance/tests/wayland/browser_audit.cpp)
checks `integration.prediction.saving`, `integration.prediction.no_output`
and `integration.prediction.output` in the workflow that creates the media.
Its first invalid observation emits gated `acceptance.prediction.failed` once.
The record retains the observed fields, `observed_event`, `reason`,
`audit_context`, and the Firefox `observed_file`/`observed_line`.
Stage/path remain in `detail`/`control`; generic `a`/`b`/`c`/`d` slots retain
the original integration observation. Media context includes admitted-directory,
JSON absence, sample and processed counts. Disabled-output context includes
`claim_only`, `processed`, `expected_processed`, `filesystem_error` and
`unexpected_entry`. A reserved empty run contains its empty
`.mmltk-run-claim` directory; that marker is not a saved media product.

When terminal evidence is missing without an earlier invalid observation,
`acceptance.prediction.incomplete` reports `level: "error"`, the blocker in
`detail`, and `media_disabled`, `saving_controls` and `saved_media` inventories.
These records observe acceptance and never control product execution.

```bash
./mmltk --logs --family latest-wayland-test \
  -q '@event:acceptance.prediction OR @event:integration.prediction' \
  --format timeline --limit 40
```

Select the workflow's archived run when the latest family belongs to a later
browser lifetime. [Validation](validation.md#packaged-wayland-acceptance)
owns the media assertions and their relation to native media tests.

### Dataset presentation evidence

The opt-in [Dataset observer](../src/frontend/iced/src/integration_control/dataset_presentation.rs)
captures the ordinary component at actual draws. The
[validation guide](validation.md#dataset-presentation-and-lifecycle) owns its
fixture matrix, real input, and native cancellation/restart requirements.

| Records | Evidence |
| --- | --- |
| `integration.benchmark_baseline`, `.benchmark_click`, `.benchmark_choice`, `.benchmark_visibility`, `.benchmark_inactive` | Native settings baseline, real clicks, settled choices/restoration, current-tree presence/absence, and disabled source-control behavior |
| `integration.dataset_draw`, `.dataset_viewport`, `.dataset_frame` | Row text/bounds and absolute page viewport from one draw; the final frame record carries observation key, horizontal/vertical scroll offsets, and theme |
| `integration.dataset_paint`, `.dataset_label`, `.dataset_clip`, `.dataset_font`, `.dataset_label_reference` | Actual control/label bounds and clip, available resolved paragraph metrics, and recovery-label shaping using the description's small-text metrics |
| `integration.dataset_native_work`, `.dataset_native_track` | Native generation and exact current-work/track facts captured with the draw; `detail` contains JSON with decimal integer values |
| `integration.compile_progress`, `.compile_metrics`, `.compile_cancelled`, `.dataset_complete` | Real native activity, metrics, inactive cancellation, and the later successful generation |
| `integration.compile_resize_requested`, `.compile_resize_wait` | Resize request and an unmet predicate, with step/generation/revisions, viewport/fullscreen, input count, root, and presentation facts when available |
| `integration.compile_resize` | Completed active-compilation resize: current native/root revisions, resize-request/root-draw/presentation sequences, CSS/backing/configured/presented extents, actual UI root, scale, and zero recorded pointer/wheel/keydown events |
| `integration.dataset_fixture`, `.dataset_pixels`, `.dataset_label_pixels`, `.dataset_divider_pixels` | Settled local fixture receipt, canvas contrast, visible label samples, and measured divider color/span/stroke/gaps |
| `integration.dataset_reversal`, `.dataset_hidden_input`, `.dataset_drag_edit`, `.dataset_offscreen_release` | Actual interrupted motion, rejected outgoing input, settled drag-selection editing, and unchanged native value after offscreen release |
| `integration.dataset_custody` | Probe admission/stimulus/outcome and retired-callback drain, with request scope, owner, geometry, snapshot/read/report counts, and replacement-owner isolation |

In this table a leading dot continues the `integration` prefix. The draw,
viewport, paint, label, and clip records put `x`, `y`, `width`, and `height` in
string-valued `a`, `b`, `c`, and `d`. Font records use `a`/`b` for size/line height;
label-reference records use `a`/`b` for measured width/height and `c`/`d` for those
small-text metrics. Observation keys and probe scopes belong only to acceptance
diagnostics. They never become native operation generations, settings revisions,
or resource-lifetime state.

The compile-resize sequence uses steps 0/1 for 480×900 and 960×720 canvas sizes,
then 2/3 for real fullscreen entry and exit. A successful record puts its facts
in JSON `detail`; request/wait diagnostics expose fields directly. Wait `reason`
identifies `request`, `root`, `native-state`, `presentation`, `canvas-size`,
`fullscreen-viewport`, `root-coverage`, `input`, `pending-resize`, `backing-size`,
`configuration`, or `presented-size`. These records explain which predicate
remains unmet; a wait record alone is not failure or a deadline extension.

Iced's actual window loop emits gated `iced.window.resize_pending`,
`.resize_applied`, `.requested`, `.redraw`, `.configured`, `.root`, `.draw`, and
`.presented` observations. Model delivery adds `.model_resize`; Dataset draw
observation adds `.root_draw`. The index bridge records sequences, actual
dimensions, pending-resize state, root bounds, and request/redraw/configure/draw
counters. Surface tracing or the installed acceptance observer enables these
hooks. They neither request a redraw nor send application socket traffic.
`presented` means successful Iced compositor presentation; physical Wayland/GPU
completion retains its [independent evidence](#physical-presentation-evidence).

A frame closes preceding row/paint/viewport/native observations. The audit checks
complete work/track captions against both real generations; fixture completion
must join its fully exposed expected caption draw. Intermediate clipped text can
demonstrate motion, not caption correctness. Canvas contrast is not OCR: exact text and shaped-label
geometry remain required. Divider probes check card-outline color, centered span,
stroke, and gaps at actual scale.

Before deferring, the JavaScript probe copies borrowed Wasm bounds, divider/label
rectangles, and colors into owned arrays. Snapshots recheck owner, request, and
canvas geometry. Invalidated callbacks leave replacement scratch/report counts
unchanged; supersession, geometry restoration, and owner replacement explicitly
drain before settlement. Disabled reporting collects no Dataset fixtures/paint/probes.

Select the retained or DPI archive using [capture selection](#select-captures-and-histories),
then replace `ARCHIVE_ID` with that run's identity:

```bash
./mmltk --logs --family latest-wayland-test --run ARCHIVE_ID \
  -q '@event:integration.dataset_ OR @event:integration.compile_' \
  --fields @event,control,detail,a,b,c,d --format jsonl --limit 80
./mmltk --logs --family latest-wayland-test --run ARCHIVE_ID \
  -q '@event=integration.dataset_custody' --format jsonl --limit 40
./mmltk --logs --family latest-wayland-test --run ARCHIVE_ID \
  -q '@event:integration.compile_resize OR @event:iced.window.' \
  --fields @event,reason,step,generation,revision,detail,width,height,root,pending_resize \
  --format jsonl --limit 60
```

## Start with a bounded investigation

```bash
./mmltk --logs --family latest-wayland-test --triage
./mmltk --logs build/validation/latest-wayland-test-firefox.log --triage
./mmltk --logs --family latest-wayland-test --triage -q 'probe_failed OR "rendered probe"'
./mmltk --logs --error 'Presentation unavailable

Explore requires a measured non-empty gallery.'
```

Triage ranks explicit failures/assertions/terminals/damaged records/incomplete
stages, selects one run, and follows bounded identities, explaining each row.
`--query` narrows anchors; `--where` constrains every pass. [Process snapshots](commands.md#process-snapshots)
show current activity beside a quiet transcript, without establishing a stall.

Single-file triage discovers current family siblings or only its matching
archive batch; it never mixes archived-only input with current captures.
`@discovery` labels additions; rotation joins remain inferred. Ordinary explicit
path queries add no siblings.

`--error` accepts complete pasted Status title/body, searches normalized phrases
first (including logged colon separators), then reports an all-words fallback.
It searches histories, prefers physical records to copied Catch context, and
focuses the earliest matching capture by mtime, retaining physical provenance
and clocks. Add `--triage` to investigate identities around that error.

## Catch selection and terminal evidence

| Parsed evidence | Meaning |
| --- | --- |
| **No test cases matched** | Error `catch.test_selection_failed`; requested text stays in `message`/`catch.filters` |
| **No tests ran** | Error `catch.summary`, `tests: 0` |
| Assertion/exception headers, uppercase `FAILED:`/`FATAL ERROR:` | Source file/line retained, including headers without inline text |
| Dashed title + source location | Restores named test after delayed stderr follows another test's `RUN` |
| Exact `{Unknown expression after the reported line}` | `catch.expression` transcript text |

Selection errors appear under `--errors` and terminal context. Parsed summaries
alone prove no intended execution. Use a [comma-separated union](validation.md#selection-environment-deadlines-and-debugging)
for alternative case names and inspect the command terminal. Exception follow-on
text retains test identity. Genuine malformed/truncated JSON still sets
`@parse_error` and fails `--strict`.

```bash
./mmltk --logs build/logs/test-investigation.log --errors --format timeline
./mmltk --logs build/logs/test-investigation.log \
  -q '@event=catch.assertion_failed OR @event=catch.assertion_fatal_error' \
  --fields @event,@test,source_file,source_line,message --context 3 --format timeline
```

### JavaScript and Rust outcomes

| Format | Normalized events/context |
| --- | --- |
| TAP | `tap.test_started`, `.test_passed`, `.test_failed`, `.test_skipped`; SKIP/TODO remain skips. `tap.summary` flags failures, cancellations, or zero tests. Indented assertion objects/diffs remain named-test transcript context |
| Rust libtest | `libtest.test_passed`, `.test_failed`, `.test_skipped`, `.summary`; named stdout/stderr sections restore context for panic/assertion/left-right values |

```bash
./mmltk --logs build/logs/browser-app-investigation.log \
  -q '@test:"atlas_pixel_evidence"' --format timeline --limit 40
```

Failure words in passed/skipped names, successful summaries, compiler progress,
or numbered source excerpts are not failures. Explicit failures, compiler
errors, and failed build terminals retain error classification.

## Query expressions

```bash
./mmltk --logs --errors --tail
./mmltk --logs -q 'onnx OR "CUDA error"' --context 2
./mmltk --logs -q '@event:shutdown AND NOT @event:started' --format timeline
./mmltk --logs -q 'duration_ns>=1000000' --fields @event,duration_ns,trace_id
./mmltk --logs --where '@file:"latest-wayland-test"' --group-by @event
```

Quote the expression for the shell:

```text
expr      := expr OR expr | expr [AND] expr | NOT expr | '(' expr ')'
primary   := text | '*' | has(field) | field operator value
operator  := = != : ~ !~ > >= < <=
```

Precedence: `NOT`, `AND`, `OR`; adjacent terms imply AND. Bare/quoted text and
`:` are case-insensitive literal substrings; `=`/`!=` compare exact typed values;
ordering is numeric. `~`/`!~` use Python regex, case-sensitive unless `(?i)`.
Quote values containing spaces/punctuation. Missing fields fail comparisons,
even `!=`; `NOT` can include them. `has` includes null/zero.

`--errors` adds `@error=true`. Error flags/counts aid search, not verdicts;
`error=0` alone is no failure. Use owner assertions and terminal status.

## Fields and correlation

JSON paths work directly. Unqualified names also inspect `fields`; qualified
paths specify location. Canonical nonnegative list indices work, e.g.
`referenced_objects.0.handle`; negative/out-of-range/`01` indices are missing.
Explicit `.json` files (`progress.json`, `preparation.json`, `run.json`) parse as
one ≤1 MiB document with physical span; damaged/oversized input stays searchable
with parse error. Complete object-valued `message`/`fields.message` JSON ≤4096
characters is additionally projected as `message_fields`, preserving original
text; invalid/oversized/non-object messages remain text.

```bash
./mmltk --logs output/train/RUN/progress.json --format jsonl \
  --fields record.progress.phase,record.progress.completed_images,record.progress.images_per_second
./mmltk --logs --family latest-wayland-test \
  -q '@event=explore.card.pixel_samples AND has(message_fields.clean_rgba.24)' \
  --fields @event,message_fields.compiled_index,message_fields.clean_rgba.24 --format jsonl
```

Useful metadata includes:

| Fields | Meaning |
| --- | --- |
| `@file`, `@line`, `@line_end`, `@text`, `@format` | Physical provenance, multiline span, and input representation |
| `@clock`, `@time_ns`, `@mtime_ns` | Recorded clock/time and captured file modification time |
| `@event`, `@owner`, `@level`, `@error` | Normalized event and failure-candidate metadata |
| `@surface` | Joined native/browser sample-arena identity; source lifecycle records normalize their arena into this field |
| `@workspace_source` | Joined producer import identity, distinct from the reusable sample arena |
| `@session`, `@model`, `@first_cause`, `@phase`, `@role` | Canonical format-3 training identity, failure, and progress fields |
| `@intent_request` | Exact positive uint64 request correlation from browser intent receipt/settlement and acceptance intent records |
| `@test`, `@tags`, `@context_copy`, `@part` | Test context, copied INFO, transcript segment |
| `@run`, `@archive_id`, `@family`, `@artifact` | Capture and artifact grouping |
| `@terminal`, `@exit_code`, `@signal`, `@signal_number` | Observed terminal status |
| `@process`, `@parent_process`, `@process_group` | Exact observed Linux process relationships; see [Firefox fatal causes](#firefox-fatal-causes-and-process-identity) |
| `@parse_error` | Damaged/truncated input |
| `@proximity_ns`, `@time_link` | Pasted-error proximity and its time-link method |
| `@triage_reason`, `@triage_payload_truncated`, `@discovery` | Triage inclusion and limits |

`@event` projects [training records](#local-training-failures), otherwise
wrapped `fields.event`/`fields.name` before top-level names. Bare searches include
test/tag/run/signal metadata. Catch INFO copies preserve clocks and are labeled.
`child.signaled` 139/143 decodes to SIGSEGV/SIGTERM, without classifying intent.

```bash
./mmltk --logs -q 'trace_id=42' --correlate trace_id --format timeline --limit 80
./mmltk --logs -q 'source_session=42' --correlate source_session+source_instance
```

Correlation needs query/error seeds. Repeat `--correlate` for alternatives,
`+` for composites. It joins original matches only, ignores empty/zero IDs,
never expands transitively, scopes family/history to each run, and applies
`--where` to all correlated/context rows.

`@intent_request` projects `integration.intent.control` and browser intent
received/accepted/rejected/reply_rejected `value`. Only exact positive uint64
integers/decimal strings qualify (no bool, zero, overflow, approximate floats).
Explicitly query/correlate it within one peer lifetime: counters may repeat on
peer replacement, so automatic timeline identity excludes it. `--run` selects
an archive, not necessarily one peer lifetime.

## Vulkan diagnostics and descriptor provenance

```bash
./mmltk --logs --family latest-wayland-test -q '@event=vulkan.validation' \
  --group-by validation_id --representatives --format timeline --limit 40
./mmltk --logs --family latest-wayland-test -q 'validation_id:SYNC-HAZARD' \
  --group-by validation_id --representatives --format jsonl --auto-correlate
./mmltk --logs --family latest-wayland-test -q 'workspace_allocation=34' \
  --descriptor-lineage --format timeline --limit 32
```

Adjacent Rust VALIDATION header/body/matching logger-object lines form one
`vulkan.validation` record, ≤128 lines/1 MiB. Original text/spans stay available;
overflow sets parse error and leaves subsequent lines searchable.

| Fields | Interpretation |
| --- | --- |
| `validation_id`, `message_id`, `api`, `level` | VUID/SYNC-HAZARD and reported facts |
| `objects` | Typed callback objects/names |
| `referenced_objects` | Explicit typed handles mentioned in message, separate from callbacks |
| `vk_image`, `vk_memory`, `vk_command_buffer` | Sole reported handle of that type |

Types normalize to Vulkan object names and handles to hex. Both collections
share 64 entries; overflow retains text and reports limits. Bare hex/names
create no resource. Use exact list paths. Layer reports remain allegations;
handle equality alone establishes no cause.

`--representatives` keeps first physical-preferred row per `--group-by`, with
full count in `@representative_count`; add `api`/`message` for VUID variants.
It rejects `--tail`, `--error`, `--triage`.

Automatic joins add bounded creation/allocation/binding/direct-binding evidence
for exact typed image/memory handles. Same-run exact process IDs may cross files;
conflicting device/process/source/allocation blocks joins. PID-free stderr yields
labeled same-file candidates only. Reused handles/ambiguous owners suppress
uncertain joins; missing provenance establishes no unrelatedness.

`--descriptor-lineage` follows allocation/export, SCM_RIGHTS attempt/result,
receipt, CUDA import/mapping, retirement, and supported CUDA-export capture
formats. FDs are process-local; `fstat` does not identify common GPU backing.
Only exact record-length `descriptor_send_result` with `send_errno=0` proves send
success. `retained_memory_descriptor` owns backing; `import_descriptor` is the
duplicate consumed by successful import. Reports use retained rows under
`--where`/`--limit`/`--top`, exposing missing/ambiguous links. Diagnostics retain
no extra FDs/GPU resources.

`loaded_library`/`library_inventory` observe existing `/proc/self/maps` once at
native first import or Firefox first export: ≤1 MiB, ≤32 paths, each <1024 bytes.
Query separately by process to preserve allocation-chain row capacity.

## Automatic timeline context

Timeline queries with `--query`/`--errors` add bounded direct frame/publication/
snapshot/lifecycle evidence. `--no-auto-correlate` disables it; summary/JSONL
require `--auto-correlate`. Explicit correlation, related-run, pasted-error,
and triage keep their own selection rules.

Original matches retain order/priority. Added rows use spare `--limit`, explain
their reason, and never seed expansion. Recent matches drive one streaming
pass with bounded nearest-identity indexes, not an all-run index. Shared
source/surface/publication/allocation/trace facts must agree; bare counters
and generic numeric slots are not IDs.

Pixel chains compare source→Iced in direct mode and source→Firefox import→mailbox→
Iced in copy mode. Source/arena forwarding plus exact transfer chooses source;
direct settlement and copy completion remain distinct. Valid optional probes
extend required evidence. Indexes accept either arrival order, report conflicts,
and distinguish missing from capacity-discarded evidence.

Weaker phase/control proximity is labeled: ≤250 ms comparable time, or eight
same-source lines when clocks cannot align. Cross-clock IDs do not align clocks.
`--where` applies throughout; help owns current event-specific identity limits.

## Select captures and histories

```bash
./mmltk --logs --family latest-wayland-test --history --list-runs
./mmltk --logs --family latest-wayland-test --run current --errors
./mmltk --logs --family latest-wayland-test --run ARCHIVE_ID \
  -q '@event=integration.workflow.caption_pixels' \
  --fields @event,case,stage,patches,backgrounds,glyphs,compared,verified \
  --strict --format jsonl --limit 100
./mmltk --logs --family latest-wayland-test --errors --related-run --tail
```

| Selection | Behavior |
| --- | --- |
| `--family STEM` (repeatable) | Artifact siblings plus `STEM.log` below `build/validation` or explicit repository path |
| `--history` | Add rotated `.history` siblings |
| `--run ID` / `current` | Exact capture; implies history |
| `--related-run` | Other records from matched captures |
| Explicit paths/quoted globs | Repository-relative or absolute inside checkout; any file suffix, deduplicated |
| Directories | `.jsonl`, `.log`, `.out`, `.txt`, Mozilla process logs; `--recursive` adds nested/history directories |
| No input | `build/validation`, without recursive archives |

Direct history directories work without current artifacts. Mozilla main/child
names retain physical/logical IDs, with up to four module-log rotations per
process; application `.1.log` through `.5.log` belong to the family.

Every logged Wayland browser lifetime rotates its family, including successful
workflows. Latest means latest **logged lifetime**, not the full suite. Find the
scenario via transcript test/location/terminal, list runs, then query that
archive's native/acceptance/Firefox siblings. Successful browser evidence need
not appear in Catch excerpts; an empty later capture says nothing about it.
`--strict` proves parse validity, independently of record presence/assertions.
Partial transcripts remain partial regardless of excerpt size.

Current harness siblings share one rotation ID. Independently rotated captures
may be grouped within 10 ms for equal PID, labeled inferred. Exact event/steady
anchors join transcript segments and propagate test/tags; missing anchors leave
separate runs. Select one reproduction because IDs/monotonic times can repeat.

## Triage conventions and limits

| Option | Default | Range |
| --- | --- | --- |
| `--triage-anchors` | 4 | 1–12 |
| `--triage-identities` | 12 | 1–64 |
| `--triage-hops` | 2 | 0–3 |
| `--triage-gap-ms` | 1000 | 1–600000 |
| `--near-ms` | 1000 | 0–600000 |
| `--limit` | 20 | 1–10000 |
| `--top` | 5 triage / 10 otherwise | 1–100 |

Strong run-qualified surfaces, typed handles, nonempty `*_id`/`*_identity`, and
source-session composites can expand hops. [Fatal/process rules](#firefox-fatal-causes-and-process-identity)
apply separately. Ambient process/thread/device/user/session IDs require explicit
correlation. Sequence/generation/slot joins use nearby composites, without
further expansion.

Generic suffix analysis suggests unmatched starts/ends, duplicate terminals,
failed text, incomplete stages, and handoffs, using specific operation/resource
IDs and exact families. Shared-identity handoffs reject conflicting operations;
copied INFO does not inflate balances, and numeric outcome enums are not guessed.
Missing counterparts may reflect disabled/truncated/other-source evidence.

Gaps require comparable timestamps within one physical source. Bounded terminal
evidence is reported separately from row budget; absent terminals show source
ends without inferred success/timeout/failure. Capacity truncation is explicit.
`--triage` rejects related-run/tail/list-runs; JSONL rows go to stdout, summary
to stderr.

## Time, output, and exit status

| Ordering/window | Meaning |
| --- | --- |
| `time` | Separate steady, UTC, timezone-free wall, per-file elapsed, untimed groups; no guessed offsets |
| `file` | Physical file/line order |
| `capture` | Captured file mtime then lines; recency is not causality |
| `--tail` | Last selected records, printed ascending |
| `--last-lines N` | Parse only final 1–100,000 physical lines per file, found with bounded byte scan; report byte/line start and preserve absolute lines |
| `proximity` | Pasted-error default; requires `--error`, not causal order |

Windows restrict all counts/statistics/context/correlation and exclude older
identities/failures. Proximity uses same-clock recorded distance; cross-clock
estimates align each source's final timestamp to mtime, optionally borrowing a
preceding timestamp for untimed rows. `@time_link` labels estimates.

Output formats are `summary`, `timeline`, `jsonl`. `--fields` projects
comma-separated fields with provenance; `--group-by` selects summary groups.
`--context` admits ≤100 neighboring nonblank same-file records per side. Counts
cover all selected records even when displayed rows are limited; group/correlation
cardinality is ≤10,000.

`--stats FIELD` (≤32 distinct fields) aggregates exact query matches, independent
of row limit; related/context rows never contribute. It keeps source, capture,
explicit run_id, clock, and group values separate. Numeric count/sum/min/max/mean,
missing/null, and invalid/nonfinite counts remain distinct. Integer arithmetic
is exact; nonintegral results can use numerator/denominator strings. Accumulators
are ≤8192 bits, total groups/buckets ≤10,000; overflow fails explicitly.

`--bucket-ms N` (1–86,400,000) adds clock-local half-open buckets and rates divided
by full requested width; capture edges can be partial. Untimed rows contribute
only totals. Statistics retain line/time anchors without clock alignment or
completeness claims. Stats/buckets reject triage/list-runs. JSONL rows use stdout,
Statistics JSON stderr.

```bash
./mmltk --logs build/validation/compile-trace.jsonl \
  --last-lines 100 -q '@event=compile.execution.retired' --format jsonl
./mmltk --logs build/validation/compile-trace.jsonl \
  -q '@event=benchmark.cost.completed' --group-by kind,split \
  --stats duration_ns --stats bytes --format jsonl --limit 10
```

Malformed/oversized records remain searchable with warnings; `--strict` fails
on malformed/truncated input. Exit 0=output, 1=no matches, 2=invalid query,
unreadable/changing inputs, or strict parse failure.

The published native query tool runs in the existing build image, read-only,
without GPU/network. Reads stop at captured sizes; multipass queries require
unchanged input. There is no follow mode/persistent index. [Interface](../tools/logging/log_query.cpp),
[runtime](../tools/logging/log_query_runtime.cpp), and [private implementation/help](../tools/logging/log_query_internal.cpp)
share [one module family](build.md#composition-and-fixture-isolation). Fixtures
run in unfiltered `all` or [focused log-query selection](validation.md#selection-environment-deadlines-and-debugging).
