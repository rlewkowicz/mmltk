# Explore and compiled-image streaming observations

Phase 1 inspection, 2026-09-05. Source baseline: `d8533040`. This is an
observational report, not a throughput or latency benchmark. Proposed outcomes
below remain subject to the later implementation and Final Validation phases.
`CONTRACT.md` governs ownership and observable behavior.

## Captures and limits

The new read-only command completed successfully:

```bash
./mmltk --diagnose-io ./compiled/train.bin
```

Raw local evidence is in
`.mmltk-data/logs/phase1-io-capabilities-20260905.json` (initial capture) and
`.mmltk-data/logs/phase1-io-capabilities-final-20260905.json` (final diagnostic
revision, including immutable image identity). These are local generated
reports, not required repository artifacts. The command uses an existing
development image, a read-only filesystem and dataset mount, utility GPU
visibility, and no network. It does not read dataset contents, allocate CUDA
buffers, run vendor experiments, build, pull, or change the dataset. Individual
external queries have five-second limits. Docker/container startup errors
remain explicit stderr failures; they cannot produce in-container JSON.

Before the packaged observation, historical files were copied without
overwriting existing evidence:

| Evidence | Provenance and interpretation |
| --- | --- |
| `phase1-historical-gui-trace-20260905.jsonl` | Copy of the 1,017-line, 254,336-byte September 3 native capture; acceptance events show it is not a fresh manual reproduction. |
| `phase1-historical-firefox-20260905.log` | Copy of the 86,992,242-byte September 5 Firefox log; includes accumulated integration runs and repeated frame/slot observations. |
| `phase1-packaged-gui-20260905.log` | Wrapper output from a fresh, bounded packaged GUI launch; no build or test invocation. |
| `phase1-fresh-gui-trace-20260905.jsonl` | Copy of the fresh 12-line native capture, also left at `gui-trace.jsonl` after the run. |

All these paths are under `.mmltk-data/logs/`. The fresh launch used:

```bash
timeout --signal=INT --kill-after=10s 30s env \
  MMLTK_GUI_TRACE_FILE=.mmltk-data/logs/gui-trace.jsonl \
  MMLTK_FIREFOX_LOG_FILE=.mmltk-data/logs/firefox.log \
  ./mmltk --gui
```

The wrapper selected the existing packaged browser host, discovered
`/run/user/1000/wayland-0`, and the native trace recorded a browser peer and
1500×1100 renderer observations. No Explore generation appeared without
interaction. The observation therefore establishes packaged startup, not tile
completion or shutdown correctness. The outer timeout returned 124 after the
intended observation window. The native trace contains no shutdown event.
The Firefox log's historical tail remained unchanged; its old channel errors
are not fresh shutdown evidence. No packaged browser-host process remained
visible after the wrapper returned.

`MMLTK_GUI_TRACE_FILE` is the supported variable in the wrapper and
`src/controller/services/diagnostics_client.cpp`. The removed
`MMLTK_DIAGNOSTICS_FILE` is deliberately not an alias.

## Observed storage and GPU capabilities

| Boundary | Observation | Limit on inference |
| --- | --- | --- |
| Dataset | `compiled/train.bin`, 320,898,662,016 bytes, regular file | Format/content validation was not attempted. |
| Kernel | Linux `7.1.7-200.fc44.x86_64`, x86-64 | Host kernel shared by the container; development userspace remains independently packaged. |
| GPU | RTX 3090 Ti, driver `610.57.04`, 24,564 MiB reported | Model and driver presence do not establish native GDS support. |
| GPU PCI | `0000:02:00.0`, parent `0000:00:01.1`; maximum 16 GT/s ×16, current observed 2.5 GT/s ×8 | Snapshot of link state; no load or negotiated-bandwidth conclusion. |
| Filesystem | Btrfs, mount source `/dev/nvme0n1p3[/home/ryan/Repos/win/cplusplusloader/compiled/train.bin]`, `compress=zstd:1` | Mount options do not prove this file's extents are compressed or establish all members of a multi-device filesystem. |
| Block device | `nvme0n1`, Samsung SSD 980 PRO 1TB, 512-byte logical/physical sectors | Sector size is not the same as required direct-I/O alignment. |
| PCI storage | Five container-visible NVMe controllers, each showing maximum/current 16 GT/s ×4; sysfs reports parent paths | Full ACS/IOMMU/peer-DMA accessibility and file-to-physical-extent routing were not established. |
| Memory lock | Diagnostic soft/hard limit 8,388,608 bytes | Applies to this diagnostic container, not a measured CUDA allocation limit or the production process's usable pinned-memory budget. |
| cuFile | Linker cache and conventional CUDA paths contain `libcufile.so.1.19.0` and RDMA library | Library availability does not establish selected native versus compatibility route. |
| GDRCopy | Linker cache contains `/opt/nvidia/lib/libgdrapi.so.2`; `/dev/gdrdrv` not visible | CPU BAR-copy library, not disk-to-GPU DMA; absence of the legacy node alone does not rule out another supported backend. |
| Kernel modules | `nvidia`, `nvme`, `nvme_core` visible; `nvidia_fs`, `gdrdrv`, `nvidia_peermem` absent from observed module list | Host module visibility and library presence are separate facts; native P2PDMA modes need not use `nvidia_fs`. |
| Direct I/O | `O_RDONLY | O_DIRECT` open accepted; no content read | No claim that an aligned read, particular file extent, or GPU destination uses direct I/O. |
| Alignment | `STATX_DIOALIGN` absent from returned mask | Information unavailable for this file/kernel/filesystem; not a finding that all direct I/O is unsupported. |

