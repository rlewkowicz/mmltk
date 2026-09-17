# Application Architecture

This is the authoritative architecture for the Linux, CUDA, and Wayland
application. It defines system boundaries, resource ownership, and stable
product outcomes. Implementation and action plans converge toward it.

## Application shape

The application shell constructs and connects independent product systems:

```text
Application shell
├─ Browser server
├─ Settings
├─ File dialogs
├─ Dataset compilation and inspection
├─ Model selection and preparation
├─ Training
├─ Validation
├─ Export
├─ Prediction
├─ Exploration
├─ Annotation
├─ Upscaling
├─ Live capture
├─ Presentation
└─ Firefox process ownership
```

Each product domain is an independent ordinary C++ system with a cohesive
public API, direct typed methods, minimal reflected snapshots, and typed UI
events. The shell supplies narrow dependencies and the application event sink.
Each system owns its mutable domain state and execution for its workload.

## Ownership

Consumers use the owner's typed API, snapshots, or borrowed reads. Generated
bindings project native facts; mutation remains with the owning system.

| Concept | Sole mutable owner |
| --- | --- |
| Application construction, connections, and shutdown request | Application shell |
| Local HTTP/WebSocket listener, active peer, transport queues, and backpressure | Browser server |
| Application settings and their persistence | Settings system |
| Native file-dialog requests | File-dialog system |
| Dataset compilation and artifact inspection | Dataset system |
| Model selection, weight acquisition, and preparation | Model system |
| Local and remote training runs, saved history, and continuation inspection | Training system |
| Validation runs, metrics, and retained sample products | Validation system |
| Model export and engine preparation | Export system |
| Prediction runs and latest image products | Prediction system |
| Dataset exploration, preview products, and exploration work | Exploration system |
| Editable annotation documents and editing history | Annotation system |
| Upscale work and derived image products | Upscale system |
| Live capture, processing, and image products | Live system |
| Foreground source routing, workspace admission, and graphics connection lifetime | Presentation system |
| Navigation, drafts, modal visibility, scroll, selection, and typed event reduction | Rust presentation model and the component owning each UI fact |
| Widgets, view transforms, styling, image metadata interpretation, and retained redraw images | Owning Rust/Iced components |
| Firefox process lifetime and Linux process registrations | Firefox process owner |
| Vulkan source images, independent backing allocations, views, external timelines, browser sample storage, graphics queues, swapchain, compositor cadence, and Wayland presentation | Firefox graphics integration |

Each domain system also owns its private workers, GPU resources, models, and
reusable content. All workspace sources use shared input and rendering facilities.
The common renderer owns autonomous incremental drawing, reusable display storage,
damage, and asynchronous graphics handoffs. Product systems supply their native
content and domain drawing operations. Rendering retained content reuses completed
inference, upscaling, decoding, and capture results.

Firefox allocates the foreground workspace's two physical Vulkan display buffers
and exports their independent memory and timeline resources. Native rendering
finalizes display pixels and signals readiness in its GPU execution boundary.
Presentation coordinates source routing and admission; native CUDA imports retain
backing and context custody through all native writes and raw readers. The FD
graphics connection and external GPU semaphores own the complete graphics handoff.

Every workspace tab captures and delivers mouse input through the same immediate
path and canonical vocabulary. Movement, all button transitions, click semantics,
wheel input, and cancellation reach the appropriate native owner in order.
Iced retains local view transforms and component state. Domain owners apply their
own interaction behavior through the shared input facilities.

Annotation owns its sole mutable document, editing history, hit testing, and
gestures. Its input executor uses the shared workspace input facilities.
The shared autonomous renderer consumes coherent native state independently of
input arrival.
GPU source preparation and color sampling return ordered continuations to the
document owner. Accepted ordinary input and document commands progress
independently of rendering and external image readers.

## Model, presentation model, and views

