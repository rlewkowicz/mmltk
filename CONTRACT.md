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
| Foreground source selection, final native composition, exported backbuffer, and native graphics timeline | `PresentationSystem` |
| Navigation, drafts, modal visibility, scroll, selection, and typed event reduction | Rust presentation model and the component owning each UI fact |
| Widgets, view transforms, styling, rendering, and retained redraw images | Owning Rust/Iced components |
| Firefox process lifetime and Linux process registrations | `FirefoxProcessOwner` |
| Native texture import, browser sample storage, graphics queues, swapchain, compositor cadence, and Wayland presentation | Firefox graphics integration |

Each domain system also owns its private workers, GPU resources, models, and
reusable staging as its workload requires. `PresentationSystem` is the single
native compositor and writer of the exported backbuffer. Producer execution
stays with the producer.

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
native semantic image planes remain separate until final native composition.
Iced draws text labels from matching typed annotation facts. Class colors
remain deterministic and stable across filtering and transformations.
Visibility controls preserve the underlying objects. Annotation imports are
atomic; editing, undo, redo, and saving preserve operation order and geometry.
Derived results match the current source and requested processing parameters.

Iced owns fit, crop, pan, zoom, clipping, sampling, and redraws of completed
browser images. Native product dimensions remain independent of window size.
Images and semantics use the same view geometry. Same-image revisions retain
viewer identity and transforms; a new image resets them. Stable widget
identities preserve interaction state through ordinary updates.

## GPU buffer flow

```text
Independent producers: private GPU products and resources
                      │ selected complete product; borrowed read
                      ▼
PresentationSystem: receiver-owned GPU copy + final composition
                      │ copy completes; borrowed read releases
                      ▼
One published native application backbuffer
                      │ synchronized import and browser-owned GPU copies
                      ▼
Completed browser image → Iced view transforms and rendering
                      │
          Firefox swapchain/compositor → Wayland
```

Source selection changes which product Presentation copies. Producers retain
independent, long-lived private resources and continue producing while
backgrounded. The native graphics timeline protects final backbuffer writes
and browser reads. Firefox's downstream graphics queues, swapchain, compositor,
and Wayland cadence remain independent.

Cross-system images use typed borrowed read views for one explicit
receiver-owned copy. Source storage stays valid until GPU completion, and the
receiver publishes only its own storage. Copies use the shortest supported
route: device-local or peer access where available, with reusable pinned
staging for transfers that require it. Browser display remains entirely on
the GPU through native import and WebGPU/Vulkan.

Pending presentation work coalesces to the newest complete selected product
with bounded outstanding work. The last valid completed browser image remains
available during subsequent native work, capacity pressure, or presentation
failure. Obsolete results retain their physical release identity and cannot
replace a different current product.

Capacity growth prepares an unpublished replacement, completes initialization
and the required import, then atomically promotes the completed replacement.
The previous allocation remains valid until its consumers release it. There
is one published native application backbuffer; private producer buffers and
browser-owned samples have their own lifetimes.

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

Execution is event-driven and bounded. Independent systems run concurrently;
replaceable high-rate work coalesces in constant time, while ordered editing
retains its order. Long-lived buffers retain useful high-water capacity and
reuse tightly sized storage. Steady-state graphics work avoids allocation and
unnecessary CPU/GPU transfers, blocking, and memory churn.

Opt-in JSONL diagnostics provide granular system, operation, resource, and
failure context. Disabled diagnostics avoid material data collection and
formatting. Fatal diagnostics capture bounded troubleshooting context before
orderly shutdown.
