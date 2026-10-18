# GUI interaction and presentation

[Wiki index](README.md) · [Source guide](architecture.md) · [GPU execution](gpu-execution.md) · [Diagnostics](logging.md)

[CONTRACT.md](../CONTRACT.md) defines ownership. This page describes current
input, native-state, image and Iced behavior. Event-driven execution provides
no hard frame-rate or input-to-display latency guarantee.

## Workflow layout and navigation

Header order: **Train, Validate, Predict, Export, Live, Annotate, Explore**.
[Navigation](../src/frontend/iced/src/view/navigation.rs) owns visual order and
ordinary workflow traversal (excluding Explore). **Status** centers after Explore;
the connection label precedes bordered **Settings** at the right. These remain
outside the tab scroller. Narrow tabs retain button height; at minimum widths
connection text yields to Status/Settings. The Status popup follows measured
trigger bounds.

Destination pages select foreground sources. Validate→Explore restores Explore
even when Validation later updates. Upscale requests survive only for their
owning destination viewer. Dataset readiness does not imply drawable readiness.

The [page canvas](../src/frontend/iced/src/view/mod.rs) is at least 1020 logical
pixels wide, centers content up to 1500, and scrolls horizontally below that
minimum with reserved scrollbar space. Ordinary pages have one vertical scroller;
Explore owns its gallery/sidebar scrollers. The [workflow compositor](../src/frontend/iced/src/view/workflow/mod.rs)
uses 19% setup, 62% workspace/Advanced, 19% tools/status. Annotate shares it:

| Setup | Center | Tools/status |
| --- | --- | --- |
| Source, output, Open, Save annotations | Viewer; Advanced Timeline below | Tools, objects/classes, edits, operation progress, Stop |

Annotation lists/long labels remain in the right column and grow with page
content, without nested object/class inspectors or a compact alternative.
Viewer/editor identities survive ordinary layout and native updates.

## Training, validation, and prediction

Train/Validate/Predict/Export place **Output** then **GPU** at right, setup/primary
action at left. [Workflow reference](rfdetr-workflows.md) owns weights, inputs,
reservations, metrics, artifacts and continuation. Shared weights controls provide
direct custom-file selection, preparation and cancellation, without manual
backend-kind/Class layout.

| Page | Controls/workspace |
| --- | --- |
| Train | Aspect-selected dashboard and separate live progress; Dataset inference/manual split fields with independent optional test; From scratch and Transfer/Resume; saved Output history; Advanced EMA, augmentation/perceptual, lanes/recipes |
| Validate | Open Dataset override or inherited validation split; fixed metrics/preview workspace |
| Predict | Compiled/image/video sources; image-only RGB resize; threshold/aspect; video Pause/Resume/Stop; batch one, no batch field; source-specific saving disclosures retain choices |
| Export | Independent ONNX/TensorRT checkboxes; at least one required |

Train/Validate/Predict hide H2D/NUMA controls while retaining native/generated
settings; Explore keeps loading controls. An explicit primary action retains
one request through settings settlement, model preparation and Start. Changing
inputs/leaving cancels unstarted work; repeated clicks cannot duplicate starts.
Unknown totals stay indeterminate. Video controls share pending-system admission,
including events before replies. Progress stays in setup except Train's live card;
card-to-action gaps remain empty. Preparation reports GPU connection, compiled
opening, sampling, initialization and optimizer setup before measurements exist.
Validate/Predict retain saving options captured at Start through preparation;
later viewer edits do not change them. Prediction inspection replies use the
same revision-aware reduction as events and cannot overwrite newer source facts.

### Scratch and continuation controls

Train's **From scratch** checkbox defaults off. When checked, its description is
**Pretrained base encoder; new RF-DETR detector weights.** Canonical presets stay
available; Custom Weights and Transfer/Resume remain visible but disabled.
The checkbox itself disables while settings or the primary action are busy.