```text
  C++ systems (model)
    reflected types, snapshots, events, operations,
    defaults, validation, catalogs, domain display facts
                      │
            generated typed Rust module
                      │
          CBOR over bidirectional WebSockets
                      │
                      ▼
  Rust presentation model (controller/view-model)
    navigation, drafts, modal visibility, scroll, selection,
    typed event reduction, typed intent submission
                      │
                      ▼
  Iced views
    composition, widgets, layout, styling, themes, responsive behavior
```

Canonical C++ declarations own domain types, defaults, constraints, stable
identities, and operations. C++26 reflection derives structural projections,
validation, exhaustive dispatch, schema identity, and generated typed Rust
bindings and codecs. A schema change flows from that declaration through the
boundary. Ordinary classes retain direct public contracts and private state;
reflection supplies reusable structural machinery.

At runtime, the browser server and the Rust presentation model exchange bounded,
typed CBOR over bidirectional WebSockets bound to the local application
session. Rust verifies schema agreement before installing current native
snapshots. Input is fully validated before dispatch. Transport queues and
backpressure remain bounded; loss of essential state continuity closes the
peer so reconnection can install current snapshots.

The application connection carries input, settings, navigation, document
commands, and logical UI facts. Graphics allocations, completed images,
image-dependent metadata, and GPU read/write ownership flow through the separate
FD graphics connection. A workspace component binds to that graphics source
independently of application snapshot delivery.

Rust owns presentation state and deliberate Iced views. Typed native replies
and events update that state; UI actions submit typed operations to the owning
C++ systems. Component-local interaction stays local, and routing composes
pages through domain outcomes.

## Product interface

The Iced interface provides coherent training, validation, prediction, live,
annotation, export, and exploration workflows with Fluent styling, light and
dark themes, responsive workspaces, settings, diagnostics, and error surfaces.
Progress appears with its owning operation or model. Known totals support
determinate progress; open-ended work exposes stage, activity, and completed
work. Completion and failure come from typed native results and events.

Training, validation, and prediction start through their primary action,
including required settings settlement, model preparation, and input inspection.
Training owns current-format history and validated checkpoint continuation;
Rust/Iced owns a bounded retained chart dashboard in the aspect-selected center
workspace, defaulting to 16:9. Selected charts fit the available workspace without
internal scrolling and expand within that same region; wheel input belongs to
the ordinary page scroller. Live training progress appears in a separate card
below the charts, using current native image counts and measured rates independently
of explicitly selected saved history. Sparse scheduled evaluation observations
remain separate from live training samples and final-test products. Only the
selected evaluation weights are plotted; validation loss is not calculated or
plotted by the GUI. Validation owns
detailed metrics and up to six retained samples from its evaluation pass, with
paired prediction/ground-truth geometry and a detail viewer. Prediction
incrementally processes compiled images, ordinary images, and local video,
retaining the latest completed preview through completion or cancellation.
GUI prediction uses one image per batch; video has pause, resume, and stop.

Training admits target populations above the model's query count while retaining
the established assignment and loss semantics. Optional EMA remains GPU-resident;
each scheduled validation and best-weight decision use one selected weight set.
Metric publication and persistence progress independently of browser rendering
and telemetry storage pressure, with incomplete history explicitly visible.
Optional perceptual downscaling belongs to existing compilation and augmentation
owners and preserves categorical annotations and their geometric transforms.

Images and their annotation meaning share source identity and geometry through
preview, augmentation, upscale, and editing. Clean pixels and native semantic
image planes remain separate until producer-owned final display composition.
Iced draws text labels from typed facts paired with the displayed image through
the graphics connection. Class colors remain deterministic and stable across
filtering and transformations.
Visibility controls preserve the underlying objects. Annotation imports are
atomic; editing, undo, redo, and saving preserve operation order and geometry.
Derived results match the current source and requested processing parameters.

Iced owns fit, crop, pan, zoom, clipping, sampling, and redraws of completed
workspace images. Native product dimensions remain independent of window size.
Images and their attached metadata use the same view geometry.
Same-image revisions retain viewer identity and transforms; a new image resets
them. Stable widget identities preserve interaction state through ordinary
updates.

