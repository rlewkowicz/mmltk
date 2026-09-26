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
| Prediction runs, saved media, and latest image products | Prediction system |
| Dataset exploration, preview products, and exploration work | Exploration system |
| Editable annotation documents and editing history | Annotation system |
| Upscale work and derived image products | Upscale system |
| Live capture, processing, and image products | Live system |
| Foreground source routing, workspace admission, and graphics connection lifetime | Presentation system |
| Navigation, drafts, modal visibility, scroll, selection, and typed event reduction | Rust presentation model and the component owning each UI fact |
| Widgets, view transforms, styling, image metadata interpretation, and retained redraw images | Owning Rust/Iced components |
| Firefox process lifetime and Linux process registrations | Firefox process owner |
| Vulkan source images, independent backing allocations, views, external timelines, browser sample storage, graphics queues, swapchain, compositor cadence, and Wayland presentation | Firefox graphics integration |

Shared input and rendering facilities serve every workspace source. Product
systems retain domain interaction and content ownership; the common renderer
draws retained content independently of input and computation. Presentation
coordinates foreground routing and graphics admission.

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

Canonical C++ declarations own domain facts and operations. C++26 reflection
derives validation, exhaustive dispatch, schema identity, and typed bindings
from those declarations. Ordinary classes own behavior and private state;
Rust/Iced owns presentation and interaction.

The local application connection carries validated input, operations, and
logical state through bounded transport. Schema agreement precedes snapshot
installation; loss of essential continuity reconnects to current snapshots.
The separate FD graphics connection carries completed images, paired metadata,
and GPU ownership. Application state and display readiness progress independently.

Typed results update the Rust presentation model; UI actions reach the owning
C++ system. Components retain local interaction state, and destination pages
select their foreground sources. The [source guide](docs/architecture.md) locates
these boundaries.

## Product interface

Train, Validate, Predict, and Export share model preparation, artifact selection,
and output presentation while retaining independent admission and execution.
Explicit start actions capture validated settings, inputs, destinations, and
compute-device choices; active operations expose cancellation. Completed artifacts
and previews survive interruption or failure, and success settles required output
writes.

Validation and exploration share viewers while retaining their own results and
display policies. Prediction owns incremental image/video processing and output
policy; native media facilities preserve source timing, audio, and interrupted
output. Display filtering preserves inference, evaluation, and annotation meaning.
Annotation owns atomic import, ordered editing, undo/redo, and explicit saving.
Upscale consumes the selected native source; Live owns capture and processing.

Rust/Iced provides responsive themed workflows, retained interaction state, and
operation-local progress. A bounded history in frontend memory retains warnings
and errors across reconnects while preserving active work. Settings and Status
remain reachable, and browser recovery remains available if Iced cannot render.
[Workflow behavior and artifacts](docs/rfdetr-workflows.md) and
[GUI interaction](docs/gui-interaction.md) have their detailed references.

## Training lanes and handoffs

Native training separates logical lanes from physical execution capacity. The
session owns data planning and distributed communication order. Each logical
model owns its optimizer and training state, with an AdamW, Muon, or SGD recipe.

```text
SHARED GRADIENTS                    INDEPENDENT / PERIODIC
┌─────────────────────────────┐    ┌─────────────────────────────────┐
│ Logical lanes               │    │ Lane A → Model A + optimizer A  │
│       ↓ gradients           │    │ Lane B → Model B + optimizer B  │
│ One model + one optimizer   │    │   …                             │
└─────────────────────────────┘    └─────────────────────────────────┘

EVERY MODEL • distributed data parallel (DDP) across all selected GPUs
Global microbatches → GPU slices → gradient SUM → agreed optimizer update

PERIODIC ONLY • after admitted work settles
Model weights → native average → every model on every GPU
                Optimizers and other training state stay per model
```

Models share a verified initialization while retaining independent trajectories.
Logical batches and training semantics survive changes in physical concurrency.
Periodic averaging weights models by images in successful updates. Validation
and final selection use a common evaluation context independently of merge
cadence; individual artifacts survive selection or export failure.

