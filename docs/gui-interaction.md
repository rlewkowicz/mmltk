# GUI interaction and presentation

[Wiki index](README.md) · [Source guide](architecture.md) · [GPU execution](gpu-execution.md) · [Diagnostics](logging.md)

The [architectural contract](../CONTRACT.md) defines ownership. This page
explains how input, native state, GPU images, and Iced views cross those
boundaries. These paths are event-driven; they make no hard frame-rate or
input-to-display latency guarantee.

## Workflow layout and navigation

The header order is **Train, Validate, Predict, Export, Live, Annotate,
Explore**. [navigation.rs](../src/frontend/iced/src/view/navigation.rs) owns
that visual order; ordinary workflow traversal derives from it, excluding
Explore.

The [page canvas](../src/frontend/iced/src/view/mod.rs) is at least 1020 logical
pixels wide and centers a page of at most 1500 logical pixels. A narrower
window exposes horizontal scrolling, with space reserved for its scrollbar so
the rail cannot cover bottom-row controls. Ordinary workflows use one vertical page
scroll beneath the header; Explore retains its gallery and sidebar scroll
owners.

The shared [workflow compositor](../src/frontend/iced/src/view/workflow/mod.rs)
assigns 19% of page width to setup, 62% to the workspace with Advanced below,
and 19% to tools/status. Annotate uses those same columns:

| Left: setup | Center: workspace and Advanced | Right: tools and status |
| --- | --- | --- |
| Source, output destination, Open, Save annotations | Viewer controls and image; Timeline in Advanced underneath | Tools, objects, classes, editing controls, operation status, Stop |

[Annotate](../src/frontend/iced/src/view/annotation.rs) supplies these regions
to the compositor. Its [sidebar](../src/frontend/iced/src/view/annotation/sidebar.rs)
keeps tools, object/class lists, and long labels within the right column.
Those lists grow with the page's vertical content; Annotate has no separate
compact layout or nested inspector, object, or class scroll area. Viewer and
editor widget identities survive ordinary layout and native-state updates.

## Training, validation, and prediction