## GPU buffer flow

```text
Independent producers: raw products + reusable final display workspaces
                      │ completed image, attached metadata, ready semaphore
                      ▼
FD graphics connection: Vulkan allocations and CUDA/Vulkan handoffs
                      │ latest completed image with GPU read custody
                      ▼
Iced workspace drawing → Firefox swapchain/compositor → Wayland
                      │ final GPU read and reuse semaphore
                      └──────────────────────→ producer back-buffer reuse
```

Producers retain independent, reusable resources while backgrounded. A clean
single-plane product writes directly into available admitted final storage when
its raw-reader and graphics lifetimes permit. Otherwise raw production continues
in retained native storage. Products with native semantic planes retain those
raw planes and perform one fused final composition. Algorithm systems continue
to borrow raw products for explicit
receiver-owned copies; their clean pixels, documents, and semantic meaning
remain independent of display preparation.

Layout negotiation belongs to the exact browser device incarnation and physical
capacity. Firefox validates the shared-image capabilities and layout, creates
each independent Vulkan allocation, and completes initial external ownership
before exporting memory and timeline resources. Native CUDA imports the full
allocation on the matching producer execution owner. It retains its context
and independent backing custody through every native alias, including browser
replacement and process exit. The producer then fills the final workspace.
Late admission uses retained raw data without rerunning inference, reapplying
edits, or requiring another camera frame.
Logical completion and ordered input consumption never wait for the browser.

The foreground workspace has two persistent shared display buffers with front/back
ownership, independently of retained native image products and document history.
The producer writes a reusable back buffer, completes its image and attached
metadata, and signals readiness through the external GPU semaphore.
The graphics owner selects the latest completed buffer locally during draw
preparation and retains the previous completed image while newer work is
unfinished. The browser returns a buffer for reuse after its final GPU read
and ownership release. External semaphore completion orders actual device access.
Raw consumers independently retain the products they read.

Buffer promotion exchanges roles without copying pixels. Firefox directly samples
shared source storage when its requesting device supports the layout and usage.
The capability fallback performs one GPU copy into bounded reusable sample
storage. Copied pixels retain their own lifetime for future draws. Source
retirement waits for the source's physical GPU reads. Capacity growth preserves
the completed image until replacement is usable and retains useful high-water
capacity. Setup, growth, and retirement remain asynchronous and bounded.

Image dimensions, crop, atlas placement, labels, and other image-dependent facts
travel with the image they describe. Firefox transports application metadata
opaquely; the owning Rust workspace component interprets the generated native
types. The graphics handoff makes that image and its metadata available together.
Application snapshots independently describe logical UI state and operation
outcomes.

Display custody and each encoded draw retain independent resource references.
Actual submission settlement or unsubmitted abandonment releases draw custody.
Direct reads and capability copies preserve their respective physical completion
requirements. Page and device teardown settle terminal resource ownership
independently from rendering success. The last completed image remains drawable
through newer work, capacity pressure, source changes, or presentation failure.

Display-device mismatch uses producer-owned finalization through the existing
peer or reusable pinned transfer route. Same-GPU display keeps pixels on the
GPU; explicit diagnostic probes may read back samples. Firefox owns downstream
graphics queues, swapchain, compositor cadence, and Wayland presentation
independently of Live capture rate.

## Execution, failure, and shutdown

Ordinary C++ calls, compact results, exceptions, and RAII govern control flow.
A genuine domain lifecycle keeps its states and transitions private within its
system. Long-running systems own their workers, stop mechanism, GPU context,
streams, models, and bounded working storage. Threads bind the appropriate GPU
context before issuing work, and algorithmic parallelism stays with the
algorithm's owner. GPU boundary execution declares device-local permitted CPU
and memory placement. Workers retain fixed CPU assignments and verify strict
memory locality and required high normal-scheduler and storage I/O priorities
before accepting work. Owned host transfer and solver storage retains verified
local pages through every consumer; unavailable required placement or policy
becomes an operation failure. Independent systems own their worker budgets and
may overlap eligible CPUs.

