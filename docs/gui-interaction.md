# GUI interaction and presentation

[Wiki index](README.md) · [Source guide](architecture.md) · [GPU execution](gpu-execution.md) · [Diagnostics](logging.md)

The [architectural contract](../CONTRACT.md) defines ownership. This page
explains how input, native state, GPU images, and Iced views cross those
boundaries. These paths are event-driven; they make no hard frame-rate or
input-to-display latency guarantee.

## Typed application boundary

The current application package protocol is **17**. Canonical C++ declarations
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
agreement before installing native state. Compact workspace mouse and Explore
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
established by the connection. Mouse records preserve fractional image
coordinates, button values, modifiers, click counts, wheel units and deltas,
and source/peer/document ownership. Optional coordinates also allow input to
an empty workspace. CBOR floats use the smallest exact representation.

Protocol 17 is a complete package boundary: native host, Iced bundle, generated
bindings, and cross-language fixtures must agree. Follow
[binding generation and packaging](build.md#generated-bindings-and-dependency-maintenance);
generated Rust is build output. The separate native/Firefox graphics ABI
projects physical import records and frame signals; it does not carry
application intents or own UI behavior.

The WebSocket carries controls and logical UI facts. The independent FD graphics
channel carries completed images and immutable `WorkspaceImageMetadata`:
schema identity, frame dimensions/content region, and the corresponding native
product projection, including atlas layout and labels where applicable.
Firefox transports this payload opaquely. The
[metadata decoder](../src/frontend/iced/src/presentation_surface/metadata.rs)
validates its extent, schema, exact image identity, and product/source pairing
before installing it. Malformed or mismatched metadata rejects that candidate
while preserving safe read custody and the last valid image.

## Shared immediate workspace input

[workspace_input.rs](../src/frontend/iced/src/workspace_input.rs) captures
movement, every button press/release, enter/leave, wheel, and cancellation for
all workspace tabs. Click count accompanies a press, so one physical gesture
has one native effect. Iced converts coordinates through the local view
transform and retains pan, zoom, hover, and other component interaction state.
Native Annotation resolves editing targets; passive products consume the same
vocabulary without acquiring editing tools.

```text
Iced capture
    → ordered connection, retaining unsent records during transport pressure
    → generated compact interaction on the existing socket
    → the selected native owner's queue and domain effects
          └─ changed content → dirty renderer → completed graphics image
```

The connection preserves the order of mouse records and ordinary commands,
including records waiting for Bootstrap's peer epoch. It reuses encoding
storage and retains mouse records through temporary pressure. Admission does
not depend on frontend consumption credits, document-settlement barriers, or
display completion. Allocation or essential transport failure closes the peer;
it cannot silently shorten an accepted stroke.

## Ordered annotation input and retained storage

The shared native
[WorkspaceInputQueue](../src/controller/presentation/workspace_input.h)
starts with 64 entries, grows geometrically, retains high-water capacity, and
pops in constant time. Each domain owns its synchronization and execution.
[AnnotationSystem](../src/controller/subsystems/annotation/annotation_system.h)
places mouse input, document commands, and peer boundaries in one native
queue. Its independent input worker owns `AnnotationDocument`, the reducer,
and history. It validates peer ownership on admission, consumes stale-document
records as cancellation, and preserves every accepted path-dependent sample.
Peer replacement queues old-gesture cleanup before replacement input.

## Document commands and settlement

Annotation Open, Edit, and Save execute after earlier admitted mouse records
and commands on the input owner. Ordinary document editing and saving settle
independently of display publication. Source opening and color sampling use
asynchronous GPU continuations that suspend later dependent document effects.
Transport can continue admitting ordered records, independent systems keep
running, and Stop can request cancellation.

An admission reply with `busy=true` describes queued work. Completion produces
later native state; admission and completion can advance separate revisions,
and reply/event arrival order may differ. These are logical operation facts.
They neither validate a texture nor acknowledge a GPU read. Domain rejection,
wire failure, resource failure, and cancellation retain their typed outcomes.

## Reduction, publication, and source changes

[annotation_system.cpp](../src/controller/subsystems/annotation/annotation_system.cpp)
separates ordered reduction from a dedicated renderer. The input owner captures
an immutable description with shared scene storage, editor facts, and preview
geometry. Three reusable descriptions cover input scratch, newest pending work,
and active GPU work. The renderer retains its active description through
settlement and may replace the newest unsubmitted description. If output storage
is occupied, it retains the clean baseline and pending work and arms a real
availability notification while input progresses.

Logical UI facts describe the latest committed document; rendered scene and
frame facts describe the exact completed pixels, even when newer input has
overtaken them. Full Annotation UI state and compact frame progress have
separate revisions. Preview-only publications do not resend the document.
Bootstrap restores logical UI state; graphics binding and image custody
progress independently. Document import and size validation complete before
committed state is replaced.

A press resolves its target against the current native document and logical
tool, preserving selected-handle precedence and image-coordinate tolerances.
The accepted gesture retains that target through subsequent samples, with
runtime object/element identities preserved by history and Undo/Redo. These
identities are not part of the saved named format. A positive fractional Box
preview whose bounds truncate to empty integer raster coverage skips just the
outline; the gesture, mask content, selection, and damage processing remain
valid.

All five visual systems use the shared
[VisualRuntimeOwner](../src/controller/presentation/detail/visual_runtime_owner.h).
Content/input changes, workspace admission, and actual GPU/storage completion
wake dirty work. Clean and semantic planes, allocation-local damage, geometry,
and staging retain reusable capacity. An idle native renderer has no repeating
render timer. Redrawing a completed browser image does not rerun inference,
upscaling, decoding, capture, or document edits.

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

Each visual producer owns its raw processing products and display preparation:

| Producer | Raw product and retained work | Display preparation |
| --- | --- | --- |
| [Explore](../src/controller/subsystems/explore/native_explore_algorithm.cpp) | Individual thumbnail cache, gallery clean/semantic atlas, and separate detail document/product | Fused clean/semantic finalization; changed atlas regions update the matching workspace |
| [Annotation](../src/controller/subsystems/annotation/native_annotation_algorithm.cpp) | Receiver-owned clean baseline and clean/semantic output from the input owner's immutable document description | Fused finalization after native raster work |
| [Predict](../src/controller/subsystems/system/compute_systems.cpp) | Uploaded prediction pixels and a rasterized box plane | Fused finalization in the prediction visual runtime |
| [Live](../src/controller/subsystems/live/native_live_algorithm.cpp) | Media composite output lease and a clean system output | The existing media receiver copy writes the system output; admitted same-device output uses final workspace storage directly |
| [Upscale](../src/controller/subsystems/upscale/upscale_system.cpp) | Receiver-owned clean/semantic input, transformed document, and cached derived products | Fused finalization of the selected retained result |

The retained product pool has three slots for Explore, four for Upscale, and
two each for Annotation, Predict, and Live. These raw pools include inactive
gallery/detail/derived results. The foreground graphics binding separately owns
two persistent display buffers. Selection reuses that pair; growth or device
replacement may temporarily retain old buffers until their readers settle.

Raw product readers and actual browser acquisitions protect their own storage.
A same-device clean-only output may alias available final storage when both
lifetimes permit it. Otherwise raw work continues in independent retained
storage while display buffers are occupied. Clean/semantic products retain both
raw planes and use fused finalization. Completed products and front/back display
roles can be selected without copying pixels. Allocation and logical product
revision remain independent.

The native runtime factories linked above and
[SystemImageRuntime](../src/frameworks/gpu/system_image_runtime.h) define these
inventories.

`ImageWorkspace` owns native custody of the Vulkan allocation and immutable
layout. `VisualRuntimeOwner` services layout/admission requests on the producer's
existing worker. A raw product can finish before browser layout is available.
After Firefox allocation and CUDA import, the worker prepares that exact
retained product without another command, repeated inference/editing, or
another camera frame. First admission or growth can require a fill from retained
raw data; subsequent production finalizes into the admitted storage.
See [external workspace interoperability](gpu-execution.md#shared-workspace-interoperability)
for the initial ownership and device requirements.

Raw `BorrowFrame` and `BorrowDocument` consumers keep their existing meaning.
Annotation, Upscale, and other processing receivers finish their required
copies before releasing borrowed inputs. Device mismatch uses the
producer-owned same-device/peer/pinned transfer facilities. Those processing
and device-boundary transfers are separate from display publication.

Presentation observes the selected product through `VisualSourceReader`,
captures that product's metadata, and coordinates admission/finalization on
the producer execution owner. Firefox owns the display allocation; the native
writer owns its imported custody, publication, and timeline signaling. There
is no additional Presentation pixel-copy/composition pass. A short producer
borrow protects readiness publication; a completed offer itself grants no
browser read permission.

[native_presentation_writer.cpp](../src/controller/presentation/native_presentation_writer.cpp)
awaits producer readiness on its own GPU execution path, signals an odd ready
value, publishes the exact frame identity with paired metadata, and raises the
source's `eventfd`. Publication completes the metadata before exposing the slot.
During draw preparation, Firefox's stable graphics binding acquires the latest
completed offer through the physical allocation's nonblocking generation gate.
It does not require a matching application snapshot or wait for unfinished
production. An acquisition miss retains the completed fallback and retries on
a real readiness edge.

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
The native writer admits at most two source allocations per arena and six
source records in total, with at most three live arena generations across
active, candidate, and retiring state. Arena capacity retains its high-water
extent; growth prepares an unpublished candidate while preserving the completed
fallback.

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
uses metadata paired with the eligible pending sample or retained displayed
sample. Cropping, label placement, hit tests, and selection use that image,
including circular row placement, while a newer layout is pending.

A known placeholder cell already has a compiled-image identity. Hover and
selection remain available before its thumbnail is ready; readiness controls
pixels and labels. Blank padding in a partial final row has no selectable
image. Same-image detail revisions preserve pan/zoom and widget identity.

Gallery and Detail Labels use the current local checkbox state over the labels
and geometry paired with the displayed image. A label-only toggle needs no new
GPU image. Boxes and Masks update native semantic planes through dirty work
and completion notifications, including while the viewport stays still.
During an augmentation refresh, retained clean pixels and their meaning remain
paired until each replacement completes; see the
[thumbnail cache rules](datasets.md#explore-thumbnails-and-atlas-residency).

Select, Next, Previous, and Close cancel the departed detail viewer's derived
request while retaining the last displayed GPU frame for replacement.
Explicit route departure or component teardown retires its display custody.
Transforms use component identity and paired image geometry. An accepted
Original content preference stays with the same image through route changes
and reconnect; a different image starts from its own native preference.
Opening Annotation captures the exact displayed source and applied crop at
dispatch, so a later preference or image change cannot alter that import.

## Browser draw eligibility and retained fallback

The graphics binding chooses a completed image locally and returns its immutable
metadata with read custody. The frontend constructs labels and layout from that
pair. Application snapshots independently update logical controls; they neither
grant graphics permission nor select the image for a draw.
Iced samples the direct source texture or copied arena slice without an
additional full-image capture texture or capture pass. Copy and subsequent draw
share queue ordering, so a submitted copy can be drawn before its completion
callback arrives.

An authorized direct acquisition already refers to completed source pixels and
can become the retained fallback. Copy mode additionally requires physical copy
completion. An obsolete completion cannot promote a different current source,
release a newer sample, or replace its metadata. A capacity retry may reuse a
logical publication while advancing its physical source transfer sequence.

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
| Native command admission/completion | Update logical operation state independently of graphics |
| Native image publication | Expose a completed image and its paired metadata through graphics |
| Browser acquisition and validated paired metadata | Draw the direct image, or queue a draw after the capability copy, with that image's geometry |
| Source read-release submission | Arm native observation of the matching even timeline value; no reuse yet |
| Actual even-value GPU settlement and `ReadSettled` | Settle the exact physical source read |
| Direct acquisition, or completed copy, with matching metadata | Promote that exact image as the retained completed fallback |
| Actual encoder/submission settlement | Release that draw's independent sample hold |
| Final display/draw/probe hold release | Release page custody of the exact sample; direct source reuse also requires GPU read settlement |

[presentation_surface.rs](../src/frontend/iced/src/presentation_surface.rs)
owns page sample custody, completed images, and draw preparation;
[its gallery module](../src/frontend/iced/src/presentation_surface/gallery.rs)
keeps atlas metadata attached to the image it describes.
[app/presentation.rs](../src/frontend/iced/src/app/presentation.rs) owns
foreground routing, viewer transitions, and graphics notifications.

The last valid completed browser image remains drawable while native work,
capacity growth, or a newer copy is pending. Iced fit, pan, zoom, clipping,
sampling, and redraws reuse browser-owned images. Same-image revisions retain
viewer identity and transforms. New source identity resets them; a reconnect
also carries its own connection identity, even when revision numbers repeat.

Unacquired offers require no source-read settlement. An acquired image rejected
by the page still completes its GPU read and ownership release.
Hidden/idle pages and capacity pressure do not turn unused offers into GPU reads.
Page-local `ArenaTexture` retirement
explicitly destroys textures after views, bindings, probes, and exact readers
release them; normal retirement does not depend on JavaScript collection.

## Browser redraws and FPS

The visible browser requests redraws from its graphics/window loop and can
submit the last completed image continuously while native content is unchanged
or newer work is pending. Native dirty rendering has its own event-driven
progress.

Show FPS enables one component-owned counter of actual browser queue
submissions containing workspace draws, including retained pixels. Several
workspace primitives using the same meter in one submission count once.
Clipped, abandoned, or unsubmitted work counts zero. Encoding, swapchain
presentation, and work-done callbacks are separate events.

[workspace_fps.rs](../src/frontend/iced/src/workspace_fps.rs) caches the text and
paragraph, refreshing them every 500 ms. The counter sits six pixels inside the
workspace's upper-right corner. Disabling it removes the meter, clock, counter,
and submission observer. This product setting is independent of diagnostics;
opt-in acceptance uses an asynchronous canvas capture to check its placement
and rendered text.

## Diagnostics and acceptance

Diagnostic identities do not govern input order, draw authorization,
cache validity, or resource lifetime. Ordinary `./mmltk --gui` leaves diagnostic
collection and probes inactive. The [logging guide](logging.md) owns activation,
delivery modes, and artifact formats, including the quiet acceptance driver's
separate control and reporting responsibilities.

The [validation guide](validation.md#gui-behavior-and-evidence-ownership) maps
input, schema, rendering, physical-custody, and retained-session checks to their
existing executables. Native unit fixtures establish causal ordering and
failure behavior; packaged Wayland acceptance establishes actual browser
interaction and physical image evidence.
