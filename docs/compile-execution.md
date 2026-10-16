# Compilation execution and working storage

[Wiki index](README.md) · [Dataset format](datasets.md) · [Benchmark recipes](benchmark-datasets.md) · [Diagnostics](logging.md#benchmark-compilation-traces)

Directory and benchmark compilation share execution, admission, pinned storage,
file services, slot layout, and atomic filesystem publication. Compiler/recipe
owners retain source policy, membership, repair, and publication decisions.
Runtime [batch loading](datasets.md#loading-a-compiled-file) uses its own reader
pool.

## Owners and ready work

| Owner under `src/backend/data/` | Responsibility |
| --- | --- |
| [CompileExecution](../src/backend/data/detail/compile_execution.h) | Session CPU admission, live classes, bounded ready/parked records, dependencies, completion, cancellation, join |
| [CompileResources / CompileAllowance](../src/backend/data/detail/compile_resources.h) | Bytes, descriptors, dependent commitments, external CPU grants, shared physical custody |
| [CompileWorkspacePlan / CompileArenaBank](../src/backend/data/detail/compile_workspace.h) | Checked preparation, fixed NUMA-local backing, aligned leases, protected windows, exceptional storage |
| [CompileImageDecoder](../src/backend/data/detail/compile_image_decoder.h), [CompilePixelWorkspace](../src/backend/data/detail/compile_pixel_workspace.h), [CompiledLabelWorkspace](../src/backend/data/detail/compiled_label_workspace.h) | Checked codec/resizer plans, supplied scratch, direct pixels, complete label leases |
| [FileWorkQueue](../src/backend/data/detail/file_work_queue.h) | Bounded borrowed operations, class-aware service admission, physical completion |
| [CompiledSlotStorage](../src/backend/data/detail/compiled_slot_storage.h) | Stable block mappings, occupied-slot preparation, write custody |
| [StagedArtifact](../src/backend/data/detail/staged_artifact.h), [StorageReservationPool](../src/backend/data/detail/storage_reservation.h) | Ordered inode mutations, cleanup, destination-filesystem growth promises, staged publication |

[Directory](../src/backend/data/compiler/dataset_compiler.cpp) retains numbered
slots and strict source failures. Its bounded image records progress through
file preparation, parsing, conversion, and labels.
[BenchmarkCompilePipeline](../src/backend/data/benchmark/detail/benchmark_pipeline.h)
retains source/image joins, arrival-order slots, selected label claims, geometry
agreement, and generation-safe withdrawal. Source owners submit work directly.

| Shared work class | Work |
| --- | --- |
| Acquisition | Network/cache |
| Extraction | Headers, archive/Parquet projection |
| Annotations | Parse, normalize, recover, project labels |
| Pixels | Image conversion |
| Gather | Normalized component assembly |
| Output | File/cache settlement |

Directory enables Extraction, Annotations, Pixels, and Output; benchmarks can
use all six. A class remains live until its producers, tasks, I/O, and dependent
callbacks settle. An empty queue is insufficient. Repair opens producer custody
before publishing new work.

Eligible CPUs resolve in physical-core-first order. For `W` CPU slots and `C`
live classes, `W >= C` reserves one baseline lane per class and `W - C` elastic
lanes. Waiting classes retain their baseline; reassignment occurs at safe
boundaries. Elastic lanes return after bounded tasks. When `W < C`, classes
share fairly at task/chunk boundaries. Feasible tasks may pass blocked tasks;
aged byte demands participate in admission.

Workers persist for the session. Reusable records have independent active and
ready links, fresh completion state per physical step, and a separate ordered
index for exceptional drain. A retained old completion cannot observe recycled
work. `CompileExecution::Continuation` retains one record across external
preparation without retaining a CPU lane; predecessor completion settles before
successor use. Cancellation/join retain its physical custody until retirement.

The benchmark image stages are Header → Workspace → Input → Output → Pixels.
Header establishes geometry/plan; Workspace atomically retains the complete
conversion envelope and successor record without a CPU lane; Input preloads;
Output prepares the occupied extent; Pixels writes it directly. Every probe,
including denial, consumes the `max(8, 4 × W)` dispatch budget. Successful
admission rotates stages and releases the facts lock between attempts.
Resource-denied workspace requests rotate through the initial ready count,
letting fitting peers progress. Stage/workspace/label cursors survive yields:
unexamined work requests a local follow-up; a fully denied unchanged pass parks;
a full record bank waits for capacity. No stage waits for a batch to fill.

Benchmark image-file service has `max(2, min(4, 2 × W))` records; the separate
label/output service has `max(4, 2 × W)`. Each has `min(2, W)` service threads.
Curl and file calls borrow the same CPU budget, replacing a compute lane while
active; archive helpers use admitted elastic capacity. Unstarted file records
remain selectable by class until dispatcher and CPU are available. After the
physical call returns its grant, the callback still retains record/producer.

Compute completion returns CPU/scratch before callbacks; the settling worker
retains class identity through callback return, so another class's floor goes
to a worker able to dispatch it. File services may borrow that returned CPU.
Parked tasks retain inputs, outputs, and completion custody, never borrowed
lane scratch. Generation observation and dependency arming are atomic; work
resumes or retires once. Elastic workers do not wait for file calls, resources,
or peer completion. Controllers/file services own necessary waits; mapped
stores can still page fault.

### Selective readiness and resource pressure

[CompileChange](../src/backend/data/detail/compile_resources.h) distinguishes
Resources, Cpu, Records, Local, Pressure, Fairness, and Interruption. Owners
subscribe only to causes affecting their predicates; local notices address that
subscription and interruption reaches all owners. Registration checks generation
atomically. Destruction joins callbacks, whose borrowed registration survives
through return, including teardown. Pending causes share one armed eventfd turn;
acknowledgement detaches the batch under the admission lock before delivery,
and arrivals during delivery arm another turn.

Compute selectors wait separately from owner-local notifications. They wake
for queued compute work, pressure needing idle scratch retirement, or stop.
Public generations and physical-service subscriptions keep separate delivery.
Resource eligibility is cached by resource generation: CPU, record, and local
changes do not invalidate it. A failed local attempt suppresses wakes caused
only by its temporary reservations; external changes remain effective. Image
dispatch listens for resources/records/fairness; file service also needs CPU.

`Fairness` parking is distinct from `Bytes`/`Descriptors` exhaustion and does
not request reclamation. Removing an aged demand wakes fairness dependents
without invalidating resource eligibility. Completion windows wait only for
own completions and relevant pressure, preserving archive/Arrow cursors across
unrelated scheduling. Oversized work has an explicit drain signal.

Reclamation uses blocked demand and actual bytes/descriptors held. An opaque
library charge cannot relieve pinned-arena fragmentation with aggregate
headroom; several candidates may contribute partial relief. Changed demand
notifies pressure subscribers even while pressure stays active. Exceptional
drain, unknown external pressure, and physical retirement retain their rules;
the archive owner decides decoder suspension. Diagnostics never decide admission.

## Fixed pinned arenas

The [planner](../src/backend/data/compile_workspace.cpp) combines enabled class
profiles, output geometry, codec/resizer needs, complete labels, and source windows:

```text
R = page-rounded shared source, compaction, and class-completion windows
Q = largest enabled normal context fitting below the protected ceiling
B = min(6 GiB, max(256 MiB, R + W × Q))
```

Arithmetic is checked; protected windows must fit. Context capacities size
CPU/NUMA backing, not exclusive image slots. Each NUMA node pools ordinary
capacity in one allocation; the budget tail remains usable. Protected regions
occupy the first selected node. CPU class floors are independent of storage.
All `B` bytes stay pinned, including free space. Small explicit plans support
internal bounded work; no public memory-budget switch exists.

Normal image sizing uses a 2,048-pixel reference extent, not an input limit.
Actual headers/rows size requests. Benchmark preparation includes a 32 MiB
encoded-image window and COCONut's 256 MiB Arrow profile; shared windows count
once. [Resize policy](datasets.md#rgb-resampling-methods) also affects sizing.

[NumaMemory](../src/common/system/numa_memory.cpp) binds, prefaults, locks, and
verifies pages before publication. Allocation, `mlock`, or placement failure
discards the candidate and preserves prior storage. Per-page errors identify
offset, allocation extent, and required node, distinguishing kernel errors from
resident-node mismatch. Extents exceeding the node's total memory are rejected;
Linux prefaulting determines actual availability, including reclaimable file
cache. Transient `MemFree` is not an allocation bound. The wrapper supplies
`IPC_LOCK` and unlimited memlock. CPU compilation requires no CUDA context;
[runtime CUDA registration](gpu-execution.md#device-and-numa-placement) is separate.

Arenas retain addresses/capacity/pinning across reassignment. Leases round actual
bytes to 64-byte alignment. Size-binned extents split and coalesce on last-borrower
release within each region; ordinary space precedes eligible protected windows.
Coalescing never crosses NUMA/protected boundaries. Aliases/subspans retain the
original allocation. Supplied slices hold encoded input, STB decode, RGB/CMYK,
AVIR/perceptual/Torch scratch, parsing/masks, Arrow, and labels. Completion/source/
compaction windows retain their admission rules. Opaque TurboJPEG controls or
ATen temporaries have separate checked charges. Directory uses STB; benchmark
JPEG uses TurboJPEG and PNG uses STB. Torch's task-local CPU-thread limit restores
on return and creates no nested image pool.

Preload retains the admitted file generation. If pooled source bytes prevent
complete conversion-envelope admission, the pipeline releases that charge and
reopens the same admitted inode before reserving workspace; path replacement
cannot substitute input.

A legal shape larger than every compatible normal region uses one exclusive
exceptional lineage. Ordinary occupancy/fragmentation waits for release and
coalescing. Unrelated transient borrowers drain while `B` remains pinned;
one page-rounded additional arena serves the input and admitted descendants.
It cannot grow while borrowed and retires after its last physical consumer,
before ordinary admission resumes. Allocation/pin failure preserves prior output.

Peak storage includes normal `B`, exceptional `E`, and retained products/library
controls. The 6 GiB ceiling bounds normal arenas, not RSS, file cache, or all
immutable products. Mapped files are not whole-file pinned arenas. Charges,
live slices, promises, descriptors, and physical allocation are distinct.

Readers must quiesce before offering unused future capacity; finite consumers
borrow only participating offers. Immutable batches, aliases, and split storage
retain backing without spending a commitment twice. Logical wrapper release
does not free a batch/output/frame/file operation's bytes. Descriptor admission
measures headroom and reserves dependent reader/writer/transport needs before
retaining sources. Copied allowances share one commitment; closing a reader
returns its descriptors/unused promises while descendants retain theirs. Small
file/lease controls have separate accounting from payload workspace.

## File preparation and settlement

Short metadata locks claim slots. File service prepares occupied pages outside
source/facts/admission locks, then publishes write readiness. Pixel workers
write disjoint mapped destinations; label leases survive the physical write.
[Final compaction](datasets.md#staging-and-in-place-compaction) is separate.

Each staged inode orders allocation, hole punching, and truncation. The storage
ledger serializes capacity transactions per destination `st_dev`: initial or
growing promises sample `statvfs` once under that filesystem's lock, check sibling
promises, and commit together. Shrink needs no sample. Allocation, mapped writes,
and sync occur outside the transaction; allocation settlement updates the ledger.
Other filesystems are independent; unrelated processes may still consume space.

`StagedArtifact` accepts bounded range batches with one admission/reconciliation;
single ranges use the same path. Occupied-page accounting stays conservative
after partial failure: a missing map entry does not prove a hole. Compaction can
split rejected batches without increasing its prior minimum free-space need.
It joins borrowed writers/mappings and preserves source-before-overwrite order.
Independent split seals may overlap but join before directory publication.
Physical file completion includes callbacks and borrowed context lifetime.
Failure/cancellation joins users before files, mappings, or arenas retire.

### Filesystem publication

[path_publication](../src/common/io/path_publication.cpp) serves staged files,
benchmark directories, and outer artifact publication. Source/destination share
a filesystem; callers own validation/sync, cancellation, admission, overwrite.

Publication tries no-replace rename first, exchanging an occupied destination
only when overwrite is allowed. Parent-sync failure restores staged/prior paths;
impossible rollback is fatal. After commit, old-generation removal is best effort
without following symlinks. `PathPublicationResult` reports replacement/cleanup
error; cleanup failure does not revoke new output. Benchmark-specific
[destination admission](benchmark-datasets.md#persistent-cache-and-publication)
remains with that compiler.

## Progress delivery

[CompileProgressDelivery](../src/backend/data/detail/compile_progress_delivery.h)
serves Directory telemetry and benchmark progress. Producers update exact counters
and mark dirty. One observer materializes the replaceable snapshot after 100 ms,
before copying/serialization, and sleeps while clean. Producers never wait for
display cadence/capacity.

Lifecycle/cumulative repair boundaries capture immediately. A bounded 64-record
queue may replace older cumulative boundaries; warnings are best effort and
indispensable lifecycle exhaustion fails delivery. Flushes order publication and
terminal observations. Intermediate activity/transfer decisions may coalesce.
[Progress units](benchmark-datasets.md#reading-compilation-progress) and
[application revision guards](application-wire.md#dataset-operation-snapshots)
have separate homes. Render receipts and trace identities authorize no native
work, publication, or reuse. [Functional coverage](validation.md#benchmark-compilation-evidence)
and [performance interpretation](benchmark-performance.md) remain distinct.
