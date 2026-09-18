# Source performance review

## Goal

Identify valuable performance improvements in five native domains plus the
Rust/Iced frontend and its vendored UI libraries, using static code inspection
and documented hardware/API behavior. Prioritize unnecessary work, memory
movement and blocking before adding parallelism or specialized instructions.
Target both installed GPUs: the primary RTX 4070 SUPER, compute capability
**8.9 (Ada)**, and RTX 3090 Ti, compute capability **8.6 (Ampere)**. Compute
placement and the browser's display GPU are independent.
Preserve behavior, numerical requirements, bounded storage, resource custody and
cohesive ownership.

## Execution gate and assignments

Finish the current `actionplan.md`, its Final Validation, all required commits,
and its complete documentation pass and documentation commit first. This review
is a separately requested subsequent activity; it does not reopen that plan's
validation or cleanup review.

Then record the completed documentation commit as `<BASE COMMIT>`. Spawn exactly
one fresh `gpt-6-astra` agent with `max` reasoning and no inherited conversation
for each assignment below. Supply the common prompt verbatim with its placeholders
replaced, together with this complete file. Use available concurrency slots in
waves, wait for returns, and do not spawn replacement or nested reviewers.
All six inspect the same committed source state.

| Agent | Primary directories | Report name |
| --- | --- | --- |
| annotation | `src/backend/imaging/annotation` | `.cache/annotation.md` |
| resample | `src/backend/imaging/resample` | `.cache/resample.md` |
| upscale | `src/backend/imaging/upscale` | `.cache/upscale.md` |
| gpu | `src/frameworks/gpu` | `.cache/gpu.md` |
| serialization | `src/frameworks/serialization` | `.cache/serialization.md` |
| frontend | `src/frontend`, `third_party/iced_aw`, `third_party/iced`, `third_party/iced_plot`, `third_party/iced-fluent-theme` | `.cache/frontend.md` |

The main agent gives each agent its distinct absolute `<REPORT PATH>` from this
table. Each report identifies `<BASE COMMIT>`. Reviewers may inspect callers,
callees, dependencies, configuration, tests and other domains outside their
primary directories. Each reviewer may write only its assigned report.
The frontend reviewer covers all five directories in its assignment, including
their source, feature configuration, build rules and tests, and records coverage
of each in the single `.cache/frontend.md` report.

No implementation, builds, tests, generation, benchmarks, profiling, timing
instrumentation, new counters, runtime experiments or compiler tuning runs occur
in this review. Existing source, tests, generated artifacts and already-recorded
evidence may be read. Do not change `actionplan.md`, `remediationplan.md`,
`reviewplan.md`, product files or documentation during the reviews.

After all six reports return, spawn exactly one additional fresh standalone
`gpt-6-astra` agent with `max` reasoning and no inherited conversation. It reads
all six reports and creates a **new** cohesive `actionplan.md` using the
consolidation prompt below. The main agent reviews that plan for scope and
architectural alignment. Do not replace the current plan early. This instruction
authorizes preparing the next plan, not executing that new optimization plan.

## Technical brief for every reviewer

These notes summarize the requested NVIDIA pages and relevant CPU, Iced and
Cargo references checked on 2026-09-18. They are review criteria, not findings
about this repository. Reconfirm API/version assumptions against current
authoritative documentation and the vendored implementation where a proposed
correction depends on them.

### Hardware and existing build policy

