# Application Architecture

This is the authoritative architecture for the Linux, CUDA, and Wayland
application. It defines system boundaries, resource ownership, and stable
product outcomes. Implementation and action plans converge toward it.

## Application shape

`ApplicationShell` constructs and connects the application:

```text
ApplicationShell
├─ BrowserServer
├─ SettingsSystem
├─ FileDialogSystem
├─ DatasetSystem
├─ ModelSystem
├─ TrainingSystem
├─ ValidationSystem
├─ ExportSystem
├─ PredictSystem
├─ ExploreSystem
├─ AnnotationSystem
├─ UpscaleSystem
├─ LiveSystem
├─ PresentationSystem
└─ FirefoxProcessOwner
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
| Application construction, connections, and shutdown request | `ApplicationShell` |
| Local HTTP/WebSocket listener, active peer, transport queues, and backpressure | `BrowserServer` |
| Application settings and their persistence | `SettingsSystem` |
| Native file-dialog requests | `FileDialogSystem` |
| Dataset compilation and artifact inspection | `DatasetSystem` |
| Model selection, weight acquisition, and preparation | `ModelSystem` |
| Local and remote training runs | `TrainingSystem` |
| Validation runs and results | `ValidationSystem` |
| Model export and engine preparation | `ExportSystem` |
| Prediction runs and image products | `PredictSystem` |
| Dataset exploration, preview products, and exploration work | `ExploreSystem` |
| Editable annotation documents and editing history | `AnnotationSystem` |
| Upscale work and derived image products | `UpscaleSystem` |
| Live capture, processing, and image products | `LiveSystem` |
| Foreground source selection, workspace admission, exact publication, and native graphics timelines | `PresentationSystem` |
| Navigation, drafts, modal visibility, scroll, selection, and typed event reduction | Rust presentation model and the component owning each UI fact |
| Widgets, view transforms, styling, rendering, and retained redraw images | Owning Rust/Iced components |
| Firefox process lifetime and Linux process registrations | `FirefoxProcessOwner` |
| Native texture import, browser sample storage, graphics queues, swapchain, compositor cadence, and Wayland presentation | Firefox graphics integration |

Each domain system also owns its private workers, GPU resources, models,
reusable staging, and final shareable display workspaces. Producers finalize
display pixels in their own execution boundary. Presentation coordinates their
admission and publication; it does not allocate, clear, copy, or compose images.

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

At runtime, `BrowserServer` and the Rust presentation model exchange bounded,
typed CBOR over bidirectional WebSockets bound to the local application
session. Rust verifies schema agreement before installing current native
snapshots. Input is fully validated before dispatch. Transport queues and
backpressure remain bounded; loss of essential state continuity closes the
peer so reconnection can install current snapshots.

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

Images and their annotation meaning share exact source identity, revision, and
geometry through preview, augmentation, upscale, and editing. Clean pixels and
native semantic image planes remain separate until producer-owned final display composition.
Iced draws text labels from matching typed annotation facts. Class colors
remain deterministic and stable across filtering and transformations.
Visibility controls preserve the underlying objects. Annotation imports are
atomic; editing, undo, redo, and saving preserve operation order and geometry.
Derived results match the current source and requested processing parameters.

Iced owns fit, crop, pan, zoom, clipping, sampling, and redraws of browser-owned
images authorized by matching native state. Native product dimensions remain
independent of window size. Images and semantics use the same view geometry.
Same-image revisions retain viewer identity and transforms; a new image resets
them. Stable widget identities preserve interaction state through ordinary
updates.

## GPU buffer flow

```text
Independent producers: raw products + final shared display workspaces
                      │ exact selected workspace read
                      ▼
PresentationSystem: layout/import coordination + ready/release timeline
                      │ no native image copy
                      ▼
Firefox: imported source → retained two-slot sample arena
                      │ queue-ordered sample + matching model
                      ▼