Train uses the [retained dashboard](#training-dashboard) in the center column,
with the shared workspace aspect selector and a separate live progress card.
It has no native GPU image workspace. Its
[Dataset card](#dataset-compilation-controls) keeps the optional test split
independent of inferred train/validation paths; manual split text fields appear
when inference is disabled. Its weights card owns Transfer/Resume.
Its Output card selects Auto Output or Browse Output, shows the selected/active
path, and loads supported saved charts. Advanced includes
**Exponential moving average**, disabled by default, and separate
augmentation/perceptual-downscaling settings. The
[workflow reference](rfdetr-workflows.md) owns the metric conventions, current
file formats, input admission, live progress, and continuation requirements.

Train and Validate share the weights-selector presentation and native artifact
compatibility facts. Validate's Open Dataset shows its explicit path or the
effective inherited Train validation split. It uses the
[fixed Validation workspace](#validation-workspace-and-shared-viewer) below.

Predict offers compiled-dataset, single-image, and local-video inputs, a preview
threshold, optional output JSON, and video Pause/Resume/Stop. It retains the
workspace aspect selector. Every GUI prediction request uses batch size 1;
there is no batch-size field on this page. Train, Validate, and Predict hide
H2D/NUMA controls while preserving the generated settings and backend support.
Explore retains its loading controls.

The primary action retains one preparation request across settings settlement,
model selection/preparation, and native start. Changing its inputs or leaving
the workflow cancels an unstarted request. Repeated clicks do not create
duplicate starts. Progress shows the preparation stage and then the owning
operation's completed work; unknown totals stay indeterminate. Video controls
use the same pending-system admission as their typed requests, including an
event arriving before its reply.

### Dataset compilation controls

The [Dataset component](../src/frontend/iced/src/view/train/dataset.rs) offers
Stretch and Letterbox radios and a separate Perceptual downscaling checkbox.
Native [resize defaults and geometry](datasets.md#resize-geometry) drive those
choices. **Compile Benchmark Dataset Override** switches the explicit compile
action between a source directory and a built-in recipe.

With the override on, **Coco custom** and **Coconut** radios appear. Coco custom
is the native default. Selecting Coconut additionally reveals **Coconut
validation**, **Stock validation**, and **Coconut stock**; Coconut validation
is the native initial choice. The
[recipe membership table](benchmark-datasets.md#recipes-and-validation-membership)
defines exactly which images and labels each choice compiles.

Both selections persist in settings. Returning to Coco custom or disabling the
override hides the dependent radios without clearing their values. The hidden
validation choice has no effect on Coco custom, and Directory compilation ignores
both benchmark choices. Override mode disables source-directory text and Browse;
turning it off restores those controls. Selecting a radio never starts work.

**Compile Benchmark Dataset** or **Compile Dataset** requires settled settings
and native admission. The accepted native request captures those settings for
the entire operation. While compilation is active, the two recipe and three
validation radios are disabled; the established Dataset controls keep their
existing enablement rules. The action becomes **Cancel compilation**, using
the Dataset system's Stop operation. Progress uses native
[acquisition and output facts](benchmark-datasets.md#reading-compilation-progress),
and completion, cancellation, or failure comes from the native terminal result.

## Shared primary actions

The [primary-action widget](../src/frontend/iced/src/view/workflow/primary_action.rs)
owns the common presentation:

| Workflow | Green idle action | Red active action |
| --- | --- | --- |
| Train | Start Training | Stop Training |
| Validate | Start Validation | Stop Validation |
| Predict | Run Predict | Stop Predict |
| Export | Run Export | Stop Export |
| Live | Start Live | Stop Live |
| Annotate | Save Annotations | Remains green and save-only |

Native activity and accepted explicit preparation/save state determine the
active presentation. Stop keeps each owner's cancellation behavior. An accepted
save disables duplicate saves until settlement without changing its label.
Train, Validate, Predict, and Export share the card-to-action gap; Train,
Validate, and Predict have no separate start-status sentence. Native progress
and failures retain their owning displays, while a successful Validate result
does not leave “Succeeded” immediately above the action.

During active work, ten equal blue segments move clockwise by perimeter distance
through the existing white, three-logical-pixel rounded border. They use the
theme color of Browse dataset source without changing bounds, radius, or the
button core. The [border renderer](../src/frontend/iced/src/view/workflow/primary_action/border.rs)
retains one small uniform binding per widget and a shared pipeline per Iced
device. It draws narrow strips and corner regions; it builds no per-frame mesh.
Only an active, visible widget requests continuing redraws. Animation is local
presentation state and adds no native protocol field or polling loop.

## Validation workspace and shared viewer

Validate preserves the outer setup/status columns. Its center image/metrics
region is always 16:9, independent of the aspect selected on other pages.
**Metrics** and **Validation Preview** share a centered heading strip of the
same height as the other pages' aspect selector. Equal 8:9 halves contain the
[twelve COCO summaries](rfdetr-workflows.md#evaluation-metrics-and-retained-samples)
and a non-scrolling native atlas of two columns by three rows. Each cell is
4:3. Smaller sample sets retain explicit empty cells.

The atlas reuses Explore's native image containment and padding plus the shared
surface border, spacing, placeholder, and hit geometry. Source images retain
their aspect rather than stretching into cells. Shared
[image containment](../src/backend/imaging/raster/image_containment.h) has no
Explore scheduler or dataset dependency. Validation keeps its own bounded
sample products and composition owner.

Explore and Validation call the complete
[image-viewer panel](../src/frontend/iced/src/view/image_viewer.rs): Sample
heading, Previous/Next, Fit, wheel zoom, right-drag pan, source/overlay controls,
Close, Basic/Fast/Neural Upscale, and Open in Annotation. Validation navigation
stays within its retained available samples. Its opaque modal covers both
headings and the center workspace, constrains the image there, and leaves the
sidebars outside the overlay.

Sample selection uses metadata paired with the displayed atlas. A held pointer
admits a target once until that target changes or the hold ends; a publication
revision alone does not create another selection. Navigation and overlay
requests use the common connection/settings/system admission. Upscale follows
the current selected sample; Annotation captures the exact displayed sample and
view at dispatch. Annotation receives the sample's ground truth, including
masks; detections remain a Validation viewer overlay.

Validation's atlas and detail panel have **GT labels** and **Det labels**
checkboxes. Each controls its entire ground-truth or detection layer—text,
boxes, and masks—while retaining that layer's finer Labels/Masks/Boxes choices.
Fine label toggles stay local to Iced; layer and box/mask changes request native
composition from retained products.

For one class, GT RGB is `255 - Det RGB` per channel, preserving the established
alpha. Overlapping native masks and box outlines use Direct Addition / Linear
Dodge: sum RGB with saturation at 255, then apply the existing alpha. Matching
complements therefore have white overlap RGB before alpha application. Text and
caption backgrounds use ordinary painter order, with GT drawn first and Det
drawn second. Separate Iced layers keep Det backgrounds above GT glyphs as well
as GT backgrounds. The existing font, glyph layout, contrast, and alpha policy
remain. These layer controls and color/composition rules belong to Validation;
Explore keeps its existing palette and single caption layer.

## Original view and annotation import

Explore's **Original content** and Validation's **Original** controls restore
the source aspect ratio using the compiled pixels. Stretch uses the full
canvas with inverse anisotropic display scaling. Letterbox crops the stored
content rectangle before restoring source aspect. Turning Original off shows
the actual model canvas, including padding. No source image file is opened and
no discarded source-resolution detail is recovered.

The exact displayed `VisualFrame` carries canvas extent, content rectangle,
and source extent through the graphics metadata. The Iced
[surface geometry](../src/frontend/iced/src/presentation_surface/geometry.rs)
keeps pixel sampling and displayed aspect distinct while applying one coherent
mapping to boxes, masks, labels, fit, pan, zoom, clipping, and inverse input
coordinates. This view choice reuses completed pixels; toggling Original does
not launch another upscale or alter its processing identity.

Basic, Fast, and Neural Upscale use the currently selected native detail source
and its document facts. A previous image can remain visible while navigation
settles, but it does not become the source of a new explicit Upscale request.
The derived product carries the scaled canvas/content geometry and unchanged
source extent. Checked geometry scaling and the native output-scale constant
are [generated from native declarations](architecture.md#nativerust-boundary).
The same Original preference then applies to the derived image.

**Open in Annotation** instead captures the exact displayed image and its
applied Original choice at dispatch, including a displayed derived result.
The receiving owner completes its pixel copy before releasing the borrow and
materializes continuous annotation coordinates through the same crop and aspect
transform. The materialized aspect fits within the available compiled or
derived content extent; it does not allocate the original source resolution.
Later navigation or a preference change cannot alter that accepted import.

Masks retain their own support bounds independently of boxes. Import can
therefore preserve mask pixels beyond a detection box, and a present empty
mask stays present with no runs. The native
[document materializer](../src/controller/presentation/visual_document.cpp)
owns this conversion and validates it before replacing the editable document.

## Training dashboard

The dashboard fits the center column's available width and page-body height
using the selected workspace aspect ratio. The default is 16:9; the shared
selector also offers 9:16, 4:3, 3:2, 1:1, and 16:10. Charts fit this bounded
region without an internal scrollbar. Clicking a chart title expands that
chart in the same region; **Back to charts** restores the grid. Setup, status,
and the [live progress card](rfdetr-workflows.md#live-training-progress) remain
part of the page.

**Charts** opens the multi-select list:

| Initially selected | Additional charts |
| --- | --- |
| Training loss | Loss components |
| AP50 | AP75 |
| AP50:95 | Class / cardinality errors |
| Average recall | Mask metrics |
| Precision / recall / F1 | Area breakdowns |
| Learning rates | |

**Reset to main charts** restores the six initial selections and closes an
expanded chart. Selecting no charts leaves an explicit empty-selection state.
Charts with no measurements show a waiting/empty state instead of fabricated zero values.
Average-recall labels use the recorded detection caps. Mask and area values
remain unavailable when the selected evaluation did not produce them.

**Epoch axis** is on initially and uses fractional epoch positions for live
training; turning it off selects the global optimizer-step axis.
**Log loss scale** changes only training-loss and loss-component charts. There
is no validation loss curve; images/second belongs to the live progress card.
A loss-scale change preserves non-loss cameras, legends, and prepared geometry,
including hidden charts and explicitly selected saved history. Reapplying the
current loss-scale value does not invalidate charts. Each chart's preparation
key records its effective scale, so later non-loss updates cannot apply the
global loss setting to that chart. The
[workflow guide](rfdetr-workflows.md#saved-history-and-plots) explains sparse
validation observations, loss conventions, and saved-history selection.

Each chart retains its plot, series, camera, and legend interaction through
expansion, hiding/revealing, and navigation. A remount with unchanged data and
axis configuration preserves the settled view; actual data, limit, or scale
changes still follow the plot's autoscale policy. Wheel and trackpad scrolling,
including modified wheel input over the plot, axes, or legend, belongs to the
ordinary page scroller and does not zoom or pan the chart. Other plot gestures
remain available.

The [metrics component](../src/frontend/iced/src/view/metrics.rs) owns separate
live and saved histories. Each curve retains at most 128 summary buckets with
endpoints and extrema. Sequence gaps, unavailable values, missing records, and
new attempts break lines, while isolated points remain visible as markers.
When older disconnected summaries must be retired, the Output card reports
chart omissions; retired extrema do not affect the retained plot. Missing
observations keep a pending gap even when live display samples are coalesced.
Saved history is unchanged. Hidden views continue ingesting records without
rebuilding geometry; only visible changed charts prepare their retained
summaries. Plot objects, series, and GPU buffers retain useful capacity.

The vendored [plot widget](../third_party/iced_plot/src/plot_widget.rs) keeps
settled view state independently of the temporary Iced widget tree. Axis labels
sit outside the shader region, with a rotated Y title and theme-aware text.
Cursor captions reconcile after each event's final camera and bounds changes.
Their immutable payload is shared by pending publication, messages, and settled
state; unchanged views reuse it, and leaving an active caption clears it once.
The [plot shader](../third_party/iced_plot/src/plot_renderer/shader.rs) draws
content, selection/highlights, and crosshairs in painter order within one
chart-sized MSAA pass and resolve, followed by the existing clipped composite
pass.
These ownership and bounded-work properties are not measured overhead or
throughput guarantees. Vendored
[plot picking](../third_party/iced_plot/src/picking.rs) bounds pending GPU
readbacks and settles cancellation without reusing an unsettled mapping.

## Numeric editing

Application integer inputs omit increment/decrement buttons and ignore wheel
mutation. Keyboard editing, selection, paste, and bounded arrow-key adjustment
remain available. The shared
[numeric fields](../src/frontend/iced/src/view/workflow/fields.rs) retain their
generated signed or unsigned type, field identity, and native constraints;
integer values do not round through floating-point text conversion.

Explore's [dataset controls](../src/frontend/iced/src/view/explore/dataset.rs)
show Minimum instances and Maximum instances as labeled, full-width integer
inputs. Both retain the native 0–10,000 range. Raising the minimum past the
current maximum stops at that maximum; lowering the maximum past the current
minimum stops at that minimum. Shuffle seed and compiled-index inputs retain
their exact `u64` values and existing range/unlimited policy.

These filter inputs remain mounted with stable native field identities when
mutation is temporarily unavailable. Disabling their input callback preserves
focus through pending filter admission and settlement. Settings and filters
still use their owning typed mutation paths.

The vendored [NumberInput](../third_party/iced_aw/src/widget/number_input.rs)
retains its text-input and modifier child trees through diff and layout.
Layout and widget operations share the modifier construction and reconcile
existing child state, including row/column changes driven by padding. This
preserves focus, selection, and editing state under the application policy above.

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
System/endpoint routing, snapshot/event/reply variants, and visual observations
also derive from the native schema; the [source guide](architecture.md#nativerust-boundary)
locates their emitters and the handwritten presentation-state reducers.

Application output objects in snapshots, replies, and events use positional
CBOR arrays in canonical reflected member order. Each declared field has a
slot, including null optionals, and generated decoders enforce the exact field
count and constraints. Generated failures name the enclosing type/member;
array decoding adds the failing index. Bootstrap and event errors retain their
system/event context, with fixed-text failures reporting the actual size and
allowed range. Schema agreement authorizes those positions; they are
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
schema identity, frame dimensions/content region/source extent, and the
corresponding native product projection, including atlas layout and labels where
applicable.
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

Undo/Redo uses a directional view of the retained journal entry, then moves that
entry to the opposite journal after admission and live-payload preparation.
It preserves the bounded history, stored editor facts, object/element identities,
and complete mutation payloads without constructing a reversed entry copy.

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

The [document owner](../src/controller/subsystems/annotation/annotation_reducer.cpp)
retains committed scene/identity content by document revision in four reusable
immutable backings. Editor facts and gesture previews are captured separately,
so a selection or tool change can reuse scene content while readers hold older
descriptions. A capture after a document edit selects an available backing;
source replacement invalidates retained content even when revision numbers coincide.

The [native renderer](../src/controller/subsystems/annotation/native_annotation_algorithm.cpp)
keeps packed mask/geometry upload content separately from each output allocation's
semantic damage. Reuse follows actual content, packed offsets, dimensions, and
backing capacity. Changed words upload as one enclosing range; removed or disabled
objects invalidate their packed membership. Upload validity is restored only
after the existing upload event settles, and pinned staging stays protected
until then. Growth or failure requires refilling affected storage.
Raster work intersects each object's conservative bounds with output damage,
including mask support beyond boxes and selection handles, while preserving
painter order. Shared flat-run rasterization enumerates only pixels admitted
by the run and clip.

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

All six visual producers use the shared
[VisualRuntimeOwner](../src/controller/presentation/visual_runtime_owner.h).
Content/input changes, workspace admission, and actual GPU/storage completion
wake dirty work. Clean and semantic planes, allocation-local damage, geometry,
and staging retain reusable capacity. An idle native renderer has no repeating
render timer. Redrawing a completed browser image does not rerun inference,
upscaling, decoding, capture, or document edits.

Producer events notify native Presentation directly through
[ApplicationEventPublisher](../src/controller/browser/application_event_publisher.h)
and [the shell wiring](../src/controller/shell/application_system_storage.cpp).
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
| [Predict](../src/controller/subsystems/system/predict_system.cpp) | Latest prediction pixels with retained boxes, masks, class meaning, and receiver-owned preview storage | Fused finalization in the prediction visual runtime |
| [Validate](../src/controller/subsystems/validate/detail/validation_samples.cpp) | Up to six selected sample products with prediction/ground-truth meaning and retained atlas/detail selection | Fused composition into reusable atlas/detail output |
| [Live](../src/controller/subsystems/live/native_live_algorithm.cpp) | Media composite output lease and a clean system output | The existing media receiver copy writes the system output; admitted same-device output uses final workspace storage directly |
| [Upscale](../src/controller/subsystems/upscale/upscale_system.cpp) | Receiver-owned clean/semantic input, transformed document, and cached derived products | Fused finalization of the selected retained result |

The retained product pool has three slots for Explore, four for Upscale, and
two each for Annotation, Predict, Validate composition, and Live. Validation's
selected raw samples have separate custody from its two composed outputs.
These product pools include inactive
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

Live capture and fresh/warm Upscale writers replace every active output pixel
through `PublishRetained`, reusing storage without preparing an overwritten
baseline. Semantic-only Upscale publications use ordinary `Publish` with clean
plane preservation. Other partial producers retain their existing initialization
and preservation requirements. Provider warm execution is described in
[GPU execution](gpu-execution.md#upscale-warm-execution).

Pending workspace finalization retains its completion-draining product owner.
Even if a cross-device transfer has released its raw input read, that product
cannot detach, replace its workspace, or become writable before final display
work settles. Raw-reader release and completed finalization are separate facts.

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

The retained gallery sensor continues measuring beneath Detail. The component
records the latest size, native capacity, column count, scroll row, and row
fraction independently of the current Gallery/Detail mode. Measurements alone
do not submit gallery viewport changes while Detail is open. Returning to
Gallery reconciles the retained measurement through the ordinary native-state
event path, including repeated landscape-to-portrait and portrait-to-landscape
changes, without requiring another resize or scroll.

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

When the displayed gallery's paired metadata has zero matching samples, an
opaque workspace-background overlay shows “No samples match the filters” and
covers the empty atlas tiles. The underlying native empty-gallery publication,
paired metadata, encoded draw, and resource custody remain intact.

Gallery and Detail Labels use the current local checkbox state over the labels
and geometry paired with the displayed image. A label-only toggle needs no new
GPU image. Boxes and Masks update native semantic planes through dirty work
and completion notifications, including while the viewport stays still.
During an augmentation refresh, retained clean pixels and their meaning remain
paired until each replacement completes; see the
[thumbnail cache rules](datasets.md#explore-thumbnails-and-atlas-residency).

The [paired content owner](../src/frontend/iced/src/presentation_surface/metadata.rs)
prepares caption data when validated image metadata is installed, then shares
it through pending and retained display content. Detail reads its original or
derived scene from that same immutable metadata owner. The
[category-caption index](../src/frontend/iced/src/presentation_surface/labels.rs)
provides constant-time lookup by catalog reference while constructing text and
paragraphs only for referenced categories. Label visibility and class filtering
apply during drawing and reuse this prepared content.

Select, Next, Previous, and Close cancel the departed detail viewer's derived
request while retaining the last displayed GPU frame for replacement.
Explicit route departure or component teardown retires its display custody.
Transforms use component identity and paired image geometry. An accepted
Original content preference stays with the same image through route changes
and reconnect; a different image starts from its own native preference.
The [Original and import rules](#original-view-and-annotation-import) also apply
when a retained fallback is visible during navigation or derived work.

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