CC 8.6 supports up to 48 resident warps and 16 blocks per SM, with 64K 32-bit
registers per SM, 100 KB shared memory per SM and up to 99 KB per block.
Shared-memory use above the ordinary 48 KB threshold requires the appropriate
dynamic allocation and opt-in. These limits differ from A100/CC 8.0; do not import
A100 bandwidth, cache capacity or occupancy assumptions into this review.
Explicit 8.6 code generation matters for its FP32 execution capabilities.
[NVIDIA Ampere tuning guide](https://docs.nvidia.com/cuda/ampere-tuning-guide/index.html)

CC 8.9 also supports 48 resident warps, 64K 32-bit registers and 100 KB shared
memory per SM, but allows 24 resident blocks per SM rather than CC 8.6's 16.
Distinguish those limits and each physical GPU's resources when assessing
occupancy; preserve both architectures.
[NVIDIA Ada tuning guide](https://docs.nvidia.com/cuda/ada-tuning-guide/index.html)

The repository already includes `86` and `89` among its CUDA architectures in
`cmake/MmltkToolchain.cmake`. That file also supplies host `-march=native`,
`-mtune=native` and x86 SSSE3/SSE4.2/AVX2/FMA flags. Inspect target propagation
before proposing flags already present. Do not narrow the existing architecture
matrix or change toolchain/dependency policies as an incidental optimization.
GPU compute capability says nothing about CPU AVX-512 availability.

### Programming model and useful concurrency

Host and device can perform independent work simultaneously. Ordinary blocks
must remain correct under any scheduling order; neither overlap nor concurrent
residency is guaranteed. Expose independence in the actual dependency graph,
without spin-waiting for an unscheduled block. Preserve the correct scope for
shared data and synchronization. Thread-block clusters require CC 9.0 or newer
and are unavailable on either target.
[CUDA programming model](https://docs.nvidia.com/cuda/cuda-programming-guide/01-introduction/programming-model.html)

For this codebase, a concurrency proposal must name the work that can overlap,
the dependency removed, its existing owner, completion notification, bounded
in-flight capacity, cancellation and physical lifetime. Prefer existing workers,
streams and completion mechanisms. Reject a new thread, queue or scheduler when
it merely moves a wait, duplicates state, adds small-task overhead or competes
for the same saturated resource without a concrete source-based benefit.

### Memory access and work volume

Inspect warp address patterns, row pitches, channel layout and edge tiles for
coalescing and wasted transactions. Shared-memory tiling is useful when it removes
repeated global reads or repairs access layout; account for its staging,
synchronization and residency cost. Ampere has 32 shared-memory banks: distinct
addresses mapping to one bank serialize, while a same-address read can broadcast.
Padding is appropriate only for a demonstrated conflicting stride. Warp-multiple
block sizes avoid partial warps, but no universal block size is optimal, and
higher occupancy does not automatically improve performance.
[CUDA best practices](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html)

Do not assume every kernel is memory-bound. Derive bytes touched, source
traversals, arithmetic, launches and work frequency from the relevant path.
Prioritize eliminating redundant full-image work, copies and conversions over
making the same unnecessary work parallel. Compare unavoidable receiver-owned
copies with avoidable intermediate materialization.

### Host submission, transfers and synchronization

Prefer the narrowest sufficient dependency: a recorded non-timing event and
`cudaStreamWaitEvent` can order device work without blocking the CPU. An
unrecorded event does not prove completion. Stream priorities are scheduling
hints and do not preempt already-running kernels. Inspect implicit/default-stream
dependencies and allocation or configuration calls that can serialize independent
work. Batched asynchronous copies may reduce submission overhead for independent
copies, but must preserve the API's ordering and source-lifetime requirements;
they are not permission to batch dependent copies.
[Advanced host programming](https://docs.nvidia.com/cuda/cuda-programming-guide/03-advanced/advanced-host-programming.html)

Asynchronous host/device transfer needs appropriate pinned host storage and
valid lifetimes through completion. Pageable staging can prevent useful overlap;
separate streams alone do not guarantee overlap. Reuse this repository's pinned,
NUMA-local storage and established completion ownership. Never replace a
required settlement with an API whose name contains `Async` and then release
borrowed storage early.
[Asynchronous execution](https://docs.nvidia.com/cuda/cuda-programming-guide/02-basics/asynchronous-execution.html)

Consider retained CUDA graph replay only where the existing repeated topology
and owners support it; include capture/rebuild, shape changes and failure costs.
Consider stream-ordered allocation only for demonstrated allocation churn that
existing high-water storage cannot remove, preserving cross-stream consumers
and external graphics custody. Neither mechanism justifies a parallel resource
framework.

### GPU ownership and multiple devices

Inspect the existing `DeviceContext`, `ImageCopyBackend`, `ImageProductBuffer`
and `ImageWorkspace` boundaries before proposing another adapter. They own
context binding, same-device/peer/pinned copy routing, reusable staging,
completion and resource retirement. Product systems choose their work device;
they must not independently choose data-transfer mechanisms. Physical CUDA
ordinals and enumeration order are process-local, not cross-API identities.
Match CUDA and exported Vulkan allocations by device UUID. Firefox's actual
display DRM/PCI identity determines its WebGPU adapter independently of compute
selection. Keep UUID resolution at admission, not in frame loops.
[CUDA multiple GPUs](https://docs.nvidia.com/cuda/cuda-programming-guide/03-advanced/multi-gpu-systems.html),
[Vulkan interoperability](https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/graphics-interop.html)

Cross-device dependencies may use stream event waits; bind the correct context
for stream/event creation, copies and destruction. Check actual peer capability
and retain reusable pinned staging when peer access is unavailable. Preserve
source read custody until the receiver has physically completed, and preserve
imported backing, context and semaphore custody through replacement and browser
exit. A device name, ordinal, exporter exit or diagnostic receipt cannot prove
that handoff. Existing hardware tests enumerate the visible GPU pairs; do not
replace them with hard-coded GPU identities or an assumed peer topology.

### Kernel execution and hardware-assisted asynchronous copies

CC 8.x supports hardware-assisted global-to-shared asynchronous copies, allowing
data movement to overlap independent computation and avoid intermediate register
traffic. Establish eligible alignment, byte counts, bounds and completion before
consuming a tile or reusing a stage. An immediate wait with no independent work
does not establish useful overlap. TMA, distributed shared memory and the newer
Hopper asynchronous matrix facilities require newer hardware; they are not
8.6/8.9 remedies. Independent thread scheduling also means warp cooperation needs
valid participant masks and explicit synchronization where required. Use the
narrowest correct atomic scope/order; do not weaken a real dependency.
[Advanced kernel programming](https://docs.nvidia.com/cuda/cuda-programming-guide/03-advanced/advanced-kernel-programming.html)

Ampere LDGSTS copies 4, 8 or 16 bytes with matching pointer alignment. Prove the
size/alignment assertions passed to `cuda::memcpy_async`, including sliced tiles
and tails. A thread's copy completion alone does not make another thread's
reads safe; retain the required block/group coordination before shared use.
[Asynchronous data copies](https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/async-copies.html)

Cooperative Groups supplies grouping and synchronization; it is not itself a
Tensor Core replacement. Consider supported MMA/Tensor Core paths only for
genuine matrix work with compatible layouts and precision, preferably through
the existing inference/library owner. A resampling or lookup kernel should not
be converted into matrix work merely to use Tensor Cores.

Register caps and launch bounds trade residency against spills and instruction
count; blanket `-maxrregcount` is not an improvement by itself. Fixed-loop
unrolling also trades branch overhead against code size and register pressure.
Fast intrinsics and fast-math options change numerical behavior. Any proposal
must preserve required accuracy, rounding, finite/exceptional-value handling,
color transfer, alpha and output determinism. Do not claim register counts,
spills or realized occupancy from source alone.
[CUDA language extensions](https://docs.nvidia.com/cuda/cuda-programming-guide/05-appendices/cpp-language-extensions.html)

### Dynamic parallelism

CDP is a candidate for sufficiently substantial, data-dependent GPU work that
would otherwise require host decisions or transfers; it is not a default
replacement for ordinary host launches. CDP2 is the default from CUDA 12.
Child completion participates in parent-grid completion, but parent/child
concurrency is not guaranteed. Parent local/shared pointers and host-created
stream/event handles cannot be handed to child kernels. CDP2 does not provide
device-side `cudaDeviceSynchronize`; consuming child results requires the
documented continuation/tail-launch ordering. Account for launch-pool capacity,
device-runtime tracking and memory costs, and required build/device-link
changes. Runtime overhead can affect kernels that do not launch children.
Recommend CDP only when removing the demonstrated host dependency outweighs
these costs and ordinary batching, fusion or an existing graph cannot solve it
more simply.
[CUDA dynamic parallelism](https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/dynamic-parallelism.html)

### CPU SIMD and parallel work

Start with contiguous data and ordinary loops the compiler can vectorize; GCC
enables loop and SLP vectorization at `-O2`. Inspect loop-carried dependencies,
gathers, aliasing, branches and conversion passes before proposing intrinsics.
`__restrict__` is a caller-visible non-aliasing promise, not a speculative hint.
[GCC optimization options](https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html),
[restricted pointers](https://gcc.gnu.org/onlinedocs/gcc/Restricted-Pointers.html)

Follow the existing CPU deployment policy. `-march=native` selects the compilation
machine's ISA; it does not establish every destination machine's capabilities.
Do not infer that AVX-512 is present or faster simply because AVX2 is used.
[GCC x86 options](https://gcc.gnu.org/onlinedocs/gcc/x86-Options.html)

Prove alignment through allocation, pitch and sliced offsets before using an
aligned load/store or alignment assumption. A 32-byte AVX or 64-byte AVX-512
alignment requirement applies to the selected aligned operation; unaligned
variants exist and are already used here. Preserve tails, small inputs,
page/row boundaries and no-overread guarantees. Reuse the existing RAII allocator
and pinned-storage owners instead of adding raw allocation families. For CPU
parallelism, account for task size, cache locality, false sharing, NUMA placement,
worker budgets and memory-bandwidth contention.

### Frontend state, drawing, images and build profiles

Treat these ideas as review considerations whose applicability must be
established from the current Rust/Iced owners and vendored feature set.
Inspect granular component state and narrow borrowed inputs to views: unrelated
updates should not require cloning complete snapshots, rebuilding derived
collections or repeating expensive formatting. Smaller state structs alone do
not establish selective view recomputation. Trace the actual message reduction,
subscriptions, view construction, widget reconciliation, layout and drawing
paths, preserving stable widget identity, focus, selection and local transforms.
Keep native domain facts in generated projections and UI state with its owning
Rust component. Keep application logic in the frontend and reusable widget,
theme, plot and rendering behavior in the corresponding vendored owner.

Where an Iced Canvas is actually used, consider a retained `canvas::Cache` for
geometry that remains valid across draws. Its geometry is regenerated when layer
dimensions change or the cache is cleared; verify invalidation for every other
relevant input, including content, theme, scale and interaction. Apply the same
necessary-work analysis to existing custom shaders, retained charts, labels and
workspace drawing without replacing their rendering mechanism merely to add a
Canvas cache. Separate expensive content/geometry preparation from presentation:
preserve the visible browser's required graphics cadence and redraws of retained
completed images, while unchanged or hidden content avoids repeated preparation
and unnecessary wakeups.
[Iced Canvas cache](https://docs.iced.rs/iced/widget/canvas/type.Cache.html)

For paths that already consume CPU RGBA pixels, consider `Handle::from_rgba`
without intermediate image encoding/decoding, and inspect pixel storage,
handle identity, allocation reuse and upload frequency across view calls.
Establish byte ownership and renderer lifetime before proposing buffer reuse;
constructing a handle does not prove an upload or allocation is reused.
The existing workspace path uses shared GPU images through the FD graphics
connection. Preserve that path, paired metadata, direct/copy acquisition,
retained fallback and physical read custody; do not introduce CPU readback or
per-frame RGBA uploads to apply a generic image-handling suggestion.
[Iced image handles](https://docs.iced.rs/iced/widget/image/enum.Handle.html#method.from_rgba)

Inspect effective product and test build configuration before recommending
compiler flags. `src/frontend/iced/Cargo.toml:44` already sets release
`codegen-units = 1`, `lto = "fat"` and `opt-level = 3`;
`src/frontend/iced/Trunk.toml:4` selects release, and
`src/frontend/iced/CMakeLists.txt` invokes Trunk with `--release`.
Keep production frontend output optimized. Treat `opt-level = "z"` as a
conditional binary-size tradeoff, not an assumed runtime improvement.
Review host Rust test compilation, Wasm release packaging, shared prerequisites
and wrapper target dependencies separately. If justified, recommend separating
frontend test and release build needs in the eventual `actionplan.md`, with
concrete targets, cache/dependency effects and preserved validation coverage.
Do not require release optimization for every test merely because product
bundles use it. Preserve generated-binding agreement, useful crash symbols and
vendored toolchain policies.
[Cargo profiles](https://doc.rust-lang.org/cargo/reference/profiles.html)

## Assignment-specific entry points

Confirm paths, symbols and line anchors again at `<BASE COMMIT>`. These are
starting points, not a file boundary or a prewritten findings list.
The frontend reviewer owns the `src/frontend` row and all four `third_party`
rows below.

| Folder | Initial owners and inspection focus |
| --- | --- |
| `src/backend/imaging/annotation` | `core.cppm`, `core.cpp`, `core_preview.cpp`, `core_persistence.cpp`, `renderer.cpp`, `manual_mask_mapping.cpp` and `manual_mask_mapping_cuda.cu`: document/history and scene projection, repeated geometry/mask traversal, dirty-region work, CPU/GPU mask mapping, copies during editing/save, and whether independent work can proceed without blocking ordered document commands. Trace the controller Annotation owner and raster/shared-renderer consumers. Preserve editing, Undo/Redo, save formats, label meaning and render custody. |
| `src/backend/imaging/resample` | `image_resize.cpp`, `image_resize_cuda.cu`, `detail/perceptual_downscale.cpp`, `detail/perceptual_downscale_views.*` and their references/tests: existing AVX2/SIMD before adding new paths, RGB/planar conversion and padding, sample-footprint reuse, cache/coalescing, filter passes, scratch lifetime and useful tile overlap. Preserve categorical masks, coordinate transforms, transfer functions, compensated accumulation, alpha and numerical bounds. |
| `src/backend/imaging/upscale` | `image_upscaler.cpp`, `image_upscaler_runtime.cpp`, ONNX/TensorRT owners, NIS and ShiftLUT CUDA implementations, `detail/image_upscaler_internal.h`: model/context/buffer reuse, graph replay, tile packing/halos/stitching, LUT access, tensor normalization, repeated conversions and stream dependencies. Trace controller Upscale clean/semantic identity and borrowed-source/receiver-owned-result lifetimes. Preserve all modes, cancellation, model generation and teardown. |
| `src/frameworks/gpu` | `image_product_pool.*`, `image_buffer.*`, `image_workspace.*`, `imported_image_buffer.*`, `system_image_runtime.*`, `system_image_worker.*`, `cuda_high_water_allocation.h`, `pinned_host_buffer.*`, GDR, execution placement and retirement owners: actual allocation reuse, growth, context/stream/event churn, host/peer transfers, pending work, backpressure, physical settlement and quiet idle behavior. Trace CUDA/Vulkan/Firefox dependencies where necessary; preserve two display allocations, direct/copy fallback and terminal custody. |
| `src/frameworks/serialization` | `cbor_wire.*`, `reflected_cbor*`, `reflected_json.h`, `json_wire.*` and generated-boundary consumers: repeated measuring/encoding, transient Value/JSON trees, strings, scratch reuse, named/positional projection, segmented borrowed reads, validation complexity and realistic SIMD candidates. GUI messages are binary CBOR; JSON adapters serve local persistence/progress. Preserve limits, UTF-8, duplicate/unknown-key policy, exact numeric representation, deterministic errors, persisted forms and canonical C++26-reflected vocabulary. Do not parallelize tiny records or invent a second schema/codec. |
| `src/frontend` | `iced/src/app.rs`, `iced/src/app/`, `iced/src/view_model/`, `iced/src/view/`, `iced/src/presentation_surface.rs`, `iced/src/presentation_surface/`, `iced/src/workspace_input.rs`, `iced/src/workspace_fps.rs`, transport/protocol and integration-control owners: state scope, update/view work, subscriptions, retained charts, geometry/label preparation, image and buffer reuse, redraw cadence, hidden/idle work and cross-language copies. Inspect `iced/Cargo.toml`, `iced/.cargo/config.toml`, `iced/Trunk.toml`, `iced/CMakeLists.txt`, `iced/wasm-opt-wrapper` and relevant wrapper/build dependencies for effective release optimization and possible test/release separation. Follow vendored Iced, `iced_plot`, Firefox graphics and native presentation/serialization boundaries where needed. Preserve UI behavior, widget identity, styling, input order, generated native vocabulary and physical image custody. |
| `third_party/iced_aw` | `Cargo.toml`, `src/lib.rs`, `src/widget/number_input.rs`, `src/widget/selection_list.rs` and `src/style/`: widget state, layout and event work, numeric conversion/formatting, selection, overlays and redraw requests. Trace application use and feature selection; preserve editing, focus, wheel behavior, typed numeric bounds and styling. |
| `third_party/iced` | `Cargo.toml`, `runtime/src/user_interface.rs`, `core/`, `widget/`, `graphics/`, `renderer/`, `wgpu/`, `winit/` and `program/`: actual update/view/layout/draw scheduling, widget reconciliation, Canvas and image-cache semantics, text/geometry preparation, renderer storage reuse and shader-resource settlement. Trace the application's vendored graphics integration and preserve event order, stable widget state, required redraw cadence and encoded/submitted resource custody. |
| `third_party/iced_plot` | `Cargo.toml`, `src/lib.rs`, `src/plot_widget.rs`, `src/picking.rs`, `src/style.rs` and `src/shaders/`: retained plot/series geometry, GPU buffer capacity, changed/visible work, interaction state, picking/readback frequency and settlement. Trace frontend chart/history owners; preserve plotted values, bounded storage, hidden-view state, expansion/navigation behavior and pending-reader lifetime. |
| `third_party/iced-fluent-theme` | `Cargo.toml`, `src/lib.rs`, `src/theme.rs` and remaining theme/component sources: repeated theme/style construction, allocations, widget composition and invalidation. Trace frontend and vendored widget consumers; preserve Fluent appearance, light/dark themes, fonts and interaction styling while keeping application policy in the application. |

## Common agent prompt

Replace `<DIRECTORIES>`, `<BASE COMMIT>` and `<REPORT PATH>` before sending.
Supply every primary directory from the assignment's row:

> Work as a fresh standalone source-performance reviewer in
> `/home/ryan/Repos/win/cplusplusloader`, primarily responsible for
> `<DIRECTORIES>`.
> Inspect committed source at `<BASE COMMIT>`. Read `AGENTS.md`, `CONTRACT.md`,
> `reviewplan.md`, and relevant pages reached through `docs/README.md` completely
> before forming findings. Apply the full performance meaning in AGENTS.md's
> adversarial verifier paragraph. Use reviewplan.md's technical brief and your
> assignment-specific entry points; verify API claims with primary documentation.
>
> Inspect every source, header/module, CUDA implementation, Rust/JavaScript and
> shader source, build rule, configuration and test in your assigned directories,
> including relevant generated boundaries and persisted forms.
> Follow callers, callees and resource owners outside those directories as far as
> needed to understand real frequency, complete behavior and the correct home
> for a correction. Do not assume a suspicious name, isolated loop, allocation
> or synchronization is a defect. Identify the product path and why the work
> occurs before judging its necessity.
>
> First map ownership, data flow, allocation lifetimes and execution dependencies.
> Then inspect complexity and repeated work: unnecessary allocation or resource
> destruction/recreation, deep copies, temporary materialization, conversions,
> full-image passes, redraws, transfers, redundant initialization, invalidation,
> blocking, lock scope, duplicate submission, polling and idle wakeups. Compare
> normal steady state with input changes, progressive delivery, growth, pressure,
> cancellation, retry, failure, restart and teardown. Preserve necessary physical
> completion and receiver-owned copies; a faster unsafe lifetime is not a fix.
>
> Evaluate GPU memory access, useful shared-memory reuse, register/occupancy
> tradeoffs, divergence, launch granularity, eligible Ampere asynchronous copies
> and existing library acceleration. Evaluate CPU auto-vectorization, existing
> SIMD, data alignment, bounds and task granularity. Target CC 8.6 and 8.9, distinguish
> hardware support from toolkit API availability, and preserve current numerical
> and build policies. Treat dynamic parallelism, new streams, graphs, SIMD
> intrinsics and extra workers as conditional tools, not requirements.
>
> For frontend paths, apply the state, drawing, image and build-profile brief.
> Trace component inputs and actual invalidation before proposing narrower state
> or caches. Preserve retained GPU presentation, widget identity, interaction
> behavior and visible redraw cadence. Verify existing release settings and
> assess separate frontend test/release build needs only where source supports
> a concrete improvement; report the necessary wrapper and dependency changes.
>
> Recommend parallel or asynchronous work only when independent useful work can
> overlap a demonstrated dependency or wait with a clear net structural benefit.
> Specify the existing owner, bounded resources, ordering/completion mechanism
> and failure/cancellation custody. Account for added transfers, launches,
> allocation, contention and coordination. Prefer eliminating work and reusing
> existing ordinary systems over introducing scheduling frameworks, passthrough
> owners, runtime reflection or duplicate state. Preserve canonical C++26 schema
> projection and direct, sealed component APIs.
>
> Perform a second adversarial source pass over every proposed correction:
> smallest and largest valid inputs, empty/tail/unaligned data, aliasing,
> progressive replacement, concurrent readers, partial writes, stale identities,
> capacity limits, exceptions, dependency loss, cancellation and shutdown.
> Reconstruct prior observable behavior through tests and integrations before
> proposing deletion, fusion or interface changes. Reject an apparent
> simplification that loses behavior, precision, error reporting or resource
> safety.
>
> This is static inspection only. Do not edit implementation, plans or
> documentation; do not build, run tests or generators, introduce instrumentation,
> run benchmarks/profilers, tune experimentally, commit or spawn subagents.
> Write only your consolidated report to `<REPORT PATH>` and return its path
> with a concise summary. Do not create `remediationplan.md` or another action
> plan. A separate standalone planning agent will consolidate all six reports
> afterward.
>
> Lead the report with the highest-value demonstrated findings. For each give:
> a stable finding ID, severity/priority and confidence; exact path, symbol and
> confirmed line; triggering workload and frequency; current work/bytes/passes
> or complexity and proposed reduction; causal source evidence; the minimum
> cohesive correction and true owning domain; affected callers/files; any
> cross-folder duplicate; safety/numerical invariants and required standard
> functional evidence for later implementation. Cite authoritative documentation
> where a claim depends on API or hardware semantics. Separate demonstrated
> waste from conditional opportunities whose benefit cannot be established by
> source, and make no measured throughput or latency claims.
>
> Finish with a coverage ledger for inspected files and relevant external
> boundaries, existing efficient mechanisms worth preserving, rejected
> optimizations and their costs, unresolved uncertainties, and a dependency
> ordering for actionable corrections. An assignment with no worthwhile finding
> is a valid result; do not manufacture optimization work.

## Standalone consolidation-agent prompt

Send only after all six reviews are complete. Replace `<BASE COMMIT>`:

> Work as a fresh standalone optimization-plan author in
> `/home/ryan/Repos/win/cplusplusloader`. Do not rely on or request inherited
> conversation context. The reviewed baseline is `<BASE COMMIT>`. Read
> `AGENTS.md`, `CONTRACT.md`, `reviewplan.md`, the relevant technical wiki and all
> six reports completely: `.cache/annotation.md`, `.cache/resample.md`,
> `.cache/upscale.md`, `.cache/gpu.md`, `.cache/serialization.md`, and
> `.cache/frontend.md`.
>
> Turn these reports into one actionable, cohesive `actionplan.md`. Use them as
> evidence, not six independent implementation roadmaps. Confirm each retained
> finding against source, callers, resource ownership, current dependencies and
> existing tests. Verify every expected file and line anchor. Explore outside
> the assigned directories where needed to resolve a shared cause or conflicting
> recommendation. Identify any stale or unsupported finding instead of
> propagating it into implementation requirements.
>
> Consolidate findings with the same owner or root cause. Assign each correction
> one authoritative home and arrange dependent work into the smallest cohesive
> phases that permit complete cutovers. Prefer eliminating repeated work,
> bounded storage reuse, direct ordinary APIs and canonical reflected declarations.
> Introduce parallel/asynchronous work only where the reports and source show
> useful independent work and a clear benefit after accounting for allocation,
> transfer, synchronization, contention, ownership and maintenance costs.
> Preserve both CC 8.6 and 8.9 targets, existing build policy, numerical behavior, formats,
> failure paths, cancellation, physical resource custody and integration outcomes.
>
> Incorporate demonstrated frontend state, redraw, cache and image-handling
> improvements in their actual Rust/Iced application or vendored library owners,
> covering `src/frontend`, `third_party/iced_aw`, `third_party/iced`,
> `third_party/iced_plot` and `third_party/iced-fluent-theme`. Keep application
> logic in the application layer. If the review justifies
> separating frontend test and release build needs, include that focused build
> change with exact wrapper/CMake/Cargo ownership, optimized product output,
> generated-artifact dependencies and standard validation coverage. Keep this
> an explicit planned adjustment; do not change unrelated build policies or
> prescribe Canvas, RGBA uploads or compiler flags already supplied by the
> existing path.
>
> Separate actionable source-proven reductions from speculative tuning. Do not
> manufacture a phase to use every CUDA feature or CPU instruction family.
> Do not add benchmarks, profiling, instrumentation or experimental tuning as
> implementation requirements or evidence. Standard functional, boundary,
> numerical, concurrency, failure and resource-safety checks belong to the
> future plan's normal Final Validation workflow.
>
> Write the plan with a problem statement or goal, Summary, Scope, Architecture,
> concrete dependency-ordered phases, all expected files, confirmed locations,
> exact actions, required cases/evidence, handoffs, risks and post-cleanup
> concerns. Follow current AGENTS.md plan-writing, executor/reviewer,
> post-main-phase audit, Final Validation and documentation rules without
> duplicating policy text. Preserve complete existing behavior unless an
> explicit accepted optimization requirement changes it.
>
> Edit only `actionplan.md`, publishing it atomically. Do not change source,
> tests, build wiring, reports, documentation, `reviewplan.md` or
> `remediationplan.md`. Do not implement the plan, build, test, generate, run
> benchmarks/profilers, commit or spawn subagents.
>
> Re-read the complete plan for coherent ownership, dependencies, vocabulary and
> executable requirements. Return its path, a concise phase/dependency summary,
> a mapping of every report finding to its retained phase or reason for exclusion,
> and any unresolved decision. The main agent will review scope and architecture;
> execution of this new plan is a separate authorization.