Iced rendering → Firefox swapchain/compositor → Wayland
```

Producers retain independent, reusable resources while backgrounded. A clean
single-plane product uses its admitted final storage directly. Products with
native semantic planes retain those raw planes and perform one fused final
composition. Algorithm systems continue to borrow raw products for explicit
receiver-owned copies; their clean pixels, documents, and semantic meaning
remain independent of display preparation.

Layout negotiation belongs to the exact browser device incarnation and physical
capacity. Firefox validates external-image support, UUID, memory requirements,
pitch, offset, and ownership. The producer creates its exportable allocation
from those immutable facts. Firefox completes initial source ownership before
the producer fills the final workspace. Late admission uses retained raw data
without rerunning inference, reapplying edits, or requiring another camera frame.
Logical completion and ordered input consumption never wait for the browser.

Presentation borrows the selected completed workspace, awaits producer readiness,
and publishes its exact identity on that source's native graphics timeline. Its
counted source read lasts through Firefox's matching GPU release, including
release-only consumption when no sample slot is available. Source observation,
product revision, physical allocation, source admission, and sample-arena
identity have separate meanings. Selecting a retained gallery may publish an
older real product revision under a newer domain observation.

Firefox retains one reusable two-slot sample arena across producer and pool-slot
rotation. One slot can hold a completed fallback while another receives a new
sample. Source retirement waits for that source's GPU reads; it does not wait
for future draws of copied browser pixels. Arena retirement independently waits
for page references and GPU readers. Capacity growth prepares an unpublished
replacement, retains the old completed arena until replacement is usable, then
drains its readers. Active, candidate, and retiring storage remain bounded.

Matching native model state authorizes a queue-ordered browser sample immediately
after submission. Physical native-to-sample completion and page sample release
are separate receipts. The application currently captures that sample into its
retained Iced image, with its own exact GPU completion hold; direct Iced sampling
is a subsequent cutover. The last completed browser image remains drawable
through newer work, capacity pressure, source changes, or presentation failure.

Display-device mismatch uses producer-owned finalization through the existing
same-device, peer, or reusable pinned transfer route. Browser display remains
entirely on the GPU. Firefox owns downstream graphics queues, swapchain,
compositor cadence, and Wayland presentation independently of Live capture rate.

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

Duplicate discrete operations report busy; cancellation requests the owning
system's stop mechanism. Synchronous exceptions propagate through direct calls
and become typed failures once at the nearest operation, worker, or external
service boundary. A failed system preserves valid snapshots, reports failure,
and retires its failed resources safely. Recoverable runtimes reconstruct
lazily within that system, while independent systems continue operating.
Persisted settings, dataset, model, and artifact formats remain stable.

RAII protects complete and partial construction, borrowed views, mappings,
asynchronous GPU work, and external consumers. Resource release respects GPU
completion and consumer lifetime, including cancellation, dependency loss,
capacity failure, and shutdown.

```text
Stop browser ingress → request system stops → return from browser loop
    → stop and join Firefox → join system workers → release resources
```

The shell initiates shutdown; systems finish their own work and reverse-order
RAII releases physical resources. Firefox's imported allocations outlive its
use of them. Failures leave resources safe to destroy and shutdown outcomes
observable.

## Performance and observability

Execution is event-driven, with bounded native admission and outstanding GPU
work. Independent systems run concurrently; replaceable high-rate work
coalesces in constant time, while ordered editing preserves every accepted
path-dependent sample through temporary pressure.
Input consumption, document-command settlement, and image publication progress
independently while retaining their required order. Long-lived buffers retain
useful high-water capacity and reuse tightly sized storage. Steady-state
graphics work avoids allocation and unnecessary CPU/GPU transfers, blocking,
and memory churn.

Opt-in JSONL diagnostics provide granular system, operation, resource, and
failure context. Disabled diagnostics create no active diagnostic or probe
state and perform no diagnostic collection, formatting, clock reads, counter
updates, or I/O. Diagnostic identities remain effect-only observations.
Explicitly enabled fatal diagnostics capture bounded troubleshooting context
before orderly shutdown.
