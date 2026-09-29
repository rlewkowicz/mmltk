# Capturing and querying diagnostics

[Wiki index](README.md) · [Commands](commands.md) · [Validation](validation.md) · [Headless Wayland](headless-wayland.md)

## Activation and quiet execution

Ordinary `./mmltk --gui` creates no active diagnostic sink, integration driver,
reporting state, or pixel-probe owner. Disabled diagnostics perform no collection,
formatting, clock reads, counter updates, or I/O. Product state, input order,
and physical custody/completion remain independent of diagnostic identities.
[Fatal stderr](#fatal-stderr-reports) remains active.

Training run history and incomplete-history notices are product output,
independent of diagnostic activation. `run.json`/`metrics.jsonl`, progress
projections, and their writer/reader ownership are documented in
[saved history](rfdetr-workflows.md#saved-history-and-plots).

| Explicit setting | Effect |
| --- | --- |
| `MMLTK_LOG_LEVEL`, `MMLTK_LOG_FILE`, `MMLTK_LOG_DIR` | Native application logging to stderr and a rotating file |
| CLI `--log-level`, `--log-file`, `--log-dir` | Override the corresponding native logging environment values |
| `MMLTK_GUI_TRACE_FILE` | Native runtime JSONL, benchmark compilation, and browser surface lifecycle diagnostics |
| `MMLTK_GUI_PIXEL_TRACE=1` with a GUI trace path | Additional pixel probes |
| `MMLTK_FIREFOX_LOG_FILE` | Redirect Firefox stdout/stderr to a separate file; does not itself enable native lifecycle or pixel collection |

Native logging is enabled by a non-`off` level, or a destination with the build's
default enabled level. Explicit `off` wins over destinations but leaves separately
requested GUI traces active. Desktop logging uses the environment; native CLI
and ONNX tools also accept the three flags.

```bash
MMLTK_LOG_LEVEL=info ./mmltk --gui
./mmltk info --compiled ./compiled/train.bin --log-level info
./mmltk rfdetr info --onnx ./model.onnx --log-level info
```

When only an enabled level is supplied, the wrapper prepares the writable
repository directory `.mmltk-data/logs` for the resolved runtime UID/GID.
Explicit file/directory overrides retain their destination and host-path
rewriting. Native logs use the application name, a 10 MiB rotation threshold,
and five archived files.

ONNX inspection/simplification needs explicit diagnostics for routine metadata;
default/`off` still reports fatal stderr and failure status. `rfdetr info --onnx`
forwards logging overrides to its sibling tool. Ordinary CLI result stdout
(e.g. compiled-dataset `info`) is independent.

## Wrapper build, test, and tidy transcripts

Explicit `./mmltk --build`, `./mmltk --test ...`, and `./mmltk --tidy` commands
retain their stdout and stderr automatically. The wrapper prints the selected
path when capture starts:

| Operation | Default path below the repository | Destination override |
| --- | --- | --- |
| Build | `build/logs/mmltk-build-YYYYMMDD-HHMMSS-PID.log` | `MMLTK_BUILD_LOG_FILE` |
| Test | `build/logs/mmltk-test-YYYYMMDD-HHMMSS-PID.log` | `MMLTK_TEST_LOG_FILE` |
| Tidy | `build/logs/mmltk-tidy-YYYYMMDD-HHMMSS-PID.log` | `MMLTK_TIDY_LOG_FILE` |

Relative overrides resolve against the invocation's working directory. Capture
appends to an existing override path, so choose a new filename to isolate a
run. Test `list`, `help`, `-h`, and `--help` routes skip capture. Initial wrapper
startup text emitted before operation selection is outside the transcript.
Automatic command capture does not enable GUI traces, pixel probes, or routine
application diagnostics.

The caller still receives stdout on stdout and stderr on stderr. Separate tees
retain both streams in one file; this does not establish a total ordering
between them. On ordinary exit the wrapper drains both capture pipes before
printing and appending its terminal line:

```text
mmltk: test exited with status 0
```

Build and tidy use their corresponding operation names. Capture/cleanup failures
report an error and turn an otherwise successful command into a nonzero result.
An abrupt termination can leave a transcript without a terminal; EOF alone
does not establish success. The log parser exposes observed exit lines as
`process.terminal`, with `@exit_code` and `@terminal` available for queries.

For example:

```bash
MMLTK_TEST_LOG_FILE=build/logs/test-investigation.log ./mmltk --test core
./mmltk --logs build/logs/test-investigation.log --errors --format timeline
./mmltk --logs build/logs/test-investigation.log \
  -q '@event=process.terminal' --fields @event,@exit_code,@terminal --format timeline
```

Use the printed path for a specific build or test. The default `--logs` input
is `build/validation`, so select `build/logs` or a transcript explicitly.
[Test replacement](validation.md#replacing-an-active-test-run) may explain a
cancelled earlier run; its terminal records that run's outcome independently
of its replacement.

## Fatal stderr reports

Native CLI errors, RF-DETR command errors, ONNX tool failures, and desktop
startup/runtime failures use the common bounded reporter. Each report is a
single line:

```text
fatal: <component>: <detail> (status=<available status>)
```

The status suffix is omitted when no status is supplied. Component and detail
are capped at 160 and 800 bytes, with truncation markers; control bytes become
spaces. The fixed 1024-byte buffer bounds the complete report independently of
optional logger state. A disabled, uninitialized, or failed diagnostic sink does
not hide the stderr attempt. The reporter preserves `errno`; a broken stderr
pipe does not terminate reporting through SIGPIPE or consume a signal already
pending on the calling thread.

When enabled, the rotating file sink receives a best-effort critical copy
without emitting a second stderr copy. RF-DETR and ONNX tools retain their
named diagnostic identities. Routine successful execution and requested
healthy shutdown remain quiet when diagnostics are disabled.

Desktop failure context comes from the owning boundary. Firefox launch and
process-infrastructure failures retain the OS error separately from the mapped
process status. An unsuccessful child exit preserves its exit status; an
unexpected child signal is reported by number and yields `128 + signal` as the
desktop exit status. This does not turn an intentional healthy SIGINT/SIGTERM
shutdown into a failure.

The [logging module](../src/common/logging/mmltk_logging.cppm) and
[implementation](../src/common/logging/mmltk_logging.cpp) own this path.
`report_fatal` is for ordinary terminal boundaries, **not signal handlers**;
it must not be called from a fatal signal handler.

## Local training failures

[TrainProcessClient](../src/controller/services/training/train_process_client.cpp) scans
the child's consumed output for line-start `fatal: ` envelopes even after the
64 KiB console-retention budget fills. Its current-line and retained-cause
buffers are each bounded to 3072 bytes. It finishes chunked and unterminated
fatal lines before publishing the terminal and preserves a useful worker cause
when a later parent line reports only generic distributed failure. Progress
text and allocator warnings do not select a cause.

A complete fatal line publishes a failed training observation immediately,
while the process group remains active until physical retirement. The Train
view and session Status panel show the cause during that interval, and Start
remains unavailable. The first fatal cause also arms the existing five-second
cleanup deadline; surviving peers receive SIGKILL when it expires. Captured
children unblock SIGINT and SIGTERM inherited from the desktop's signal owner,
so ordinary distributed peer termination works without waiting for escalation.

[NativeTrainingRuntime](../src/controller/subsystems/train/training_system.cpp)
projects the decoded exit/signal status and UTF-8-safe cause into the existing
`ComputeTerminal.detail` for the [session Status panel](gui-interaction.md#session-status).
An explicit CUDA OOM cause adds the
[batch-size/lane guidance](rfdetr-workflows.md#local-training-failures).
`TrainingSystem` owns the single settled fatal stderr report when it hosts the
runtime, including preparation failures; standalone runtime use retains its
own reporting responsibility. Cancellation and process-group retirement are
independent of diagnostic activation.

Enabled GUI trace records `child.exited`, `child.signaled`, and `training.failed`
include bounded batch/lane, output, device, and available run/attempt context.
`training.worker_failed` records each newly selected fatal cause before process
retirement, with the process-group ID in `value` and the output directory in
`document_resource`. Its message admits the complete retained cause and OOM
guidance within a 4096-byte bound.

The training system also emits `training.inputs` before dataset inspection,
including the operation generation (`sequence`), captured settings revision
(`value`), requested and prepared preset/resolution, and compiled split paths.
`training.operation.failed` and `training.operation.settled` record the resulting
typed terminal, including failures before a training process exists. Their
`sequence` is the operation generation and `value` is the snapshot revision.
Resolution errors identify the file, actual geometry/channels, and required
geometry. These diagnostic messages remain bounded to 1024 UTF-8 bytes; the UI retains
the full bounded terminal detail.

`training.preparation` reports the worker's startup stage before metrics exist.
Its `sequence` is the operation generation. The small native
`preparation.json` document is watched alongside metric documents and changes
only at stage transitions. No console parsing is needed for startup
progress. Query `@event=training.preparation` for GPU connection, dataset
admission, checkpoint loading, model construction, GPU weight loading,
GPU/session synchronization, and optimizer setup. [Dataset identity](model-merging.md#whole-session-resume)
uses admitted metadata without scanning image payloads.
Distributed CLI training forwards explicit logging options to each worker, so
`--log-level` and `--log-file` also cover the native training processes. Each
worker inserts `.rank-N` before the log file extension to keep rotating sinks
independent. Without an explicit file, workers use `mmltk.rank-N.log` in the
configured log directory or the training output directory. Disabled logging
creates no worker log files.

With GUI lifecycle tracing enabled, Firefox output records
`iced.notice.added`, `iced.notice.updated`, and `iced.notice.dismissed` with the
notice ID, typed origin, content version, and title/detail text. These records
track the frontend notice store independently of native operation settlement.
For example:

```bash
./mmltk --logs .mmltk-data/logs/gui-trace.jsonl .mmltk-data/logs/firefox.log \
  -q '@event=training.inputs OR @event=training.operation.failed OR @event=iced.notice.added' \
  --format timeline --fields @event,sequence,value,notice_id,origin,message
```

The query tool projects canonical [TrainingRecord format 3](rfdetr-workflows.md#saved-history-and-plots)
role/phase as lowercase `training.<role>.<phase>`. Every `Terminal` is terminal
evidence; `progress.phase: "Error"` additionally anchors failure triage. Counters
such as `class_error` imply no failure. Bounded triage prioritizes `run_id`, `attempt_id`,
role, sequence, phase, session/model, and first cause over optional configuration,
follows run/attempt identity, and excludes these records from generic suffix
balancing.

For a captured run:

```bash
./mmltk --logs output/train/run-0002/metrics.jsonl --errors --format timeline
./mmltk --logs output/train/run-0002/metrics.jsonl --triage
./mmltk --logs output/train/run-0002/metrics.jsonl \
  -q '@model=2 AND (@phase=Merge OR @role=Terminal)' --format timeline
./mmltk --logs --family latest-wayland-test \
  -q '@owner=training AND (@event=child.exited OR @event=child.signaled OR @event=training.failed)' \
  --format timeline --limit 20
```

Replace the run path with the selected Output directory. A Terminal/Error
record establishes failed training; use the owning operation's terminal detail
or captured fatal stderr to establish the cause.

## Capture one reproduction

```bash
mkdir -p .mmltk-data/logs
MMLTK_GUI_TRACE_FILE=.mmltk-data/logs/gui-trace.jsonl \
MMLTK_FIREFOX_LOG_FILE=.mmltk-data/logs/firefox.log \
./mmltk --gui
```

`MMLTK_GUI_TRACE_FILE` opts into native JSONL command, transport, worker,
benchmark compilation, presentation, cleanup, and failure diagnostics. The native
sink opens a new capture and truncates that path; preserve earlier evidence
before reusing it.
`MMLTK_FIREFOX_LOG_FILE` captures the separate browser log.
The native trace file and Firefox output file must be separate destinations.
Their parent directories must already exist and be writable by the runtime user.

Add `MMLTK_GUI_PIXEL_TRACE=1` for pixel probes; desktop startup enables them
only when the native trace path is also set. Lifecycle-only traces do not
prove pixel continuity. Native high/low values and browser 32-hex-digit
identities join the same physical source or arena within their respective
namespaces; [normalized identity fields](#fields-and-correlation) keep them
separate.

Enabled Iced pixel probes retain small reusable compute/readback storage and
the exact sampled slot through completion. They preserve the shared sample's
shader-read layout and do not introduce a full-image capture texture. Ordinary
display execution has no pixel-probe owner.

Native and Iced frame probes compare the same 25 points: the Cartesian product
of five positions per axis, bounded by the exact content extent. Explore also
records 25 samples per card at 5%, 35%, 50%, 65%, and 95% along each axis, plus
rendered semantic/padding checks. Card records include clean, semantic, and
retained-reference RGBA arrays and their actual integer coordinates. Probes are
explicit GPU readback, separate from the same-GPU display path. Native kernel
probes and direct CUDA source-to-host samples are independent observations.
Optional Firefox direct-image probes require successful completion and valid
readback markers; undefined buffer-alias samples cannot prove image contents.

GPU-specific trace variables are covered in [GPU execution](gpu-execution.md).

## Native selective compilation

Native application logging at `debug` enables the
[selective region diagnostics](../src/backend/models/rfdetr/core/selective_compilation.cpp).
These are `rfdetr compilation` messages, separate from GUI lifecycle JSONL.
The query tool extracts their named fields from the native log:

```bash
./mmltk --logs .mmltk-data/logs/rfdetr.log \
  -q '"rfdetr compilation"' \
  --fields region,owner,reason,training,signature,grad,amp,amp_dtype,torch,graph_available,operators,fusion_groups,tensor_expr_groups \
  --format jsonl --limit 40
```

Supply the actual captured native application file. `region` identifies the
backbone, per-layer decoder tail or segmentation spatial/query region; `owner`
distinguishes instances within the process. `preparation` reports armed,
disabled or identical requests. Execution `reason` distinguishes recorded
graphs, reuse with an optimized graph when available, signature/prepared-batch
fallback and owner-device invalidation. `signature`, `training`, `grad`, `amp`
and numeric `amp_dtype` describe actual invocation state. `torch` is the
compiled version macro, while the [payload manifest](../docker/nvidia-payload.json)
owns the complete pinned build identity.

`executor_optimize`, `fuse_cpu`, `fuse_gpu`, `tf32_matmul` and `tf32_cudnn`
report runtime policy. `graph_available` and `operators` describe the inspected
graph. `fusion_groups` recursively counts `prim::FusionGroup`,
`prim::CudaFusionGroup` and `prim::TensorExprGroup`; `tensor_expr_groups`
separately counts the last form. Reuse queries inspect the last executed
optimized graph when available, rather than treating the recorded graph as
optimization evidence. Enabled graph inspection is diagnostic work; disabled
debug logging collects and formats none of it.

Capture/reuse, fuser availability and graph fusion are different facts. Fused
ATen operators such as SDPA are not graph fusion groups. Neither these counts
nor the native TorchScript path establish a throughput advantage over upstream
Dynamo/Inductor. [Validation](validation.md#training-mathematics-and-compilation-evidence)
owns the actual observed fusion boundary.

## Benchmark compilation traces

The [dataset runtime's owned target](architecture.md#native-domain-work) connects
GUI compilation to the shared diagnostic sink. No pixel-trace flag is needed for
benchmark records. CLI benchmark compilation supplies its trace callback
when native logging is enabled at `trace`, for example with `--log-level trace`.

GUI records use `kind: "benchmark_dataset"`, a top-level `name`, nested `fields`,
and `steady_ns` from the shared runtime sink. The query tool normalizes `name`
to `@event`; fields can be queried directly. Select one capture so repeated
artifact names and monotonic timestamps do not join different runs.

| Events or fields | Observed work |
| --- | --- |
| `benchmark.compile.paths` | Resolved `cache_root` and compiler `output_root`, with separate `*_truncated` and `*_utf8_replaced` diagnostic flags |
| `benchmark.progress.activity` | Current `activity`, numeric `phase`, and optional numeric `source` |
| `benchmark.download.cache_hit`, `.preseeded` | Retained archive `artifact`, `bytes`, and admission `integrity` |
| `benchmark.download.progress` | `artifact`, `completed_bytes`, `total_bytes`, `attempt`, `resumed`, `retained_bytes`, `durable_bytes`, and `redownload` |
| `benchmark.download.segmented_resume_state`, `.partial_checkpoint` | Retained range state or ordinary-transfer checkpoint and resume eligibility |
| `benchmark.download.attempt_failed`, `.segment_retry`, `.segmented_fallback` | HTTP/CURL failure, discarded partial state, or fallback context where available |
| `benchmark.images.cache_scan`, `.cache_reuse`, `.progress` | `source`/`shard`, inspected/reused/resolved images, and selection counts |
| `benchmark.images.validation_failed` | Rejected archive `member`, `image_id`, `source`/`shard`, encoded `bytes`, and `reason` |
| `benchmark.archive.scan`, `.extracted`, `.extract_cache_hit` | Archive traversal or annotation-member extraction/reuse |
| `benchmark.archive.admission_wait` | Source admission snapshot: `bytes`, `descriptors`, `committed_descriptors`, `descriptor_capacity`, `producer_ceiling`, and `active_cpus`; `reason` is `source_admission` |
| `benchmark.annotations.originals_wait`, `.originals_begin` | Entry to stock-original lifecycle admission, then admitted work with `recover_dropped_masks` and numeric `validation` |
| `benchmark.annotations.release_metadata`, `.release_complete` | COCONut release `edition` and `cache_hit` at metadata and full-mask completion respectively |
| `benchmark.annotations.preparation` | COCONut `edition`, preparation `stage`, `completed`, `total` (zero means unknown), and `unit` (`rows` or `archive entries`), independently of normalized-row completion |
| `benchmark.annotations.progress` | COCONut normalized `completed_rows` and `total_rows` by `edition`, including the first completed row |
| `benchmark.archive.resolution` | COCONut `edition`, physical `artifact`, requested `image_id`, and archive cursor `entries`; zero announces source admission, positive values observe inspected headers |
| `benchmark.annotations.component_admitted` | Reused COCONut component `edition`, physical `source`, and `images` |
| `benchmark.annotations.assembled`, `.workspace` | Assembled image/box/run counts and retained vector capacities, or a parser lane's `retained_sparse_bytes` |
| `benchmark.labels.placed` | Completed final split placement: `split`, `images`, `labels`, and `runs` |
| `benchmark.images.archive_retry`, `benchmark.pixel_compile.cache_repair` | Bounded image/archive repair context |
| `benchmark.download.failure_sha256`, `benchmark.images.failure_sha256` | Failure digest and cause for an artifact or image; COCONut annotation repair additionally reports `matches_expected` |
| `benchmark.pixel_compile.throughput`, `.complete` | Completed/total images where available, `split`, elapsed seconds, images/second, and ETA observations |
| `benchmark.storage.reserved`, `.projection` | Outstanding physical-growth reservation (`required_bytes`, aggregate `reserved_bytes`, `available_bytes`) or projected output bounds |
| `benchmark.publication.complete` | Successful final publication facts |

For a capture produced by [the GUI example](#capture-one-reproduction):

```bash
./mmltk --logs .mmltk-data/logs/gui-trace.jsonl \
  -q '@event=benchmark.compile.paths OR @event=benchmark.progress.activity' \
  --format timeline --limit 40
./mmltk --logs .mmltk-data/logs/gui-trace.jsonl \
  -q '@event:benchmark.download AND artifact="objects365-v2-train-patch-17"' \
  --fields @event,artifact,completed_bytes,total_bytes,attempt,resumed,retained_bytes,durable_bytes,redownload \
  --format jsonl --limit 60
./mmltk --logs .mmltk-data/logs/gui-trace.jsonl \
  -q '@event:benchmark.images OR @event:benchmark.archive' \
  --where 'source="objects365" AND shard="patch-17"' \
  --format timeline --limit 40
./mmltk --logs .mmltk-data/logs/gui-trace.jsonl \
  -q '@event:benchmark.annotations OR @event:benchmark.labels OR @event:benchmark.pixel_compile' \
  --format timeline --limit 60
./mmltk --logs .mmltk-data/logs/gui-trace.jsonl \
  -q '@event=benchmark.archive.admission_wait' \
  --fields @event,reason,bytes,descriptors,committed_descriptors,descriptor_capacity,producer_ceiling,active_cpus \
  --format timeline --limit 20
```

[Compilation progress](benchmark-datasets.md#reading-compilation-progress) defines
actual-write bytes, unknown totals, retry withdrawals, independent tracks, and
source aggregation. Release/pixel/acquisition records can interleave; phase names
are not exclusive execution states. These observations prove neither performance
gains nor, from unchanged fractions or absent best-effort records, deadlock or
network liveness.

Admission snapshots describe the shared ledger at an observation point, not
process RSS, filesystem-wide reservations, or a deadlock diagnosis. The
[resource reference](benchmark-datasets.md#memory-descriptors-and-storage)
distinguishes transient workspace from retained canonical products.
Metadata, originals, normalization, labels, and pixels can progress separately;
`release_complete` is not a prerequisite for every image's label conversion.
Preparation and physical-archive observations also update the visible source
activity. Their row counts and archive cursors describe that specific stage;
they are not extra normalized rows. Positive cursor observations establish
that archive traversal advanced, while a lone zero entry may precede a resource
or lifecycle wait. These work-based updates use bounded quanta without a timer
or heartbeat, and the UI can coalesce intermediate observations.

Failure hashing follows [cache repair policy](benchmark-datasets.md#persistent-cache-and-publication).
Pinned annotation checksums used to decide replacement remain required without
tracing. Optional diagnostic hashes are collected only with a trace sink;
enabling logs does not authorize cache reuse or repair. `failed.txt` is a
separate append-only [omitted-object report](benchmark-datasets.md#native-import-and-provenance),
not a trace or current recovery-count ledger.

Transfer observers exist only for progress/tracing; unobserved segmented downloads
skip progress locking and clock reads. Trace construction/serialization/callback
failures are contained, with one delivery attempt. Invalid payloads go to
diagnostic failure handling, never successful empty records or compilation
failure. Complete-delivery acceptance may independently fail its evidence gate.

## Delivery and acceptance ownership

Ordinary enabled runtime diagnostics are bounded and best effort. The existing
background writer has 256 queued records of at most 16 KiB each; contention or
capacity pressure may drop ordinary diagnostics. Disabled execution creates
none of that active state.

Explicit Wayland integration with a lifecycle sink selects complete delivery
on this same bounded queue and writer. It may wait for capacity. Encoding or
delivery failure is an acceptance failure, and shutdown drains the writer.
Complete delivery is an acceptance mode, not the ordinary GUI's policy.
Sources are [diagnostics_client.h](../src/controller/services/diagnostics/diagnostics_client.h),
[runtime_diagnostics.cpp](../src/controller/services/diagnostics/runtime_diagnostics.cpp),
and [desktop startup](../src/entrypoints/desktop/browser_runtime_entry.cpp).

Quiet acceptance still uses real receipts/input, accepted-unsent pressure,
command settlement, and exact drawn-frame readiness. Its driver is separate
from [Rust reporting](../src/frontend/iced/src/integration_control/reporting.rs)
and [JavaScript reporting](../src/frontend/iced/src/integration_control/browser.mjs).
Disabled reporting performs no payload callbacks, phase/revision/style
deduplication, passive viewport/gallery scans, benchmark widget measurements,
or sink work. Failed control receipts still retain static source-line context
and up to 4096 UTF-8-safe detail bytes, including available native error
kind/title/body, without reporting/probes. Successful receipts have no failure
payload; inactive drivers collect none.

The Wayland session assigns one writer to each artifact under
`build/validation`:

| Default family member | Writer and contents |
| --- | --- |
| `latest-wayland-test.jsonl` | Native runtime lifecycle sink |
| `latest-wayland-test-acceptance.jsonl` | Acceptance process decisions, stage blockers, and terminal evidence |
| `latest-wayland-test-native.log` | Native host stdout/stderr |
| `latest-wayland-test-application.log` | Native application logger |
| `latest-wayland-test-firefox.log` | Captured Firefox output and browser evidence |
| `latest-wayland-test-mozilla-…moz_log` | Explicit Mozilla module logs, with separate process/child and rotation identities |

The harness rejects aliased writer destinations and rotates previous family
members into their `.history` directories. The
[wrapper test transcript](#wrapper-build-test-and-tidy-transcripts) is another
source; it does not substitute for independently owned native or browser
evidence. The headless supervisor owns a separate unique artifact
directory, described in [headless Wayland](headless-wayland.md).
Use [capture selection](#select-captures-and-histories) to find the scenario's
archived family after later browser lifetimes have rotated it.

Enabled acceptance requires complete physical and rendered evidence.
Missing-record exceptions cannot establish a successful handoff or sample.
Ordinary best-effort captures and the query tool's generic hypotheses have
different purposes from these strict acceptance assertions.

## Physical presentation evidence

```bash
./mmltk --logs --family latest-wayland-test \
  -q '@event=presentation.frame.edge' \
  --correlate @workspace_source+transfer_sequence --format timeline --limit 80
./mmltk --logs --family latest-wayland-test \
  -q '@event=acceptance.physical_inventory' --format jsonl
./mmltk --logs --family latest-wayland-test \
  -q '@event=iced.surface.draw_submitted' \
  --correlate @surface+draw_identity --format timeline --limit 60
```

The packaged harness independently joins native source admission,
`workspace_allocation`, exact acquisition and ready/release, Firefox forwarding,
child dispatch, and physical read settlement. Copy mode also requires actual
copy completion. Arena identity does not replace producer-source identity.
Capacity retry can repeat a logical publication with a new `transfer_sequence`;
source plus transfer identifies that physical attempt. Unacquired offers own
no GPU read. Acquired direct reads require `firefox.workspace.read_settled`;
copied samples require `firefox.workspace.copy_completed`, each after matching
forwarding and child-dispatch evidence.

Each enabled Iced draw has a diagnostic-only `draw_identity` that joins exact
selection/acquisition, `draw_encoded`, `draw_submitted`, and terminal
`draw_settled` or `draw_abandoned` observations. Submission is recorded after
the actual queue call. Independent draws may settle out of order; their exact
identities preserve the join. Resource holds and physical timelines govern
release even with diagnostics disabled.

`acceptance.physical_inventory` reports these observed facts:

| Fields | Interpretation |
| --- | --- |
| `live_workspaces`, `workspace_bytes` | Nonretired admitted native source allocations and their allocation bytes |
| `live_sample_arenas`, `live_sample_slots` | Live copied-sample arenas and their physical sample capacity; direct source wrappers are separate from sample storage |
| `completed_browser_copies`, `settled_direct_reads` | Fully joined copy receipts or direct GPU-read settlement, according to the acquired mode |
| `settled_release_only_reads` | Acquisitions returned through the physical ownership-release path without a displayed sample |
| `unacquired_offers` | Observed offers without an acquired/releasing transfer |
| `encoded_draws`, `settled_draws`, `abandoned_draws` | Actual Iced encoding and its terminal resource-custody outcomes |
| `final_reader_releases` | Exact samples returned after their final read hold |
| `explore_peak_pinned_bytes` | Observed Explore staging high-water footprint |

These retained-session lifetime/inventory facts and cumulative operation counts
measure neither latency nor throughput and do not count every internal algorithm
transfer. Workspace allocations,
raw pools, and copied sample storage remain separate. A complete direct-read/draw
chain establishes its physical path; absent best-effort events cannot prove zero
copies. [Draw settlement](gui-interaction.md#browser-draw-eligibility-and-retained-fallback)
releases resources; rendering additionally requires matching image/pixel evidence.
Independent drains retain incomplete joins across scenario/source retirement
until final settlement; missing evidence fails acceptance.

The acceptance-only undersized-candidate gate uses typed control to hold and
release a synthetic pending allocation while the completed fallback is drawn.
It is independent of product acknowledgement and render cadence. Empty-gallery
acceptance likewise waits until paired zero-match atlas metadata reaches its
exact draw before consuming the empty notice.

Show FPS is a separate opt-in product meter. Its submission count does not
replace these draw and lifetime records. The
[FPS behavior](gui-interaction.md#browser-redraws-and-fps) and asynchronous
acceptance canvas probe have separate activation and ownership.

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
```

## Start with a bounded investigation

```bash
./mmltk --logs --family latest-wayland-test --triage
./mmltk --logs build/validation/latest-wayland-test-firefox.log --triage
./mmltk --logs --family latest-wayland-test --triage -q 'probe_failed OR "rendered probe"'
```

Triage ranks explicit failures, assertions, terminal events, damaged records,
and incomplete stages, then selects one run and follows bounded identity
chains. Each evidence row explains its inclusion. Use `--query` to narrow
anchors and `--where` to constrain every pass.

For a quiet build or test interval, the wrapper's
[process snapshots](commands.md#process-snapshots) show current activity in
this repository's running containers. Use the snapshot alongside the captured
command log; elapsed time or one wait-channel observation does not establish
a stalled operation.

A single-file triage input discovers same-family current siblings, or only
the matching archive batch for an archived file. It does not mix an
archived-only input with the current run. Added sources carry `@discovery`
metadata; rotation-based relationships remain labeled as inferred. Ordinary
explicit path queries do not automatically add sibling files.

Paste a complete Status notice, including title and blank line:

```bash
./mmltk --logs --error 'Presentation unavailable

Explore requires a measured non-empty gallery.'
```

`--error` searches whitespace-normalized full phrases first, accepting the
colon used between logged titles and bodies. If no phrase matches, it reports
an all-words fallback. It includes histories, prefers physical records over
copied Catch context, and focuses the earliest matching capture by file
modification time. It preserves original file/line and timestamp/clock data.
Combine it with `--triage` for automatic identity investigation around that
error.

## Catch selection and terminal evidence

The [log parser](../tools/log_query.py) treats Catch's **No test cases matched**
as `catch.test_selection_failed` with error level. **No tests ran** becomes an
error `catch.summary` with `tests: 0`. Both appear under `--errors` and in
terminal context, alongside existing Catch assertion failures. The original
selection text remains in `message`; `catch.filters` retains the requested
filter. A normal summary or successful log parsing does not establish that the
intended cases ran.

```bash
./mmltk --logs build/logs --errors \
  -q '@event:catch.test_selection OR @event=catch.summary' \
  --format timeline --limit 20
```

Use the [test-selection syntax](validation.md#selection-environment-deadlines-and-debugging)
for a comma-separated union of case names, and inspect the selected command's
actual terminal result.

Catch assertion/exception headers retain `source_file` and `source_line`,
including uppercase `FAILED:` or `FATAL ERROR:` and a header with no inline
message. A dashed report title followed by its source location restores the
reported test identity when delayed stderr follows another test's stdout
`RUN` record. Following exception text stays under that test context. The
exact `{Unknown expression after the reported line}` placeholder becomes
`catch.expression` transcript text. This narrow handling does not make
malformed JSON diagnostics valid; genuine malformed/truncated records still
appear under `@parse_error` and fail `--strict`.

```bash
./mmltk --logs build/logs/test-investigation.log \
  -q '@event=catch.assertion_failed OR @event=catch.assertion_fatal_error' \
  --fields @event,@test,source_file,source_line,message --context 3 --format timeline
```

### JavaScript and Rust outcomes

TAP records normalize to `tap.test_started`, `tap.test_passed`,
`tap.test_failed`, and `tap.test_skipped`; `SKIP` and `TODO` remain skipped
observations. `tap.summary` marks nonzero failures/cancellations or zero tests
as failure candidates. Indented TAP diagnostic fragments, including assertion
objects and diffs, remain searchable transcript context under the named test
instead of becoming malformed JSON.

Rust libtest results normalize to `libtest.test_passed`, `libtest.test_failed`,
and `libtest.test_skipped`, with `libtest.summary` preserving the overall
result. Named stdout/stderr sections restore test context for following panic,
assertion, and left/right values. Query `@test` to collect that context:

```bash
./mmltk --logs build/logs/browser-app-investigation.log --errors --format timeline
./mmltk --logs build/logs/browser-app-investigation.log \
  -q '@test:"atlas_pixel_evidence"' --format timeline --limit 40
```

Use the actual command transcript path. Failure-related words inside passed or
skipped test names, successful summaries, compiler progress, and numbered
compiler source excerpts are not failures. Explicit failing outcomes, compiler
errors, and failed build terminals retain their error classification.

## Query expressions

```bash
./mmltk --logs --errors --tail
./mmltk --logs -q 'onnx OR "CUDA error"' --context 2
./mmltk --logs -q '@event:shutdown AND NOT @event:started' --format timeline
./mmltk --logs -q 'duration_ns>=1000000' --fields @event,duration_ns,trace_id
./mmltk --logs --where '@file:"latest-wayland-test"' --group-by @event
```

Quote the entire expression for the shell:

```text
expr      := expr OR expr | expr [AND] expr | NOT expr | '(' expr ')'
primary   := text | '*' | has(field) | field operator value
operator  := = != : ~ !~ > >= < <=
```

`NOT` binds most tightly, then `AND`, then `OR`. Adjacent terms imply `AND`.
Bare/quoted text and `:` perform case-insensitive literal substring matches.
`=` and `!=` compare exact typed values; ordering compares numbers.
`~` and `!~` use Python regexes, case-sensitive unless you supply `(?i)`.
Quote values containing spaces or punctuation.

Missing fields do not satisfy comparisons, including `!=`; `NOT` can include
them. `has(field)` tests presence, including null and zero. `--errors` adds
`@error=true`; it and failure-candidate counts are broad search aids, not test
verdicts. Use terminal status and the owning suite's assertions to establish
success or failure. An `error=0` metric alone is not an error.

## Fields and correlation

JSON paths such as `fields.name` work directly. Canonical nonnegative decimal
segments index nested lists: `referenced_objects.0.handle` and
`message_fields.clean_rgba.24` are examples. Negative, out-of-range, or noncanonical
indices such as `01` are missing fields. Unqualified names also look inside
`fields`; qualified paths specify the complete location.

A `message` or `fields.message` containing a complete JSON object of at most
4096 characters is also exposed as `message_fields`. Its original message stays
verbatim. Invalid, oversized, or non-object message content stays ordinary text.
This supports filters and projections over structured diagnostic details
without maintaining an event-specific schema:

```bash
./mmltk --logs --family latest-wayland-test \
  -q '@event=explore.card.pixel_samples AND has(message_fields.clean_rgba.24)' \
  --fields @event,message_fields.compiled_index,message_fields.clean_rgba.24 \
  --format jsonl
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
| `@parse_error` | Damaged/truncated input |
| `@proximity_ns`, `@time_link` | Pasted-error proximity and its time-link method |
| `@triage_reason`, `@triage_payload_truncated`, `@discovery` | Triage inclusion and limits |

`@event` projects canonical [training records](#local-training-failures);
otherwise it resolves wrapped `fields.event`/`fields.name` before top-level names.
Bare searches also examine test/tag/run/signal metadata. Catch INFO copies keep
their original timestamps and are labeled as copies. A `child.signaled` value
of 139 decodes to SIGSEGV and 143 to SIGTERM; the signal does not establish
whether termination was expected.

```bash
./mmltk --logs -q 'trace_id=42' --correlate trace_id --format timeline --limit 80
./mmltk --logs -q '@event=firefox.workspace.ready' --correlate @surface
./mmltk --logs -q 'source_session=42' --correlate source_session+source_instance
```

Ordinary correlation requires a query or error seed. Repeat `--correlate FIELD`
for alternative identities; join field names with `+` for a composite
identity. It joins original matches, excludes empty/zero identities, and
does not expand transitively. Family/history correlations are scoped to each
run. `--where` filters correlated and context rows too.

`@intent_request` projects `control` from `integration.intent` and `value` from
`browser.intent.received`, `.accepted`, `.rejected`, and `.reply_rejected`.
It accepts exact positive uint64 integers or decimal strings, excluding zero,
booleans, overflow, and approximate floating-point values. It is an explicit
correlation field, not an automatic timeline identity: counters can repeat
after a browser peer changes. Select one peer lifetime before querying it.
The following example requires the selected capture to contain exactly one peer
lifetime; `--run` isolates an archive, but does not establish that condition:

```bash
./mmltk --logs --family latest-wayland-test --run ARCHIVE_ID \
  -q '@intent_request=42' --correlate @intent_request --format timeline --limit 60
```

## Vulkan diagnostics and descriptor provenance

```bash
./mmltk --logs --family latest-wayland-test -q '@event=vulkan.validation' \
  --group-by validation_id --representatives --format timeline --limit 40
./mmltk --logs --family latest-wayland-test \
  -q 'validation_id:SYNC-HAZARD' \
  --group-by validation_id --representatives --format jsonl --auto-correlate
./mmltk --logs --family latest-wayland-test \
  -q 'workspace_allocation=34' --descriptor-lineage --format timeline --limit 32
```

Adjacent Rust `VALIDATION` headers, indented bodies, and matching logger object
lines form one searchable `vulkan.validation` record, bounded to 128 lines and
1 MiB. Original text and physical line spans remain available. Overflow becomes
a parse error and subsequent lines remain searchable.

`validation_id` identifies a VUID or SYNC-HAZARD; `message_id`, `api`, and `level`
retain reported facts. `objects` contains typed callback objects and names.
`referenced_objects` separately contains explicitly typed handles mentioned in
the message, such as `VkImage` or `VkSemaphore`. Types normalize to Vulkan
object names and handles to hexadecimal. A name or bare hexadecimal number
does not establish another resource. Both collections share a 64-entry bound;
overflow preserves the original text and reports the limit.

Use `referenced_objects.0.type` or `objects.0.handle` for an exact list entry.
A sole reported image or memory handle is also available as `vk_image` or
`vk_memory`; a sole command buffer is available as `vk_command_buffer`.
The message is a validation-layer allegation; a handle match does not establish
its cause.

`--representatives` retains one first record per `--group-by` value, preferring
physical records to copied test context. Full occurrence counts remain
available as `@representative_count`. Add `api` or `message` when one VUID covers
different conditions. This option cannot combine with `--tail`, `--error`, or
`--triage`.

Automatic correlation adds bounded image/memory creation, allocation, binding,
and direct-binding evidence for exact typed handles. Explicit matching process
IDs may join files within one run; conflicting device, process, source, or
allocation facts reject the join. PID-free stderr yields only labeled
same-file candidates. Handle reuse and ambiguous owners suppress uncertain
matches. Missing provenance is not evidence of an unrelated object.

`--descriptor-lineage` follows Vulkan allocation/export, `SCM_RIGHTS` send
attempts and exact send results, native receipt, CUDA import/mapping, and
retirement. It also understands historical CUDA-export captures. Numeric FDs
are process-local and never join different processes; `fstat` metadata does
not prove common GPU backing. `descriptor_send` is only an attempt. An exact
record-length `descriptor_send_result` with `send_errno=0` proves send success.
The native `retained_memory_descriptor` owns backing; `import_descriptor` is
the duplicate consumed by successful CUDA import.

This report uses retained rows only, subject to `--where`, `--limit`, and
`--top`; missing and ambiguous links stay explicit. Diagnostics retain no extra
FDs or GPU resources. `loaded_library`/`library_inventory` observe existing
`/proc/self/maps` once at native first import or Firefox first export, bounded
to 1 MiB and 32 paths shorter than 1024 bytes. Query those by process separately
so they do not consume an allocation chain's row budget.

## Automatic timeline context

Human `--format timeline` queries with `--query` or `--errors` add bounded
direct frame, publication, snapshot, and lifecycle matches beside the query
rows. `--no-auto-correlate` disables this addition. Summary and JSONL formats
require `--auto-correlate`; explicit correlation, related-run, pasted-error,
and triage modes retain their own selection behavior.

```bash
./mmltk --logs --family latest-wayland-test \
  -q 'probe_failed' --format timeline --limit 60
./mmltk --logs --family latest-wayland-test \
  -q 'probe_failed' --format jsonl --auto-correlate --limit 60
```

Original matches keep their order and priority. Added rows use spare `--limit`
capacity, are labeled with their reason, and never become new seeds.
Specific recent matches seed one extra streaming pass with bounded nearest
identity indexes; the tool does not build an all-run index. Shared source,
surface, publication, allocation, or trace identities must agree wherever both
records provide them. Bare counters and generic numeric report slots do not
establish identity.

For pixel chains, direct mode compares native source to Iced sample; copy mode
compares native source, Firefox import, mailbox, and Iced sample. The observed
source-to-arena forwarding record and exact physical transfer choose the source.
Direct read settlement and copy completion remain separate mode-specific facts.
Optional valid direct-image and source-copy probes extend those comparisons
without replacing required evidence. The bounded query indexes accept either
arrival order, report conflicting source facts, and distinguish missing
evidence from evidence discarded at an index limit.

Phase/control proximity matches are weaker evidence and are labeled as such.
They use at most 250 ms of comparable time, or eight same-source lines when
clocks cannot be aligned. Cross-clock identity matches never align clocks.
`--where` constrains every added row. Current limits and event-specific
identity projections are in `./mmltk --logs --help`.

## Select captures and histories

```bash
./mmltk --logs --family latest-wayland-test --history --list-runs
./mmltk --logs --family latest-wayland-test --run current --errors
./mmltk --logs --family latest-wayland-test --errors --related-run --tail
./mmltk --logs build/validation/viewer-copy-ownership-trace.log \
  --family latest-wayland-test --history -q 'buffer="BufferId(21,1)"' --context 2
```

Use a returned archive ID with `--run` to select a historical capture;
`--run` implies `--history`. `--family STEM` selects the artifact members above,
plus `STEM.log`, under `build/validation` or an explicit repository path.
Families are repeatable. `--history` adds rotated `.history` siblings.
Mozilla main/child filenames retain physical and logical child identities;
each process can have four bounded module-log rotation files. Native
application rotations `STEM-application.1.log` through `.5.log` also belong
to that family.

Each new logged Wayland browser lifetime rotates the prior family, including
successful workflow captures. The current `latest-wayland-test` members therefore
describe the latest logged lifetime, not every scenario in the suite. Successful
caption and other browser diagnostics live in the matching Firefox artifact;
the Catch transcript need not repeat them. List the runs, select the workflow archive,
then query its successful observations explicitly. For example, replace
`ARCHIVE_ID` with the returned identity:

```bash
./mmltk --logs --family latest-wayland-test --run ARCHIVE_ID --strict \
  -q '@event=integration.workflow.caption_pixels' \
  --fields @event,case,stage,patches,backgrounds,glyphs,compared,verified \
  --format jsonl --limit 100
```

A later capture's empty result says nothing about earlier records. `--strict`
checks parsing, independently of record presence, rendered assertions, and test
outcome.

Failure excerpts in a Catch transcript or a bounded query may omit the decisive
browser records. First use the transcript's test name, source location, and
terminal status to identify the failure, then list the artifact-family histories
and select its archive. Query the native/acceptance/Firefox siblings for that
lifetime; increasing a transcript excerpt cannot recover records that only
the owning artifact contains. A historical transcript that was never fully
captured remains partial evidence.

The current Wayland harness uses one rotation identity for its artifact family.
Older or independently produced captures can have separate rotation IDs.
The tool groups adjacent same-PID rotations within 10 ms and labels that
relationship inferred. Identical event/steady-time anchors join transcript
segments to native captures and
propagate known tests/tags; missing anchors leave them separate. IDs and
monotonic timestamps may repeat between runs, so select one reproduction.
`--related-run` includes other records from a matched run.

Inputs are repository-relative/absolute paths or quoted globs inside the
repository. Explicit files may have any suffix. Directories select
`.jsonl`, `.log`, `.out`, `.txt`, and Mozilla process logs; `--recursive`
includes nested histories. Repeated files are deduplicated. Direct history
directories can be queried even when the current artifact is absent.
The default is `build/validation` without recursive archived captures.

## Triage conventions and limits

| Option | Default | Range |
| --- | --- | --- |
| `--triage-anchors` | 4 | 1–12 |
| `--triage-identities` | 12 | 1–64 |
| `--triage-hops` | 2 | 0–3 |
| `--triage-gap-ms` | 1000 | 1–600000 |
| `--near-ms` | 1000 | 0–600000 |
| `--limit` | 20 | 1–10000 |
| `--top` | 5 in triage, 10 otherwise | 1–100 |

Triage follows strong run-qualified surface IDs, typed handles, nonempty
`*_id`/`*_identity` fields, and source-session composites. Ambient process,
thread, device, user, and session IDs are excluded unless explicitly selected
with `--correlate`. Related identities co-occurring on evidence can seed the
next hop. Sequence/generation/slot values only join as nearby multi-field
composites; they do not start further identity expansion.

Generic event suffixes identify possible unclosed starts, duplicate endings,
failed textual outcomes, incomplete stages, and handoffs. Lifecycle balances
use the most specific operation/resource identity and exact event families.
Hand-off hypotheses require shared identities without conflicting operation
IDs. Copied INFO does not inflate balances; numeric outcome enums are not
guessed. These are investigative leads: disabled diagnostics, truncated
captures, or a completion in another source may explain missing counterparts.

Gap findings require comparable timestamps in the same physical source.
Triage also reports bounded terminal evidence independently of the row budget;
without terminals, it shows source ends without inferring successful exit,
timeout, or failure. Internal identity/event/payload capacities are bounded,
and reports state when those limits truncate evidence.

`--triage` rejects `--related-run`, `--tail`, and `--list-runs`. In JSONL mode,
only bounded evidence rows go to stdout; triage summaries go to stderr.

## Time, output, and exit status

Ordinary time ordering groups steady, UTC wall, timezone-free wall, per-file
elapsed, and untimed records separately. It does not guess clock offsets.
`--order file` preserves physical ordering; `--order capture` orders files by
captured modification time, then by line. Modification time describes an
artifact, not causality. `--tail` selects the last records of the chosen order
and prints them ascending.

Pasted-error lookup uses `--order proximity` by default. Same-clock distances
use recorded times. Cross-clock estimates align each source's last timestamp
with its file modification time; untimed rows may borrow a preceding timestamp
for proximity only. `@time_link` labels these estimates. Proximity ordering
requires `--error` and does not establish causal order.

`--format summary`, `timeline`, and `jsonl` control output.
`--fields` projects comma-separated fields while retaining provenance;
`--group-by` controls summary groups. `--context` includes up to 100 nearby
nonblank records on either side in the same file. Counts cover all selected
records even when `--limit` bounds the displayed sample. Group/correlation
cardinality is capped at 10,000.

Malformed or oversized records remain searchable with warnings.
`--strict` makes malformed/truncated input an error. Status 0 indicates
successful output, 1 indicates no matches, and 2 reports invalid queries,
unreadable/changing inputs, or strict parse failures.

The tool uses the existing build image, no GPU or network, and a read-only
repository mount. It reads files only up to captured sizes; multi-pass
correlation requires unchanged inputs. There is no follow mode or persistent
index. Implementation and current help live in
[tools/log_query.py](../tools/log_query.py). Unfiltered `./mmltk --test all`
includes its fixture suite. The standalone `./mmltk --test log-query-tool`
route and focused selectors are documented under
[test selection](validation.md#selection-environment-deadlines-and-debugging).
