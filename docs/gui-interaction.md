# GUI interaction and presentation

[Wiki index](README.md) · [Source guide](architecture.md) · [GPU execution](gpu-execution.md) · [Diagnostics](logging.md)

The [architectural contract](../CONTRACT.md) defines ownership. This page
explains how input, native state, GPU images, and Iced views cross those
boundaries. These paths are event-driven; they make no hard frame-rate or
input-to-display latency guarantee.

## Typed application boundary

The current browser protocol is **14**. Canonical C++ declarations own native
types, field identities, constraints, defaults, endpoints, events, and
snapshots. C++26 reflection generates typed Rust projections, compact
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

Protocol 14 is a complete package boundary: native host, Iced bundle, generated
bindings, and cross-language fixtures must agree. Follow
[binding generation and packaging](build.md#generated-bindings-and-dependency-maintenance);
generated Rust is build output.

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
| Native pending render state | Accumulates the latest reduced state while output storage is unavailable |

[AnnotationInput](../src/frontend/iced/src/annotation_input.rs) owns accepted
samples, in-flight batches, document-command barriers, and reusable encoding
storage. A batch does not cross a document epoch or a queued command's sample
frontier. Both native slots being occupied leaves further samples retained
in the frontend. It does not cancel the gesture or retire the peer. Extended
stalls can grow frontend memory; failed growth reports a connection/input
failure instead of silently committing a shortened stroke.

[AnnotationSystem](../src/controller/subsystems/annotation/annotation_system.h)
validates the peer epoch, document epoch, next batch sequence, sample bounds,
and available credits before admission. Its worker applies batches in order
and preserves every path-dependent brush segment. Each consumed batch frees
its slot and publishes a cumulative watermark **before GPU publication or a
GPU wait**. Input-progress records have reserved transport capacity and are
processed before ordinary frontend application events. A stale peer's credits
cannot release a replacement peer's samples.

## Document commands and settlement

Annotation Open, Edit, Save, and Stop share ordered command admission.
A command waits until every earlier accepted sample has been CPU-consumed.
Later samples wait behind that command's settlement barrier.

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
separates ordered reduction from publication. If no output allocation is
writable, it retains the completed clean baseline and accumulated state,
arms an availability notification, and continues draining input. A receiver
release triggers another publication attempt. There is no fixed batching
delay, frame timer, or FIFO of rendered intermediate gesture states.

Full Annotation UI state and compact frame progress have separate revisions.
Frame-only progress can advance pixels without rebuilding the full annotation
scene or discarding canvas interaction state. Changed UI facts still travel
through the full typed state. Document import and size validation complete
before committed state is replaced.

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

Producers own private image products. Presentation borrows the exact selected
product, performs a receiver-owned GPU copy and final composition, and exports
only its own backbuffer. The producer's borrowed storage stays valid through
copy completion. Clean pixels, semantic planes, and matching annotation facts
keep the same source identity and geometry.

[native_presentation_writer.cpp](../src/controller/presentation/native_presentation_writer.cpp)
submits CUDA completion callbacks for source-copy, ready, and release work.
Callbacks publish completion/status and signal an `eventfd`; the Presentation
worker processes those notifications and releases source custody. Its pump
does not synchronously wait for ordinary ready/release completion. Initial
allocation/import setup and terminal retirement still perform the required
synchronization.

Reading a callback's ready flag does not prove that the callback has returned.
Terminal retirement settles every submitted callback before destroying its
storage or wake descriptor. If completion cannot be established, the complete
writer and affected custody remain retained. A dead browser is not asked to
advance an external semaphore before this owner can be retained safely.

Capacity growth constructs and imports an unpublished replacement before
promotion. The previous allocation remains alive until its consumers release
it. Native backbuffer storage, private producer buffers, and browser-owned
images serve different ownership requirements; their count is not an
intentional extra-frame delay.

## Browser draw eligibility and retained fallback

The frontend reconciles native Presentation metadata with the owning domain's
source facts and the exact imported frame. A matching reconciliation submits
the receiver-owned WebGPU capture before Iced constructs labels and layout.
The capture and subsequent Iced draw use the same queue, so the submitted image
can draw once matching model metadata authorizes it.

Physical capture completion is separate. The queue-completion callback owns
the borrowed browser sample until the copy completes, even if the widget or
pending selection is replaced. Completion then permits the matching capture
to become the retained fallback. An obsolete completion cannot promote a
different current source, release a newer sample, or replace its metadata.

| Event | What it permits |
| --- | --- |
| Native input watermark | Reuse frontend sample storage and native admission credits |
| Native Presentation publication | Reconcile the exported frame with matching domain and control state |
| Browser capture submission plus matching metadata | Queue an Iced draw of that exact image and its labels/gallery/detail geometry |
| Browser capture completion plus matching metadata | Promote that image as the retained completed fallback |

[presentation_surface.rs](../src/frontend/iced/src/presentation_surface.rs)
owns browser import/capture custody, draw authorization, and completed images;
[its gallery module](../src/frontend/iced/src/presentation_surface/gallery.rs)
keeps atlas metadata attached to the image it describes.
[app/presentation.rs](../src/frontend/iced/src/app/presentation.rs) reconciles
transport state and viewer selection before view construction.

The last valid completed browser image remains drawable while native work,
capacity growth, or a newer capture is pending. Iced fit, pan, zoom, clipping,
sampling, and redraws reuse browser-owned images. Same-image revisions retain
viewer identity and transforms. New source identity resets them; a reconnect
also carries its own connection identity, even when revision numbers repeat.

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