```text
TRAINING PRODUCTS
Complete immutable session ───────────────────────→ exact Resume

Retained model candidates → native selection / weight averaging
                                             │
                                             ▼
                                  frozen native artifact
                                             ├─ final test
                                             ├─ export
                                             └─ Transfer
```

Resume restores the complete session; Transfer maps deployment weights by class
meaning. Final test consumes the frozen selection. Metrics and persistence
progress independently of browser rendering.

Inference workflows own bounded parallel lanes with ordered delivery. Immutable
weights may be shared; each lane retains its mutable execution and storage.
Readers settle before model weights change or lane pools retire.

## Data and model meaning

An immutable data catalog defines class meaning independently of model execution.
Dataset categories, catalog references, model output slots, and external identities
remain distinct. Admission validates artifact identity and class meaning.
Unresolved external identities remain raw and are excluded from semantic
evaluation. Systems preserve the declared catalog and reference domain with
their products.

Compilation shares acquisition, retained source caching, and atomic publication
across dataset recipes. Readers retain consistent source generations through
completion and repair. Products retain source identity, annotation meaning,
provenance, and the relationship between source and derived geometry through
compilation, preview, augmentation, upscaling, and editing. Clean pixels and
semantic planes remain separate until final display composition.

Iced applies a common view transform to completed pixels and their metadata,
independently of native product dimensions. Annotation imports preserve the
displayed geometry; derived products retain their source correspondence.
Visibility changes preserve underlying objects.
[Dataset formats and compilation](docs/datasets.md) own the detailed data rules.

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

Independent producers retain raw products and reusable display resources.
Shared native rendering prepares final pixels from retained content; input and
logical work progress independently of browser availability. Product handoffs
retain borrowed reads until receiver-owned copies complete.

Firefox allocates and exports Vulkan storage; native CUDA owns imported access
and producer completion. Persistent front/back buffers exchange roles after
completed writes, with GPU semaphores ordering access. Image-dependent metadata
travels with its image: Firefox transports it opaquely and Rust interprets the
generated native types.

Firefox samples shared storage directly where supported and otherwise uses a
bounded reusable GPU copy. The last completed image remains drawable through
new work, capacity growth, and presentation failure. Producers, readers, and
draws retain storage, backing, devices, and contexts until physical use
settles, including across browser replacement and process exit.

Wayland GPU presentation is the sole display path. The display device follows
the Wayland session independently of workflow compute devices; producers own
required transfers between them. Same-device display stays on the GPU, and
Firefox owns presentation cadence independently of capture.
[GPU execution](docs/gpu-execution.md) owns allocation and transfer details.

## Execution, failure, and shutdown

Each long-running system owns its workers, cancellation, GPU context, and
bounded working storage. Device-local CPU and memory placement and required
execution policies are verified before work is admitted. Event-driven execution
allows independent systems to progress concurrently.

Discrete jobs report busy while occupied. Accepted input and document commands
retain their required order independently of rendering. Failures become typed
outcomes at the owning boundary, preserve useful causes and valid state, and
retire failed resources safely. Recovery stays within the affected system while
independent systems continue.

The shell initiates coordinated shutdown; each system settles its own work.

```text
Stop browser ingress → request system stops → return from browser loop
    → stop and join Firefox → join system workers → release resources
```

RAII protects partial construction, borrowed data, asynchronous work, and external
consumers. Release follows physical GPU completion and consumer lifetime through
cancellation, dependency loss, and shutdown.

## Performance and observability

Work follows product changes, demand, and completion notifications. Independent
input, computation, publication, and redraw paths reuse completed products and
retained capacity. Incremental rendering updates affected content; idle, hidden,
and unchanged display work remains quiet. Resource growth and outstanding work
stay bounded.

Opt-in diagnostics provide system and failure context. Disabled diagnostics do
no diagnostic work. Diagnostic identities observe behavior; product ordering,
validity, and resource lifetime remain independent of them. Crashes and fatal
operation or process failures report a concise cause on stderr even when
diagnostics are disabled. Healthy shutdown remains quiet.
[Logging](docs/logging.md) owns operational details.
