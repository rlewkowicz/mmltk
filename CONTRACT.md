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
| Vulkan source images, independent backing allocations, views, external timelines, browser sample storage, graphics queues, swapchain, compositor cadence, and Wayland presentation | Firefox graphics integration |

Each domain system also owns its private workers, GPU resources, models,
reusable staging, and final display workspace demand and reuse. Producers finalize
display pixels in their own execution boundary. Firefox allocates each physical
Vulkan workspace and exports its independent memory and timeline resources.
Presentation coordinates admission; native CUDA imports retain backing and
context custody through all native writes and raw readers.

Annotation owns an independent input executor and its sole mutable document,
editing history, and gesture reduction. Its renderer receives bounded immutable
descriptions, retaining the newest unsubmitted work. GPU source preparation and
color sampling return ordered continuations to the document owner. Accepted
ordinary input and document commands progress independently of rendering and
external image readers.

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
native semantic image planes remain separate until producer-owned final display
composition.
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
                      │ completed workspace and exact model facts
                      ▼
PresentationSystem: allocation/import coordination + completed offers
                      │ nonblocking exact read acquisition
                      ▼
Firefox: directly sampled source, or capability-selected single copy
                      │ acquired sample + matching model
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
pitch, offset, and ownership. Firefox creates and binds each independent Vulkan
allocation and completes initial external ownership before exporting memory and
timeline descriptors. Native CUDA imports the full allocation on the producer
execution owner and exposes its pitched image view. It retains its CUDA context
and an independent backing descriptor through every native alias, including
browser replacement and process exit. The producer then fills the final workspace. Late admission uses retained raw data
without rerunning inference, reapplying edits, or requiring another camera frame.
Logical completion and ordered input consumption never wait for the browser.

Presentation publishes a completed workspace's exact identity on that source's
native graphics timeline. Availability alone grants no browser read custody.
Firefox acquires the newest completed publication authorized by available model
facts during draw preparation. Acquisition and producer reservation share the
physical allocation's nonblocking gate. A failed acquisition retains the completed
fallback without waiting for unfinished production. Source observation,
product revision, physical allocation, source admission, and sample-arena
identity have separate meanings. Selecting a retained gallery may publish an
older real product revision under a newer domain observation.

Each selected producer has reusable current and overflow capacity. Promotion
exchanges roles without copying pixels. Raw consumers and externally acquired
readers independently prevent reuse; unacquired overflow remains replaceable.
Firefox directly samples its shared source storage when the actual requesting device
supports the exact linear image, sampled usage, external handle, and legal image
layout. Otherwise, it uses one copy into a reusable two-slot sample arena.
Direct mode allocates no sample arena and performs no presentation copy.
Source retirement waits for that source's GPU reads. Copied pixels retain their
own lifetime for future draws. Capacity growth retains the old completed image
until replacement is usable. Setup and retirement run through asynchronous
graphics owners; active, candidate, and retiring storage remain bounded.

Physical source-read settlement and page sample release are separate receipts.
Copy mode additionally proves actual native-to-sample completion. Iced samples
the exact acquired source or copied slot with its retained model facts. Display
custody and each encoded draw retain independent references; actual submission
settlement or unsubmitted abandonment releases draw custody. A completed direct
acquisition or a completed capability copy promotes the matching fallback.
Page and device teardown settle terminal resource ownership independently from
rendering success. The last completed browser image remains drawable through
newer work, capacity pressure, source changes, or presentation failure.

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
RAII releases physical resources. Vulkan images and native CUDA imports each
retain physical backing through their own completed uses. Failures leave resources safe to destroy and shutdown outcomes
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

Logical annotation UI facts describe the latest committed document. Published
frame facts retain the exact rendered scene and preview generation, including
completed frames overtaken by newer input. Preview-only publications use compact
progress without repeating document storage. Canonical structural projections
retain body hit geometry and selected handles separately from editable names and
other persisted scene data. Reconnect supplies current logical state and retained
drawable facts within the existing snapshot and Bootstrap bounds. New gestures
bind to displayed document geometry and the current logical tool. Runtime object
and element identities belong to the document and its history, so surviving
targets remain editable while rendering lags, deleted targets cannot silently
retarget reused indices, and Undo/Redo preserves identity without changing saved
formats. Same-document accepted progress retains its gesture target independently
of newer rendering. Input credits acknowledge reduction;
GPU-dependent command continuations retain their separate settlement ordering.

Opt-in JSONL diagnostics provide granular system, operation, resource, and
failure context. Disabled diagnostics create no active diagnostic or probe
state and perform no diagnostic collection, formatting, clock reads, counter
updates, or I/O. Diagnostic identities remain effect-only observations.
Explicitly enabled fatal diagnostics capture bounded troubleshooting context
before orderly shutdown.