Toggling either way submits one settings edit: select canonical weights, preserve
the preset, and clear detector, launch-only encoder, Resume and class-layout
paths. It does not start work. The complete generated
[selection identity](application-wire.md#canonical-declarations-and-generator-ownership)
includes scratch mode, so changing it invalidates stale preparation, continuation
inspection and queued starts. Other workflows retain their existing selection.

Prepared encoder and admitted checkpoint details can show optional source
provenance. For scratch-origin Resume, turn scratch off and select the saved
session through Custom Weights; the ordinary inspection/Resume flow restores it.
Native [initialization](rfdetr-workflows.md#training-initialization) and
[continuation](model-merging.md#encoder-provenance) own semantics; Iced owns labels,
checkbox/radio presentation, availability and interaction state.

### Lane and recipe controls

Train offers **Shared gradients**, **Independent models**, **Periodic averaging**.
[Native batch policy](rfdetr-training.md#logical-lanes-and-global-batch) owns
logical membership/effective batch/capacity. Training and validation lanes are
independent. Advanced groups batches/lanes, validation, recipes/schedules,
precision/encoder, sampling/augmentation, supervision and merging/soup. Dividers
have ten-pixel gaps here. The Advanced title/subtitle are 18/12 px; H2 is 16 px
with 8 px before settings; Recipe scope/optimizer H3 is 14 px with 8 px before
and 6 px after.

**Global batch (all GPUs)** edits microbatch images. Native facts supply
**Batch / GPU / microbatch** (range for uneven ranks), **Effective batch / model
update**, **Images / session round**. Calculated values use numeric-field size/
alignment without outline/callback. Revision-matched calculations remain during
preparation until admitted facts arrive; stale drafts/membership show **Updating**.
Validation distinguishes configured/admitted capacity. Rust does not duplicate
formulas. Independent/periodic modes display their experimental limitation.

Recipe scope chooses globals/stable model ID. New models copy settled globals;
existing models retain overrides through mode changes. Model scope adds seed and
soup coefficient. Selecting a preset or custom weights populates Train
controls from [catalog training defaults](rfdetr-training.md#recipes-and-schedules).
AdamW/Muon/SGD use generated defaults; switching optimizer
preserves explicit overrides. **Use optimizer defaults** clears only that scope,
preserving optimizer. Native policy determines Step/Cosine/Ultralytics linear
availability; [recipe reference](rfdetr-training.md#recipes-and-schedules) owns
values/clocks. Task loss summaries show native coefficients, including joint
L1/findability/visibility/NLL and dataset-owned joint meaning. Unsupported
Match-Free/DN choices follow native task/decoder capabilities.

Class balancing offers Off/Stratified/Rare repeats + Stratified, revealing rare
limits when needed. Periodic cadence is Each epoch/Every N rounds. Final soup is
Mode default/Off/Uniform/Explicit/Validation-greedy. [Merging](model-merging.md)
owns resolution. Encoder-unfreeze and augmentation-disable final-epoch counts
are independent, zero-disabled edits, never implicit starts. Inference lanes are
separate for Validate/Predict/training validation; native admission may reduce
physical capacity, and single-image prediction admits one.

### GPU selection

The [GPU card](../src/frontend/iced/src/view/workflow/gpu.rs) shows native CUDA
ordinal/name/total GiB. Validate/Export/Predict select one. Train edits an ordered
checkbox list with **Rank order**: add appends, remove preserves remaining order/
NUMA choices, and one GPU must remain. Edits use ordinary generated settings,
preserve pending drafts and disable while the primary action/settings are busy.
Missing saved devices remain **unavailable**; empty inventory shows **No CUDA GPUs
available**, without replacement selection. Native preparation/admission uses
settled choices; [GPU execution](gpu-execution.md#workflow-device-selection)
owns persistence/locality/retirement and independent display-device selection.

### Dataset compilation controls

Dataset exposes Stretch/Letterbox geometry and **Resize method**:
AVIR, Perceptual downscaling, PyTorch bilinear, PyTorch bilinear + antialias.
The group edits method/perceptual settings together; [data reference](datasets.md#rgb-resampling-methods)
owns native semantics/defaults. Predict Image file offers AVIR/two PyTorch methods,
retaining choice across source switches. Rust owns labels, native declarations
own values.

**Compile Benchmark Dataset Override** chooses Directory or built-in recipes,
disabling source text/Browse while on. **Coco custom** is default; **Coconut**
reveals validation (Coconut validation default, Stock validation, Coconut stock)
and default-off dropped-mask recovery. [Benchmark reference](benchmark-datasets.md)
owns membership/recovery; changed recovery requires recompilation. Coconut's
group is indented 16 px; descriptions/recovery use 12 px and radio-label alignment.
Hidden choices persist: Coco custom ignores Coconut options, Directory ignores
benchmark options. Edits never start compilation.

Compile requires settled settings/native admission and captures them for the
operation. While active, recipe/validation/recovery choices disable, other controls
retain their rules. The action becomes **Cancel compilation**. Progress shows
current work, Acquisition/Labels/masks/Pixels and source details with native
[units](benchmark-datasets.md#reading-compilation-progress). [Coalesced revisioned
snapshots](application-wire.md#dataset-operation-snapshots) preserve Stop/terminal/
reconnect ordering. Remaining work replaces Indexing captions; track state is
independent. Compacting then reports processed pixel/label bytes before sync/
publication; completed tracks do not settle Compile. Rejected images/annotations
remain distinct from successful pixels and healthy-subset admission.

Progress reserves its greatest measured height per generation/width; width changes
remeasure, new generations reset. Native cancellation shows **Cancelling…**, hides
tracks/details and disables repeat Stop while retaining reserved space. Only the
native terminal settles it: Completed/Cancelled show supplied detail/path;
failure/refusal posts Status and collapses terminal body. Reservation releases
through the shared transition. Late progress cannot revive work or undo cancellation.

## Live capture and analysis

[Live](../src/frontend/iced/src/view/live.rs) defaults to capture-only operation.
**Analyze captured frames** enables the shared Predict model and settings;
the model card, Skeletons and Joint uncertainty controls edit that shared state.
For analysis, prepare an ONNX or TensorRT artifact on the capture device before Start. Native
weights and mismatched compute devices are refused. Start captures the current
configuration; changing it for another run requires Stop and an explicit restart.

Completed frames retain clean pixels and paired analysis, including a valid
analyzed frame with zero detections. Upscale and Open in Annotation use the
selected native product and preserve its named joint meaning. A provider-start
failure whose cleanup settles preserves the last completed image and permits a
later Start; Status retains the cause. Physical capture, model and receiver
retirement remain with the [native owners](architecture.md#model-and-media-owners).

## Session Status

[Status](../src/frontend/iced/src/view/status.rs) is the single operational
warning/error surface. Empty history shows green **Ok**. Notices add a warning
triangle/white outline and pink two-second border pulse. Only visible active
motion redraws; reduced motion is static, hidden windows quiet. Arrival neither
opens the panel nor steals focus. Healthy hover-close restores borderless
appearance after pointer leave; keyboard use restores focus outline without
changing focus target.

Click/key/touch toggles. Hover opens across trigger/gap/panel; leaving closes a
hover-opened panel. Explicit close/Escape inside latches closed until leave/
reentry or activation. Root-overlay placement avoids page/sidebar clipping;
width is bounded by 60% of page/viewport with 12 px margins, rows scroll. Complete
selectable read-only title/detail text wraps; Copy emits title, two newlines,
detail exactly. Rows support Dismiss, without clear-all.

Status remains available over file dialogs while navigation is disabled.
It captures pointer/scroll/keyboard so underlying pages/modals/viewers/shortcuts
cannot act. Focus traverses eligible panel controls; restoration checks ownership,
including delayed annotation replies. Clipboard completion is checked by notice,
content version and copy attempt.

[NoticeStore](../src/frontend/iced/src/view_model/notices.rs) retains 128 ordinary
notices plus overflow in memory. Typed sources include native/transport/dialog/
checkpoint/provider/annotation/history/clipboard failures. Identity deduplicates
snapshot/event/reply occurrences, updating existing rows. Bounded acknowledgment
frontiers survive dismissal/eviction and prevent replay. Reconnect retains notices,
rebasing only changed owners; Bootstrap seeds old terminals without replaying
failures, while active work may later fail. Nothing persists.

Systems still own recovery/retry/Stop/results/resources; progress/paths/recovery
controls stay local. Static help remains local; diagnostics are separately opt-in.
The [emergency page](../src/frontend/iced/index.html) works when Iced cannot render.
Async WebGPU failure stops configuration/rendering and preserves first cause.
GPU OOM immediately offers manual reload while native work continues. Other device
failures allow one automatic reload/episode, then manual recovery; a healthy
interval clears the session-storage counter, independently of notices.

A Firefox process segfault permits one automatic browser replacement while
native work and committed documents remain. The replacement reconnects to current
native snapshots; browser-local view state and unsubmitted drafts reset.
[Crash reporting](logging.md#firefox-sigsegv-recovery) remains active with logging
disabled and explains the recovery limit and retained record.

## Shared form expansion and dividers

[Transitions](../src/frontend/iced/src/view/shared/transition.rs) keep conditional
bodies mounted with stable widget identities, revealing natural-scale layout
through a top-fixed 200 ms ease-in/out clip. Reversal starts at current height;
first layout/width changes measure directly; unchanged updates do not restart.
Nested bodies follow reflow without double animation. Consumers: Dataset choices/
splits/size/progress, custom weights, Export Advanced, Predict saving and diagnostics
(the latter retains its bottom Show/Hide trigger).

Drawing/input share reveal and scroller clips. Closing disables input immediately,
retires gestures without activation, releases hidden/partial focus and exposes
only eligible controls to traversal/operations. Popups require full reveal;
offscreen pressed controls still receive release cleanup. Only visible motion redraws.

[Section dividers](../src/frontend/iced/src/view/shared.rs) are centered 1 px lines
at 75% width in theme outline color, with 4 px above/below (9 px total). Dataset
places them after the entire benchmark override and after Overwrite, without
extra boundary spacing; Advanced uses the spacing specified above.

## Compact operational text

[Status text](../src/frontend/iced/src/view/shared/status_text.rs) fits ordinary
status/progress/paths on one line, reducing by 0.5 px to minimum 8, then ellipsizing.
Short text never grows. Hover exposes original; display replaces line breaks
with spacing while selection/copy retain them. Colors/widget identity survive.
Headings/help/fields/tables/Status notices retain ordinary wrapping.
Bounded binary search caches content/width/normal size/font/scale; intrinsic and
finite layouts retain separate sizes, toolbars separate paragraphs. Unchanged
redraw reuses measurements and leaves disclosure/progress reservations intact.

## Shared primary actions

| Workflow | Green idle | Red active |
| --- | --- | --- |
| Train | Start Training | Stop Training |
| Validate | Start Validation | Stop Validation |
| Predict | Run Predict | Stop Predict |
| Export | Run Export | Stop Export |
| Live | Start Live | Stop Live |
| Annotate | Save Annotations | Green save-only; duplicate save disabled |

[Primary action](../src/frontend/iced/src/view/workflow/primary_action.rs) uses
native activity and accepted preparation/save state; Stop preserves owner policy.
Train/Validate/Predict/Export retain an empty 10 px card gap in every state;
progress stays in cards, failures in Status. Active visible buttons animate ten
blue segments clockwise around the existing white 3 px rounded border, using
Browse-source blue. Bounds/radius/core remain unchanged. The retained renderer
uses small per-widget uniforms/shared device pipeline, narrow strips/corners,
no per-frame mesh, native field or polling. Settled/hidden buttons stop redraws.

## Validation workspace and shared viewer

Validate's center is fixed 16:9 regardless of other pages' aspect. Equal 8:9
halves contain twelve [COCO summaries](rfdetr-workflows.md#evaluation-metrics-and-retained-samples)
and a non-scrolling 2×3 atlas with 4:3 cells/explicit empties. Metrics/Validation
Preview share a heading strip matching aspect-selector height. Atlas uses shared
containment/padding/border/spacing/hits, preserving source aspect; sample ownership
stays Validation-local. Capture continues during detail; closing returns to retained
population. [Sample retention](rfdetr-workflows.md#evaluation-metrics-and-retained-samples)
owns generation/refusal behavior. Boxes and available Masks/OKS columns retain
their actual native detection caps and unavailable values.

Explore/Validate share the [image-viewer panel](../src/frontend/iced/src/view/image_viewer.rs):
Sample, Previous/Next, Fit, wheel zoom, right-drag pan, overlays, Close,
Basic/Fast/Neural Upscale, Open in Annotation. Validation navigates retained samples
only. Its opaque modal covers headings/center while leaving sidebars visible.
Selection uses displayed atlas metadata; a held target is admitted once until
it changes/release, not on every publication. Requests use common connection/
settings/system admission. Upscale targets selected native sample; Annotation
captures drawn sample/view and imports GT (including masks/joints), not detections.

Groundtruth/Detections checkboxes control complete layers, retaining finer Labels/
Masks/Boxes/Skeletons. Labels are Iced-local; native plane changes recompose
retained products. Joint uncertainty is independently selectable. Atlas groups
sit under preview, not metrics; complete groups wrap with 20 px gaps. Explore
keeps Fit/Original left and overlays right with ≥20 px separation. Hidden atlas
controls retain measured footprint during detail without duplicate identities/input.

GT RGB complements Det RGB per channel (`255−value`) with existing alpha. Native
mask/outline overlap sums saturated RGB (Linear Dodge), then applies alpha;
complements yield white before alpha. Captions use painter order GT then Det,
including Det backgrounds above GT glyphs. Font/contrast/alpha stay shared.
Validation owns this dual-layer policy; Explore uses its single palette/layer.
[Caption preparation](../src/frontend/iced/src/presentation_surface/labels.rs)
retains separate GT/Det collections, shared paragraphs/colors and derived bounds;
hidden layers paint nothing. Diagnostic visits preserve original sample/label order.

### Display confidence

Advanced **Display confidence**, help **Preview only**, accepts finite `[0,1]`,
default 0.4, with 450 ms debounce. Decimal precision survives native float storage;
there is no 0.01 quantization, button/wheel/arrow increment. Partial text survives
rebuilds; empty/out-of-range input cannot replace accepted setting.
[ValidationDisplaySettings](../src/controller/contracts/validation_display.h)
persists under `workflows.validate.display`, separately from evaluation request.
Scores ≥threshold recompose retained detection labels/boxes/masks/joints;
GT/raw predictions/inference/metrics/reports/import are unchanged. Lowering reveals
retained detections without evaluation. Image metadata records the completed
setting, so pending newer edits cannot misfilter old captions.

## Original view and annotation import

Explore **Original content**/Validate **Original** restores source aspect using
completed pixels: Stretch uses full content, Letterbox crops stored padding.
Off shows whole product at native input-canvas aspect, even for restored Upscale
pixels. Missing source extent falls back to sampled pixels. It opens no source
file and recovers no discarded detail.

Paired `VisualFrame` contains canvas/content/source extents and optional resize
provenance. Upscale separates native/prepared/output geometry. [Surface geometry](../src/frontend/iced/src/presentation_surface/geometry.rs)
applies a coherent transform to pixels, boxes, masks, joints, labels, fit/pan/zoom,
clipping and inverse input while distinguishing sampling from displayed aspect.
Original reuses completed pixels without launching/changing processing.

Basic/Fast/Neural Upscale uses current selected native detail/document, even while
an older fallback remains visible. Shared [input preparation](gpu-execution.md#upscale-input-preparation)
restores known Stretch before processing and retains Letterbox, independently of
Original. Derived products retain source identity/extent and prepared/scaled
geometry; the same view preference applies.

Open in Annotation captures exact drawn image, crop and target extent at dispatch,
including derived output. `AnnotationOpen` does not reinterpret later settings.
Target aspect fits sampled extent and rounds to integer dimensions; screen zoom/
clipping does not change import. Receiver copies settle before borrow release;
continuous annotations/joints/uncertainty use the same crop/aspect mapping.
Navigation/preferences cannot alter accepted import. Masks have support independent
of boxes, including pixels beyond them and present-empty masks. [Materialization](../src/controller/presentation/visual_document.cpp)
validates before replacing the editable document.

## Training dashboard

Charts fit center width/body height and selected aspect: default 16:9, also 9:16,
4:3, 3:2, 1:1, 16:10, without internal scroll. Title click expands within that
region; Back restores grid, preserving sidebars/progress. **Charts** selects:

| Initial | Additional |
| --- | --- |
| Training loss | Loss components |
| AP50 | AP75 |
| AP50:95 | Class/cardinality errors |
| Average recall | Mask metrics |
| Precision/recall/F1 | Area breakdowns |
| Learning rates | Images/second; GPU utilization/memory/temperature |

Reset restores six main selections and closes expansion; no selections/no data
have explicit empty/waiting views. Joint losses and OKS metrics join generated
task-aware series. AR labels use actual caps; unavailable mask/area/sensor values
leave gaps. Tooltips use four significant figures/scientific notation without
changing coordinates. Sources select model/weight or synchronized trajectories;
live/saved state remains independent. Selected output shows method/path/ingredients/
validation separately from curves; [history](rfdetr-workflows.md#saved-history-and-plots)
owns identity/defaults/sparse observations.

Epoch axis defaults on with fractional live positions; off uses global optimizer
steps. Log loss scale affects training loss/components only; no validation-loss
curve. Throughput is epoch-average; GPU units are %, GiB and °C. A loss-scale
change preserves non-loss cameras/legends/geometry, including hidden/saved views;
reapplying the same value does not invalidate. Preparation keys retain effective
per-chart scale.

Plots retain series/camera/legend through expansion, hiding and navigation.
Unchanged remounts preserve view; data/limit/scale changes follow autoscale.
Grid wheel/trackpad (including modifiers) scrolls the page. Expanded plots bind
unmodified/Ctrl-wheel to camera zoom and capture handled gestures; outside or
unbound input reaches the page. Returning clears bindings; other gestures remain.

[Metrics history](../src/frontend/iced/src/view/metrics/history.rs) retains ≤128
endpoint/extrema buckets per curve, independently live/saved. Sequence gaps,
unavailable values, missing records and new attempts break lines; isolated points
remain markers. Retired disconnected summaries post Status omissions and remove
their extrema; coalesced live data preserves pending gaps, saved history is
unchanged. Hidden charts ingest without geometry work; visible changed charts
prepare, retaining plots/series/GPU capacity.

Owned [iced_plot](../third_party/iced_plot/src/plot_widget.rs) preserves settled
state outside transient widget trees. Axis text is outside shaders, rotated Y
and themed. Cursor captions reconcile after camera/bounds, share immutable
publication payloads and clear once on leave. Rendering uses painter-order
content/highlight/crosshair in one chart-sized MSAA pass/resolve, then clipped
composite. [Picking](../third_party/iced_plot/src/picking.rs) bounds GPU readbacks
and never reuses unsettled mappings. These are ownership guarantees, not measured
performance claims.

## Numeric editing

Integer fields have no increment buttons/wheel mutation; keyboard edit/select/
paste and bounded arrows remain. [Typed fields](../src/frontend/iced/src/view/workflow/fields.rs)
retain native signed/unsigned identity/constraints without floating conversion.
Explore min/max instances are full-width 0–10,000 fields, clamping to each other.
Seeds/index inputs preserve exact u64/range policy. Compiled ranges address
physical IDs through `slot_count−1`, not eligible `image_count−1`; filters/
navigation skip holes.

Unavailable mutation disables callbacks without remounting, preserving focus
through admission. Explore borrows submitted drafts or ready snapshots; only
editing allocates requests. Class checklists derive membership once per list,
retaining All/None/Subset without per-row vector scans.

Vendored NumberInput retains child text/modifier trees through diff/layout and
row/column padding changes. Incomplete/equivalent spellings (empty text, `0.`, `0.40`) survive
unrelated rebuilds while accepted value is unchanged. Changed authoritative values
replace it; rejected numbers restore accepted text. Display confidence additionally
disables arrow increments.

## Typed application boundary

[Application wire](application-wire.md) owns schema/generation, record formats,
limits and connection failures. Compatible complete Bootstrap precedes native
state installation. Generated dispatch reaches handwritten reducers; Rust owns
presentation/interaction. WebSocket carries controls/logical facts; independent
FD graphics carries completed images with schema/frame/product/source metadata.
Firefox transports it opaquely. [Metadata decoding](../src/frontend/iced/src/presentation_surface/metadata.rs)
checks extent/schema/exact image/product pairing. Invalid candidates preserve
safe read custody and last valid image.

## Shared immediate workspace input

[Input capture](../src/frontend/iced/src/workspace_input.rs) records movement,
all button edges, enter/leave, wheel and cancellation across workspaces; click
count belongs to press, giving one native effect per gesture. Iced transforms
coordinates and retains pan/zoom/hover; native Annotation resolves editing targets.
Passive products use the same vocabulary without tools.

```text
Iced capture → ordered connection → generated compact interaction
    → native owner queue/effects → dirty render → completed graphics image
```

Mouse records and commands retain order even before Bootstrap epoch, reuse
encoding storage and survive temporary pressure. Admission waits on neither
frontend credits nor document/display settlement. Essential allocation/transport
failure closes the peer rather than shortening a stroke.

## Ordered annotation input and retained storage

[WorkspaceInputQueue](../src/controller/presentation/workspace_input.h) starts
at 64, grows geometrically, retains capacity and pops O(1). Annotation queues
mouse/document/peer records together; its input worker owns document/reducer/
history and synchronization. Peer admission is checked; stale-document records
cancel; accepted path-dependent samples are preserved. Peer replacement queues
gesture cleanup before replacement input.

Undo/Redo directionally reads the journal then moves the entry after admission/
live-payload preparation, preserving bounded history, editor facts, stable runtime
object/element identity and full payloads without reversed copies.

## Document commands and settlement

Open/Edit/Save follow admitted mouse/commands. Edits/save settle independently of
display. Opening/color sampling use GPU continuations that suspend dependent
document effects while transport/other systems progress; Stop remains available.
`busy=true` means queued admission; later state reports completion with separate
revisions and potentially reversed reply/event arrival. Neither proves texture
validity or GPU read settlement. Rejection, wire/resource failures and cancellation
retain typed outcomes.

Named class/joint schemas preserve connections, flip meaning and visibility.
Skeleton controls select/edit connections and set Unannotated/Occluded/Visible;
skip selects next and reset clears the selected point to Unannotated. Known
schemas initialize all named joints and retain matching joints by name on category
changes; free-form skeleton editing remains available. Import/save/history keep
joint meaning independently of labels.

[Saving](../src/controller/subsystems/annotation/annotation_persistence.cpp) writes
named CBOR through exclusive sibling staging, file sync, rename and parent sync.
Directory destinations use document-identity filenames. Publication failure keeps
the incumbent; failed parent sync after rename reports uncertain durability.
[Decoding](../src/controller/contracts/annotation_serialization.cpp) also admits
older documents without joint schemas, converting boolean node visibility to
Visible/Unannotated without inventing joint names. Explicit Load can attach
legacy constant-resource documents only to a ready image with matching geometry;
it reports that conversion and never falls back from a malformed modern document.

## Reduction, publication, and source changes

Annotation input captures immutable scene/editor/preview descriptions for a
separate renderer. Three reusable descriptions cover scratch/newest pending/
active GPU work; only unsubmitted newest work coalesces. Occupied output retains
baseline/pending work and arms actual availability while input proceeds.
[Document reduction](../src/controller/subsystems/annotation/annotation_reducer.cpp)
keeps four revisioned immutable scene backings, separately capturing editor/
gesture facts. Readers can hold old content; replacement invalidates even equal
revision numbers.

[Raster execution](../src/controller/subsystems/annotation/native_annotation_algorithm.cpp)
tracks packed geometry/masks separately from allocation-local damage. Reuse checks
content, offsets, dimensions and capacity; changed words upload one enclosing
range, removed/disabled objects invalidate membership. Upload events protect
staging; growth/failure requires refill. Raster clips conservative bounds to
damage, including mask support/handles, preserving painter order; flat runs visit
only admitted pixels.

Logical UI follows latest committed document; frame facts describe completed
pixels even when input leads. Full UI and compact frame progress have separate
revisions; preview-only events omit document. Bootstrap restores logical facts
independently of graphics. Import/size validation precedes committed replacement.
Press resolves current document/tool with selected-handle precedence/image
coordinate tolerances, then retains that target through samples/history. Runtime
identities are not saved. Fractional boxes truncating to empty raster skip only
outline, preserving gesture/mask/selection/damage.

Mask hits find one normalized row/start predecessor in O(log R), preserving
painter/handle order. Cleanup caches disk reach per absolute row offset across
erosion/dilation; preview construction accumulates exclusive support bounds while
writing, retaining empty/HSV semantics without another scan.

All six visual producers share [VisualRuntimeOwner](../src/controller/presentation/visual_runtime_owner.h).
Input/content, workspace admission and actual completion wake dirty work;
planes/damage/geometry/staging reuse capacity. Idle renderers have no repeating
timer. Browser redraw does not rerun domain processing. Producer events notify
Presentation directly; frontend selects only on viewed-product changes.
Presentation retains one submitted/latest pending selection/notification. Newer
source facts can retry a refused borrow; unchanged unavailable state waits for
notification, never stale polling.

## Native GPU custody and completion

Only an explicitly bound Firefox `WebGPUParent` actor opens the workspace
channel. A process-exclusive claim and serialized bind/unbind protect generation/
device/owner admission. Supersession closes old admission while reads/submissions/
callbacks retain obligations; claim lasts until final physical owner releases.
Replacement gets a fresh connection. Peer loss is authoritative; logical selection
never proves GPU completion.

| Producer | Retained raw products | Display |
| --- | --- | --- |
| Explore | Thumbnail cache, clean/semantic atlas, separate detail | Fused finalization; changed atlas regions |
| Annotation | Copied clean baseline and immutable scene planes | Fused raster finalization |
| Predict | Latest pixels/annotations and owned preview | Fused visual-runtime finalization |
| Validate | Six captures, progressive/displayed selection | Fused reusable atlas/detail |
| Live | Composite lease and clean output | Receiver copy; same-device final storage where admitted |
| Upscale | Copied clean/semantic input, document, derived cache | Fused selected-result finalization |

Raw pools: Explore 3, Upscale 4, others 2, including inactive results; Validation
captures are separate. Foreground graphics owns two persistent display buffers,
reused across selection; growth/replacement retains older backing through readers.
Raw reads and browser acquisition protect distinct storage. Clean-only same-device
work may alias final storage when lifetimes permit; otherwise independent raw
work continues despite display pressure. Semantic products retain both planes.
Front/back changes need not copy, and allocation identity is not product revision.

Live and fresh/warm Upscale overwrite every active pixel via `PublishRetained`;
semantic-only Upscale preserves clean through ordinary Publish. Partial producers
retain initialization rules. Pending finalization holds its completion-draining
owner even after cross-device raw read release: workspace replacement/writes
still wait for final display work. [GPU interop](gpu-execution.md#shared-workspace-interoperability)
owns admission/layout/growth and physical imports.

BorrowFrame/BorrowDocument receivers complete copies before release; device
mismatch uses producer-owned peer/pinned transfers, separate from display.
Presentation borrows exact metadata and admits finalization on the producer;
Firefox owns allocation, native writer import/publication/timeline. There is no
extra Presentation pixel pass. Offers alone grant no browser read.

[Native writer](../src/controller/presentation/native_presentation_writer.cpp)
waits GPU readiness, signals odd ready value, publishes exact frame/metadata then
eventfd. Firefox nonblockingly acquires latest completed offer without matching
application snapshot/unfinished work. Misses retain fallback and retry on a
readiness edge. Direct samples GENERAL; copy mode writes one reusable sample.
Source reuse requires the final Vulkan ownership release's even timeline signal
and actual native settlement. Acquired, ReleaseSubmitted and ReadSettled are
different receipts. Callbacks wake the worker; ordinary pumping is nonblocking,
initialization/terminal retirement synchronize where required.

Ready-flag visibility does not prove callback return. Counted completion retains
backing until callback settlement. Outstanding ownership defers retirement via
wake notifications; unproven completion retains custody/closes unsafe admission.
Dead-browser cleanup cannot await its timeline. Source/arena identities differ:
≤2 sources/arena, ≤6 records total, ≤3 active/candidate/retiring arena generations.
High-water growth prepares unpublished candidates without losing fallback.

Copy uses two Primary sample slots (fallback/incoming), shared across rotation;
direct retains control identity without arena pixels/copy. Copied source may
retire while sample remains; direct fallback holds source. Page/draw/source/
arena obligations settle independently. Vulkan devices/FDs/CUDA contexts outlive
registry/IPC removal and partial failure. Browser exit is not completion. Canvas
DMA-BUF descriptors/semaphores retain actual [submission custody](gpu-execution.md#browser-canvas-descriptors-and-submission-custody).
Live rate controls capture/consumption; Firefox queue/swapchain/compositor/Wayland
controls display. Reuse adds no intentional frame delay or hard one-frame promise.

## Explore gallery and displayed geometry

Stop is available during opening/open/Detail, cancels/closes and retires resources
after reads, then returns **Waiting for a dataset** with saved settings. Detail
Close returns retained gallery. [Dataset residency](datasets.md#explore-thumbnails-and-atlas-residency)
owns caching/tile admission. Demand includes partial rows, so measured N/N+1 rows
may alternate without card raster changes.

Eligible image_count differs from physical slot_count. Matches/navigation count
eligible filtered samples; displayed IDs, selection, donors/cache retain physical
IDs. Holes are never background/dense substitute samples. A ready nonempty gallery
without paired drawable pixels shows Preparing visible tiles while work/readiness
is pending, otherwise Restoring gallery. Refused/failed presentation shows Gallery
unavailable; domain failure keeps its own cause. Logical status is Dataset ready.
Counts/scroll persist, and the GPU/input surface stays mounted beneath status so
first/replacement binding can form.

[Routing](../src/frontend/iced/src/app/presentation.rs) retains restoration for
current dataset/frame. Return requests publication; reply settles selection but
only matching paired drawable metadata satisfies restoration. Obsolete Detail
retirement still protects SampleRead/encoded draws. Capacity waits for real
readers. Rejected offers retain exact product through settlement/reconnect gaps;
receipts are consumed once and stale duplicates cannot displace newer gallery
work. Recovery uses retained products/notifications without reopening/polling.

The gallery sensor measures under Detail, retaining size/capacity/columns/row/
fraction without submitting viewport edits. Return reconciles via native events,
including repeated orientation changes. First usable measurement wakes pending
Open; zero capacity keeps it pending without another click/event. Independent
gallery/detail handles preserve unchanged gallery revision under newer domain
observation; changed requirements resume cache work without discarding neighbors
or waiting for unrelated disk lanes.

Paired `ExploreAtlasLayout` records first row/count/capacity/origin/columns/card
extent. Visible rows differ from high-water storage. Pending/displayed metadata
controls cropping, captions, circular placement and hits, even with newer layout
pending. Identified placeholders can hover/select before thumbnail readiness;
blank final-row padding cannot. Same-image revisions preserve transforms/identity.
Zero paired matches adds an opaque No samples match the filters overlay without
retiring native empty publication/read custody.

Labels use local checkbox over paired geometry; toggling needs no GPU image and
persists with view settings. Boxes/Masks/Skeletons dirty native planes, including
idle viewport. Augmentation replacements preserve clean pixels/meaning until
completion. Metadata installation prepares shared caption text/paragraph/color/
contrast; category index is O(1) and constructs referenced categories only.
Detail folds selection into that index. Pending edits cannot recolor older captions.

Select/Next/Previous/Close cancels departed derived work while retaining last
frame. While requests settle, navigation accumulates bounded net offset modulo
match count; opposite clicks cancel, explicit select/Close clears, admission later
drains it. Route teardown releases display custody. Original preference remains
with same image through routes/reconnect; another image uses its own preference,
even while fallback is displayed.

## Browser draw eligibility and retained fallback

Graphics acquisition returns immutable metadata/read permission; Iced derives
labels/layout from that pair. Logical snapshots never grant draw permission.
Direct sources/copied arena slices are sampled without full-image capture pass;
queue ordering allows drawing after submitted copy before callback. Direct completed
acquisition can become fallback immediately; copy requires actual completion.
Obsolete completion cannot promote another source/release newer sample/metadata.
Capacity retry may retain logical publication with newer physical transfer sequence.

SampleRead independently protects fallback, encoded draws and enabled probes.
Vendored Iced resource batches attach to the actual encoder and release after
submission settlement/abandoned encoding, including screenshots. View construction/
diagnostic callbacks cannot release encoded reads; work-done means settlement,
including failure, not successful rendering.

| Event | Permission |
| --- | --- |
| Command admission/completion | Logical state change |
| Image publication | Expose completed pixels/metadata |
| Acquisition + valid metadata | Direct draw or queue copy-ordered draw |
| Source-release submission | Arm even-value observation; no reuse |
| Actual even settlement/ReadSettled | Settle exact physical source read |
| Direct acquisition/completed copy + matching metadata | Promote exact fallback |
| Encoder/submission settlement | Release independent draw hold |
| Final display/draw/probe release | Release page sample; direct reuse also needs read settlement |

[presentation_surface](../src/frontend/iced/src/presentation_surface.rs) owns page
custody/draw preparation; its gallery module retains atlas pairing, app routing
owns source/view transitions. Last completed pixels remain drawable during work,
growth/copy. Fit/pan/zoom/clipping/redraw reuse them. Same-image revisions preserve
transforms; changed source or connection identity resets, even with repeated revisions.

Renderer caches actual TextureViews/bind groups, reusing returning direct views.
Replacement refreshes affected bindings before backing release; teardown drops
bindings before texture retirement. View equality authorizes binding reuse only;
SampleRead/completion still govern lifetime. Redraw skips unused coordinate
projection but reconciles input cancellation/view/FPS; pointer input maps fully.
Unacquired offers require no read settlement; rejected acquisitions still release
GPU ownership. Hidden/pressure state does not acquire unused offers. ArenaTexture
explicit retirement waits for views/bindings/probes/readers, not JavaScript GC.

## Browser redraws and FPS

Visible browser redraw may continuously reuse fallback; native dirty rendering
progresses independently. Canvas resize and real Wayland fullscreen entry/exit
retain layout/configuration/presentation during compilation; a document fullscreen
flag alone does not prove window resize. [Validation](validation.md#dataset-presentation-and-lifecycle)
and [gated observations](logging.md#dataset-presentation-evidence) define evidence
without driving work/redraws.

Show FPS counts actual queue submissions containing workspace draws, once per
meter/submission; clipped/abandoned/unsubmitted work counts zero. Encoding,
presentation and work-done are separate. [FPS owner](../src/frontend/iced/src/workspace_fps.rs)
refreshes retained text every 500 ms, six pixels inside upper-right. Disabling
removes meter/clock/counter/submission observer. It is a product setting independent
of diagnostics; rendered placement checks use asynchronous capture.

## Diagnostics and acceptance

Diagnostics never authorize input order, drawing, cache validity or lifetime.
Ordinary `./mmltk --gui` leaves collection/probes off. [Logging](logging.md) defines
activation/formats and independent acceptance control/reporting; [validation](validation.md#gui-behavior-and-evidence-ownership)
maps causal native tests and actual packaged Wayland evidence.
