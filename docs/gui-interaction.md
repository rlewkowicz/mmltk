# GUI interaction and presentation

[Wiki index](README.md) · [Source guide](architecture.md) · [GPU execution](gpu-execution.md) · [Diagnostics](logging.md)

The [architectural contract](../CONTRACT.md) defines ownership. This page
explains how input, native state, GPU images, and Iced views cross those
boundaries. These paths are event-driven; they make no hard frame-rate or
input-to-display latency guarantee.

## Typed application boundary

The current application package protocol is **16**. Canonical C++ declarations
own native types, field identities, constraints, defaults, endpoints, events,
and snapshots. C++26 reflection generates typed Rust projections, compact
interaction codecs, validation, exhaustive dispatch, and schema identity.
Handwritten Rust owns component state, visual copy, navigation, layout,
styling, and interaction.

[client_record.h](../src/controller/browser/client_record.h) declares the
outer records and protocol version;
[application_schema.h](../src/controller/browser/application_schema.h) and
[application_materializer.h](../src/controller/browser/application_materializer.h)
derive the application boundary. The bootstrap supplies the schema
fingerprint, current snapshots, and input peer epoch. The client validates
agreement before installing native state. Compact Annotation and Explore
interactions travel on the existing session-bound CBOR/WebSocket connection.

Application output objects in snapshots, replies, and events use positional
CBOR arrays in canonical reflected member order. Each declared field has a
slot, including null optionals, and generated decoders enforce the exact field
count and constraints. Schema agreement authorizes those positions; they are
not handwritten field inventories. Scalars, byte strings, named variant
discriminators, and opaque relation storage retain their canonical policies.
Ordinary intent field IDs and named request values keep their existing
representation. Settings and annotation persistence keep their named
representation; application transport does not change saved formats.
The separate transport projection is
defined in [reflected_cbor.h](../src/frameworks/serialization/reflected_cbor.h).

Compact interactions use numeric opcodes generated from canonical endpoint
order; their envelopes do not repeat protocol and endpoint identities already
established by the connection. Annotation movement uses point deltas only
when reconstruction is bit-exact and retained gesture metadata is unchanged.
Begin/End/Cancel, changed parameters or targets, and inexact deltas use full
absolute pointer records. Fractional coordinates and every accepted
path-dependent sample survive this encoding. CBOR floats use the smallest
exact representation.

