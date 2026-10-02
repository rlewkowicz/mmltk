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
| [CompileWorkspacePlan and CompileArenaBank](../src/backend/data/detail/compile_workspace.h) | Checked preparation, fixed NUMA-local pinned backing, compatible context leases, and exclusive exceptional storage |
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

The benchmark image pipeline keeps FIFO ready lists for Header, Workspace,
Input, Output, and Pixels. It rotates between stages after successful admission;
a denied probe does not consume a turn. Header admission establishes geometry
and a checked plan. Workspace admission reserves the complete input/conversion
envelope before retaining it, Input preloads encoded bytes, Output prepares the
occupied destination, and Pixels converts directly into its disjoint extent.
These stages can advance alongside source, label, and cache work.

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

## Fixed pinned arenas

[The checked planner](../src/backend/data/compile_workspace.cpp) combines enabled
class profiles, output geometry, decoder/resizer requirements, complete label
output, and source windows. Its normal budget is:

```text
R = page-rounded shared source, compaction, and class-completion windows
Q = largest enabled normal context that fits below the protected ceiling
B = min(6 GiB, max(256 MiB, R + W × Q))
```

Arithmetic is checked. Protected windows must fit the normal ceiling. When
CPU capacity permits, preparation first fits contexts for enabled classes,
then adds interchangeable contexts up to the remaining budget/useful CPU count.
Class floors guarantee CPU availability, not a separate maximum-sized workspace
for every class. The complete budget is pinned, including protected and currently
unused bytes. Explicit small internal plans support bounded work; the public
compiler has no memory-budget tuning switch.

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
reassignment. Leases select reusable contexts and stable slices for encoded
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

A legal request that cannot fit a normal context enters one exclusive
exceptional lineage. Unrelated transient borrowers drain while the normal bank
stays pinned. One checked additional arena is then locked for that input and
its admitted descendants. It cannot grow while borrowed; its last physical
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
Label output retains its pinned lease through the actual file write. There is
no final serial image-copy queue.

Each staged inode owns its allocation, hole-punch, and truncation ordering.
The filesystem ledger protects numeric promises while capacity and allocation
observations run outside its lock. New physical growth, overwrites, and removed
backing update the same reservation. This coordinates one compilation's users;
unrelated processes can still consume disk space.

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