The manifest `docker/nvidia-payload.json` selects CUDA 13.4.1 and does not
enumerate cuFile or GDRCopy as explicit named components. The cached development
image nevertheless contains both libraries. The report records image identity;
do not infer runtime-image contents or why a library entered the cached image
from the manifest's named component list.

NVIDIA's GDS release notes distinguish native DMA capabilities and compatibility
behavior, including Btrfs compatibility support and newer NVMe P2PDMA routes.
Neither general Ampere architecture nor the successful open proves this exact
3090 Ti/Btrfs/topology combination supports a native path. No production GDS
backend is proposed here.
[NVIDIA GDS release notes](https://docs.nvidia.com/gpudirect-storage/release-notes/index.html).

GDRCopy gives CPU access to GPU mappings; its current repository also describes
a DMA-BUF backend with newer driver requirements. It is a different facility
from storage-to-GPU DMA. Component discovery does not establish that either
backend is usable in this container.
[NVIDIA GDRCopy](https://github.com/NVIDIA/gdrcopy).

`STATX_DIOALIGN` reports alignment only when returned in `stx_mask`; returned
zero alignment fields indicate unsupported direct I/O. `O_DIRECT` behavior can
vary with filesystem, kernel, alignment, and compressed extents. The diagnostic
only opens read-only and requests metadata; it does not attempt an aligned
read. [statx(2)](https://man7.org/linux/man-pages/man2/statx.2.html),
[open(2)](https://man7.org/linux/man-pages/man2/open.2.html).

## Image completion evidence and missing boundaries

Native diagnostic `sequence` represents generation for Explore records.
Presentation uses its own revision/sequence and native-frame value; these
numbers must not be joined by equality across owners.

| Stage | Current source and historical evidence | Required later evidence |
| --- | --- | --- |
| Desired view | `ExploreSystem::Render` / `GalleryStream::Begin`; `placeholder.published` and `render.completed` at generation 76, historical lines 999–1000 | Desired generation, exact slot set, image IDs, retained versus loading state, admission and stale rejection. `render.completed` alone is not all-tile completion. |
| File read | `GalleryStream::ReadLanePayload`, `native_explore_algorithm.cpp:1328`; acceptance read events map generation/slot/image | Ordinary gated read start, actual read completion, donor admission, cancellation and failure; existing detailed read events require acceptance configuration as well as logging. |
| Concrete correlation | Historical line 949 records `acceptance.compiled.read.completed`: generation 75, slot 5, compiled image 104 | Preserve that identity through augmentation, matching semantics and GPU completion, not only into a batch counter. |
| GPU submission | `PrepareBatch`, source/donor H2D, augmentation and rendering on owning stream | Per-submission generation/slot/image and callback admission/failure; distinguish enqueue from finished GPU work. |
| GPU completion | `CompleteLaneCallback` / `CompleteLane`, around line 1385, changes lane state and calls ready sink | Completion notification, drain-start/drain-end and arrivals while draining; stale completion must still release the physical lane. |
| Native publication | Generation 75 `tile.batch.published` reaches cumulative 12 at line 966; generation 76 subsequently starts | Exact ready bits and matching semantic revision, immutable publication identity, rejection of stale publication, borrow/release and receiver-copy completion. |
| Presentation borrow | Historical line 1014 is `presentation.source_borrow.started`, presentation sequence 11/native-frame value 25, with no matching completion before capture ends | Owner-lock/product-lock acquisition boundaries and resource identity. An unmatched event in a finite capture alone does not prove deadlock. |
| Browser capture/redraw | `presentation_surface.rs` frame stream, capture and widget redraw; old Firefox integration lines carry separate browser session/revision/slot/image values | Join native product and browser capture identity; record admission/backpressure, successful retained capture, release, redraw request and completed draw. Current mixed historical logs cannot establish that join end to end. |

`VisualRuntimeOwner::Borrow` holds its scheduler mutex while calling the
runtime's product borrow (`visual_runtime_owner.cpp:179`).
`SubmitContinuation` acquires that same mutex (`:112`). The Explore CUDA
completion callback obtains `lanes_mutex_`, copies a `std::function` ready sink,
releases that lock, and invokes the sink, which schedules a continuation.
GPU submission while holding a product lock can interact with CUDA callback
serialization. This identifies a reachable lock-order hypothesis requiring
deterministic evidence in Phase 4; no deadlock correction was made in Phase 1.
The callback's function copy may also allocate, and scheduler locking is not
appropriate for the proposed bounded notification boundary.

CUDA host functions block subsequent stream work, must not call CUDA or wait
on unordered outstanding work, and are not invoked on context error.
Successful callback admission is therefore insufficient as the only terminal
failure wakeup. Later streaming and completion work must provide failure
propagation without polling or retiring borrowed resources prematurely.
[CUDA host-function contract](https://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__EXECUTION.html).

Two independent image issues are visible in source: augmentation `effects`
always applies ImageNet normalization (`gpu_augment_cuda.cu:180`) while Explore
samples its output as unit RGB; and `sample_nchw`
(`explore_render_core.cu:64`) has an incorrect bilinear expression. For
`a=0, b=1, c=0, d=1, fx=0, fy=0.5`, it returns −0.5 instead of 0. Phase 3 needs
deterministic pixel evidence before behavioral correction. These findings do
not justify reducing the saved augmentation color ranges.

## Shutdown trace and demonstrated wrapper failure

The ordinary path is:

```text
SIGINT/SIGTERM → SignalWaiter::sigwait → ApplicationShell::request_shutdown
  → close browser admission → request system stops → stop browser loop
  → browser close → Firefox request_stop/wait → Presentation browser-peer loss
  → Presentation shutdown → independent system joins → diagnostic flush
  → reverse-order RAII resource destruction
```

The signal waiter is in `src/entrypoints/desktop/browser_runtime_entry.cpp:71`.
The shell implements request and join ordering in
`src/controller/shell/application_shell.cpp:188–256`; Firefox process monitoring
and physical child reaping belong to
`src/controller/services/firefox_process_owner.cpp`. Runtime worker stop/wait
belongs to `VisualRuntimeOwner`; product and borrowed-view resources must
survive asynchronous GPU completion.

The fresh run proved an external wait defect: the existing wrapper passed
`2000ms` to GNU `timeout`, which rejected it twice as an invalid time interval.
`mmltk`'s `wait_for_local_pid_exit` uses `"${timeout_ms}ms"` (baseline line 474);
`request_gui_runtime_shutdown`'s nested `wait_runtime` similarly uses
`"${wait_milliseconds}ms"` (baseline line 442). The intended grace wait is not
performed on that path, so subsequent TERM/KILL escalation can precede the
intended waiting period. Phase 4 should correct duration formatting and then
reobserve shutdown before attributing missing trace events to native locks.
This is a proven wrapper error, not evidence that native shutdown itself is
deadlocked.

Neither the historical native tail nor the fresh 12-event trace covers signal
receipt, ingress closure completion, individual producer stop/completion,
Firefox wait completion, worker join, and physical resource release together.
Add gated begin/end/failure records at these owning boundaries, with reason
and resource identity, during Phase 4. `request_shutdown` currently discards
its reason argument. Preserve disabled-logging avoidance of data collection.
Window close and peer loss while work is active remain Final Validation cases.
Historical `corrected-final-wayland-result.txt` says 802 assertions passed in
three cases on September 2; it is not validation of the current implementation.

## Time, storage, and transfer accounting

Let N be dataset images, V desired visible images, B active training batch
size, C donor-cache capacity, P bytes per compiled image, S reusable stream
slots, and A active annotation/RLE bytes. These are source-derived bounds;
there are no measured speedup claims.

| Work | Current implementation | Proposed ownership and bound |
| --- | --- | --- |
| Compiled metadata | Train and Explore separately own validation/mapping/read paths; metadata/order storage O(N), mapped pixel address range O(NP) | `CompiledDataset` shares implementation and immutable indexed views. Metadata stays O(N); no assumption that independent systems share a CUDA runtime or a single process-global mapping. |
| View generation | `GalleryStream::Begin` copies the full annotated catalog O(N), plus visible state O(V) | Reuse immutable catalog; desired-state construction O(V). Dataset/filter/order changes may remain O(N) or O(N log N) when sorting. |
| Contains/Adjacent | Linear `ranges::find` O(N) per request | Inverse order built on order/filter changes, O(N) storage/build and O(1) lookup. |
| Cached donor choice | First eligible circular scan O(BC), worst O(B²) when C=B | O(C) batch index plus O(B) equivalent seeded choices; keep first-eligible circular semantics and invalid/self exclusions. |
| Image gather | Adjacent source runs coalesced into memcpy; O(BP) bytes, at most B run copies | Same unavoidable O(BP) pixel bytes; bounded read-ahead and reusable slots, adjacent ranges coalesced, cancellation between jobs. |
| Donor admission | Explore prepares/read-copies donor payload before augmentation's seeded paste admission | Determine the same seeded admission first; read/upload only required donors. O(V) decisions and bounded pending reads. |
| Host/device buffers | Train slots follow prefetch factor (default 6); Explore keeps lane and atlas high-water buffers | Stream staging O(SP+A), separately owned by each long-running system; atlas remains O(V × tile area). Preserve host batch access and GPU consumer-release lifetime. |
| Read-ahead | Loader prescan issues access-pattern/huge-page advice, not a full pixel prefault | Bounded coalesced ranges; optional aligned prefault only on I/O workers, completion distinct from advice. Resident page-cache memory remains OS-managed. |
| Reconfigure | Augmentation executor synchronizes even for unchanged config | Compare config and make equal configuration a no-op; preserve needed synchronization when resources/config really change. |
| Consumer handoff | Several training paths call `wait_batch` before `LoaderBatchGuard`; loader also offers stream event handoff | Remove CPU waiting only where the existing consumer-stream event supplies ordering and no host access depends on completion. Preserve synchronous API semantics for callers that require them. |
| Completion | Lane scan over bounded active lanes; callbacks schedule owner work | Bounded completion state and event-driven draining; O(V) generation accounting, no O(N) scan per callback or interaction required to finish visible work. |

The current ordinary Train image path is mapped file/page cache → one CPU
gather copy into pinned host storage → one H2D image transfer into slot device
storage → augmentation/model. Storage reads/page faults depend on cache state;
they cannot be counted as fixed disk operations from source alone. Labels,
RLE and augmentation plans have separate metadata costs.

Explore similarly copies each selected image into a pinned lane and uploads
it to source-batch GPU storage; a donor adds its own pixel copy/H2D plus
annotation/mask transfers when prepared. GPU rendering writes clean/semantic
tiles and copies them into reusable tile caches. Retained tiles may incur
device-to-device cache copies when a generation changes. Atlas composition
and Presentation's receiver-owned copy remain explicit GPU work, followed by
Firefox native import and browser-owned GPU copies. Detail also has a small
semantic-count D2H transfer; this is not a CPU image-display fallback.

The proposed shared stream does not claim zero-copy or eliminate necessary
receiver ownership. It retains one pinned gather and one H2D per required
image, avoids unneeded donor work and dataset-wide per-view CPU copies, reuses
storage, and leaves GPU presentation copies intact. Unit-RGB Explore output
should be produced in the existing effects kernel without another full-image
pass.

`MADV_WILLNEED` is advisory; successful advice is not a notification that all
requested data is resident. Optional population is a separate operation with
failure and partial-work behavior. Workers must bound ranges and cancellation
granularity rather than eagerly populating the entire 320 GB file.
[madvise(2)](https://man7.org/linux/man-pages/man2/madvise.2.html).

## Historical and DirectStorage comparison

The oldest surviving commit is `6e6805a1` (`init`). Its
`src/dataset/loader/dataset_loader_pipeline.cpp:46–63` already groups adjacent
images into memcpy runs; `dataset_loader_runtime.cpp:95–113` applies
`MADV_HUGEPAGE` and access-pattern advice. This is evidence of mmap plus pinned
gather, not a lost implementation that previously prefaulted all pixels.

`../DirectStorage/Docs/diagrams.mmd` shows the useful uncompressed path
storage → upload heap → GPU copy → VRAM in its commented branch. Its active
compressed diagram has additional input/output staging and decompression;
that is not the current compiled raw-float format. The adjacent
`Docs/DeveloperGuidance.md` describes bounded queues whose slots remain in
use until completion and grouped submission/completion. Cancellation examples
in `Samples/BulkLoadDemo/BulkLoadDemo/MarcFile.cpp:75–76` use request tags.
The reusable principle is that logical cancellation must still respect
physical completion before reuse.

Transferable concepts are bounded submission, reclaimable low-priority
read-ahead, generation-tagged cancellation, grouped completion, and stable
buffer ownership. Windows-specific APIs, compressed staging, arbitrary queue
multipliers, and vendor benchmark claims are not part of the Linux design.

## Remaining validation

The diagnostic itself ran in the existing container; no full product builds,
tests, sanitizer, profiler, benchmark, or vendor capability experiment ran.
Later phases must verify exact seeded donor equivalence, preserved ordering
and sharding, host availability, error/cancellation wakeup, borrowed-resource
lifetime, normalized Train versus unit-RGB Explore output, and exact visible
readiness. Phase 4 must reproduce the lock interaction before fixing it and
recheck shutdown after correcting the demonstrated wrapper wait defect.
Rendered square geometry, fractional clipping, saved overlay controls,
progressive matching semantics, browser backpressure and redraw without more
input remain end-to-end acceptance requirements. Native GDS/GDRCopy support,
physical I/O route, effective pinned-memory capacity, and throughput remain
unestablished.


## Phase 3A locality and policy implementation

The read-only wrapper report was repeated on 2026-09-05 at 21:20 UTC using the
cached development image `sha256:82d87b3a06538632b5d6097c7f8fe8f771296c7ab9c935e842ef834914a6f81b`.
Online memory nodes and effective memory eligibility both reported node 0;
effective CPUs were 0–23. `nvidia-smi topo -m` reported GPU0 CPU affinity 0–23
and host NUMA affinity 0. PCI sysfs for GPU `0000:02:00.0` reported node -1.
This is the genuinely single-node resolution case. It supplies no evidence
of cross-node performance gains.

The diagnostic container had no capabilities, seccomp mode 2, nice limits 0/0,
and memlock limits 8,388,608/8,388,608 bytes. A read-only `get_mempolicy` query
returned EPERM. The cached image lacked `numactl`; its hardware/policy commands
were explicitly recorded unavailable. Development and runtime Dockerfiles now
install it, but those image changes have not been built during implementation.
The wrapper's runtime/test launches declare SYS_NICE, IPC_LOCK, unlimited
memlock and nice limit 40, with the focused native runner retaining its existing
unconfined syscall policy. These launch declarations do not constitute a
successful page-allocation or priority test.

The common execution component captures CPU/package/core/node relationships and
permitted sets, orders distinct physical cores before SMT siblings, and resolves
strict local placement once for each runtime. Child pools receive the immutable
resolved placement, including training's existing loader/lane/solver slices.
Visual worker policy remains active until runtime retirement; synchronous
training boundaries restore caller policy. Library helper initialization checks
for accidental one-CPU inheritance before runtime workers start. Systems share
implementation without sharing mutable queues, buffers or CPU reservations.

Owned host extents use anonymous mappings, node binding before prefault, and
page-residency verification before publication. Pinned storage registers those
pages in its CUDA owner context, preserving portable registration for compiled
stream consumers. The stream uses bounded borrowed task records and a reusable
FIFO ring, with no allocating lambda capture on image submissions. GPU completion
worker startup is acknowledged before read admission. Failed CUDA settlement or
unregistration retains physical registered storage under the existing terminal
retirement mechanism.

Standard tests cover synthetic sparse/multi-node/SMT/restricted topologies,
invalid overrides, denied required syscalls, partial worker construction,
fixed placement inherited by child pools, PMR alignment, page residency,
high-water reuse and portable registration. These tests and full builds are
reserved for Final Validation; their addition is not recorded as successful
execution. Training LSAP scratch and solver arrays now use runtime-owned per-worker
PMR storage; Phase 3D retains the full matcher metadata and host-boundary cutover. No throughput, latency or
cross-node improvement is claimed.


Phase 3A visual execution remediation adds reflected desktop `--device-id` and
`--numa-node` options, forwarded by `./mmltk --gui` into shell configuration and
every native visual factory. Native factories retain resolved execution across
recoverable reconstruction; the visual owner restores its policy before failure
publication and retry admission. Explore resolves locality before automatic
parallelism selection, and explicit system worker budgets can overlap local CPUs
with stable modulo assignments. Hardware tests distinguish unknown multi-node
automatic rejection from deliberate explicit-node selection. These additions
remain unbuilt and untested pending Final Validation.


Phase 3A distributed and compute remediation materializes reflected rank-ordered
`numa_nodes` overrides into single-device child requests and rejects ambiguous
scalar overrides for multiple devices. Validation/export receive the shell's
resolved execution; LocalRun acknowledges required worker policy before admission,
and direct CUDA runtimes scope construction and operation with that execution.
Training owns one PMR LSAP workspace per solver worker and joins those workers
before releasing node-backed arrays. Pinned growth registers a separately owned
candidate before settling and retiring existing storage, so registration failure
preserves the active allocation and transition failures retain physical custody.
Standard coverage includes distinct-node and unknown-locality rank selection,
compute admission denial before CUDA, solver high-water reuse, and deterministic
registration failure. Validation remains deferred to Final Validation.

## Persistent mapped storage prerequisite (Phase 3B)

The application now vendors the complete tracked GDRCopy source snapshot at
`fcec3ce0bb40a97a6cc45dd4afeec4bccb509712`. The private static archive is built
from a build-directory copy using upstream C and per-architecture copy flags.
Hidden visibility and linker archive exclusion keep it from interposing on the
NVIDIA payload. Runtime packaging carries `LICENSE` and `UPSTREAM.md`; the donor
payload manifest, donor libraries, and both image-base ownerships are unchanged.

Source inspection identified races in CPU dispatch initialization, logging,
debug-once counters, mapping counts, and CUDA function-table lifetime. The local
library initializes immutable dispatch/logging once, serializes control-plane
operations and handle lists, and leaves established independent mappings free
of a global copy lock. A monotonic atomic set of observed mapping types retains
the strong fence conservatively after mixed mappings have existed. This avoids
counter snapshots that could lose a mixed-mapping fence during retirement.

CUDA DMA-BUF CPU mmap support is attribute 152, distinct from generic export
support (124). Both the library and the native allocation owner check actual
mmap support; the owner checks the device of its bound context. Driver 13.3+
and another visible GPU are insufficient. CPU DMA-BUF accesses use Linux
START/END synchronization around GDRCopy's existing dispatch and fences.
Default mappings retain CUDA-selected cacheability. NVIDIA exports descriptors
atomically with `O_CLOEXEC`; the library verifies it and closes uninstalled
exports on failure. These corrections follow the [CUDA memory API](https://docs.nvidia.com/cuda/cuda-driver-api/group__CUDA__MEM.html),
[13.3 release notes](https://docs.nvidia.com/cuda/cuda-toolkit-release-notes/index.html),
and [NVIDIA DMA-BUF export implementation](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/main/kernel-open/nvidia/nv-dmabuf.c).

`GdrMappedBuffer` owns the original CUDA allocation, aligned usable span, pin,
CPU mapping, a fixed capacity of reusable consumer events, and a bounded
terminal-retirement reservation. Construction captures the actual current owner
context; registration and growth bind that context, including isolated Explore
contexts. `SYNC_MEMOPS` applies to the original allocation. The returned mapping
extent must fit that allocation, and CPU addressing includes its returned offset.
Growth prepares a complete replacement before promotion. Read leases retain old
storage, including consumption submitted after promotion. Explicit shutdown
rejects retained current or prior storage. Failed cleanup keeps dependent physical
resources under custody and prevents further growth by that buffer family.

CPU copies return after their stores and required protocol/fences. CUDA consumers
must be submitted afterward and retain a read lease through every consumer, then
record the final work on each stream. Writes reject live CPU leases and wait for
recorded GPU consumption before reuse. An unrecorded or failed-record lease
conservatively settles its owning context. Normal copies through already settled
mappings do not bind a CUDA context or allocate events. Cancellation during a
synchronous CPU copy does not let the caller recycle that storage before return.
Each system still owns and joins its CPU writers before shutdown.

At fixed capacity, one allocation/registration/mapping and a configured number
of events serve repeated copies. Copy work is O(bytes), with O(1) readiness checks
and no allocation when logging is disabled. Recording a consumer searches only
the explicitly bounded stream capacity. Mapping growth is an explicit allocation
boundary; there is no inferred threshold or automatic H2D fallback. The dual
transport stream and matcher integrations remain the subsequent 3C/3D cutovers.

Standard fake cases cover offset/tail addressing, high-water identity, all
construction boundaries, selected-device rejection, transactional failed growth,
old-lease retention, cancellation during a gated copy, consumer-event capacity
and recording failures, context restoration, and retryable release boundaries.
Small hardware cases use a real CUDA kernel to consume written bytes, verify
close-on-exec and closure of descriptors, retain a mapping across growth, and
copy on one isolated context while another registers/retires independent handles.
They are parameterized by startup backend/device environment, not by runtime
mutation of the process environment. Capability skips explicitly leave that
hardware case unverified.

These additions have not been built or executed. Final Validation must run the
new `[gdr]` cases under both supported backend selections, capture gated
`MMLTK_GDR_TRACE_FILE` records when diagnosing failures, and retain the subsequent
transport integration tests. The records identify device/context, backend,
allocation, allocated/mapped capacity, cacheability, owned export count, and
completed bytes. `--diagnose-io` now reports per-device export/mmap capabilities
and descriptor limits, without claiming an allocation or transfer succeeded.
No throughput or latency improvement is claimed.


## Phase 3C source cutover

The canonical native DataLoadingOptions defaults to required GDRCopy and composes
strict NUMA selection. CLI, saved workflow execution settings, child training
commands, distributed rank requests, inference, validation, and test-split loaders
carry that selection. Explore factory construction reads saved execution facts;
changes reopen its dataset after retirement on its owning GPU worker.

Both destinations use CompiledDataset's validated coalesced reads and 16 MiB
advice bound. Each stream slot owns persistent image storage. H2D reads once into
local pinned staging and uses a persistent copy stream; GDR writes source spans
directly to its mapped slot. GDR retains no eager host image allocation. Explicit
DatasetLoader::host_images validates the batch owner and lease, aliases contiguous
source pixels, and materializes a scattered GDR view only on request. Production
training continues to use device_images.

Explore owns stable primary/donor images per lane. Existing pointwise and remap
kernels consume a compact pointer list while output remains compact. Original
cards directly reference their lane storage. Metadata staging has no image-sized
holes, and there is no device repack. I/O completion and transport completion are
separate bounded notifications: slow source observers remain on their I/O workers
and cannot block the single GPU completion worker. Obsolete slots await both
boundaries before reuse; final atlas readiness remains a separate product fact.

Standard source coverage parameterizes stream/epoch cases and native Explore
acceptance over H2D and GDR, and exercises augmentation through permuted physical
input/donor pointers. CPU-view owner/release checks, persisted options, command
forwarding, and runtime reconstruction have explicit assertions. Product builds,
tests, and hardware transfer evidence remain deferred to Final Validation. Binding
generation uses only ./mmltk --generate-application-bindings. No throughput or
latency improvement has been measured or claimed.

## Phase 3D host-boundary and matcher source inventory

This inventory records source ownership and intended completion boundaries. It
is not hardware validation. No builds, tests, throughput measurements, or timing
benchmarks have been run for Phase 3D; the remaining Final Validation phase owns
that evidence. Page locality is enforced by anonymous `NumaMemory` extents bound
and verified before `PinnedHostBuffer` registers them. Torch CPU tensors alias
those extents through `NumaHostTensor`; constructing a tensor view does not copy
its image, cost, or metadata payload. Active tensor shapes are independent of
page-rounded reserved capacity. Escaped tensor views retain their backing
storage, including across owner growth.

| Production boundary / source | Host owner and placement | Capacity and direction | Completion / retained lifetime |
| --- | --- | --- | --- |
| `CompiledDataset` source mappings and indexed reads | Dataset owns mapping lifetime; existing shared file-cache pages retain their actual placement | Validated file spans, CPU reads | Mapping and stream read leases; worker memory policy affects new faults, not existing shared cache pages |
| `CompiledImageStream` H2D images and donor slots | Independent stream slot, receiver GPU's resolved node, portable registered storage | Persistent high-water slot; source read directly to pinned bytes, then one image H2D | Slot copy event, I/O and CUDA observers both finish before slot reuse |
| `CompiledImageStream` GDR images and donors | Independent mapped device slot; source pages remain file-cache pages | Persistent mapped GPU capacity; CPU source writes directly to mapped GPU storage | GDR stores/fences precede CUDA; final stream consumers and CPU leases prevent overwrite |
| `DatasetLoader::host_images` explicit CPU access | Batch/stream lease, local owned materialization for scattered spans | Active batch shape; no eager GPU-training host gather | Batch lease; contiguous source spans alias existing validated source data |
| Augmentation executor keys, parameters, paste parameters, pointer-slot metadata | Executor's local registered `PinnedAllocation` staging slots | Reserved batch capacity, active metadata H2D only | Existing submission guards, staging completion events and slot ownership |
| Training copy-paste maps and donor boxes | `GpuBatchAugmenter` tensors over local registered pages | Persistent batch capacity; active maps/boxes H2D | Existing cache stream, cache-ready and image-read-complete events |
| Target IDs, offsets, counts, boxes, labels, area, crowd flags, mask words, transforms and erasure | `TargetScratch` staging slot, lane GPU's node | Batch/instance/mask high-water shapes; active H2D views | Slot copy event and target-consumer lease through forward/backward |
| Training scalar metrics | `TrainingMetricHandoff`, selected device's local registered tensor | Eight floats, D2H | Existing settlement stream and handoff event |
| RF-DETR matcher costs | Runtime lane `MatcherWorkspace`; standalone calls own a scoped workspace | Flat GPU high-water backing and local pinned CPU view with compact current `[layers,batch,max_queries,max_targets]`; one active-region D2H | One cost-copy event and one CPU completion wait before parallel LSAP reads |
| LSAP doubles, duals, paths, visited arrays, sorted indices and grouped assignments | Runtime CPU worker slot, node-bound PMR resource; standalone scoped solver | High-water PMR vectors, CPU only | Fixed assigned solver worker; parallel task join before returning costs/results |
| Public Hungarian CPU results | Matcher-owned reusable local result extents, tensor-storage retention | One compact local source/target result region, active per-layer/per-batch views | Public CPU tensor lifetime prevents reuse; unchanged public index API |
| Criterion assignment metadata | Immutable lane/result slot, local pinned packed batch/source/global-target arrays | One packed set with layer offsets; one GDR write by default or one explicitly selected pinned H2D upload | Device tensor storage retains the slot through saved autograd indices; explicit lane post-backward event and last CPU owner control reuse |
| Prediction image input | Prediction run, selected device node | One reusable normalized-input host tensor; active H2D | Existing preprocessing stream completion |
| Prediction boxes/labels/scores | Prediction session's `PredictionReadback` owners, selected device node | Three reusable high-water tensors, compact active detections, D2H | One wait after the three current-stream copies before CPU result construction |
| Evaluation fixed bbox/mask readback slots | Evaluation slot pool on explicitly bound device, local registered tensor storage | Reserved batch/prediction/mask capacities, active D2H | Slot ready event and CPU worker lease before slot reuse |
| Evaluation direct result conversion | Scoped local readback tensor per requested result | Active result/mask D2H; CPU conversion storage is output-owned | Synchronous readback boundary before CPU conversion |
| Sample drawing | Draw job's local registered RGB readback tensor, retained by sample writer job | Active RGB image D2H | Existing draw event, settlement stream and pending-write job retain the tensor through encoding |
| Compiler binary mask resize utility | Resizer-owned local registered input/output/width/height/offset buffers | Separate high-water byte capacities; active metadata/input H2D and output D2H | Resizer stream completes before copying public CPU output; fixed CPU policy applied to each resize call. The utility currently has no production caller outside its own definition |
| Explore descriptor family and semantic count | Gallery-owned registered allocation family on Explore node | Descriptor high-water H2D; one semantic count D2H | Existing descriptor-pending, count completion and gallery quiescence boundaries |
| Explore annotation/donor metadata | Gallery's stream metadata lanes and local descriptor storage | Active donor box/mask slices H2D | Existing lane input-read and final atlas completion boundaries remain separate |
| Annotation mask runs | Annotation algorithm's local registered high-water host owner | Active run-pair H2D; native device mask storage retained | Existing mask upload event and runtime resource settlement |
| Capture owned user buffers | Capture host slot owns local anonymous or local portable-registered pages; immutable selected execution supplied to capture worker | Page-rounded fixed capture slots; camera writes CPU pages, ingress H2D | V4L2 stream-off/driver ownership return and ingress leases precede storage release; unregister failure retains the allocation |
| Live manual overlays | Per-output-slot local registered masks/runs/points/edges/brush owners | Fixed bounded upload capacities, active H2D | Slot CUDA stream, ready event, and existing physical resource retirement |
| Live raw frame readback | `LiveRawFrameCache` local registered buffer | Fixed maximum RGB frame, active pitched D2H | Readback callback notifies only after DMA; CPU result is consumed before the next readback |
| Cross-device image staging | Receiver image plane owns portable local registered pages, explicitly using receiver context/placement | Reusable maximum plane capacity; source D2H then receiver H2D | Source event, completed source download, receiver stream completion before borrowed source release |
| Decoded-image publication (`compute_systems.cpp`) and sample-drawing label/color conversion | Existing decoder/Torch CPU result owners; these cached or decoder-returned pages are not local registered transfer extents | Existing CPU-source upload and small label/color conversions retain their format/library ownership | Existing publication stream settlement and drawing stream boundaries; no claim of strict locality for those allocator-owned source pages |
| Native presentation / same-device and peer image copies | Device allocations and foreign native/browser imports remain under their actual owners | Device-local or peer copies; no new host stage | Existing imported timelines, producer completion and receiver-owned copy completion |
| Checkpoint/model/artifact transfers, runtime library initializers and external model outputs | Existing Torch/runtime/file-format or external-library owners | Model/state tensor transfers and framework scalar/metadata conversions retain their library ownership; not an image-transfer staging pool | Existing synchronous serialization, tensor ownership and runtime stream boundaries; no claim that foreign/cached allocator pages are locally owned |

The assignment packing removes repeated batch/source/global-target H2D work in
label, box, dense-mask, sparse-mask, auxiliary and encoder consumers. Sparse
masks scan the retained CPU batch view and select the already-uploaded device
query view. The fused GPU cost kernels and the LSAP algorithm are unchanged;
shrinking a later batch changes active contiguous cost views without copying
old high-water dimensions. Cost D2H remains ordinary DMA with its necessary CPU
completion dependency. There is no default BAR-read path or inferred transport
size threshold.

The runtime owns one matcher workspace for each training lane plus its calling
lane and one PMR solver scratch per CPU worker. Thread-local runtime access only
borrows these owners. Criterion JIT trace caches now share this workspace
lifetime instead of leaked thread-local allocations. Standalone matching owns a
scoped workspace. Primary CUDA context references outlive escaped assignment
storage. A caller using an isolated context retains that actual owning context
through all returned tensors, as it does for other borrowed GPU resources;
workspaces reject changing their owning device or context. Normal training records the lane's final assignment consumer after
backward and after saved index tensors have released; retained graphs cannot be
marked reusable prematurely. Graph abandonment or a consumer outside the explicit
lane completion contract conservatively settles the owning context before
unrecorded storage can be reused or destroyed. This fallback is a resource-safety
boundary, not evidence of asynchronous overlap or a performance improvement.

`MMLTK_MATCHER_TRACE_FILE` enables JSONL cumulative exact counters per workspace:
active cost submissions/bytes/dependencies, packed materializations,
assignment bytes/uploads, H2D submissions, GDR writes, storage growth, and final
consumer dependencies. `materializations` counts the one packed upload set;
public CPU result tensor views do not represent additional H2D submissions.
The explicit test statistics switch enables those same counters without file
output. With both disabled no counter collection or record formatting occurs.
Existing `MMLTK_PROFILE` counters remain available independently.

`MMLTK_NUMA_TRANSFER_TRACE_FILE` enables JSONL allocation/registration records
with owning CUDA context, node, allocation identity, active requested bytes,
reserved capacity and portable-registration status. Cross-device staging records
identify both source and receiving devices, the receiver's node, active bytes,
and its completion route. `MMLTK_GDR_TRACE_FILE` continues to report actual mapping
backend, capacity, copies and physical lifetime. Shared cache and imported/foreign
pages are explicitly outside the owned-local-page claim.

Added standard source coverage checks zero-copy Tensor views across growth and
owner destruction, CUDA pin recognition, compact active cost DMA sizes, public
CPU views, exact packed indices, H2D/GDR upload counts, fixed-capacity reuse,
retained autograd graphs across overlapping results and exception-driven owner
exit, grouped/masked criterion
loss and gradient parity across varying layer shapes, and receiver selection for
image staging. Existing LSAP tie/nonfinite, target-slot, augmentation, capture,
Live, GDR cancellation/failure-construction and Explore completion suites remain
part of the required Final Validation coverage. Hardware capability skips leave
those transport/device cases unverified. No throughput or latency benefit is
claimed.

## Integrated Phase 4 adaptation

The retained implementation is reviewed against `b07630b4`, including the
prerequisite commits through `88127c5a`. The diff from only the latest commit
does not contain the complete Explore handoff change. In particular, the
strict worker policy, pinned/GDR ownership, stream completion queue, compact
augmentation input slots, runtime reconstruction, and browser mailbox changes
are dependencies of exact atlas readiness.

Source inspection retains three distinct boundaries: `FinishReadLane` finishes
source/metadata preparation, `FinishTransfer` reports H2D DMA or synchronous GDR
stores/fences, and `CompleteLane` reports final atlas rendering and cache copies.
Only the final boundary promotes pending annotation meaning and current-generation
readiness. The one registered runtime continuation uses an atomic pending bit;
callbacks do not acquire the runtime scheduler mutex or allocate work records.
The existing Borrow and drain-during-notification tests remain the deterministic
lock-interaction coverage. Stream shutdown joins source writers and completion
observers before lane storage is destroyed.

The ordinary gated events `gallery.read.started`, `gallery.read.completed`, and
`gallery.transfer.completed` now expose source and transport boundaries without
requiring the acceptance gate. Each carries device, generation, destination slot
and compiled index. Read completion's capacity-width field is the completed-read
flag; capacity-height is the failure flag for read and transfer completion.
The existing `gallery.gpu.completed` / `gallery.gpu.failed` records remain the
final product boundary. These facts are collected only with diagnostics enabled.

The additional controlled settings case holds a discrete filter render while
saved transport selection changes. Admission defers reconstruction while that
operation is busy; its stale settings candidate then rolls back. Failure
notifications now recheck saved execution settings after scheduler finalization,
as successful notifications already do, so that deferred transport change does
not depend on another user action.

The native cancelled-lane/retained-completion case now also runs a full training
loader epoch while Explore's source worker is held. It retains a second training
batch across stale Explore work, atlas growth and Explore shutdown, and compares
its device pixels before and afterward. The same case runs explicit H2D and GDR
selection. This covers the shared training loader boundary, not a full model
optimization step; the required Train-plus-Explore hardware acceptance remains
part of Final Validation.

No build, tidy, cleanup, test or hardware run was performed for this adaptation.
The new assertions and settings failure path remain unexecuted until Final
Validation. Existing observed shutdown evidence still justifies only the shared
GNU timeout duration conversion; no additional shutdown behavior was changed.

## Final Validation observations

Final Validation completed on 2026-09-06. Statements above that defer builds,
tests, or hardware evidence describe the phase in which they were written; the
results in this section supersede those deferrals.

The repository-configured tidy, full build, C++ and frontend cleanup profiles,
second tidy, cleanup audit, and final full build completed successfully through
`./mmltk`. The focused native, CUDA, application-system, browser, serialization,
and Explore suites completed with 640 passing test cases. Coverage included
NUMA placement and policy failures, loader order and sharding, active-region
matcher transfers, grouped and masked criterion results, autograd lifetime,
allocation reuse, cancellation, restart, concurrent Train and Explore work,
atlas replacement, progressive matching, and browser presentation semantics.

The final `./mmltk --test workspace-wayland` run exercised all 11 packaged
Wayland cases using explicit H2D selection. Its H2D branches passed all 344
assertions, including window close, SIGINT with logging enabled and disabled,
browser peer loss, rapid atlas changes, wide/tall/square and partial-row
geometry, fullscreen restore, checkbox semantics, and independent box and mask
editing with exact rendered-pixel probes. Catch2 reports one passed case and
ten skipped cases because each parameterized case also contains a
capability-gated GDR branch.

An exact rerun of the window-close seed exposed two final integration defects.
An Iced surface-state change could become visible without requesting a redraw,
and a box-only annotation scene could enter the renderer without mask staging.
The final implementation requests a redraw only when the reconciled surface
state changes and skips mask staging when the scene contains no mask runs. The
same seed then passed 67 assertions, and the complete Wayland suite passed
afterward.

The top-level `./mmltk --gui` SIGINT case was also exercised independently of
the packaged native fixture. With `MMLTK_GUI_TRACE_FILE` and
`MMLTK_FIREFOX_LOG_FILE` enabled, the trace recorded browser peer connection,
signal-driven shutdown, ingress stop, system stops, Firefox termination, all
system joins, and `shutdown.complete`. The interval from
`shutdown.requested` to `shutdown.complete` was approximately 304 ms, which
exercised the wrapper's millisecond-duration conversion. Repeating the launch
without the opt-in diagnostic variables exited cleanly and created no GUI,
Firefox, or native diagnostic file.

GDRCopy hardware behavior remains unverified. The validation environment did
not expose `/dev/gdrdrv` or a supported DMA-BUF import capability, so every GDR
branch reported a capability skip. The H2D results do not establish GDR
correctness or performance, and no throughput or latency benefit is claimed.