Protocol 16 is a complete package boundary: native host, Iced bundle, generated
bindings, and cross-language fixtures must agree. Follow
[binding generation and packaging](build.md#generated-bindings-and-dependency-maintenance);
generated Rust is build output. The separate native/Firefox graphics ABI
projects physical import records and frame signals; it does not carry
application intents or own UI behavior.

## Ordered annotation input and retained storage

The owning Iced canvas converts pointer movement through its view transform
into native image coordinates and typed pointer targets. Each sample carries
Begin, Update, End, or Cancel, plus interaction and sample sequence identities.
The frontend keeps accepted samples until the native CPU-consumption watermark
acknowledges their batch:

```text
Iced gesture
    → retained frontend samples
    → compact batches on the existing socket
    → native ordered CPU reduction
          ├─ cumulative consumption credit → frontend storage reuse
          └─ newest renderable state → GPU publication → presentation
```

| Storage | Capacity and lifetime |
| --- | --- |
| Frontend sample deque | Starts at 64 samples, grows geometrically as needed, retains high-water capacity across gestures |
| Frontend batch and codec buffers | Reused; capacities derive from generated native limits |
| Native input slots | Two reusable slots, each holding at most 32 samples |
| Native render descriptions | At most three reusable descriptions across input scratch, newest pending work, and active GPU work |

[AnnotationInput](../src/frontend/iced/src/annotation_input.rs) owns accepted
samples, in-flight batches, document-command barriers, and reusable encoding
storage. A batch does not cross a document epoch or a queued command's sample
frontier. Both native slots being occupied leaves further samples retained
in the frontend. It does not cancel the gesture or retire the peer. Extended
stalls can grow frontend memory; failed growth reports a connection/input
failure instead of silently committing a shortened stroke.

[AnnotationSystem](../src/controller/subsystems/annotation/annotation_system.h)
validates the peer epoch, document epoch, next batch sequence, sample bounds,
and available credits before admission. Its independent input worker owns
the mutable `AnnotationDocument`, reducer, and history. It applies batches in
order and preserves every path-dependent brush segment. Each consumed batch
frees its slot and publishes a cumulative watermark **before GPU publication or a
GPU render wait**. Input-progress records have reserved transport capacity and
are processed before ordinary frontend application events. A stale peer's
credits cannot release a replacement peer's samples.

## Document commands and settlement

Annotation Open, Edit, Save, and Stop share ordered command admission.
A command waits until every earlier accepted sample has been CPU-consumed.
Later samples wait behind that command's settlement barrier. Ordinary document
editing and saving settle on the input owner independently of display
publication. Source opening and color sampling use explicit asynchronous GPU
continuations; subsequent document effects retain their required order.

A successful admission reply with `busy=true` is not completion. The frontend
retains its barrier until the corresponding later non-busy native state has
arrived; reply and event arrival order may differ. Rejected commands clear
their barrier through the typed reply. Stop can pass an already unsettled
command to request cancellation without taking over that command's settlement.
Renderer observations and other systems' traffic continue independently.

CPU credits therefore prove input consumption, not successful saving,
rendering, or command completion. Domain rejections remain typed application
outcomes. Invalid wire input, continuity loss, allocation failure, and peer
replacement have their own connection/failure paths. Peer closure orders the
native gesture cleanup before admitting input for a new peer.

## Reduction, publication, and source changes

[annotation_system.cpp](../src/controller/subsystems/annotation/annotation_system.cpp)
separates ordered reduction from a dedicated renderer. The input owner captures
an immutable exact description, including shared scene and identity storage,
editor facts, and preview geometry. It replaces only the newest unsubmitted
description. The renderer retains its active description through GPU settlement.
If no output allocation is writable, it retains the completed clean baseline
and pending description and arms an availability notification while input keeps
progressing. A receiver release triggers another publication attempt. There is
no fixed batching delay, frame timer, or FIFO of intermediate render requests.

Logical UI facts describe the latest committed document; rendered scene and
frame facts describe the exact completed pixels, even when newer input has
overtaken them. Full Annotation UI state and compact frame progress have
separate revisions. Preview-only publications do not resend the document.
Canonical reflected projections keep body hit geometry, selected handles, and
preview geometry separate from editable names and persistence-only facts.
Reconnect carries both current logical state and retained drawable geometry
within the existing snapshot and Bootstrap limits. Document import and size
validation complete before committed state is replaced.

A new gesture resolves its target from displayed geometry and uses the current
logical tool. Runtime object and element identities travel with that geometry;
the document resolves them into its current indices. Surviving objects remain
editable while rendering lags, and deleted targets cannot silently retarget a
reused index. Undo/Redo retains those identities with history. They are not saved
in the named persistence format. Accepted same-document gesture progress keeps
its original target through newer render publications.

Producer events notify native Presentation directly through
[ApplicationEventPublisher](../src/controller/browser/application_event_publisher.h)
and [the shell wiring](../src/controller/shell/direct_visual_systems.cpp).
`PresentationSystem::SourceChanged` schedules the currently selected source.
The frontend selects a source when the viewed product changes; it does not
reselect that source for each new frame.

Presentation keeps one submitted selection and the newest pending selection
or source notification. If a borrow is rejected, newer observed source facts
can trigger immediate catch-up. An unchanged unavailable observation waits
for the owner's next notification. It cannot make progress by repeatedly
borrowing the same stale observation.

## Native GPU custody and completion

Each visual producer owns its raw processing products and final shareable
display workspaces:

| Producer | Raw product and retained work | Display preparation |
| --- | --- | --- |
| [Explore](../src/controller/subsystems/explore/native_explore_algorithm.cpp) | Individual thumbnail cache, gallery clean/semantic atlas, and separate detail document/product | Fused clean/semantic finalization; changed atlas regions update the matching workspace |
| [Annotation](../src/controller/subsystems/annotation/native_annotation_algorithm.cpp) | Receiver-owned clean baseline and clean/semantic output from the input owner's immutable document description | Fused finalization after native raster work |
| [Predict](../src/controller/subsystems/system/compute_systems.cpp) | Uploaded prediction pixels and a rasterized box plane | Fused finalization in the prediction visual runtime |
| [Live](../src/controller/subsystems/live/native_live_algorithm.cpp) | Media composite output lease and a clean system output | The existing media receiver copy writes the system output; admitted same-device output uses final workspace storage directly |
| [Upscale](../src/controller/subsystems/upscale/upscale_system.cpp) | Receiver-owned clean/semantic input, transformed document, and cached derived products | Fused finalization of the selected retained result |

The runtime's physical output pool has three slots for Explore, four for
Upscale, and two each for Annotation, Predict, and Live. These pools also retain
raw products and inactive gallery/detail/derived results; they are not all
queued display frames. Current and overflow roles exchange on promotion without
a pixel copy. Unacquired overflow may be replaced while the current image is
externally held. Raw product readers and actual browser acquisitions separately
prevent reuse. If no slot is writable, display work defers.

Each admitted slot's workspace is reusable. A same-device clean-only output
aliases its final storage; clean/semantic products retain both raw planes
alongside final display storage. Allocation and logical product revision are
independent.
The native runtime factories linked above and
[SystemImageRuntime](../src/frameworks/gpu/system_image_runtime.h) define these
inventories.

`ImageWorkspace` owns native custody of the Vulkan allocation and immutable
layout. `VisualRuntimeOwner` services layout/admission requests on the producer's
existing worker. A raw product can finish before browser layout is available.
After Firefox allocation and CUDA import, the worker prepares that exact
retained product without another command, repeated inference/editing, or
another camera frame. First admission or growth can require a fill from retained
raw data; subsequent
production finalizes into the admitted storage. See [external workspace
interoperability](gpu-execution.md#shared-workspace-interoperability) for the
initial ownership and device requirements.

Raw `BorrowFrame` and `BorrowDocument` consumers keep their existing meaning.
Annotation, Upscale, and other processing receivers finish their required
copies before releasing borrowed inputs. Device mismatch uses the
producer-owned same-device/peer/pinned transfer facilities. Those processing
and device-boundary transfers are separate from display publication.

Presentation observes and borrows the selected final workspace through
`VisualSourceReader`. It owns admission, selection, publication, and native
timeline signaling, with no display image allocation or pixel-copy/composition
pass. Its short producer borrow protects readiness publication; a completed
offer grants no browser read permission.

[native_presentation_writer.cpp](../src/controller/presentation/native_presentation_writer.cpp)
awaits producer readiness on its own GPU execution path, signals an odd ready
value, publishes the exact frame identity, and raises the source's `eventfd`.
During draw preparation, Firefox acquires the newest completed offer authorized
by matching model facts through the physical allocation's nonblocking generation
gate. It never waits for unfinished production on the interactive draw path.
An acquisition miss retains the completed fallback and retries on a real
readiness edge.

Direct mode samples the exact source image in `GENERAL`. Copy mode transfers
it once into an available reusable sample slot. An acquired source stays
protected until its final Vulkan read/ownership-release submission signals
the matching even timeline value and native completion actually settles.
`Acquired`, `ReleaseSubmitted`, and `ReadSettled` are separate graphics receipts;
release submission alone grants no reuse. CUDA callbacks report status and wake
the Presentation worker. Its ordinary pump is nonblocking; initialization and
terminal retirement perform their required synchronization.

Reading a callback's ready flag does not prove that the callback has returned.
Counted completion retains the backing allocation until callback settlement.
Healthy outstanding workspace ownership can defer runtime retirement; a
wake-only notification lets the existing worker finish it. Unestablished
physical completion retains the affected custody and closes unsafe admission.
Terminal cleanup does not rely on a dead browser advancing its semaphore.

Source allocations and sample arenas have separate identities and lifetimes.
The native writer bounds live source records to 24 across producer slots and
retiring runtime generations, and admits at most three live arena generations
across active, candidate, and retiring state. Arena capacity retains its
high-water extent; growth prepares an unpublished candidate while preserving
the completed fallback.

Copy mode reuses a two-slot Primary-layer sample arena across source and
pool-slot rotation: a completed fallback and an incoming sample. Direct mode
keeps the arena's negotiation/control identity but allocates no sample-arena
pixel storage and performs no presentation copy. A copied source can retire
after its GPU read while copied pixels remain drawable. A direct fallback
continues to retain the source it samples. Page references, encoded draws,
physical source reads, and arena retirement settle independently.

Physical Vulkan image and semaphore owners retain device custody through
registry removal, IPC shutdown, and partial-construction failure. Native imports
retain backing FD and CUDA context custody through their own completed uses.
Browser exit is not a GPU completion receipt; terminal rescue never waits for an
unavailable peer to advance a timeline.

Live's requested rate configures capture and its system-owned consumption
cadence. Firefox's graphics queue, swapchain, compositor, and Wayland determine
display cadence. Reusing storage and the next eligible graphics submission
introduces no intentional frame delay, but makes no hard one-frame guarantee.

## Explore gallery and displayed geometry

[Explore residency](datasets.md#explore-thumbnails-and-atlas-residency) owns the
cache, priority, and physical atlas rules. The GUI includes every partially
visible row in demand. Its measured viewport may alternate between N and N+1
rows while card raster extent remains unchanged.

Explore retains independent completed gallery and detail handles in its shared
output pool. An unchanged return selects the real retained gallery product
revision under a newer domain observation. Changed requirements
reconcile cached content and resume necessary work. An unfinished gallery
can resume after detail without discarding ready neighbors or waiting on an
unrelated held disk lane.

The reflected `ExploreAtlasLayout` travels with readiness and the exact
product: `first_row`, `row_count`, `row_capacity`, `row_origin`, `columns`, and
`card_extent`. Logical visible rows and physical high-water dimensions are
different facts. The [gallery presentation module](../src/frontend/iced/src/presentation_surface/gallery.rs)
supplies metadata for the eligible pending sample or the retained displayed
sample. Cropping, labels, hit tests, and selection use that displayed snapshot,
including circular row placement, while a newer layout is pending.

A known placeholder cell already has a compiled-image identity. Hover and
selection remain available before its thumbnail is ready; readiness controls
pixels and labels. Blank padding in a partial final row has no selectable
image. Same-image detail revisions preserve pan/zoom and widget identity.

## Browser draw eligibility and retained fallback

The frontend reconciles native Presentation metadata with the owning domain's
source facts before requesting exact draw-time acquisition. The returned sample
retains those immutable facts. Iced samples either the direct source texture
or the copied arena slice; it allocates no additional full-image capture texture
and submits no capture pass. Copy and subsequent draw share queue ordering, so
an authorized submitted copy can be drawn before its completion callback arrives.

An authorized direct acquisition already refers to completed source pixels and
can become the retained fallback. Copy mode additionally requires physical copy
completion. An obsolete completion cannot promote a different current source,
release a newer sample, or replace its
metadata. A capacity retry may reuse a logical publication while advancing its
physical source transfer sequence.

`SampleRead` retains exact source/sample permission independently for displayed
fallback, encoded/submitted draws, and enabled probes. Vendored Iced's
`shader::Resources` attaches one reusable resource batch to the actual command
encoder. It releases draw holds after submission settlement or abandoned
encoding, including the screenshot route. Constructing a view or receiving a
diagnostic callback cannot release an encoded GPU read. A work-done callback
proves resource settlement, including terminal failure, rather than successful
rendering.

| Event | What it permits |
| --- | --- |
| Native input watermark | Reuse frontend sample storage and native admission credits |
| Native Presentation publication | Reconcile the exact producer product with matching domain and control state |
| Browser exact acquisition plus matching metadata | Draw the direct image, or queue a draw after the capability copy, with that image's geometry |
| Source read-release submission | Arm native observation of the matching even timeline value; no reuse yet |
| Actual even-value GPU settlement and `ReadSettled` | Settle the exact physical source read |
| Direct acquisition, or completed copy, with matching metadata | Promote that exact image as the retained completed fallback |
| Actual encoder/submission settlement | Release that draw's independent sample hold |
| Final display/draw/probe hold release | Release page custody of the exact sample; direct source reuse also requires GPU read settlement |

[presentation_surface.rs](../src/frontend/iced/src/presentation_surface.rs)
owns page sample custody, draw authorization, and completed images;
[its gallery module](../src/frontend/iced/src/presentation_surface/gallery.rs)
keeps atlas metadata attached to the image it describes.
[app/presentation.rs](../src/frontend/iced/src/app/presentation.rs) reconciles
transport state and viewer selection before view construction.

The last valid completed browser image remains drawable while native work,
capacity growth, or a newer copy is pending. Iced fit, pan, zoom, clipping,
sampling, and redraws reuse browser-owned images. Same-image revisions retain
viewer identity and transforms. New source identity resets them; a reconnect
also carries its own connection identity, even when revision numbers repeat.

Unacquired offers require no source-read settlement. An acquired image rejected
by the page still completes its GPU read and ownership release.
Hidden/idle pages and capacity pressure do not turn unused
offers into GPU reads. Page-local `ArenaTexture` retirement
explicitly destroys textures after views, bindings, probes, and exact readers
release them; normal retirement does not depend on JavaScript collection.

## Diagnostics and acceptance

Diagnostic identities do not govern input order, credits, draw authorization,
cache validity, or resource lifetime. Ordinary `./mmltk --gui` leaves diagnostic
collection and probes inactive. The [logging guide](logging.md) owns activation,
delivery modes, and artifact formats, including the quiet acceptance driver's
separate control and reporting responsibilities.

The [validation guide](validation.md#gui-behavior-and-evidence-ownership) maps
input, schema, rendering, physical-custody, and retained-session checks to their
existing executables. Native unit fixtures establish causal ordering and
failure behavior; packaged Wayland acceptance establishes actual browser
interaction and physical image evidence.