Duplicate discrete jobs report busy. Ordered document commands retain their
admitted sequence, and cancellation requests the owning system's stop mechanism.
Synchronous exceptions propagate through direct calls
and become typed failures once at the nearest operation, worker, or external
service boundary. A failed system preserves valid snapshots, reports failure,
and retires its failed resources safely. Recoverable runtimes reconstruct
lazily within that system, while independent systems continue operating.
Persisted settings and the format-7 compiled dataset remain stable. Native RF-DETR
checkpoints use only the current version-3 format; external upstream assets retain
their independent import formats.

An independent immutable data catalog owns exact foreground names and dense
zero-based foreground references. Source category IDs, foreground references,
physical model output slots, and external output IDs are distinct domains. Model
artifacts carry validated output roles, score encoding, class layout, and
provenance. Unknown external identities remain visibly raw and cannot enter
semantic evaluation. Artifact and descriptor identity is checked at admission
and replacement, never per prediction.

Fresh transfer maps verified class-dependent state by semantic identity while
preserving unmatched initialized values; resume requires exact layout and state.
Training splits share exact ordered catalogs; standalone evaluation admits an
explicitly verified catalog permutation. Background/no-object meaning belongs
to the artifact's validated layout and never becomes a foreground detection.
Prediction, analysis, annotation, and presentation preserve the declared reference
domain and catalog with their owned data. Mask storage is consumed only when
the current result explicitly declares masks available.

RAII protects complete and partial construction, borrowed views, mappings,
asynchronous GPU work, and external consumers. Resource release respects GPU
completion and consumer lifetime, including cancellation, dependency loss,
capacity failure, and shutdown.

```text
Stop browser ingress → request system stops → return from browser loop
    → stop and join Firefox → join system workers → release resources
```

The shell initiates shutdown; systems finish their own work and reverse-order
RAII releases physical resources. Vulkan images and semaphores retain their
device, and native CUDA imports retain their context and independent backing,
through their own completed uses. Registry removal, IPC closure, and exporter
exit do not substitute for physical resource settlement. Failures leave
resources safe to destroy and shutdown outcomes observable.

## Performance and observability

Execution uses system-owned workers and completion notifications, with bounded
outstanding GPU work. Independent systems run concurrently. Ordered mouse input
preserves every accepted event through temporary pressure and reaches the native
owner immediately. Native event storage retains useful high-water capacity.
Input consumption, document-command settlement, and image publication progress
independently while retaining their required order. Long-lived buffers reuse
tightly sized storage; steady-state graphics reuses geometry, image planes, and
staging while updating affected content.

Logical annotation UI facts describe the latest committed document. A new gesture
resolves its target from the current native document and logical tool; accepted
progress retains that target independently of rendering. Document/history-owned
identities preserve editing and Undo/Redo without changing saved formats. Input
consumption and GPU-dependent document commands retain their separate completion
requirements. The autonomous renderer consumes coherent native state and keeps
its useful caches while external display storage is occupied.

Explore atlas loading follows the visible viewport and scroll direction: visible
rows first, then leading rows, then prior rows. Cached rows entering the viewport
populate its first result before additional content is fetched. Viewport changes,
newly loaded content, and actual product changes update the content used by shared
incremental rendering. All visual producers use that autonomous rendering path.
Browser redraws follow the visible window's graphics cadence and can reuse unchanged
completed pixels.
The optional workspace FPS display counts actual browser queue submissions
containing workspace draws and belongs to the workspace component.

Opt-in JSONL diagnostics provide granular system, operation, resource, and
failure context. Disabled diagnostics create no active diagnostic or probe
state and perform no diagnostic collection, formatting, clock reads, counter
updates, or I/O. Diagnostic identities remain effect-only observations.
Explicitly enabled fatal diagnostics capture bounded troubleshooting context
before orderly shutdown.
