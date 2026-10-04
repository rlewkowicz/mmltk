# Compilation execution and working storage

[Wiki index](README.md) · [Dataset format](datasets.md) · [Benchmark recipes](benchmark-datasets.md) · [Diagnostics](logging.md#benchmark-compilation-traces)

Directory and benchmark compilation share the data-owned execution, workspace,
file-service, slot-storage, and compiled-format boundaries below, plus common
filesystem publication. Their source front ends and codec policies remain
distinct. Recipe membership, cache repair, source identity, and publication
decisions stay with their compiler and recipe owners.
Runtime batch loading uses its existing reader pool, described in
[compiled loading](datasets.md#loading-a-compiled-file).

## Owners and ready work

| Owner under `src/backend/data/` | Responsibility |
| --- | --- |
| [CompileExecution](../src/backend/data/detail/compile_execution.h) | Session CPU admission, live work classes, bounded ready/parked records, dependencies, completion, cancellation, and join |
| [CompileResources and CompileAllowance](../src/backend/data/detail/compile_resources.h) | Bytes, descriptors, dependent commitments, external CPU grants, and shared physical custody |
| [CompileWorkspacePlan and CompileArenaBank](../src/backend/data/detail/compile_workspace.h) | Checked preparation, fixed NUMA-local pinned backing, aligned task-sized leases, protected windows, and exclusive exceptional storage |
| [CompileImageDecoder](../src/backend/data/detail/compile_image_decoder.h), [CompilePixelWorkspace](../src/backend/data/detail/compile_pixel_workspace.h), [CompiledLabelWorkspace](../src/backend/data/detail/compiled_label_workspace.h) | Checked codec/resizer plans, supplied scratch, direct pixel output, and complete label-output leases |
| [FileWorkQueue](../src/backend/data/detail/file_work_queue.h) | Bounded borrowed file operations, class-aware service admission, and physical completion |
| [CompiledSlotStorage](../src/backend/data/detail/compiled_slot_storage.h) | Stable block mappings, occupied slot preparation, and pixel/label write custody |
| [StagedArtifact](../src/backend/data/detail/staged_artifact.h), [StorageReservationPool](../src/backend/data/detail/storage_reservation.h) | Per-file mutations and cleanup, destination-filesystem growth promises, and staged-file publication |

[DatasetCompiler](../src/backend/data/compiler/dataset_compiler.cpp) retains
numbered Directory slots and strict source failures. Its bounded image records
advance through file preparation, parsing, decoding/resizing, and label output.
[BenchmarkCompilePipeline](../src/backend/data/benchmark/detail/benchmark_pipeline.h)
retains the source/image join, stable arrival-order slots, selected label
claims, geometry agreement, and generation-safe withdrawal. Source owners submit
generic work directly to the shared execution owner.

Benchmark image work keeps a persistent file service separate from label and
output file operations. For `W` CPU slots, its record capacity is
`max(2, min(4, 2 × W))`, with `min(2, W)` service threads. The output service
has `max(4, 2 × W)` records and the same service-thread limit. Both services
still acquire CPU grants from `CompileExecution`; these limits do not add CPU
capacity outside the shared budget.

The canonical work classes are Acquisition, Extraction, Annotations, Pixels,
Gather, and Output. Their task kinds distinguish network/cache work, headers
and archive/Parquet projection, parsing/normalization/recovery/labels, image
conversion, normalized-component gathering, and file/cache settlement. Directory
enables Extraction, Annotations, Pixels, and Output; benchmark sessions can use
all six. A producer keeps its class live while it can still publish work.
An empty queue alone does not finish a class: producers, tasks, I/O, and dependent
callbacks must settle. Repair opens new producer custody before publishing work.

The worker pool resolves eligible CPUs in physical-core-first order. For `W`
CPU slots and `C` live unfinished classes, `W >= C` reserves one baseline lane
per class and leaves `W - C` elastic lanes. A waiting class retains its baseline.
Assignments change at safe boundaries; elastic lanes return to shared selection
after bounded tasks. With fewer CPUs than live classes, fair sharing occurs at
task/chunk boundaries. Feasible work can pass a blocked task, and aged byte
demands participate in admission so fitting larger work can progress.

Compute workers persist for the session. The bounded task-record bank owns
independent active and ready links, so enqueue/removal reuses record storage.
Each physical step still receives fresh completion state; an older retained
completion cannot observe a recycled record's later work. Exceptional drain
candidates have their own ordered index, separate from ordinary resource waits.

The benchmark image pipeline keeps ready lists for Header, Workspace,
Input, Output, and Pixels. Successful admission rotates between stages;
every candidate or admission probe, including a denial, consumes the bounded
dispatch budget of `max(8, 4 × workers)`. Header admission establishes geometry
and a checked plan. Workspace admission atomically retains the complete
input/conversion envelope and a successor record without taking a CPU lane.
Input preloads encoded bytes, Output prepares the occupied destination, and
Pixels converts directly into its disjoint extent.
Resource-denied workspace requests rotate within a pass bounded by the initial
ready count, allowing fitting successors to progress. The stage, workspace,
and label cursors survive a budget yield. Unexamined work requests a local
follow-up; a fully denied unchanged pass parks. A full record bank stops that
stage until capacity returns. Admission releases the facts lock between
successful attempts, so completion and source publication can advance alongside
it. No turn waits for a batch to fill.

Curl pumps and file-service calls draw CPU grants from the same owner; a service
grant replaces a compute lane while active. External archive helpers can use
admitted elastic capacity. Unstarted file records remain selectable by class
until both a dispatcher and its CPU grant are available. The completion callback
retains the record and producer after the physical call returns its CPU grant.
This preserves service readiness without adding an unaccounted full worker pool.

Compute completion returns its CPU grant and scratch before invoking callbacks.
The settling worker retains its class identity until those callbacks return, so
another class's floor is assigned to a worker that can dispatch it. File services
can still borrow the returned CPU while that callback retains its worker.

Dependency registration checks the observed generation atomically with arming
readiness. Parked records retain inputs, output leases, and completion custody.
They resume or retire once; they do not retain borrowed lane scratch. Elastic
lanes do not block on file operations, resource admission, or another task's
completion. System-owned controllers and the bounded file service own necessary
waits. Ordinary mapped stores can still incur page faults.

`CompileExecution::Continuation` retains the same bounded scheduler record
across external preparation without holding a CPU lane. The predecessor's
completion settles before the successor uses that record; cancellation and
join retain physical custody until the continuation and its admitted work
retire. This lets preloaded image input and its workspace survive asynchronous
file preparation without requiring a second scheduler record.

### Selective readiness and resource pressure

The canonical [CompileChange](../src/backend/data/detail/compile_resources.h)
causes separate Resources, Cpu, Records, Local, Pressure, Fairness, and
Interruption. Owner subscriptions select the causes that can change their
predicates; owner-local notifications address that subscription, and
interruption reaches every owner. Registration checks its observed generation
atomically, and subscription destruction joins callbacks. A callback borrows its
registered subscription through return, including during owner teardown; it
does not recover that registration through a concurrently retiring owner field.
Pending notifier causes share one armed eventfd turn. Acknowledgement detaches
that batch under the admission lock before delivery; arrivals during delivery
arm another turn.

Compute workers wait on a separate channel from owner-local notices. Physical
admission changes wake compute selectors when queued work exists, pressure may
require idle scratch retirement, or interruption/shutdown needs their attention.
Public admission generations and physical-service subscriptions retain their
own delivery. Idle selector wake counts are distinct from worker creation.

Resource eligibility is cached against the resource generation. CPU-only
changes, returned scheduler records, and local product arrivals do not
invalidate it. A failed local admission attempt suppresses retries caused only
by its own temporary reservations; external changes retain their wake. Image
dispatch listens for resource changes, returned records, and fairness changes,
while file services additionally need CPU availability.

An aged demand can delay fitting work for fairness without indicating exhausted
storage. Its parked reason is `Fairness`, separate from `Bytes` and
`Descriptors`; only actual resource waits request reclamation. Removing an aged
demand wakes its fairness dependents without changing resource eligibility.
Completion windows wait on their own completions and relevant pressure, so
unrelated CPU scheduling does not discard a retained archive/Arrow cursor.
Oversized workspace demand has an explicit drain signal. Diagnostic counters,
addresses, and timing never control these decisions.

Reclamation usefulness is derived from blocked requests and the candidate's
actual byte/descriptor custody. Returning an unsupplied library allocation
cannot resolve ordinary pinned-arena fragmentation when aggregate bytes and
descriptors have headroom. Several candidates may each contribute useful
relief; no single victim has to satisfy the whole request. Changed demand
notifies pressure subscribers even while pressure remains active. Exceptional
drain, unknown external pressure, and physical retirement keep their admission
rules; the archive owner decides whether to suspend its decoder.

## Fixed pinned arenas

[The checked planner](../src/backend/data/compile_workspace.cpp) combines enabled
class profiles, output geometry, decoder/resizer requirements, complete label
output, and source windows. Its normal budget is:

```text
R = page-rounded shared source, compaction, and class-completion windows
Q = largest enabled normal context that fits below the protected ceiling
B = min(6 GiB, max(256 MiB, R + W × Q))
```

Arithmetic is checked. Protected windows must fit the normal ceiling. The
planner's context capacities distribute normal backing across CPU/NUMA
placements; they are sizing inputs, not exclusive image slots. The arena bank
pools ordinary capacity for each NUMA node into one physical allocation and
registers the remaining budget tail as usable storage. Protected windows occupy
separate regions on the first selected node. Class floors guarantee CPU
availability independently of storage. The complete budget stays pinned,
including free bytes. Explicit small internal plans support bounded work; the
public compiler has no memory-budget tuning switch.

Normal image profiles use a 2,048-pixel reference extent to choose reusable
capacity; actual headers/rows determine each request. That reference is not an
input-size limit. Benchmark preparation includes its 32 MiB encoded-image
window and, for COCONut, the 256 MiB Arrow payload profile. Shared source windows
are counted once. Output geometry and perceptual filtering also affect the plan.

[NumaMemory](../src/common/system/numa_memory.cpp) binds and prefaults owned pages,
calls `mlock`, and verifies their retained placement before publishing compiler storage.
Failed allocation, locking, or placement verification discards the candidate
and preserves existing storage. Per-page placement errors identify the page offset,
allocation extent, and required node, distinguishing a per-page kernel error
from an actual resident-node mismatch.
Allocation rejects extents larger than the selected node's total memory;
Linux prefaulting decides whether actual pages can be obtained, including by
reclaiming file cache. A transient `MemFree` reading is not an allocation limit.
The wrapper supplies `IPC_LOCK` and unlimited memlock; preparation or locking
failure fails the operation. CPU compilation uses Linux page locking without
acquiring a CUDA context. CUDA-registered runtime transfer buffers retain their
[separate ownership](gpu-execution.md#device-and-numa-placement).

Normal arenas keep their addresses, capacities, and pinning across worker
reassignment. Each lease claims its actual requested bytes rounded to 64-byte
alignment. Size-binned free extents split on admission and coalesce on last-borrower
release within their region. Ordinary space is tried before an eligible protected
window; coalescing never crosses NUMA allocations or protected boundaries.
Aliases and subspans retain the complete original allocation until their last
borrower retires. The stable slices hold encoded
input, STB decode, RGB/CMYK output, AVIR/perceptual scratch, masks/parsing,
Arrow payload, and label output. Completion and named source/compaction windows
retain their admission constraints. Controllable payload allocations use the
supplied storage; opaque library bookkeeping, including TurboJPEG's coefficient
and control storage, has a separate checked charge. Directory preserves its STB
codec policy; benchmark JPEG uses TurboJPEG and benchmark PNG uses STB.

Benchmark preload fills a reusable pinned input slice from the admitted file
generation. If retained pooled source bytes prevent admission of the complete
conversion envelope, the pipeline releases that source charge and reopens the
same admitted inode before reserving the consumer workspace. A path replacement
cannot substitute a different input generation.

A legal request larger than every compatible normal region enters one exclusive
exceptional lineage. Busy or fragmented normal space waits for release and
coalescing; occupancy does not make a normal shape exceptional. Unrelated
transient borrowers drain while the normal bank stays pinned. One page-rounded
additional arena is then locked for that input and its admitted descendants.
It cannot grow while borrowed; its last physical
consumer retires it before ordinary admission resumes. Allocation or pin failure
preserves the prior published output.

The peak includes normal pinned `B`, exceptional pinned `E`, and separately
owned retained products/library controls. The 6 GiB ceiling is a normal-arena
bound, not process RSS, file-cache residency, or a bound on all immutable
products. Mapped input/output files are not whole-file pinned arenas. Byte
charges, live slices, promised capacity, descriptor commitments, and physical
file allocation remain distinct observations.

Reader future allocation and immutable batch backing have separate custody.
A reader must quiesce before offering unused capacity; finite consumers can
borrow only participating offers. Aliases and split storage cannot spend a
commitment twice. Releasing a logical wrapper does not return bytes still held
by a batch, output span, cooperative frame, or file operation.

Descriptor admission measures process headroom and reserves dependent reader,
writer, and transport needs before retaining their source. Copied allowances
share one commitment. Physically closing a reader returns its own descriptors
and unused promises while published descendants retain theirs. Small retained
file/lease controls have separate accounting from data-buffer workspace.

## File preparation and settlement

Slots are claimed under short metadata locks. File-service operations prepare
occupied page ranges outside source/facts/admission locks, then publish write
readiness. Pixel workers write directly into disjoint mapped destinations.
Label output retains its pinned lease through the actual file write. Layout
compaction remains a separate finalization pass.

Each staged inode owns its allocation, hole-punch, and truncation ordering.
The shared ledger keeps one capacity transaction per destination filesystem,
identified by `st_dev`. Initial reservation and growing outstanding capacity
sample `statvfs` once while holding that filesystem's ledger lock, check sibling
promises, then commit together. Shrinking a promise needs no capacity sample.
Allocation, mapped writes, and sync stay outside this transaction; allocation
settlement updates the same ledger. Other filesystems use independent locks.
This coordinates one compilation's users; unrelated processes can still consume
disk space.

`StagedArtifact` accepts bounded range spans for allocation and reclamation,
with one capacity admission/reconciliation for the batch. Single-range calls
use the same path. Occupied-page accounting stays conservative through partial
mutation failures; a missing occupied-map entry alone does not establish a hole.
The compactor can split a rejected batch into smaller safe operations without
requiring more free space than its prior minimum.

[Format-10 finalization](datasets.md#staging-and-in-place-compaction) waits for
borrowed writers/mappings, uses bounded pinned scratch, and preserves
source-before-overwrite order within the staged inode. Independently admitted
split seals may overlap, but the recipe joins their completion before atomic
directory publication. A file-operation completion includes its callback and
borrowed context lifetime. Failure/cancellation stops pending work and joins
physical users before staged files, mappings, or arenas retire.

### Filesystem publication

[path_publication.h](../src/common/io/path_publication.h) and its
[implementation](../src/common/io/path_publication.cpp) own the shared atomic
publication primitive used by staged files, benchmark directories, and the
outer artifact service. Staging and destination must share a filesystem.
Callers retain content validation/sync, cancellation, destination admission,
and overwrite policy.

Publication first attempts a no-replace rename. An occupied destination is
exchanged only when overwrite is allowed. A failed destination-parent sync
restores the staged and previous paths; an impossible rollback is fatal.
After commit, old-generation removal is best effort and never follows symlinks.
The returned `PathPublicationResult` reports replacement and any cleanup error;
a cleanup error does not revoke the new output. Benchmark-specific
[destination admission and outer staging](benchmark-datasets.md#persistent-cache-and-publication)
remain with their existing owners.

## Progress delivery

[CompileProgressDelivery](../src/backend/data/detail/compile_progress_delivery.h)
serves Directory `CompileTelemetry` and benchmark `ProgressReporter`. Producers
update exact counters and mark ordinary progress dirty. One observer worker
materializes the replaceable snapshot after a 100 ms dirty interval, before
copying/serialization. It sleeps while clean; producers do not sleep for display
cadence or observer capacity.

Lifecycle and cumulative repair boundaries capture their facts immediately.
The bounded 64-record queue can replace older cumulative boundaries under
pressure; warnings are best effort, while indispensable lifecycle exhaustion
fails delivery. Explicit flushes order observations through publication and
terminal settlement. An observer therefore need not see every intermediate
activity or transfer decision. The
[progress reference](benchmark-datasets.md#reading-compilation-progress) defines
units and phase selection; the
[application boundary](application-wire.md#dataset-operation-snapshots) defines
revision/generation guards and LatestState transport. Render receipts and trace
identities never authorize native work, publication, or buffer reuse.

[Existing suite coverage](validation.md#benchmark-compilation-evidence) checks
these ownership and outcome requirements. Gated observations are evidence of
the captured work, not a sustained-utilization or throughput guarantee.
