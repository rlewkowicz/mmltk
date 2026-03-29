# Shared GPU Live Session Pipeline for Predict + Annotate

## Summary
- Unify live predict and live annotate behind one modular GPU-first runtime.
- Keep capture, fanout, analysis, manual drawing, and final composition as separate responsibilities.
- End state: treat every overlay as its own separate GPU image. The base frame is never the overlay. Manual drawing and automatic analysis each render into their own RGBA surface, then a compositor blends them over the base output image in real time.
- Phase 1 may keep a narrow compatibility layer where predict still renders in-place on a controller-provided output buffer, and annotate still maintains a host `AnnotationFrame` mirror for CPU-only workspace features. Neither compatibility path is allowed to own capture or drive preview/display.
- Deliver in 3 phases so Phase 1 fixes the annotate display path and establishes the threading/buffer model without forcing the whole feature set at once.

## Core Architecture
- `LiveVideoIngress`: one dedicated thread, built on `frameshow::CaptureSession`, responsible only for capture plus pinned host-to-device upload. It publishes full-frame source buffers and frame IDs. No annotate-specific or predict-specific logic lives here.
- `LiveFrameFanout`: one dedicated thread waiting on the newest source frame. It asynchronously copies into precreated per-frame bundles:
  - `DetectBundle`: full-frame detect source, optional crop detect source, fixed model-resolution input scratch, and a dedicated detect-dimensions record.
  - `OutputBundle`: full-frame output base, optional crop output base, composited output base, and a dedicated output-dimensions record.
- `LiveAnalyzerWorker`: one dedicated thread waiting on `DetectBundle`. It owns the hot model or a no-op analyzer. Annotate is analyzer-agnostic because the session can run with no analyzer attached; assist/model-backed flows attach an analyzer explicitly.
- `LiveManualOverlayWorker`: one dedicated thread for manual drawing state. It consumes a CPU-side overlay document from the UI and redraws a separate RGBA manual overlay image on GPU whenever the document changes, independent of frame cadence.
- `LiveCompositor`: one dedicated thread or stream-owned worker that continuously composites `OutputBundle.base` + `ManualOverlayImage` + `AnalysisOverlayImage` into the published output image. Display never waits for a fresh analyzer result.
- `LiveFrameId`: add `{ session_nonce, sequence }` and carry it through source, detect, analyzer result, manual save request, output, and preview publication.
- `LiveHostFrameMirror` (annotate-only compatibility path): one optional best-effort worker backed by the controller that produces `AnnotationFrame` / `pixels_bgr` for CPU-only annotate consumers such as HSV sampling, assist, export, and save. It uses pinned staging buffers, may lag or drop, and is never on the preview/display path.

## Buffer and Sync Rules
- Precreate all CUDA buffers needed for the session up front. Use grow-only `ensure_capacity()` for buffers whose required dimensions can increase during a session. Never shrink in-session.
- Keep ingress pinned. The current `frameshow` USERPTR buffers stay page-locked because async host-to-device transfer depends on pinned memory.
- Use atomic slot state plus CUDA events/stream waits for hot-path synchronization. Do not add CPU mutexes on the per-frame path.
- "CUDA mutexes" is implemented as event-gated ownership: `cudaEventRecord`, `cudaStreamWaitEvent`, `cudaEventQuery`, and atomic slot transitions.
- Lock-free CPU structures (e.g., immutable snapshot publication via `std::atomic<std::shared_ptr>` or equivalent double buffering) must be used for cross-thread data like crop regions or UI settings, completely eliminating `std::mutex` from the worker hot paths.
- Source-frame release is always event-deferred. No source slot returns to ingress until every downstream CUDA stream that touched it has completed.

## Optimization Guidelines
- **Pinned Memory:** Ensure any remaining Host-to-Device or Device-to-Host transfers strictly use pinned memory. The current USERPTR buffers stay page-locked for zero-copy.
- **Buffer Reuse:** Most buffers should exist for the duration of the application and be aggressively reused. Pre-allocate to maximum expected sizes when bounded, or use geometric growth via `ensure_capacity()` to minimize cudaMalloc/cudaFree churn. Reduce memory constraints by using single pre-allocated GPU instances instead of runtime allocations.
- **Parallelism & Non-blocking:** Maximize CUDA stream concurrency. Launch preprocessing, inference, and rendering on distinct streams relying on `cudaStreamWaitEvent` instead of CPU sync. Ensure minimal blocking to maximize parallelism structure.
- **Datatypes:** Ensure arrays (like boxes, masks) use minimal data types (e.g. `uint8_t` vs `int`, or bfloat16 for inference) only when strictly necessary and safe for performance.
- If fanout or resize cannot immediately acquire a reusable slot/context, drop that update for the current cycle and retry next cycle. Never stall capture or analysis waiting for it.
- Detection always preprocesses to model resolution from the selected inference region. Crop size never becomes the inference tensor size.
- Any temporary annotate host-mirror D2H copy must use pinned staging buffers and must stay off the display cadence.

## Phase 1

### Goals
- Introduce `LiveSessionController` and move both live predict and live annotate onto it.
- Remove `AnnotationLiveCaptureSession` from the live preview/display path. Annotate preview must consume the controller's GPU output buffers instead of doing device-to-host BGR copies into `AnnotationFrame::pixels_bgr`.
- Keep a temporary controller-backed host mirror for `AnnotationFrame` while the annotate workspace still requires `pixels_bgr` for sampling, assist, export, and save. That mirror is not allowed to own capture or feed preview.
- Keep the existing `frameshow::CaptureSession` as the ingress implementation and remove any duplicate capture logic.
- Implement `LiveVideoIngress` and `LiveFrameFanout` first. Fanout produces separate detect and output bundles, each with its own dimension record.
- Add `UiCropState` and `RuntimeCropState` mailboxes. UI crop is canonical; runtime crop lags if needed. They do not share dimension buffers.
- Change live crop drag/edit handlers so they only mutate `UiCropState`. Runtime workers pull the latest committed UI crop when they can; they do not run synchronously inside ImGui events.
- Switch global `gui.json` persistence debounce from 1000 ms to 200 ms with a single coalesced pending write. Save only on actual state changes and never allow a write backlog.

### Core Files

**Capture / Ingress (frameshow library)**
- `include/frameshow/capture_session.hpp` — public CaptureSession API (start, stop, acquire/release inference and preview frames)
- `include/frameshow/capture_types.hpp` — CaptureConfig, CaptureRegion, InferenceFrameView, PreviewFrameView, GpuFrameBuffer
- `src/frameshow/capture_session_impl.hpp` — Impl struct, InferenceSlotRuntime (line 78-91), PreviewSlotRuntime (line 93-108), state enums InferenceState (line 29-34) and PreviewState (line 36-40), HostBuffer (line 47)
- `src/frameshow/capture_session_core.cpp` — start()/stop(), acquire/release paths, TeardownSession (line 333-339), allocation sequence (line 19-85)
- `src/frameshow/capture_session_device.cpp` — V4L2 USERPTR config, AllocateHostSlots (line 124-142), AllocateInferenceSlots (line 151-182), AllocatePreviewSlots (line 202-242)
- `src/frameshow/capture_session_preview.cpp` — CaptureLoop, ScheduleInferenceCopy (line 279-334), TrySchedulePreviewTap (line 336-387), slot reserve/reclaim logic
- `src/frameshow/capture_session_internal.cpp` — AllocateHostBuffer with pinning (line 48-80), FreeHostBuffer (line 82-91)

**Live Predict (rfdetr library)**
- `include/fastloader/rfdetr/live_predict.h` — LivePredictSession public API, LivePredictOptions (line 31-42), LivePredictStatus (line 97-119), LivePreviewFrame
- `src/rfdetr/inference/live_predict.cpp` — Impl struct (line 632), RenderedPreviewSlot (line 641-655), initialize_rendered_preview_slots (line 868-921), publish_rendered_preview (line 981-1097), thread_main (line 1100-1330), inference region cropping (line 1226-1244)

**Live Annotate (gui)**
- `src/gui/annotation_live_capture.h` — AnnotationLiveCaptureSession class (line 22-47), AnnotationLiveCaptureSnapshot (line 14-20)
- `src/gui/annotation_live_capture.cpp` — worker_main with D2H copy (line 105-222), the cudaMemcpy2D bottleneck (line 173-179), AnnotationFrame population (line 189-199)
- `src/gui/annotation_core.h` — AnnotationFrame struct with pixels_bgr (line 57-68)

**Preview / Display (gui)**
- `src/gui/live_preview_texture.h` — LivePreviewTexture class (line 34-113), double-buffered GL textures, CUDA-GL interop members
- `src/gui/live_preview_texture.cpp` — ensure_live_resources with PBO + CUDA-GL registration (line 345-423), stage_live_preview_copy (line 451-549), finalize_live_preview_copy with glTexSubImage2D (line 551-651), publish_ready_texture front/back swap (line 653-670), submit_host_bgr for static/annotate path (line 207-255)

**Crop State (gui)**
- `src/gui/app.h` — live_crop_layer_state_, live_crop_draft_box_, live_crop_draft_active_ (line 245-247)
- `src/gui/app.cpp` — apply_live_crop_region (line 1964-1994), sync_live_crop_draft (line 1996-1999), clear_live_crop_draft (line 2001-2005), active_live_crop_box (line 2007-2009), draw_crop_overlay (line 2011-2069)
- `src/gui/canvas_layers.h` — RectDragKind enum (line 11-19), RectDragState (line 21-26), RectLayerState (line 63-66), RectLayerFrameResult (line 68-76), CanvasViewport (line 28-35), CanvasPointerState (line 44-50)
- `src/gui/canvas_layers.cpp` — apply_rectangle_drag (line 49-104), rectangle_hover_kind_with_options (line 149-200), update_rect_layers (line 211-275), resolve_video_crop (line 281-300), assign_video_crop_box (line 314-330)
- `src/gui/source_selection.h` — SourceSelectionState crop fields (line 25-28), ResolvedVideoCrop (line 31-35)

**GUI Persistence**
- `src/gui/gui_settings.h` — GuiSettingsPersistence class (line 51-76), kSaveDelay{1000} (line 75)
- `src/gui/gui_settings.cpp` — notify_frame debounce (line 432-448), enqueue_save (line 461-467), writer_main background thread (line 469-495), save_to_disk atomic rename (line 497-510)

**App Integration**
- `src/gui/app.cpp` — start_annotation_live_session (line 1272-1298), poll_annotate_work snapshot consumption (line 1505-1523), submit_annotation_preview with background task (line 1203-1270)

### 1. LiveVideoIngress

Wraps a single `frameshow::CaptureSession`. One dedicated thread (the capture thread inside frameshow) handles V4L2 I/O and pinned H2D upload. LiveVideoIngress owns the session lifetime and publishes full-frame GPU source buffers with frame IDs.

**What exists today:**
- Predict creates its own `CaptureSession` inside `Impl::thread_main` at `live_predict.cpp:1166`.
- Annotate creates a separate `CaptureSession` inside `annotation_live_capture.cpp:114`.
- Both configure independently and cannot share the device.

**What changes:**
- A single `LiveVideoIngress` instance creates and owns the `CaptureSession`.
- Predict and annotate both receive source frames from the same ingress.
- `LiveVideoIngress` exposes `try_acquire_latest_source(SourceFrameView*)` and `release_source(uint32_t)` — thin wrappers around `try_acquire_latest_inference_frame` / `release_inference_frame`.

**Sketch:**
```cpp
// New file: include/fastloader/live/live_video_ingress.h
struct SourceFrameView {
    uint32_t buffer_index = 0;
    uint64_t frame_id = 0;
    CUdeviceptr data = 0;
    size_t pitch_bytes = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    cudaEvent_t ready_event = nullptr;
    LiveCaptureRegion region{};
};

class LiveVideoIngress {
public:
    explicit LiveVideoIngress(frameshow::CaptureConfig config);
    ~LiveVideoIngress();

    void start();
    void stop();

    // Acquires the newest full-frame GPU source buffer.
    // Returns false when no new frame is available.
    bool try_acquire_latest_source(SourceFrameView* out);
    void release_source(uint32_t buffer_index);

    void set_capture_region(const LiveCaptureRegion& region);
    LiveCaptureRegion snapshot_capture_region() const;

private:
    std::unique_ptr<frameshow::CaptureSession> session_;
};
```

The existing buffer model (`InferenceSlotRuntime` at `capture_session_impl.hpp:78-91`) with atomic state machine (`InferenceState` at line 29-34) and per-slot CUDA streams + events stays unchanged. `LiveVideoIngress` is a thin ownership wrapper, not a reimplementation.

### 2. LiveFrameFanout

One dedicated thread waiting on the newest source frame from ingress. It copies into precreated per-frame bundles.

**What exists today:**
- In predict, `thread_main` (line 1187-1298) does: acquire source → crop inference region → run model → publish preview. Source frame, inference input, and rendered output are tangled in one loop.
- Preview slots (`RenderedPreviewSlot` at line 641-655) mix source frame copy with detection drawing in `publish_rendered_preview` (line 981-1097).

**What changes:**
- Fanout splits the source frame into two independent bundles on separate CUDA streams before any analysis or drawing happens.
- `DetectBundle`: the region of the source frame selected for inference, at source resolution. Model preprocessing (resize to model dims) happens later in the analyzer, not here.
- `OutputBundle`: a copy of the full-frame source for compositing. This replaces the source copy in `publish_rendered_preview` (lines 993-1006).

**Sketch:**
```cpp
// New file: include/fastloader/live/live_frame_fanout.h
struct DetectDimensions {
    uint32_t width = 0;
    uint32_t height = 0;
    size_t pitch_bytes = 0;
};

struct OutputDimensions {
    uint32_t width = 0;
    uint32_t height = 0;
    size_t pitch_bytes = 0;
};

struct DetectBundle {
    uint32_t slot_index = 0;
    uint64_t frame_id = 0;
    CUdeviceptr data = 0;           // Inference region at source resolution
    DetectDimensions dims{};
    cudaEvent_t ready_event = nullptr;
    cudaStream_t stream = nullptr;
    LiveCaptureRegion region{};     // Absolute coordinates of inference region
};

struct OutputBundle {
    uint32_t slot_index = 0;
    uint64_t frame_id = 0;
    CUdeviceptr data = 0;           // Full-frame output base
    OutputDimensions dims{};
    cudaEvent_t ready_event = nullptr;
    cudaStream_t stream = nullptr;
    LiveCaptureRegion region{};     // Full capture region
};

class LiveFrameFanout {
public:
    LiveFrameFanout(LiveVideoIngress& ingress,
                    const UiCropState& ui_crop_state,
                    uint32_t detect_slot_count,
                    uint32_t output_slot_count);
    ~LiveFrameFanout();

    void start();
    void stop();

    // Called by analyzer thread.
    bool try_acquire_detect(DetectBundle* out);
    void release_detect(uint32_t slot_index);

    // Called by compositor thread.
    bool try_acquire_output(OutputBundle* out);
    void release_output(uint32_t slot_index);

private:
    void fanout_thread_main();
    void drain_pending_source_releases(bool wait);

    LiveVideoIngress& ingress_;
    const UiCropState& ui_crop_state_;
    std::thread thread_;
    std::atomic<bool> stop_requested_{false};

    // Detect slots — preallocated pitched GPU buffers
    // (mirrors the pattern from InferenceSlotRuntime at capture_session_impl.hpp:78)
    struct DetectSlot { /* CUdeviceptr, pitch, stream, event, atomic state */ };
    std::vector<std::unique_ptr<DetectSlot>> detect_slots_;

    // Output slots — preallocated pitched GPU buffers
    // (mirrors RenderedPreviewSlot at live_predict.cpp:641, but without drawing)
    struct OutputSlot { /* CUdeviceptr, pitch, stream, event, atomic state */ };
    std::vector<std::unique_ptr<OutputSlot>> output_slots_;

    // Runtime crop mailbox (lock-free)
    // Avoid CPU mutexes on the hot frame-processing path.
    // Updated via immutable snapshot + atomic shared_ptr publish.
    RuntimeCropState runtime_crop_;

    // Deferred source release — ingress buffers are returned only after all
    // detect/output consumers have finished reading the source slot.
    struct PendingSourceRelease {
        uint32_t buffer_index = 0;
        cudaEvent_t ready_event = nullptr;
    };
    cudaStream_t release_stream_ = nullptr;
    std::vector<PendingSourceRelease> pending_source_releases_;
};
```

**Fanout thread loop (pseudocode):**
```cpp
void LiveFrameFanout::fanout_thread_main() {
    while (!stop_requested_.load(std::memory_order_acquire)) {
        drain_pending_source_releases(false);

        SourceFrameView source{};
        if (!ingress_.try_acquire_latest_source(&source)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // 1. Read current runtime crop (lock-free)
        // No CPU mutexes here to avoid stalling capture on UI interactions.
        runtime_crop_.sync_from(ui_crop_state_);
        const auto snap = runtime_crop_.snapshot();
        const LiveCaptureRegion detect_region = snap.has_crop ? snap.region : source.region;

        // 2. Reserve detect slot — drop frame if none free
        DetectSlot* d = reserve_detect_slot();
        if (d) {
            cudaStreamWaitEvent(d->stream, source.ready_event, 0);
            // D2D copy of inference region from source
            // Uses pointer arithmetic like live_predict.cpp:1239-1241
            auto* src = reinterpret_cast<uint8_t*>(source.data)
                + (detect_region.y - source.region.y) * source.pitch_bytes
                + (detect_region.x - source.region.x) * 3U;
            cudaMemcpy2DAsync(d->device_ptr, d->pitch_bytes,
                              src, source.pitch_bytes,
                              detect_region.width * 3U, detect_region.height,
                              cudaMemcpyDeviceToDevice, d->stream);
            cudaEventRecord(d->ready_event, d->stream);
            publish_detect(d, source.frame_id, detect_region);
        }

        // 3. Reserve output slot — drop frame if none free
        OutputSlot* o = reserve_output_slot();
        if (o) {
            cudaStreamWaitEvent(o->stream, source.ready_event, 0);
            // D2D copy of full source frame
            // Same pattern as publish_rendered_preview lines 993-1006
            cudaMemcpy2DAsync(o->device_ptr, o->pitch_bytes,
                              source.data, source.pitch_bytes,
                              source.region.width * 3U, source.region.height,
                              cudaMemcpyDeviceToDevice, o->stream);
            cudaEventRecord(o->ready_event, o->stream);
            publish_output(o, source.frame_id, source.region);
        }

        // 4. Release source frame back to ingress only after every consumer
        // stream that touched it is done reading.
        if (d != nullptr || o != nullptr) {
            cudaEvent_t release_ready = acquire_release_event();
            if (d != nullptr) {
                cudaStreamWaitEvent(release_stream_, d->ready_event, 0);
            }
            if (o != nullptr) {
                cudaStreamWaitEvent(release_stream_, o->ready_event, 0);
            }
            cudaEventRecord(release_ready, release_stream_);
            pending_source_releases_.push_back(
                PendingSourceRelease{source.buffer_index, release_ready});
        } else {
            ingress_.release_source(source.buffer_index);
        }
    }

    drain_pending_source_releases(true);
}
```

**Buffer allocation pattern:** Follow `initialize_rendered_preview_slots` (line 868-921) — `cudaMallocPitch` + `cuda_stream_create_with_highest_priority` + `cudaEventCreateWithFlags(cudaEventDisableTiming)` per slot. Exception-safe cleanup on allocation failure.

**Slot state machine:** Reuse the same atomic-state pattern from `capture_session_impl.hpp:29-34`:
```cpp
enum class SlotState : uint32_t { kFree = 0, kWriting = 1, kPublished = 2, kAcquired = 3 };
```
Reserve with `compare_exchange_strong`, publish with `store(release)`, reclaim by checking `cudaStreamQuery` or `cudaEventQuery` (same as `ReclaimStaleInferenceSlots` in `capture_session_preview.cpp:120-191`).

### 3. LiveSessionController

Owns ingress, fanout, and the lifecycle of the full pipeline. Both predict and annotate modes create a controller instead of their own capture sessions.

**What exists today:**
- Predict: `App` creates `LivePredictSession` (app.cpp:1750-1779), which internally owns capture + model + preview.
- Annotate: `App` creates `AnnotationLiveCaptureSession` (app.cpp:1272-1284), which internally owns a separate capture session and does D2H copies.

**What changes:**
- `LiveSessionController` owns `LiveVideoIngress` + `LiveFrameFanout`.
- In Phase 1, analyzer and compositor are not yet integrated — the controller exposes output bundles directly for preview.
- `App` creates one controller. Predict mode attaches an analyzer (Phase 2). Annotate mode uses it with no analyzer.
- `AnnotationLiveCaptureSession` is removed from preview ownership. Annotate preview comes from controller's output bundles via `LivePreviewTexture`, while any temporary host mirror remains off the display path.

**Sketch:**
```cpp
// New file: include/fastloader/live/live_session_controller.h
struct LiveSessionConfig {
    frameshow::CaptureConfig capture{};
    uint32_t detect_slot_count = 2;
    uint32_t output_slot_count = 2;
    int cuda_device_index = 0;
};

class LiveSessionController {
public:
    explicit LiveSessionController(LiveSessionConfig config);
    ~LiveSessionController();

    void start();
    void stop();
    bool running() const;

    // Output bundle acquisition for preview/display.
    // LivePreviewTexture calls these instead of LivePredictSession methods.
    bool try_acquire_latest_output(OutputBundle* out);
    void release_output(uint32_t slot_index);

    // Detect bundle acquisition for analyzer (Phase 2).
    bool try_acquire_latest_detect(DetectBundle* out);
    void release_detect(uint32_t slot_index);

    // Crop mailbox owned by the controller and shared with fanout.
    UiCropState& ui_crop_state();
    const UiCropState& ui_crop_state() const;

    LiveVideoIngress& ingress();
    LiveFrameFanout& fanout();

private:
    LiveSessionConfig config_;
    UiCropState ui_crop_state_;
    std::unique_ptr<LiveVideoIngress> ingress_;
    std::unique_ptr<LiveFrameFanout> fanout_;
};
```

### 4. Remove AnnotationLiveCaptureSession from the Live Preview Path

**The bottleneck to eliminate from preview/display:**
`annotation_live_capture.cpp:162-179` — every frame does a synchronous `cudaEventSynchronize` then `cudaMemcpy2D(DeviceToHost)` into a heap-allocated `std::vector<uint8_t> pixels_bgr`. This blocks the worker thread for the entire transfer and produces a CPU-side `AnnotationFrame` that then gets re-uploaded to GPU via `submit_host_bgr` (live_preview_texture.cpp:207-255).

```cpp
// annotation_live_capture.cpp:162-179 — THIS GOES AWAY
std::vector<std::uint8_t> pixels_bgr(width * height * 3U, 0U);
cudaEventSynchronize(reinterpret_cast<cudaEvent_t>(view.buffer.ready_event));
cudaMemcpy2D(pixels_bgr.data(),
             width * 3U,
             reinterpret_cast<const void*>(view.buffer.data),
             view.buffer.pitch_bytes,
             width * 3U, height,
             cudaMemcpyDeviceToHost);
```

**Current annotate display path:**
1. `AnnotationLiveCaptureSession::worker_main` — D2H copy → `AnnotationFrame::pixels_bgr` (line 162-199)
2. `App::poll_annotate_work` — snapshot → `load_annotation_frame` (line 1505-1523)
3. `App::submit_annotation_preview` — `build_annotation_preview` on CPU → `submit_host_bgr` H2D back to GPU (line 1203-1270)

**New annotate display path:**
1. `LiveSessionController` output bundle already has the frame on GPU.
2. `LivePreviewTexture::begin_live_stream` points at the controller instead of `LivePredictSession`.
3. `stage_live_preview_copy` (line 451-549) does D2D from output bundle → PBO, same as predict.
4. No D2H copy, no `pixels_bgr`, no `submit_host_bgr`.

**Compatibility path for CPU-only annotate consumers (Phase 1-2):**
- Keep a controller-backed host mirror for `AnnotationFrame` while `draw_annotate_workspace`, `sample_annotation_hsv`, assist, and save/export still depend on `pixels_bgr`.
- That mirror is best-effort, may lag display, and uses pinned staging buffers for D2H.
- The mirror never owns a `CaptureSession`, never drives preview, and never feeds `LivePreviewTexture`.

**Files to repurpose in Phase 1 and delete only after CPU-only consumers are moved off the live cadence:**
- `src/gui/annotation_live_capture.h` — stop owning `CaptureSession`; repurpose as a controller-backed `AnnotationFrame` mirror or replace with a new host-mirror helper
- `src/gui/annotation_live_capture.cpp` — remove duplicate capture ownership and preview responsibility; keep only best-effort host-mirror logic if still needed
- `src/gui/app.cpp` — remove duplicate live preview ownership from `start_annotation_live_session` / `stop_annotation_live_session`; keep `poll_annotate_work` only as a host-mirror consumer until annotate no longer needs continuous host snapshots

**`LivePreviewTexture` adaptation:**
Currently `begin_live_stream` takes a `LivePredictSession&` (live_preview_texture.h:41). Change it to accept `LiveSessionController&` instead. The `pump()` method acquires output bundles from the controller rather than calling `live_predict_session->try_acquire_latest_preview()`. The CUDA-GL interop path (`stage_live_preview_copy` → `finalize_live_preview_copy`) stays identical — only the source of the GPU buffer changes.

### 5. UiCropState and RuntimeCropState Mailboxes

**What exists today:**
- Predict crop: `App::apply_live_crop_region` (line 1964-1994) directly calls `live_predict_session_->set_capture_region()` and `set_inference_region()` from the ImGui thread. Inside the worker, `thread_main` locks a mutex to read `inference_region_` (line 1228-1244).
- Annotate crop: `AnnotationLiveCaptureSession::update_preview_region` stores pending region under mutex (line 85-102), worker checks `preview_region_pending_` each cycle (line 129-147).

Both approaches couple UI mutation timing to worker consumption. The action plan requires decoupling: UI writes are canonical and instant, runtime reads are best-effort and never block the UI.

**Sketch:**
```cpp
// New file: include/fastloader/live/crop_state.h

struct UiCropSnapshot {
    bool has_crop = false;
    LiveCaptureRegion region{};
    uint64_t generation = 0;
};

// Written only from the ImGui/UI thread. Read by runtime threads lock-free.
// Uses immutable snapshots published via std::atomic_load/store on shared_ptr,
// avoiding data races on the region fields.
class UiCropState {
public:
    void set(const LiveCaptureRegion& region) {
        publish(true, region);
    }

    void clear() {
        publish(false, LiveCaptureRegion{});
    }

    std::shared_ptr<const UiCropSnapshot> snapshot() const {
        return std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
    }

private:
    void publish(bool has_crop, const LiveCaptureRegion& region) {
        const auto current =
            std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
        auto next = std::make_shared<UiCropSnapshot>();
        next->has_crop = has_crop;
        next->region = region;
        next->generation = current ? current->generation + 1 : 1;
        std::shared_ptr<const UiCropSnapshot> immutable = next;
        std::atomic_store_explicit(&snapshot_, std::move(immutable),
                                   std::memory_order_release);
    }

    std::shared_ptr<const UiCropSnapshot> snapshot_ =
        std::make_shared<UiCropSnapshot>();
};

// Owned by fanout thread. Updated from UiCropState at the start
// of each fanout cycle. Never shared with UI — no contention after snapshot.
struct RuntimeCropState {
    bool has_crop = false;
    LiveCaptureRegion region{};
    uint64_t last_ui_generation = 0;

    // Pull latest from UI if generation advanced.
    void sync_from(const UiCropState& ui) {
        auto snap = ui.snapshot();
        if (snap && snap->generation != last_ui_generation) {
            has_crop = snap->has_crop;
            region = snap->region;
            last_ui_generation = snap->generation;
        }
    }
    
    // Helper to read current state trivially lock-free
    UiCropSnapshot snapshot() const {
        return {has_crop, region, last_ui_generation};
    }
};
```

**App-side changes:**
- `draw_crop_overlay` (line 2011-2069) and `apply_live_crop_region` (line 1964-1994) write to `controller->ui_crop_state()` instead of calling `live_predict_session_->set_inference_region()`.
- The fanout thread calls `runtime_crop_.sync_from(controller.ui_crop_state())` at the top of each cycle. No mutex is taken on the hot path; readers do one atomic shared_ptr load and copy immutable data.

### 6. GUI Persistence Debounce Change

**Current implementation:**
`gui_settings.h:75` — `static constexpr std::chrono::milliseconds kSaveDelay{1000};`

`notify_frame` (gui_settings.cpp:432-448) checks every frame:
```cpp
if (current != last_saved_) {     // Line 434 — detect change
    dirty_ = true;
    last_change_time_ = now;
    last_saved_ = current;        // Line 439 — update cached state
}
if (dirty_ && elapsed >= kSaveDelay) {
    enqueue_save(last_saved_);    // Line 444 — fire write
    dirty_ = false;
}
```

**Changes:**
1. Reduce `kSaveDelay` from 1000 ms to 200 ms at `gui_settings.h:75`.
2. The existing architecture already handles coalescing correctly: `enqueue_save` replaces `pending_save_` if a previous save hasn't started yet (line 464: `pending_save_ = std::move(j)`). The writer thread only picks up the latest (line 481). No write backlog can form.
3. Add a guard in `notify_frame`: if `save_in_flight_` is true and `pending_save_` already has a value, skip the enqueue. This prevents queueing a second save while one is already pending+in-flight, even at 200 ms cadence. The existing code nearly does this already — the `pending_save_` assignment in `enqueue_save` overwrites any unstarted pending, so the only risk is unnecessary mutex contention. The guard makes the intent explicit.

```cpp
// gui_settings.h — change:
static constexpr std::chrono::milliseconds kSaveDelay{200};

// gui_settings.cpp — notify_frame, add early-out before enqueue:
if (dirty_) {
    const auto elapsed = std::chrono::steady_clock::now() - last_change_time_;
    if (elapsed >= kSaveDelay) {
        enqueue_save(last_saved_);
        dirty_ = false;
    }
}
```

### 7. LiveFrameId

**What exists today:**
- frameshow uses `uint64_t frame_id` as a monotonic counter (`next_frame_id_++` at `capture_session_preview.cpp:296`).
- `LivePredictSession` carries `frame_id` through `InferenceFrameView`, `RenderedPreviewSlot`, and `LivePreviewFrame`.
- Annotate carries `frame_id` through `AnnotationFrame::frame_id` (annotation_core.h:60).

**What changes:**
Add a session nonce so frame IDs from different controller lifetimes never collide. This is critical for Phase 2 save gating (frame/result mismatch detection).

```cpp
struct LiveFrameId {
    uint64_t session_nonce = 0;  // Random per-controller-start
    uint64_t sequence = 0;       // Monotonic per session

    bool operator==(const LiveFrameId& o) const {
        return session_nonce == o.session_nonce && sequence == o.sequence;
    }
    bool operator!=(const LiveFrameId& o) const { return !(*this == o); }
};
```

Replace bare `uint64_t frame_id` in `SourceFrameView`, `DetectBundle`, `OutputBundle`, and the compositor output with `LiveFrameId`. The ingress thread assigns `{nonce, next_seq++}` to each source frame.

### Phase 1 Implementation Order

1. **LiveFrameId** — standalone type, no dependencies.
2. **UiCropState / RuntimeCropState** — standalone types, no dependencies.
3. **LiveVideoIngress** — wraps existing `frameshow::CaptureSession`, uses LiveFrameId.
4. **LiveFrameFanout** — depends on ingress and crop state. Preallocate detect + output GPU bundles using the `cudaMallocPitch` + stream + event pattern from `initialize_rendered_preview_slots` (line 868-921).
5. **LiveSessionController** — owns ingress + fanout. Expose acquire/release for output bundles.
6. **Adapt LivePreviewTexture** — accept `LiveSessionController&` instead of `LivePredictSession&`. The CUDA-GL interop path in `stage_live_preview_copy` (line 451-549) stays the same; only the frame source changes from `LivePreviewFrame` to `OutputBundle`.
7. **Wire annotate** — `App::start_annotation_live_session` creates a `LiveSessionController` and calls `live_preview_texture_->begin_live_stream(controller)`. Remove `AnnotationLiveCaptureSession` from preview ownership and remove `submit_host_bgr` from the live display path. Keep any temporary controller-backed host mirror isolated from preview/display.
8. **Wire predict** — `LivePredictSession::Impl::thread_main` receives detect bundles from the controller's fanout instead of acquiring from its own `CaptureSession`. In Phase 1, `publish_rendered_preview` may remain as a temporary compatibility renderer over the controller-provided output base. That temporary in-place path is deleted in Phase 2 when `AnalysisOverlayImage` lands.
9. **Crop wiring** — `draw_crop_overlay` and `apply_live_crop_region` write to `controller->ui_crop_state()`. Fanout reads `RuntimeCropState::sync_from()` each cycle.
10. **GUI debounce** — change `kSaveDelay` from 1000 to 200 at `gui_settings.h:75`.

## Phase 2

### Goals
- Add the optional `FrameAnalyzer` interface so predict and annotate can optionally attach a model without the session requiring one.
- Predict binds RF-DETR by default. Annotate binds no analyzer by default and stays fully usable for manual workflows.
- Implement analyzer output slots carrying `LiveFrameId`, GPU detection tensors, and a separate RGBA `AnalysisOverlayImage`.
- Keep analysis results on GPU unless a specific save/export path needs a host copy.
- Add save gating for model-backed flows: only save analyzer-derived results when the output frame ID matches the analyzer result frame ID exactly. Fail loudly on mismatch.
- Remove obsolete live preview/output code paths once predict and annotate both read from the shared controller.

### Core Files

**Model Runner Abstraction (rfdetr library)**
- `src/rfdetr/inference/live_predict.cpp` — `LiveModelRunner` base class (line 147-155), `LiveWeightsRunner` (line 250-402), `LiveBackendRunner` (line 404-559), `LiveFrameRenderData` (line 133-137), `LiveSplitRenderData` (line 125-131)
- `src/rfdetr/inference/live_predict.cpp` — `LiveOverlaySelection` struct (line 157-162), `select_live_overlay_for_preview` (line 164-248)
- `src/rfdetr/inference/live_predict.cpp` — `publish_rendered_preview` (line 981-1097), `thread_main` inference loop (line 1187-1298)
- `src/rfdetr/inference/live_predict.cpp` — `RenderedPreviewSlot` struct (line 641-655), `RenderedPreviewState` enum (line 634-639), `reserve_rendered_preview_slot_locked` (line 948-979)
- `include/fastloader/rfdetr/live_predict.h` — `LivePredictSession` public API (line 123-148), `LivePredictStatus` (line 97-119), `LivePredictionFrame` (line 89-95), `LiveSplitPrediction` (line 83-87)
- `include/fastloader/rfdetr/live_predict.h` — `LivePreviewFrame` (line 74-81), `LivePreviewBuffer` (line 64-72)

**Inference Pipeline (rfdetr library)**
- `include/fastloader/rfdetr/model.h` — `NativeRfDetrModel` class (line 149-193), `ModelOutputs` struct
- `src/rfdetr/inference/predict_runtime_internal.h` — `load_native_model` (line 114-127)
- `include/fastloader/rfdetr/backends.h` — `InferenceBackend` interface (line 15-23)
- `include/fastloader/rfdetr/detection_types.h` — `OutputLayer` (line 44-61), `ModelOutputs`
- `src/rfdetr/postprocess.h` — `OutputTensors`, `postprocess_outputs_fixed_size`
- `include/fastloader/rfdetr/evaluation.h` — `Prediction` struct (line 18-25)

**Drawing / Overlay (rfdetr CUDA)**
- `include/fastloader/rfdetr/draw_cuda.h` — kernel launch declarations (line 1-86): `launch_draw_boxes_labels_bgr_pitched` (line 53-64), `launch_draw_masks_boxes_labels_bgr_pitched` (line 68-81), `launch_build_instance_colors_from_zero_based_labels` (line 43-49)
- `src/rfdetr/cuda/draw_cuda.cu` — `draw_masks_boxes_labels_bgr_pitched_kernel` (line 268-319), mask alpha blending formula (line 292-297), `draw_boxes_labels_bgr_pitched_kernel` (line 225-266), `build_instance_colors_from_labels_kernel` (line 61-105)

**Preprocessing (rfdetr CUDA)**
- `src/rfdetr/live_preprocess.h` — `launch_bgr_split_to_planar_float` declaration
- `src/rfdetr/cuda/live_preprocess.cu` — affine warp and color space kernels

**Preview Display (gui)**
- `src/gui/live_preview_texture.h` — `LivePreviewTexture` class (line 34-113), `PendingLiveFrame` (line 66-69), `live_session_` member typed as `LivePredictSession*` (line 97)
- `src/gui/live_preview_texture.cpp` — `stage_live_preview_copy` (line 451-549), `finalize_live_preview_copy` (line 551-651), CUDA-GL interop with `cudaGraphicsMapResources` (line 481)

**Annotation Save / Frame Gating (gui)**
- `src/gui/annotation_core.h` — `AnnotationFrame` struct with `frame_id` (line 57-68), `AnnotationInstance` with `seed_frame_id` (line 77-88), `AnnotationSaveConfig` (line 107-110)
- `src/gui/annotation_core.cpp` — `effective_seed_mask` with live-mode frame ID gating (line 226-250, guard at line 233), `save_annotation_scene` (line 804-902)
- `src/gui/app.cpp` — hold-save overflow mismatch detection (line 1482-1503, comparison at line 1488), `poll_annotate_work` frame ID change detection (line 1505-1523, comparison at line 1513)

**App Integration (gui)**
- `src/gui/app.h` — `annotate_frame_`, `annotate_queued_save_frame_`, `annotate_last_saved_frame_id_` (line 184-218)
- `src/gui/app.cpp` — `start_annotation_live_session` (line 1272-1298), live predict startup (line 1750-1779)

### 1. FrameAnalyzer Interface

Extracts the abstract model-runner contract out of `LivePredictSession::Impl` and behind a standalone interface that the controller can optionally hold. Predict creates an analyzer wrapping the existing `LiveModelRunner` + preprocessing. Annotate creates no analyzer. Assist/model-backed annotate flows attach one explicitly.

**What exists today:**
- `LiveModelRunner` (line 147-155) is an internal base class inside `live_predict.cpp` with two concrete implementations: `LiveWeightsRunner` (line 250-402) and `LiveBackendRunner` (line 404-559).
- Both runners accept raw `LiveFrameInputs` (BGR pitched device memory, line 114-123), run preprocessing (`launch_bgr_split_to_planar_float` at line 329-346), model forward pass (`model_->forward` at line 348-350 or `backend_->run`), postprocessing (`postprocess_outputs_fixed_size` at line 351-355), and overlay selection (`select_live_overlay_for_preview` at line 367-375) — all in a single `run_frame` call.
- `run_frame` returns `LiveFrameRenderData` (line 133-137) containing per-split render data (boxes, labels, colors, masks as GPU tensors) plus a `ready_event` and `producer_stream`. These tensors are then consumed by `publish_rendered_preview` (line 981-1097) which draws them directly onto the output BGR image in-place.
- The runner is created and owned inside `thread_main` (line 1154-1164), tightly coupled to `LivePredictSession::Impl`.

**What changes:**
- `FrameAnalyzer` becomes a public interface in the `live/` module. It receives a `DetectBundle` (from Phase 1 fanout), runs model inference, and publishes an `AnalyzerResult` containing GPU-resident detection tensors + `LiveFrameId`.
- The existing `LiveWeightsRunner` and `LiveBackendRunner` are wrapped by a concrete `RfDetrFrameAnalyzer` that adapts from `DetectBundle` → `LiveFrameInputs`-shaped preprocessing → `run_frame` → `AnalyzerResult`.
- `publish_rendered_preview` is no longer called from within the inference loop. Instead, the `AnalyzerResult` is published to an analyzer output slot, and the compositor (Phase 3) or a simplified Phase 2 preview renderer reads it.

**Sketch:**
```cpp
// New file: include/fastloader/live/frame_analyzer.h

struct AnalyzerResult {
    LiveFrameId frame_id{};
    cudaEvent_t ready_event = nullptr;
    cudaStream_t producer_stream = nullptr;

    // Per-split detection data — GPU-resident tensors.
    // Mirrors LiveSplitRenderData from live_predict.cpp:125-131
    // but keyed by LiveFrameId instead of bare uint64_t.
    struct SplitResult {
        LiveCaptureRegion source_region{};
        torch::Tensor boxes_xyxy;       // [N, 4] float32, XYXY format
        torch::Tensor labels_zero_based; // [N] int32
        torch::Tensor scores;           // [N] float32 — kept for save gating
        torch::Tensor colors_rgb;       // [N, 3] uint8
        torch::Tensor masks;            // [N, H, W] bool (optional)
        std::vector<Prediction> detections; // CPU-side, only when save is needed
    };
    std::vector<SplitResult> splits;
};

class FrameAnalyzer {
public:
    virtual ~FrameAnalyzer() = default;

    // Analyze a detect bundle. Called from the analyzer worker thread.
    // The implementation must:
    //   1. cudaStreamWaitEvent on bundle.ready_event
    //   2. Preprocess (resize to model resolution, normalize)
    //   3. Run inference
    //   4. Postprocess and select overlay
    //   5. cudaEventRecord on result.ready_event
    //   6. Return AnalyzerResult with GPU-resident tensors
    virtual AnalyzerResult analyze(const DetectBundle& bundle) = 0;

    // Model metadata for status reporting.
    virtual std::string backend_name() const = 0;
    virtual uint32_t model_resolution() const = 0;
    virtual int num_classes() const = 0;
};
```

**RfDetrFrameAnalyzer — wraps existing runners:**
```cpp
// New file: src/rfdetr/inference/rfdetr_frame_analyzer.h

class RfDetrFrameAnalyzer final : public FrameAnalyzer {
public:
    struct Options {
        LivePredictOptions predict_opts;   // model path, backend, threshold, etc.
        ResolvedModelArtifacts artifacts;
    };

    explicit RfDetrFrameAnalyzer(Options options);
    ~RfDetrFrameAnalyzer() override;

    AnalyzerResult analyze(const DetectBundle& bundle) override;
    std::string backend_name() const override;
    uint32_t model_resolution() const override;
    int num_classes() const override;

private:
    // Adapts DetectBundle to LiveFrameInputs for the existing runner.
    // DetectBundle has the inference region at source resolution.
    // The runner preprocesses (resize to model dims, normalize) internally.
    LiveFrameInputs adapt_bundle(const DetectBundle& bundle);

    // Converts LiveFrameRenderData → AnalyzerResult
    // Carries LiveFrameId through instead of bare uint64_t.
    AnalyzerResult convert_render_data(const DetectBundle& bundle,
                                       LiveFrameRenderData&& render_data);

    Options options_;
    std::unique_ptr<LiveModelRunner> runner_; // LiveWeightsRunner or LiveBackendRunner
};
```

**Adaptation from DetectBundle → LiveFrameInputs:**

The `DetectBundle` from Phase 1 carries the inference region at source resolution on GPU. The existing `LiveWeightsRunner::run_frame` (line 284-388) expects `LiveFrameInputs` (line 114-123) with `data`, `pitch_bytes`, and `region`. The adapter maps directly:

```cpp
LiveFrameInputs RfDetrFrameAnalyzer::adapt_bundle(const DetectBundle& bundle) {
    return LiveFrameInputs{
        .frame_id = bundle.frame_id.sequence,  // Compat with existing runner
        .capture_ns = 0,
        .ready_ns = 0,
        .ready_event = bundle.ready_event,
        .short_frame = false,
        .data = reinterpret_cast<const std::uint8_t*>(bundle.data),
        .pitch_bytes = bundle.dims.pitch_bytes,
        .region = bundle.region,
    };
}
```

The runner then:
1. Builds horizontal splits — `build_horizontal_splits` at line 289
2. For each split, does `launch_bgr_split_to_planar_float` to resize + convert to planar float — line 329-346
3. Normalizes: `input_gpu_.sub_(mean_).div_(std_)` — line 347
4. Runs model forward: `model_->forward(NestedTensor{input_gpu_, nested_mask_})` — line 348-350
5. Postprocesses: `postprocess_outputs_fixed_size()` — line 351-355
6. Selects overlay: `select_live_overlay_for_preview()` — line 367-375

All of this stays unchanged. The only difference is that the result is returned as an `AnalyzerResult` instead of being drawn in-place by `publish_rendered_preview`.

### 2. LiveAnalyzerWorker

One dedicated thread that waits on `DetectBundle` from fanout, runs the analyzer, and publishes `AnalyzerResult` into preallocated output slots.

**What exists today:**
- `thread_main` (line 1187-1298) does everything in one loop: acquire source → crop inference region → run model → draw overlay onto output → publish preview. The inference result lifetime is scoped to a single loop iteration, and detection tensors are retained in `RenderedPreviewSlot::retained_tensors` (line 1066-1071) until the slot is recycled.

**What changes:**
- The analyzer runs on its own thread and publishes to its own result-slot ring plus its own overlay-slot ring, decoupled from frame delivery and preview rendering.
- Analyzer results persist independently of output/preview slots. The compositor reads the latest available result, which may lag behind the frame cadence.
- When no analyzer is attached, the worker thread either doesn't start or runs as a no-op pass-through.

**Sketch:**
```cpp
// New file: include/fastloader/live/live_analyzer_worker.h

struct AnalyzerSlot {
    uint32_t slot_index = 0;
    AnalyzerResult result{};
    std::vector<torch::Tensor> retained_tensors; // Prevent dealloc during GPU use
    std::atomic<uint32_t> state{0}; // SlotState: kFree/kWriting/kPublished/kAcquired
};

class LiveAnalyzerWorker {
public:
    LiveAnalyzerWorker(LiveFrameFanout& fanout,
                       std::unique_ptr<FrameAnalyzer> analyzer,
                       uint32_t result_slot_count);
    ~LiveAnalyzerWorker();

    void start();
    void stop();

    struct AnalyzerResultView {
        uint32_t slot_index = 0;
        const AnalyzerResult* result = nullptr;
    };

    // Called by save paths or UI status consumers.
    // Returns a slot-backed view; caller must release_result(slot_index).
    bool try_acquire_latest_result(AnalyzerResultView* out);
    void release_result(uint32_t slot_index);

    struct AnalysisOverlayView {
        uint32_t slot_index = 0;
        LiveFrameId frame_id{};
        CUdeviceptr data = 0;
        std::size_t pitch_bytes = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        cudaEvent_t ready_event = nullptr;
        bool has_content = false;
    };

    // Called by compositor / preview renderer.
    // Returns a slot-backed view; caller must release_overlay(slot_index).
    bool try_acquire_latest_overlay(AnalysisOverlayView* out);
    void release_overlay(uint32_t slot_index);

    // Status for UI reporting.
    struct Status {
        bool running = false;
        bool model_hot = false;
        uint64_t frames_analyzed = 0;
        uint64_t frames_skipped = 0;
        LiveFrameId last_completed_frame_id{};
        double last_latency_ms = 0.0;
        std::string backend_name;
        std::string last_error;
    };
    Status snapshot_status() const;

    // Hot-swap analyzer (e.g., switch models without restarting session).
    // nullptr means detach — worker becomes no-op.
    void set_analyzer(std::unique_ptr<FrameAnalyzer> analyzer);

private:
    void worker_thread_main();
    struct OverlaySlot {
        uint32_t slot_index = 0;
        /* AnalysisOverlayImage image + SlotState state */
    };

    LiveFrameFanout& fanout_;
    // Lock-free analyzer ownership: exchanged via atomic_load/store
    // or std::atomic<std::shared_ptr<FrameAnalyzer>> to avoid analyzer_mutex_
    std::shared_ptr<FrameAnalyzer> analyzer_; 
    
    std::vector<std::unique_ptr<AnalyzerSlot>> result_slots_;
    std::atomic<int> latest_result_index_{-1};
    std::vector<std::unique_ptr<OverlaySlot>> overlay_slots_;
    std::atomic<int> latest_overlay_index_{-1};
    std::atomic<bool> stop_requested_{false};
    std::thread thread_;
    
    // Lock-free status publish via atomic double buffering
    Status status_buffers_[2];
    std::atomic<uint32_t> status_index_{0};
};
```

**Worker thread loop (pseudocode):**
```cpp
void LiveAnalyzerWorker::worker_thread_main() {
    while (!stop_requested_.load(std::memory_order_acquire)) {
        DetectBundle bundle{};
        if (!fanout_.try_acquire_detect(&bundle)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // Lock-free hot-swap read
        std::shared_ptr<FrameAnalyzer> analyzer = std::atomic_load(&analyzer_);

        if (!analyzer) {
            // No analyzer attached — release detect bundle, skip.
            fanout_.release_detect(bundle.slot_index);
            continue;
        }

        const auto started_at = Clock::now();
        AnalyzerResult result;
        try {
            result = analyzer->analyze(bundle);
        } catch (const std::exception& e) {
            fanout_.release_detect(bundle.slot_index);
            uint32_t old_idx = status_index_.load(std::memory_order_relaxed);
            uint32_t new_idx = 1 - old_idx;
            status_buffers_[new_idx] = status_buffers_[old_idx];
            status_buffers_[new_idx].last_error = e.what();
            status_index_.store(new_idx, std::memory_order_release);
            continue;
        }

        // Publish result into a slot.
        // Same pattern as reserve_rendered_preview_slot_locked (line 948-979):
        // scan for kFree or stale kPublished, check cudaEventQuery on ready_event,
        // transition to kWriting, fill, transition to kPublished.
        AnalyzerSlot* slot = reserve_result_slot();
        if (slot != nullptr) {
            slot->result = std::move(result);
            // Retain detection tensors to prevent GPU dealloc —
            // same as RenderedPreviewSlot::retained_tensors (line 1066-1071)
            slot->retained_tensors.clear();
            for (auto& split : slot->result.splits) {
                if (split.boxes_xyxy.defined()) slot->retained_tensors.push_back(split.boxes_xyxy);
                if (split.labels_zero_based.defined()) slot->retained_tensors.push_back(split.labels_zero_based);
                if (split.colors_rgb.defined()) slot->retained_tensors.push_back(split.colors_rgb);
                if (split.masks.defined()) slot->retained_tensors.push_back(split.masks);
                if (split.scores.defined()) slot->retained_tensors.push_back(split.scores);
            }
            slot->state.store(static_cast<uint32_t>(SlotState::kPublished),
                              std::memory_order_release);
            latest_result_index_.store(static_cast<int>(slot->slot_index),
                                       std::memory_order_release);
        }

        // Render / publish the matching RGBA analysis overlay into a separate
        // overlay slot before releasing the detect bundle. The overlay slot
        // uses the same acquire/release pattern as result slots.

        // Deferred release of detect bundle — same pattern as
        // live_predict.cpp:1250-1266 (record event on producer stream,
        // release only after event fires).
        fanout_.release_detect(bundle.slot_index);

        const double latency_ms = ms_since(started_at);
        // Lock-free status publish using double buffering
        uint32_t old_idx = status_index_.load(std::memory_order_relaxed);
        uint32_t new_idx = 1 - old_idx;
        status_buffers_[new_idx] = status_buffers_[old_idx]; // Copy previous
        status_buffers_[new_idx].frames_analyzed += 1;
        status_buffers_[new_idx].last_completed_frame_id = result.frame_id;
        status_buffers_[new_idx].last_latency_ms = latency_ms;
        status_buffers_[new_idx].last_error.clear();
        status_index_.store(new_idx, std::memory_order_release);
    }
}
```

**Slot allocation:** Follow `initialize_rendered_preview_slots` (line 868-921). Each `AnalyzerSlot` gets no device memory of its own — it holds `AnalyzerResult` which contains GPU tensors managed by PyTorch. The slot just needs the state atomic and the retained_tensors vector. No `cudaMallocPitch` needed here because the tensors are allocated by the model runner internally.

**Overlay publication:** `LiveAnalyzerWorker` also owns a small ring of `AnalysisOverlayImage` surfaces. Each published overlay is stable until `release_overlay(slot_index)`; preview/compositor code must never hold a bare pointer with no matching release.

### 3. AnalysisOverlayImage

A separate RGBA GPU surface where the analyzer worker (or a dedicated overlay renderer) draws detection visualizations. This is the key architectural shift from Phase 1: overlays are no longer drawn in-place on the BGR output base.

**What exists today:**
- `publish_rendered_preview` (line 981-1097) draws directly onto the BGR output image:
  1. Copies source frame to output slot (line 993-1006)
  2. Calls `launch_draw_masks_boxes_labels_bgr_pitched` or `launch_draw_boxes_labels_bgr_pitched` (line 1035-1063) which modify the BGR pixels in-place
  3. The output image is both the base frame AND the overlay — they cannot be separated
- The drawing kernels at `draw_cuda.cu` operate on BGR888 pitched memory (line 268-319). They read existing pixel values, alpha-blend masks over them, and write back. There is no RGBA intermediate.

**What changes:**
- Detection overlays are rendered into a separate RGBA surface (`AnalysisOverlayImage`) instead of modifying the output base.
- The existing `launch_draw_masks_boxes_labels_bgr_pitched` kernel (draw_cuda.cu:268-319) is adapted to write RGBA instead of in-place BGR modification. New variant: `launch_draw_analysis_overlay_rgba_pitched`.
- The compositor (started in Phase 2, completed in Phase 3) blends `output base` + `AnalysisOverlayImage` into the published output.

**New RGBA overlay drawing kernel:**

The existing BGR pitched kernel at `draw_cuda.cu:268-319` reads existing pixel values and modifies them in-place. The RGBA variant instead writes to a cleared RGBA surface:

```cpp
// New in: src/rfdetr/cuda/draw_cuda.cu
// Parallel to draw_masks_boxes_labels_bgr_pitched_kernel (line 268-319)
// but writes RGBA overlay instead of modifying BGR base.

__global__ void draw_analysis_overlay_rgba_pitched_kernel(
    uint8_t* overlay_out,        // RGBA pitched output, pre-cleared to (0,0,0,0)
    std::size_t pitch_bytes,
    int width,
    int height,
    const bool* masks,           // [N, H, W] bool
    const float* boxes,          // [N, 4] float XYXY
    const uint8_t* colors,       // [N, 3] uint8 RGB
    const int* labels,           // [N] int32 zero-based
    int num_instances,
    uint8_t mask_alpha,          // 0-255 alpha for mask regions
    int box_thickness
) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    // 4 bytes per pixel (RGBA) instead of 3 (BGR)
    uint8_t* pixel = overlay_out
        + static_cast<std::size_t>(y) * pitch_bytes
        + static_cast<std::size_t>(x) * 4U;
    uint8_t r = 0, g = 0, b = 0, a = 0;

    // Masks — write color with mask_alpha
    for (int i = 0; i < num_instances; ++i) {
        if (masks[i * width * height + y * width + x]) {
            r = colors[i * 3];
            g = colors[i * 3 + 1];
            b = colors[i * 3 + 2];
            a = mask_alpha;
        }
    }

    // Boxes and labels — full opacity
    for (int i = 0; i < num_instances; ++i) {
        const int x1 = static_cast<int>(boxes[i * 4 + 0]);
        const int y1 = static_cast<int>(boxes[i * 4 + 1]);
        const int x2 = static_cast<int>(boxes[i * 4 + 2]);
        const int y2 = static_cast<int>(boxes[i * 4 + 3]);
        if (pixel_hits_box_edge(x, y, x1, y1, x2, y2, box_thickness) ||
            pixel_hits_label_digit(x, y, x1, y1, labels[i])) {
            r = colors[i * 3];
            g = colors[i * 3 + 1];
            b = colors[i * 3 + 2];
            a = 255;
        }
    }

    pixel[0] = r;
    pixel[1] = g;
    pixel[2] = b;
    pixel[3] = a;
}

// Launch wrapper — added to draw_cuda.h
void launch_draw_analysis_overlay_rgba_pitched(
    uint8_t* overlay_out,
    std::size_t pitch_bytes,
    int width, int height,
    const bool* masks,
    const float* boxes,
    const uint8_t* colors,
    const int* labels,
    int num_instances,
    uint8_t mask_alpha,
    int box_thickness,
    cudaStream_t stream);
```

**AnalysisOverlayImage buffer:**
```cpp
// One slot in the analyzer-owned overlay ring.
// Preallocated with cudaMallocPitch, 4 bytes per pixel (RGBA).
struct AnalysisOverlayImage {
    CUdeviceptr data = 0;
    std::size_t pitch_bytes = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    LiveFrameId frame_id{};         // Which frame this overlay corresponds to
    cudaEvent_t ready_event = nullptr;
    cudaStream_t stream = nullptr;
    bool has_content = false;       // False when cleared / no detections

    void ensure_capacity(uint32_t required_width, uint32_t required_height) {
        if (required_width <= width && required_height <= height) return;
        // Grow-only: free old, allocate new with max(current, required)
        if (data != 0) cudaFree(reinterpret_cast<void*>(data));
        const uint32_t new_width = std::max(width, required_width);
        const uint32_t new_height = std::max(height, required_height);
        ensure_cuda_ok(
            cudaMallocPitch(reinterpret_cast<void**>(&data), &pitch_bytes,
                            static_cast<std::size_t>(new_width) * 4U, new_height),
            "cudaMallocPitch for analysis overlay RGBA");
        width = new_width;
        height = new_height;
    }

    void clear(cudaStream_t s) {
        // Zero out RGBA → fully transparent
        ensure_cuda_ok(
            cudaMemset2DAsync(reinterpret_cast<void*>(data), pitch_bytes,
                              0, static_cast<std::size_t>(width) * 4U, height, s),
            "cudaMemset2DAsync for analysis overlay clear");
        has_content = false;
    }
};
```

### 4. Phase 2 Preview Renderer (Simplified Compositor)

Phase 3 introduces the full `LiveCompositor` with manual overlay + analysis overlay + base compositing. Phase 2 needs a simpler version that just blends `AnalysisOverlayImage` over the `OutputBundle` base for preview.

**What exists today:**
- `publish_rendered_preview` (line 981-1097) both copies the source and draws overlays in one pass onto a single BGR buffer. The result goes to `LivePreviewTexture` via `stage_live_preview_copy` (live_preview_texture.cpp:451-549).

**What changes:**
- The Phase 2 preview path:
  1. Output bundle from fanout provides the base BGR frame (Phase 1).
  2. Analyzer worker publishes `AnalyzerResult` with detection tensors.
  3. A preview rendering step draws the analysis overlay onto the output base. This is either done inline before handing the output to `LivePreviewTexture`, or via a simple composite kernel.
- For Phase 2, a simple in-place approach is acceptable: draw detections from the latest `AnalyzerResult` onto a copy of the output base before publishing to preview. This avoids needing the full compositor in Phase 2 while still decoupling the analyzer from frame delivery.

**Simple Phase 2 composite kernel:**
```cpp
// Blends a pre-rendered RGBA overlay onto a BGR pitched base image.
// Used in Phase 2 as a simplified compositor. Phase 3 generalizes this
// to handle multiple overlay layers.
//
// Alpha blending: base_pixel = overlay_rgb * (overlay_a/255) + base_pixel * (1 - overlay_a/255)
__global__ void composite_rgba_over_bgr_pitched_kernel(
    uint8_t* base_bgr,              // BGR pitched image (read-write)
    std::size_t base_pitch,
    const uint8_t* overlay_rgba,     // RGBA pitched overlay (read-only)
    std::size_t overlay_pitch,
    int width,
    int height
) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    const uint8_t* ov = overlay_rgba
        + static_cast<std::size_t>(y) * overlay_pitch
        + static_cast<std::size_t>(x) * 4U;
    const uint8_t a = ov[3];
    if (a == 0) return; // Fully transparent — skip

    uint8_t* bp = base_bgr
        + static_cast<std::size_t>(y) * base_pitch
        + static_cast<std::size_t>(x) * 3U;

    if (a == 255) {
        // Fully opaque — direct write (BGR order in base, RGB in overlay)
        bp[0] = ov[2]; // B
        bp[1] = ov[1]; // G
        bp[2] = ov[0]; // R
    } else {
        // Alpha blend
        const float alpha = a / 255.0f;
        const float inv_alpha = 1.0f - alpha;
        bp[0] = static_cast<uint8_t>(ov[2] * alpha + bp[0] * inv_alpha);
        bp[1] = static_cast<uint8_t>(ov[1] * alpha + bp[1] * inv_alpha);
        bp[2] = static_cast<uint8_t>(ov[0] * alpha + bp[2] * inv_alpha);
    }
}
```

### 5. Save Gating for Model-Backed Flows

Ensures that saved analyzer-derived results match the frame being saved. Prevents silent mismatches where a save captures detections from frame N applied to frame N+k.

**What exists today:**
- Annotate live mode already has partial frame gating:
  - `effective_seed_mask` (annotation_core.cpp:233) checks `seed_frame_id == frame.frame_id` in live mode
  - Hold-save overflow (app.cpp:1488) checks `annotate_queued_save_frame_->frame_id != annotate_frame_->frame_id`
- Live predict has no save feature — results are display-only with `LivePredictStatus::last_prediction` (live_predict.h:118) tracking the last completed frame ID.
- Frame IDs are bare `uint64_t` counters. A session restart resets the counter, so frame ID 42 from session A and frame ID 42 from session B are indistinguishable.

**What changes:**
- `LiveFrameId` (Phase 1) adds `session_nonce` to disambiguate across session restarts.
- Save operations must compare full `LiveFrameId` (nonce + sequence), not just sequence.
- `AnnotationFrame::frame_id`, queued-save frame IDs, and live-only seed provenance such as `AnnotationInstance::seed_frame_id` move to `LiveFrameId`-based fields (`std::optional<LiveFrameId>` where an "unset" state is required).
- New save gating rule: when a save request includes analyzer-derived data (model detections, masks), the analyzer result's `LiveFrameId` must exactly match the output frame's `LiveFrameId`. Mismatch → loud failure, not silent fallback.

**Sketch:**
```cpp
// New file: include/fastloader/live/save_gating.h

struct SaveGatingResult {
    bool allowed = false;
    std::string rejection_reason;
};

// Check whether an analyzer result can be saved against a given output frame.
// Called before any save that includes model-derived data.
inline SaveGatingResult check_save_gating(
    const LiveFrameId& output_frame_id,
    const LiveFrameId& analyzer_frame_id,
    bool analyzer_attached)
{
    if (!analyzer_attached) {
        // Manual-only save — always allowed, no model data involved.
        return {true, {}};
    }
    if (output_frame_id != analyzer_frame_id) {
        return {false,
            "Save rejected: analyzer result frame ("
            + std::to_string(analyzer_frame_id.session_nonce) + ":"
            + std::to_string(analyzer_frame_id.sequence)
            + ") does not match output frame ("
            + std::to_string(output_frame_id.session_nonce) + ":"
            + std::to_string(output_frame_id.sequence) + ")"};
    }
    return {true, {}};
}
```

**Integration with annotation save (app.cpp):**

The existing hold-save gating at `app.cpp:1482-1503` checks `annotate_queued_save_frame_->frame_id != annotate_frame_->frame_id`. This stays but is extended:

```cpp
// In the save path, after frame ID match passes:
if (controller_->analyzer_worker() != nullptr) {
    LiveAnalyzerWorker::AnalyzerResultView result_view{};
    if (!controller_->analyzer_worker()->try_acquire_latest_result(&result_view)) {
        // No analyzer result available yet — cannot save model data
        annotate_save_error_ = "No analyzer result available for save";
        return;
    }
    const auto gating = check_save_gating(
        output_frame_id,        // LiveFrameId from the output bundle
        result_view.result->frame_id, // LiveFrameId from the analyzer
        true);
    if (!gating.allowed) {
        annotate_save_error_ = gating.rejection_reason;
        log_gui_error_to_stderr("save-gating", gating.rejection_reason);
        controller_->analyzer_worker()->release_result(result_view.slot_index);
        return;
    }
    // Proceed with save using result_view.result->splits[].detections
    controller_->analyzer_worker()->release_result(result_view.slot_index);
}
```

### 6. LiveSessionController Phase 2 Extensions

**Phase 1 controller** owns ingress + fanout. Phase 2 adds the optional analyzer worker.

```cpp
// Additions to LiveSessionController (from Phase 1 sketch):

class LiveSessionController {
public:
    // ... Phase 1 members ...

    // Phase 2: optional analyzer
    void attach_analyzer(std::unique_ptr<FrameAnalyzer> analyzer);
    void detach_analyzer();
    LiveAnalyzerWorker* analyzer_worker();  // nullptr if no analyzer

    // Phase 2: analyzer result / overlay access (convenience wrappers)
    bool try_acquire_latest_analyzer_result(LiveAnalyzerWorker::AnalyzerResultView* out);
    void release_analyzer_result(uint32_t slot_index);
    bool try_acquire_latest_analysis_overlay(LiveAnalyzerWorker::AnalysisOverlayView* out);
    void release_analysis_overlay(uint32_t slot_index);

    // Phase 2: combined status
    struct SessionStatus {
        bool running = false;
        LiveVideoIngress::Status ingress;
        LiveFrameFanout::Status fanout;
        std::optional<LiveAnalyzerWorker::Status> analyzer; // nullopt if no analyzer
    };
    SessionStatus snapshot_status() const;

private:
    // ... Phase 1 members ...
    std::unique_ptr<LiveAnalyzerWorker> analyzer_worker_;
};
```

**Predict creates controller with analyzer:**
```cpp
// In App, replaces current LivePredictSession creation at app.cpp:1750-1779:
auto config = LiveSessionConfig{...};
controller_ = std::make_unique<LiveSessionController>(config);

auto analyzer = std::make_unique<RfDetrFrameAnalyzer>(
    RfDetrFrameAnalyzer::Options{predict_options_, resolved_artifacts_});
controller_->attach_analyzer(std::move(analyzer));
controller_->start();
```

**Annotate creates controller without analyzer:**
```cpp
// In App, replaces current start_annotation_live_session at app.cpp:1272-1298:
auto config = LiveSessionConfig{...};
controller_ = std::make_unique<LiveSessionController>(config);
// No attach_analyzer — annotate is manual-only by default
controller_->start();
```

### 7. Remove Obsolete Live Predict Internal Capture/Preview Code

Once predict uses the controller's fanout for detect bundles and the analyzer worker for inference, the following code inside `LivePredictSession::Impl` becomes dead:

**Code to remove from `live_predict.cpp`:**
- `capture_session_` member and its creation in `thread_main` (line 1166-1180) — ingress is now owned by the controller
- `RenderedPreviewSlot` struct (line 641-655) and all slot management (`initialize_rendered_preview_slots` line 868-921, `release_rendered_preview_slots` line 923-946, `reserve_rendered_preview_slot_locked` line 948-979) — preview is now rendered from controller output + analyzer result
- `publish_rendered_preview` (line 981-1097) — replaced by the analysis overlay + compositor path
- The source-frame copy in `publish_rendered_preview` (line 993-1006) — replaced by fanout's `OutputBundle`
- In-place overlay drawing calls (line 1035-1063) — replaced by `launch_draw_analysis_overlay_rgba_pitched` writing to `AnalysisOverlayImage`
- `try_acquire_latest_preview` / `release_preview` (line 793-859) — preview now comes from the controller

**Code to keep from `live_predict.cpp`:**
- `LiveModelRunner` base class (line 147-155) — used by `RfDetrFrameAnalyzer`
- `LiveWeightsRunner` (line 250-402) and `LiveBackendRunner` (line 404-559) — wrapped by `RfDetrFrameAnalyzer`
- `select_live_overlay_for_preview` (line 164-248) — reused by the analyzer to select which detections to visualize
- `LiveOverlaySelection` struct (line 157-162) — feeds into `AnalyzerResult::SplitResult`
- Preprocessing: `launch_bgr_split_to_planar_float`, `build_horizontal_splits`, split region helpers

**LivePreviewTexture changes:**
Phase 1 changes `begin_live_stream` to accept `LiveSessionController&` instead of `LivePredictSession&`. Phase 2 extends `pump()` to also read the latest published analysis overlay and apply it before uploading to GL:

```cpp
// In LivePreviewTexture::pump(), after acquiring output bundle from controller:
// 1. Acquire output bundle → D2D copy to PBO (existing stage_live_preview_copy path)
// 2. If analyzer attached:
//    a. Acquire latest AnalysisOverlayView
//    b. If overlay.frame_id matches output frame_id:
//       Launch composite_rgba_over_bgr_pitched on the PBO-mapped pointer
//       (between cudaGraphicsMapResources and cudaGraphicsUnmapResources,
//        same stream as the existing D2D copy at live_preview_texture.cpp:504-511)
//    c. Release analysis overlay view
// 3. Finalize and upload to GL texture (existing finalize_live_preview_copy path)
```

### Phase 2 Implementation Order

1. **FrameAnalyzer interface** — standalone header, no dependencies beyond Phase 1 types (`DetectBundle`, `LiveFrameId`).
2. **AnalyzerResult struct** — standalone, carries `LiveFrameId` + GPU tensors.
3. **RfDetrFrameAnalyzer** — wraps existing `LiveWeightsRunner` / `LiveBackendRunner`. Adapter from `DetectBundle` → `LiveFrameInputs`. Verify model loading and inference produce identical results to existing `thread_main` path.
4. **AnalyzerSlot + LiveAnalyzerWorker** — dedicated thread consuming `DetectBundle` from fanout, publishing `AnalyzerResult`. Uses same slot state machine as `RenderedPreviewSlot` (line 634-639).
5. **AnalysisOverlayImage buffer** — grow-only `cudaMallocPitch` RGBA surface with `ensure_capacity`.
6. **RGBA overlay drawing kernel** — `launch_draw_analysis_overlay_rgba_pitched`, parallel to existing `launch_draw_masks_boxes_labels_bgr_pitched` (draw_cuda.cu:268-319) but writes to RGBA surface.
7. **Phase 2 composite kernel** — `composite_rgba_over_bgr_pitched`, blends RGBA overlay onto BGR output base.
8. **Extend LiveSessionController** — add `attach_analyzer`, `detach_analyzer`, `analyzer_worker()`, combined status.
9. **Save gating** — `check_save_gating` with `LiveFrameId` comparison. Wire into annotation save path, replacing bare `uint64_t` frame_id checks.
10. **Wire predict** — `App` creates `LiveSessionController` + `RfDetrFrameAnalyzer`. Preview reads output bundle + latest analyzer result, composites, uploads to GL.
11. **Remove dead predict code** — gut `RenderedPreviewSlot`, `publish_rendered_preview`, internal `capture_session_` from `LivePredictSession::Impl`. Keep runners and overlay selection.
12. **Update LivePreviewTexture::pump()** — after D2D copy of output base to PBO, optionally composite analysis overlay if analyzer result frame matches.

## Phase 3

### Goals
- Add `ManualOverlayDocument` as a CPU-side interaction model for crop boxes, class boxes, handles, brush strokes, and click state. This is separate from persisted settings and separate from analyzer results.
- Add a separate GPU `ManualOverlayImage` as RGBA. The manual worker redraws this image whenever the document changes, independent of frame delivery and inference timing.
- Add a separate GPU `AnalysisOverlayImage` as RGBA. The analyzer worker redraws this image whenever a new analyzer result arrives.
- Add a compositor kernel that alpha-blends `base output image` + `manual overlay image` + `analysis overlay image` into the published BGR output image every frame.
- Add the `Persistent Drawing` checkbox for the analysis overlay only. When enabled, the last positive analysis overlay stays visible on newer frames until replaced by a newer matching result or cleared by a newer negative result. Manual overlay remains fully separate and always live.

### Core Files

**Manual Drawing State (gui — current implementation to replace)**
- `src/gui/annotation_geometry.h` — `AnnotationGeometryDocument` class (line 38-70), `AnnotationGeometryInstance` struct (line 32-36), `AnnotationGeometryToolMode` enum (line 14-21: Select, Direct, AddBox, Paint, Erase, Fill)
- `src/gui/annotation_geometry.cpp` — `set_instance_box` (line 528-547), `resize_instance` (line 549-577), `move_instance` (line 579-605), `paint_instance` with circular stamps (line 607-656), `fill_instance` (line 658-682), `circle_stamp` helper (line 131-143)
- `src/gui/annotation_core.h` — `AnnotationBox` struct XYXY format (line 35-40), `AnnotationInstance` struct with seed_kind/box/seed_mask/category (line 77-88), `AnnotationMaskRegion` (line 70-75), `AnnotationFrame` with pixels_bgr (line 57-68)
- `src/gui/annotation_core.cpp` — `build_annotation_preview` CPU-side rendering (line 675-721), `draw_rect_bgr` CPU box drawing (line 112-151), `category_color` 8-color palette (line 98-110), mask alpha blend formula `(1-0.38)*base + 0.38*color` (line 706-714)

**Canvas Interaction Layer (gui — reused by ManualOverlayDocument)**
- `src/gui/canvas_layers.h` — `RectDragKind` enum (line 11-19: None/Create/Move/ResizeTL/TR/BL/BR), `RectDragState` (line 21-26), `CanvasViewport` (line 28-35), `CanvasPointerState` (line 44-50), `RectLayerSpec` (line 52-61), `RectLayerState` (line 63-66), `RectLayerFrameResult` (line 68-76)
- `src/gui/canvas_layers.cpp` — `apply_rect_drag` (line 49-104), `rectangle_hover_kind_with_options` (line 149-200), `update_rect_layers` (line 211-275), `resolve_video_crop` (line 281-300)

**Annotation Workspace Drawing (gui — the ImGui render loop Phase 3 wires to)**
- `src/gui/app.cpp` — `draw_annotate_workspace` (line 3271-4004): tool mode buttons (line 3375-3386), canvas setup with `ImDrawList` (line 3568-3575), box rendering with AddRect (line 3761-3776), handle drawing `draw_box_handles` (line 442-472), brush circle preview (line 3787-3798), Direct drag handling (line 3860-3898), AddBox creation (line 3901-3948), Paint/Erase strokes (line 3969-3979), Fill (line 3996-4001)
- `src/gui/app.cpp` — `submit_annotation_preview` background task (line 1203-1270): snapshots frame+instances → calls `build_annotation_preview` on thread pool → uploads result via `submit_host_bgr` H2D (line 1239). This entire CPU preview pipeline is replaced by the GPU manual overlay + compositor.

**App Annotation Members (gui — state that ManualOverlayDocument subsumes)**
- `src/gui/app.h` — `annotate_instances_` (line 185), `annotate_selected_instance_` (line 187), `annotate_create_drag_state_` (line 192), `annotate_direct_drag_state_` (line 194), `annotate_geometry_` (line 195), `annotate_geometry_tool_mode_` (line 196), `annotate_brush_radius_` (line 197), `annotate_painting_` (line 199), `annotate_last_paint_capture_x/y_` (line 200-201)

**Existing GPU Drawing Kernels (rfdetr CUDA — patterns for overlay kernels)**
- `src/rfdetr/cuda/draw_cuda.cu` — `draw_masks_boxes_labels_bgr_pitched_kernel` (line 268-319): per-pixel mask alpha blend `r = r*(1-alpha) + color*alpha` (line 294-296), box edge replacement (line 300-314), launch with 16×16 blocks (line 415-416)
- `src/rfdetr/cuda/draw_cuda.cu` — `draw_boxes_labels_bgr_pitched_kernel` (line 225-266): box edge + label digit replacement, same 16×16 launch (line 361-362)
- `include/fastloader/rfdetr/draw_cuda.h` — launch declarations (line 53-64 boxes, line 68-81 masks+boxes)

**Compositing Integration Point (gui — where compositor output feeds display)**
- `src/gui/live_preview_texture.h` — `LivePreviewTexture` class (line 34-113), `live_session_` typed as `LivePredictSession*` (line 97), PBO + CUDA-GL interop members (line 106-109)
- `src/gui/live_preview_texture.cpp` — `ensure_live_resources` PBO allocation + CUDA-GL registration (line 345-423), `stage_live_preview_copy` D2D to PBO (line 451-549: map at line 481, copy at line 504-511, unmap at line 514), `finalize_live_preview_copy` GL upload via glTexSubImage2D (line 551-651: query event line 574, PBO→texture line 613-626)

**Existing In-Place Compositing (rfdetr — replaced by LiveCompositor)**
- `src/rfdetr/inference/live_predict.cpp` — `publish_rendered_preview` (line 981-1097): source copy (line 993-1006), stream wait on model outputs (line 1009-1012), split image pointer offset (line 1023-1026), mask+box drawing calls (line 1035-1047), tensor retention (line 1066-1071), slot state transition (line 1087)
- `src/rfdetr/inference/live_predict.cpp` — `RenderedPreviewSlot` struct (line 641-655), `RenderedPreviewState` enum (line 634-639: kFree/kWriting/kPublished/kDisplaying), `initialize_rendered_preview_slots` (line 868-921), `reserve_rendered_preview_slot_locked` (line 948-979)

### 1. ManualOverlayDocument

Unified CPU-side state model for all manual annotation interactions in live mode. Replaces the scattered `annotate_instances_`, `annotate_geometry_`, drag states, and brush state currently spread across `App` members (app.h:185-201) and `AnnotationGeometryDocument` (annotation_geometry.h:38-70).

**What exists today:**
- `AnnotationGeometryDocument` (annotation_geometry.h:38-70) holds per-instance torch::Tensor masks on GPU for paint/erase/fill. It runs directly on the CUDA device but is called synchronously from the ImGui thread during `draw_annotate_workspace` (app.cpp:3969-3979).
- Annotation boxes, category indices, seed kinds are stored in `std::vector<AnnotationInstance>` (app.h:185). The geometry document holds a parallel `std::vector<AnnotationGeometryInstance>` that is synced via `sync_instance_from_annotation` / `export_instance` (annotation_geometry.h:50-51).
- Drag state is split across `annotate_create_drag_state_` (app.h:192), `annotate_direct_drag_state_` (app.h:194), and `annotate_canvas_layer_state_` (app.h:193).
- Brush state is `annotate_painting_` (app.h:199), `annotate_brush_radius_` (app.h:197), and `annotate_last_paint_capture_x/y_` (app.h:200-201).
- The preview is built on CPU by `build_annotation_preview` (annotation_core.cpp:675-721) which alpha-blends masks and draws boxes into a `pixels_bgr` vector, then `submit_host_bgr` (live_preview_texture.cpp:207-255) re-uploads to GPU. This CPU round-trip is the bottleneck Phase 3 eliminates.

**What changes:**
- `ManualOverlayDocument` owns the complete manual annotation state: instances, geometry, tool mode, drag state, brush state, and a generation counter that increments on every mutation.
- The document is written only from the ImGui/UI thread. The `LiveManualOverlayWorker` reads an immutable snapshot of the document when the generation advances.
- `AnnotationGeometryDocument` is absorbed into `ManualOverlayDocument` — the torch::Tensor masks and paint/fill operations stay, but they're no longer a separate parallel structure that needs syncing.
- `build_annotation_preview` CPU path is removed for live mode. The manual worker renders directly to a GPU RGBA surface.

**Sketch:**
```cpp
// New file: include/fastloader/live/manual_overlay_document.h

struct ManualOverlayInstance {
    std::string instance_id;
    bool enabled = true;
    AnnotationSeedKind seed_kind = AnnotationSeedKind::Box;
    AnnotationBox box{};                       // XYXY capture-space
    std::size_t category_index = 0;

    // Geometry — previously in AnnotationGeometryInstance
    AnnotationMaskRegion mask_region{};
    torch::Tensor mask;                        // [H, W] bool on CUDA device
};

struct ManualOverlayDocumentSnapshot {
    uint64_t generation = 0;
    std::vector<ManualOverlayInstance> instances;  // Metadata + retained torch::Tensor handles; no deep copy of mask payload
    std::optional<std::size_t> selected_instance;
    AnnotationGeometryToolMode tool_mode = AnnotationGeometryToolMode::Direct;
    uint32_t capture_width = 0;
    uint32_t capture_height = 0;
};

class ManualOverlayDocument {
public:
    ManualOverlayDocument(int device_id,
                          uint32_t capture_width,
                          uint32_t capture_height);

    // Instance management — each mutation bumps generation_.
    std::size_t add_instance(const std::string& instance_id, std::size_t category_index);
    void remove_instance(std::size_t index);
    void set_instance_enabled(std::size_t index, bool enabled);

    // Geometry operations — forwarded from current AnnotationGeometryDocument
    // (annotation_geometry.cpp:528-682). Each returns true if the overlay changed.
    bool set_instance_box(std::size_t index, const AnnotationBox& box);
    bool resize_instance(std::size_t index, const AnnotationBox& box);
    bool move_instance(std::size_t index, int dx, int dy);
    bool paint_instance(std::size_t index, int capture_x, int capture_y,
                        int radius, bool erase);
    bool fill_instance(std::size_t index, int capture_x, int capture_y);

    // Tool state — written from ImGui thread.
    void set_tool_mode(AnnotationGeometryToolMode mode);
    void set_selected_instance(std::optional<std::size_t> index);

    // Lock-free snapshot read for the worker via atomic shared_ptr swap.
    // The UI thread builds a new immutable snapshot and publishes it with
    // std::atomic_store on the shared_ptr field. Returns nullptr if the
    // generation has not changed since last_seen_generation.
    std::shared_ptr<const ManualOverlayDocumentSnapshot> snapshot_if_changed(
        uint64_t last_seen_generation) const;

    // Current generation (lock-free read for polling).
    uint64_t generation() const {
        return generation_.load(std::memory_order_acquire);
    }

    // Export back to AnnotationInstance format for save.
    void export_instances(std::vector<AnnotationInstance>* out) const;

private:
    // Single-writer immutable snapshot communication.
    // current_snapshot_ is published/read via std::atomic_store/load free functions.
    std::shared_ptr<const ManualOverlayDocumentSnapshot> current_snapshot_;
    std::atomic<uint64_t> generation_{0};
    int device_id_;
    torch::Device device_ = torch::kCPU;
    uint32_t capture_width_;
    uint32_t capture_height_;
    std::vector<ManualOverlayInstance> instances_;
    std::optional<std::size_t> selected_instance_;
    AnnotationGeometryToolMode tool_mode_ = AnnotationGeometryToolMode::Direct;
};
```

**App-side changes:**
- Replace `annotate_instances_` + `annotate_geometry_` + drag/brush state members (app.h:185-201) with a single `ManualOverlayDocument` owned by the controller.
- `draw_annotate_workspace` (app.cpp:3271-4004) keeps the ImGui interaction code (hover detection, drag handling, tool buttons) but routes mutations through `controller->manual_overlay_document()` instead of directly modifying `annotate_instances_` and calling `annotate_geometry_->paint_instance()`.
- Remove `submit_annotation_preview` (app.cpp:1203-1270) and `build_annotation_preview` usage. Preview comes from the compositor.

### 2. LiveManualOverlayWorker

One dedicated thread that watches the `ManualOverlayDocument` generation counter and redraws a GPU RGBA surface (`ManualOverlayImage`) whenever the document changes. Decoupled from frame delivery and inference timing.

**What exists today:**
- Annotation masks are rendered on CPU by `build_annotation_preview` (annotation_core.cpp:675-721): iterates every instance, alpha-blends mask pixels at 38% opacity (line 706-714), draws box outlines via `draw_rect_bgr` (line 112-151). This runs on a thread pool worker via `submit_annotation_preview` (app.cpp:1215-1228) and the result is uploaded via `submit_host_bgr` (app.cpp:1239).
- Mask painting (`paint_instance` at annotation_geometry.cpp:607-656) uses torch::Tensor operations on CUDA but is called synchronously from the ImGui thread during mouse drag.

**What changes:**
- The manual worker owns a `ManualOverlayImage` (RGBA GPU surface) and redraws it fully from the `ManualOverlayDocumentSnapshot` whenever the generation advances.
- Drawing uses new CUDA kernels that render directly to RGBA — no CPU pixels, no `submit_host_bgr` upload.
- Mask painting still happens on the ImGui thread (torch operations on the document's mask tensors), but the result is only visible after the worker picks up the new generation and redraws. At ~60fps ImGui and a worker poll of 1ms, the visual lag is imperceptible.
- Published overlays use the same acquire/release slot semantics as the other live workers. The compositor must acquire an overlay slot and release it explicitly; the manual worker never overwrites `kAcquired` slots.

**Sketch:**
```cpp
// New file: include/fastloader/live/live_manual_overlay_worker.h

struct ManualOverlayImage {
    CUdeviceptr data = 0;          // RGBA pitched
    std::size_t pitch_bytes = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t document_generation = 0;  // Which snapshot this was rendered from
    cudaEvent_t ready_event = nullptr;
    cudaStream_t stream = nullptr;
    bool has_content = false;

    void ensure_capacity(uint32_t required_width, uint32_t required_height) {
        if (required_width <= width && required_height <= height) return;
        if (data != 0) cudaFree(reinterpret_cast<void*>(data));
        const uint32_t new_width = std::max(width, required_width);
        const uint32_t new_height = std::max(height, required_height);
        ensure_cuda_ok(
            cudaMallocPitch(reinterpret_cast<void**>(&data), &pitch_bytes,
                            static_cast<std::size_t>(new_width) * 4U, new_height),
            "cudaMallocPitch for manual overlay RGBA");
        width = new_width;
        height = new_height;
    }

    void clear(cudaStream_t s) {
        ensure_cuda_ok(
            cudaMemset2DAsync(reinterpret_cast<void*>(data), pitch_bytes,
                              0, static_cast<std::size_t>(width) * 4U, height, s),
            "cudaMemset2DAsync for manual overlay clear");
        has_content = false;
    }
};

class LiveManualOverlayWorker {
public:
    LiveManualOverlayWorker(ManualOverlayDocument& document,
                            int cuda_device_index);
    ~LiveManualOverlayWorker();

    void start();
    void stop();

    // Called by compositor thread. Reads the latest rendered overlay.
    // Returns false if no overlay has been rendered yet.
    struct OverlayView {
        uint32_t slot_index = 0;
        CUdeviceptr data = 0;
        std::size_t pitch_bytes = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        cudaEvent_t ready_event = nullptr;
        bool has_content = false;
        uint64_t document_generation = 0;
    };
    bool try_acquire_overlay(OverlayView* out);
    void release_overlay(uint32_t slot_index);

private:
    void worker_thread_main();
    void render_snapshot(const ManualOverlayDocumentSnapshot& snapshot);
    struct OverlaySlot {
        uint32_t slot_index = 0;
        ManualOverlayImage image{};
        std::atomic<uint32_t> state{static_cast<uint32_t>(SlotState::kFree)};
    };
    OverlaySlot* reserve_overlay_slot();

    ManualOverlayDocument& document_;
    int cuda_device_index_;
    std::thread thread_;
    std::atomic<bool> stop_requested_{false};
    uint64_t last_rendered_generation_ = 0;
    std::vector<std::unique_ptr<OverlaySlot>> overlay_slots_;
    std::atomic<int> latest_slot_index_{-1};
};
```

**Overlay slot reservation:** use the same `SlotState` scan as the other live workers (`kFree` / `kWriting` / `kPublished` / `kAcquired`). Two or three slots are sufficient; if every slot is still published/acquired, skip that redraw and retry on the next poll rather than blocking the UI or compositor.

**Worker thread loop:**
```cpp
void LiveManualOverlayWorker::worker_thread_main() {
    ensure_cuda_ok(cudaSetDevice(cuda_device_index_), "cudaSetDevice manual overlay worker");

    while (!stop_requested_.load(std::memory_order_acquire)) {
        auto snapshot = document_.snapshot_if_changed(last_rendered_generation_);
        if (!snapshot) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        render_snapshot(*snapshot);
        last_rendered_generation_ = snapshot->generation;
    }
}
```

**render_snapshot — draws all manual annotations to RGBA:**
```cpp
void LiveManualOverlayWorker::render_snapshot(
    const ManualOverlayDocumentSnapshot& snapshot)
{
    OverlaySlot* slot = reserve_overlay_slot();
    if (slot == nullptr) {
        return;  // Skip this redraw and retry on the next generation / poll.
    }

    ManualOverlayImage& target = slot->image;
    target.ensure_capacity(snapshot.capture_width, snapshot.capture_height);
    target.clear(target.stream);

    for (std::size_t i = 0; i < snapshot.instances.size(); ++i) {
        const ManualOverlayInstance& inst = snapshot.instances[i];
        if (!inst.enabled || !inst.mask.defined()) continue;
        if (!annotation_box_has_area(inst.box)) continue;

        const std::array<uint8_t, 3> color = category_color(inst.category_index);

        // Draw mask region into RGBA overlay.
        // Instance mask is [H, W] bool on GPU — same as AnnotationGeometryInstance::mask.
        // Offset in the overlay is (mask_region.capture_x, mask_region.capture_y).
        auto* overlay_region = reinterpret_cast<uint8_t*>(target.data)
            + static_cast<std::size_t>(inst.mask_region.capture_y) * target.pitch_bytes
            + static_cast<std::size_t>(inst.mask_region.capture_x) * 4U;

        launch_draw_manual_mask_rgba_pitched(
            overlay_region,
            target.pitch_bytes,
            static_cast<int>(inst.mask_region.width),
            static_cast<int>(inst.mask_region.height),
            inst.mask.data_ptr<bool>(),
            color[0], color[1], color[2],
            kManualMaskAlpha,   // 97 — matches 0.38 * 255 ≈ 97
            target.stream);

        // Draw box outline into RGBA overlay at full opacity.
        launch_draw_box_outline_rgba_pitched(
            reinterpret_cast<uint8_t*>(target.data),
            target.pitch_bytes,
            static_cast<int>(snapshot.capture_width),
            static_cast<int>(snapshot.capture_height),
            inst.box,
            color[0], color[1], color[2],
            2,  // thickness — matches draw_rect_bgr thickness at annotation_core.cpp:716
            target.stream);
    }

    // Draw selection handles for the selected instance.
    if (snapshot.selected_instance.has_value() &&
        *snapshot.selected_instance < snapshot.instances.size()) {
        const auto& sel = snapshot.instances[*snapshot.selected_instance];
        if (annotation_box_has_area(sel.box)) {
            launch_draw_selection_handles_rgba_pitched(
                reinterpret_cast<uint8_t*>(target.data),
                target.pitch_bytes,
                static_cast<int>(snapshot.capture_width),
                static_cast<int>(snapshot.capture_height),
                sel.box,
                target.stream);
        }
    }

    ensure_cuda_ok(cudaEventRecord(target.ready_event, target.stream),
                   "cudaEventRecord for manual overlay render");
    target.has_content = true;
    target.document_generation = snapshot.generation;
    slot->state.store(static_cast<uint32_t>(SlotState::kPublished),
                      std::memory_order_release);
    latest_slot_index_.store(static_cast<int>(slot->slot_index),
                             std::memory_order_release);
}
```

### 3. Manual Overlay CUDA Kernels

New kernels that render annotation masks, box outlines, and selection handles into RGBA pitched surfaces. These parallel the existing BGR kernels in `draw_cuda.cu` (line 225-319) but write to a cleared RGBA surface instead of modifying a BGR base image in-place.

**Mask rendering kernel:**
```cpp
// New in: src/rfdetr/cuda/draw_cuda.cu
// Parallel to the mask loop in draw_masks_boxes_labels_bgr_pitched_kernel (line 292-297)
// but writes RGBA overlay for a single instance mask at its region offset.

__global__ void draw_manual_mask_rgba_pitched_kernel(
    uint8_t* overlay_region,    // RGBA pitched output, already offset to mask origin
    std::size_t pitch_bytes,
    int mask_width,
    int mask_height,
    const bool* mask,           // [mask_height, mask_width] bool
    uint8_t r, uint8_t g, uint8_t b,
    uint8_t alpha               // 97 for 38% opacity, matching kMaskBlend
) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= mask_width || y >= mask_height) return;

    if (!mask[y * mask_width + x]) return;

    uint8_t* pixel = overlay_region
        + static_cast<std::size_t>(y) * pitch_bytes
        + static_cast<std::size_t>(x) * 4U;

    // Over-composite: if a previous instance already wrote here,
    // the new instance's color replaces it (last-instance-wins, same as
    // the loop order in draw_masks_boxes_labels_bgr_pitched_kernel line 292-297).
    pixel[0] = r;
    pixel[1] = g;
    pixel[2] = b;
    pixel[3] = alpha;
}

void launch_draw_manual_mask_rgba_pitched(
    uint8_t* overlay_region,
    std::size_t pitch_bytes,
    int mask_width, int mask_height,
    const bool* mask,
    uint8_t r, uint8_t g, uint8_t b,
    uint8_t alpha,
    cudaStream_t stream)
{
    if (overlay_region == nullptr || mask == nullptr ||
        mask_width <= 0 || mask_height <= 0) return;

    dim3 block(16, 16);
    dim3 grid((mask_width + 15) / 16, (mask_height + 15) / 16);
    draw_manual_mask_rgba_pitched_kernel<<<grid, block, 0, stream>>>(
        overlay_region, pitch_bytes,
        mask_width, mask_height, mask,
        r, g, b, alpha);
}
```

**Box outline kernel:**
```cpp
// Draws a rectangular outline into RGBA at full opacity.
// Replaces CPU-side draw_rect_bgr (annotation_core.cpp:112-151) for GPU path.

__global__ void draw_box_outline_rgba_pitched_kernel(
    uint8_t* overlay_out,       // RGBA pitched, full image
    std::size_t pitch_bytes,
    int image_width,
    int image_height,
    int box_x1, int box_y1, int box_x2, int box_y2,
    uint8_t r, uint8_t g, uint8_t b,
    int thickness
) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= image_width || y >= image_height) return;

    // Same edge test pattern as pixel_hits_box_edge in draw_cuda.cu.
    bool on_edge = false;
    for (int t = 0; t < thickness; ++t) {
        const int x1 = box_x1 - t;
        const int x2 = box_x2 - 1 + t;
        const int y1 = box_y1 - t;
        const int y2 = box_y2 - 1 + t;
        if ((x >= x1 && x <= x2 && (y >= y1 && y <= y1 + 0)) ||
            (x >= x1 && x <= x2 && (y >= y2 && y <= y2 + 0)) ||
            (y >= y1 && y <= y2 && (x >= x1 && x <= x1 + 0)) ||
            (y >= y1 && y <= y2 && (x >= x2 && x <= x2 + 0))) {
            on_edge = true;
            break;
        }
    }

    if (!on_edge) return;

    uint8_t* pixel = overlay_out
        + static_cast<std::size_t>(y) * pitch_bytes
        + static_cast<std::size_t>(x) * 4U;
    pixel[0] = r;
    pixel[1] = g;
    pixel[2] = b;
    pixel[3] = 255;  // Full opacity for outlines
}
```

**Selection handles kernel:**
```cpp
// Draws 4 corner handles (small filled squares) for the selected instance.
// Replaces ImGui-side draw_box_handles (app.cpp:442-472) for the GPU overlay.

__global__ void draw_selection_handles_rgba_pitched_kernel(
    uint8_t* overlay_out,
    std::size_t pitch_bytes,
    int image_width,
    int image_height,
    int box_x1, int box_y1, int box_x2, int box_y2,
    int handle_radius           // 4 pixels, matching app.cpp:453
) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= image_width || y >= image_height) return;

    // Check all 4 corners.
    const int corners_x[4] = {box_x1, box_x2, box_x1, box_x2};
    const int corners_y[4] = {box_y1, box_y1, box_y2, box_y2};

    for (int c = 0; c < 4; ++c) {
        if (x >= corners_x[c] - handle_radius && x <= corners_x[c] + handle_radius &&
            y >= corners_y[c] - handle_radius && y <= corners_y[c] + handle_radius) {
            uint8_t* pixel = overlay_out
                + static_cast<std::size_t>(y) * pitch_bytes
                + static_cast<std::size_t>(x) * 4U;
            pixel[0] = 255;  // Gold fill — matches IM_COL32(255, 220, 96, 240)
            pixel[1] = 220;
            pixel[2] = 96;
            pixel[3] = 240;
            return;
        }
    }
}
```

### 4. LiveCompositor

One dedicated thread (or stream-owned worker) that continuously composites `OutputBundle.base` + `ManualOverlayImage` + `AnalysisOverlayImage` into the published BGR output image. Display never waits for a fresh analyzer result or manual redraw — it always shows the most recent base frame with whatever overlays are currently available.

**What exists today:**
- `publish_rendered_preview` (live_predict.cpp:981-1097) does everything in one pass: copies source (line 993-1006), waits on model output (line 1009-1012), draws masks/boxes in-place (line 1035-1063), and publishes. Source and overlay are inseparable.
- The annotate path has no GPU compositing at all — it renders masks on CPU in `build_annotation_preview` (annotation_core.cpp:675-721) and re-uploads via `submit_host_bgr`.

**What changes:**
- The compositor owns a composited output slot ring (same `SlotState` pattern as `RenderedPreviewSlot` at live_predict.cpp:634-655).
- Each frame, the compositor: (1) copies the latest `OutputBundle.base` from fanout, (2) composites `AnalysisOverlayImage` if available, (3) composites `ManualOverlayImage` if available, (4) publishes to a slot that `LivePreviewTexture` reads.
- The two overlays are blended in order: analysis first (model detections behind), manual second (user annotations on top). Both use the same RGBA-over-BGR alpha blend.
- Persistent analysis is implemented with a compositor-owned retained overlay cache. The compositor copies the last frame-matched positive analysis overlay into that cache before releasing the analyzer overlay slot.

**Sketch:**
```cpp
// New file: include/fastloader/live/live_compositor.h

struct CompositorSlot {
    uint32_t slot_index = 0;
    CUdeviceptr device_ptr = 0;        // Pitched BGR888 composited output
    std::size_t pitch_bytes = 0;
    cudaStream_t stream = nullptr;
    cudaEvent_t ready_event = nullptr;
    std::atomic<uint32_t> state{0};    // SlotState: kFree/kWriting/kPublished/kAcquired
    LiveFrameId frame_id{};
    LiveCaptureRegion region{};
    uint32_t width = 0;
    uint32_t height = 0;
};

struct CompositorConfig {
    uint32_t slot_count = 2;
    int cuda_device_index = 0;
    bool persistent_analysis_overlay = false;
};

class LiveCompositor {
public:
    LiveCompositor(LiveFrameFanout& fanout,
                   LiveAnalyzerWorker* analyzer_worker,  // nullable
                   LiveManualOverlayWorker* manual_worker, // nullable
                   CompositorConfig config);
    ~LiveCompositor();

    void start();
    void stop();

    // Called by LivePreviewTexture.
    // Returns the latest composited BGR frame.
    bool try_acquire_latest_output(CompositorSlot** out);
    void release_output(uint32_t slot_index);

    // Runtime toggle for persistent analysis overlay.
    void set_persistent_analysis_overlay(bool enabled);
    bool persistent_analysis_overlay() const;

    struct Status {
        bool running = false;
        uint64_t frames_composited = 0;
        uint64_t frames_dropped = 0;  // No free slot available
        LiveFrameId last_composited_frame_id{};
        bool analysis_overlay_active = false;
        bool manual_overlay_active = false;
    };
    Status snapshot_status() const;

private:
    void compositor_thread_main();
    CompositorSlot* reserve_slot();
    void cache_analysis_overlay(const LiveAnalyzerWorker::AnalysisOverlayView& view);
    void clear_retained_analysis_overlay();

    LiveFrameFanout& fanout_;
    LiveAnalyzerWorker* analyzer_worker_;
    LiveManualOverlayWorker* manual_worker_;
    CompositorConfig config_;
    std::vector<std::unique_ptr<CompositorSlot>> slots_;
    std::atomic<int> latest_slot_index_{-1};
    std::atomic<bool> stop_requested_{false};
    std::thread thread_;

    // Persistent analysis overlay state.
    std::atomic<bool> persistent_analysis_{false};
    AnalysisOverlayImage retained_analysis_overlay_; // Compositor-owned copy of the last positive overlay
    LiveFrameId last_analysis_frame_id_{};
    bool last_analysis_had_content_ = false;
};
```

**Compositor thread loop:**
```cpp
void LiveCompositor::compositor_thread_main() {
    ensure_cuda_ok(cudaSetDevice(config_.cuda_device_index),
                   "cudaSetDevice compositor");

    while (!stop_requested_.load(std::memory_order_acquire)) {
        // 1. Acquire latest output base frame from fanout.
        OutputBundle base{};
        if (!fanout_.try_acquire_output(&base)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // 2. Reserve a compositor slot.
        CompositorSlot* slot = reserve_slot();
        if (slot == nullptr) {
            fanout_.release_output(base.slot_index);
            continue;
        }

        // 3. Copy base frame into compositor slot.
        // Same pattern as publish_rendered_preview (live_predict.cpp:993-1006).
        cudaStreamWaitEvent(slot->stream, base.ready_event, 0);
        const std::size_t copy_width_bytes =
            static_cast<std::size_t>(base.dims.width) * 3U;
        cudaMemcpy2DAsync(
            reinterpret_cast<void*>(slot->device_ptr),
            slot->pitch_bytes,
            reinterpret_cast<const void*>(base.data),
            base.dims.pitch_bytes,
            copy_width_bytes,
            static_cast<std::size_t>(base.dims.height),
            cudaMemcpyDeviceToDevice,
            slot->stream);

        // 4. Composite analysis overlay (model detections — behind manual).
        if (analyzer_worker_ != nullptr) {
            // Phase 2 introduced AnalysisOverlayImage. The compositor reads
            // it from the analyzer worker's published overlay.
            LiveAnalyzerWorker::AnalysisOverlayView analysis{};
            bool use_analysis = false;
            bool use_retained_analysis = false;

            if (analyzer_worker_->try_acquire_latest_overlay(&analysis)) {
                const bool frame_matches = analysis.frame_id == base.frame_id;
                const bool persistent =
                    persistent_analysis_.load(std::memory_order_acquire);

                if (frame_matches && analysis.has_content) {
                    use_analysis = true;
                    last_analysis_frame_id_ = analysis.frame_id;
                    last_analysis_had_content_ = true;
                    if (persistent) {
                        cache_analysis_overlay(analysis);  // compositor-owned retained copy
                    }
                } else if (frame_matches && !analysis.has_content) {
                    clear_retained_analysis_overlay();
                    last_analysis_had_content_ = false;
                } else if (persistent && retained_analysis_overlay_.has_content) {
                    use_retained_analysis = true;
                }
            }

            if (use_analysis) {
                cudaStreamWaitEvent(slot->stream, analysis.ready_event, 0);
                launch_composite_rgba_over_bgr_pitched(
                    reinterpret_cast<uint8_t*>(slot->device_ptr),
                    slot->pitch_bytes,
                    reinterpret_cast<const uint8_t*>(analysis.data),
                    analysis.pitch_bytes,
                    static_cast<int>(base.dims.width),
                    static_cast<int>(base.dims.height),
                    slot->stream);
            } else if (use_retained_analysis) {
                cudaStreamWaitEvent(slot->stream, retained_analysis_overlay_.ready_event, 0);
                launch_composite_rgba_over_bgr_pitched(
                    reinterpret_cast<uint8_t*>(slot->device_ptr),
                    slot->pitch_bytes,
                    reinterpret_cast<const uint8_t*>(retained_analysis_overlay_.data),
                    retained_analysis_overlay_.pitch_bytes,
                    static_cast<int>(base.dims.width),
                    static_cast<int>(base.dims.height),
                    slot->stream);
            }

            if (analysis.data != 0) {
                analyzer_worker_->release_overlay(analysis.slot_index);
            }
        }

        // 5. Composite manual overlay (user annotations — on top of analysis).
        if (manual_worker_ != nullptr) {
            LiveManualOverlayWorker::OverlayView manual{};
            if (manual_worker_->try_acquire_overlay(&manual)) {
                if (manual.has_content) {
                    cudaStreamWaitEvent(slot->stream, manual.ready_event, 0);
                    launch_composite_rgba_over_bgr_pitched(
                        reinterpret_cast<uint8_t*>(slot->device_ptr),
                        slot->pitch_bytes,
                        reinterpret_cast<const uint8_t*>(manual.data),
                        manual.pitch_bytes,
                        static_cast<int>(base.dims.width),
                        static_cast<int>(base.dims.height),
                        slot->stream);
                }
                manual_worker_->release_overlay(manual.slot_index);
            }
        }

        // 6. Record ready event and publish.
        cudaEventRecord(slot->ready_event, slot->stream);
        slot->frame_id = base.frame_id;
        slot->region = base.region;
        slot->width = base.dims.width;
        slot->height = base.dims.height;
        slot->state.store(static_cast<uint32_t>(SlotState::kPublished),
                          std::memory_order_release);
        latest_slot_index_.store(static_cast<int>(slot->slot_index),
                                 std::memory_order_release);

        // 7. Release base output bundle back to fanout.
        fanout_.release_output(base.slot_index);
    }
}
```

**Compositor slot allocation:** Follow `initialize_rendered_preview_slots` (live_predict.cpp:868-921) — `cudaMallocPitch` for BGR888 at capture resolution, one `cudaStream_t` with highest priority + one `cudaEvent_t(cudaEventDisableTiming)` per slot.

**Slot reservation:** Same atomic-state scan as `reserve_rendered_preview_slot_locked` (live_predict.cpp:948-979): scan for `kFree` or stale `kPublished` where `cudaEventQuery(ready_event) == cudaSuccess`, transition to `kWriting`.

### 5. Composite Kernel (Dual-Layer)

Phase 2 introduced `composite_rgba_over_bgr_pitched_kernel` for single-overlay compositing. Phase 3 reuses the same kernel called twice (analysis first, manual second) in the compositor loop above. No new kernel is needed for the basic case.

For a fused two-overlay variant that avoids reading/writing the base twice:

```cpp
// Optional optimization — fused 2-layer composite.
// Reads base BGR once, blends both RGBA overlays, writes once.
// Only worth it if profiling shows the two-pass approach as a bottleneck.

__global__ void composite_dual_rgba_over_bgr_pitched_kernel(
    uint8_t* base_bgr,                 // BGR pitched image (read-write)
    std::size_t base_pitch,
    const uint8_t* analysis_rgba,       // RGBA analysis overlay (read-only, may be nullptr)
    std::size_t analysis_pitch,
    const uint8_t* manual_rgba,         // RGBA manual overlay (read-only, may be nullptr)
    std::size_t manual_pitch,
    int width,
    int height,
    bool has_analysis,
    bool has_manual
) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    uint8_t* bp = base_bgr
        + static_cast<std::size_t>(y) * base_pitch
        + static_cast<std::size_t>(x) * 3U;
    float b = bp[0], g = bp[1], r = bp[2];

    // Layer 1: analysis overlay (behind manual).
    if (has_analysis) {
        const uint8_t* ap = analysis_rgba
            + static_cast<std::size_t>(y) * analysis_pitch
            + static_cast<std::size_t>(x) * 4U;
        const uint8_t aa = ap[3];
        if (aa > 0) {
            const float alpha = aa / 255.0f;
            const float inv = 1.0f - alpha;
            r = ap[0] * alpha + r * inv;  // RGBA overlay is RGB order
            g = ap[1] * alpha + g * inv;
            b = ap[2] * alpha + b * inv;
        }
    }

    // Layer 2: manual overlay (on top of analysis).
    if (has_manual) {
        const uint8_t* mp = manual_rgba
            + static_cast<std::size_t>(y) * manual_pitch
            + static_cast<std::size_t>(x) * 4U;
        const uint8_t ma = mp[3];
        if (ma > 0) {
            const float alpha = ma / 255.0f;
            const float inv = 1.0f - alpha;
            r = mp[0] * alpha + r * inv;
            g = mp[1] * alpha + g * inv;
            b = mp[2] * alpha + b * inv;
        }
    }

    // Write back BGR.
    bp[0] = static_cast<uint8_t>(fminf(255.0f, fmaxf(0.0f, b)));
    bp[1] = static_cast<uint8_t>(fminf(255.0f, fmaxf(0.0f, g)));
    bp[2] = static_cast<uint8_t>(fminf(255.0f, fmaxf(0.0f, r)));
}

void launch_composite_dual_rgba_over_bgr_pitched(
    uint8_t* base_bgr, std::size_t base_pitch,
    const uint8_t* analysis_rgba, std::size_t analysis_pitch,
    const uint8_t* manual_rgba, std::size_t manual_pitch,
    int width, int height,
    bool has_analysis, bool has_manual,
    cudaStream_t stream)
{
    if (base_bgr == nullptr || width <= 0 || height <= 0) return;
    if (!has_analysis && !has_manual) return;

    // Same 16x16 block config as existing drawing kernels (draw_cuda.cu:361-362).
    dim3 block(16, 16);
    dim3 grid((width + 15) / 16, (height + 15) / 16);
    composite_dual_rgba_over_bgr_pitched_kernel<<<grid, block, 0, stream>>>(
        base_bgr, base_pitch,
        analysis_rgba, analysis_pitch,
        manual_rgba, manual_pitch,
        width, height,
        has_analysis, has_manual);
}
```

### 6. Persistent Analysis Overlay

When enabled, the last positive (non-empty) analysis overlay stays visible on newer frames until replaced by a newer matching result or cleared by a newer negative (empty) result.

**What exists today:**
- No persistence concept. `publish_rendered_preview` (live_predict.cpp:981-1097) draws the current frame's detections and the result is gone when the next frame publishes.

**What changes:**
- The compositor tracks `last_analysis_had_content_` (bool), `last_analysis_frame_id_` (LiveFrameId), and a compositor-owned `retained_analysis_overlay_` cache containing the last frame-matched positive overlay pixels.
- When `persistent_analysis_` is true and the latest analyzer overlay doesn't match the current output frame, the compositor composites `retained_analysis_overlay_` if that cache still has content.
- When a new frame-matched analyzer result arrives with detections, the compositor refreshes `retained_analysis_overlay_` before releasing the analyzer overlay slot.
- When a new frame-matched analyzer result arrives with no detections (`has_content == false`), the compositor clears `retained_analysis_overlay_`, clears `last_analysis_had_content_`, and stops showing the stale overlay.
- The UI checkbox is wired via `controller->compositor()->set_persistent_analysis_overlay(bool)`.

**Persistence state machine:**
```
State A: No overlay shown (last_analysis_had_content_ = false)
  → Analyzer publishes result with detections → State B
  → Analyzer publishes empty result → stay in State A

State B: Overlay shown, frame-matched (last_analysis_had_content_ = true)
  → New frame arrives, no new analyzer result, persistent=true → State C
  → New frame arrives, persistent=false → State A
  → Analyzer publishes empty result → State A

State C: Stale overlay shown (persistent mode)
  → Analyzer publishes new result with detections → State B
  → Analyzer publishes empty result → State A
  → persistent toggled off → State A
```

### 7. LivePreviewTexture Adaptation

Phase 1 changed `begin_live_stream` to accept `LiveSessionController&` instead of `LivePredictSession&`. Phase 2 extended `pump()` to optionally composite analysis overlay. Phase 3 simplifies this: `pump()` just acquires the compositor's published output and feeds it through the existing CUDA-GL interop path.

**What changes:**
- `pump()` calls `controller->compositor()->try_acquire_latest_output()` instead of `controller->try_acquire_latest_output()`.
- `stage_live_preview_copy` (live_preview_texture.cpp:451-549) receives the compositor slot's `device_ptr` and `pitch_bytes` directly — same D2D copy to PBO as today (line 504-511).
- `finalize_live_preview_copy` (live_preview_texture.cpp:551-651) stays unchanged — it polls the event, does `glTexSubImage2D` from PBO (line 616-624), and swaps front/back textures.
- The Phase 2 inline analysis composite in `pump()` is removed — the compositor handles all blending before the frame reaches `LivePreviewTexture`.

```cpp
// In LivePreviewTexture::pump(), Phase 3 version:
void LivePreviewTexture::pump() {
    // 1. Try to finalize any pending GL upload.
    std::string error;
    if (!finalize_live_preview_copy(&error)) {
        set_error(error, false);
        return;
    }

    // 2. Acquire latest composited output from compositor.
    if (controller_ == nullptr) return;
    CompositorSlot* slot = nullptr;
    if (!controller_->compositor()->try_acquire_latest_output(&slot)) {
        return;  // No new frame
    }

    // 3. Stage D2D copy from compositor slot → GL PBO.
    // Reuses same path as current stage_live_preview_copy (line 466-549):
    // ensure_live_resources → cudaStreamWaitEvent → cudaGraphicsMapResources →
    // cudaMemcpy2DAsync → cudaGraphicsUnmapResources → cudaEventRecord.
    if (!stage_composited_copy(*slot, &error)) {
        set_error(error, false);
        controller_->compositor()->release_output(slot->slot_index);
        return;
    }

    // 4. Store pending frame for finalize_live_preview_copy to pick up.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_compositor_slot_ = slot;
    }
}
```

### 8. LiveSessionController Phase 3 Extensions

Phase 1 owns ingress + fanout. Phase 2 adds analyzer worker. Phase 3 adds manual overlay worker and compositor.

```cpp
// Additions to LiveSessionController:

class LiveSessionController {
public:
    // ... Phase 1 + Phase 2 members ...

    // Phase 3: manual overlay document (written by UI thread).
    ManualOverlayDocument& manual_overlay_document();

    // Phase 3: manual overlay worker (reads document, writes GPU RGBA).
    LiveManualOverlayWorker* manual_overlay_worker();

    // Phase 3: compositor (reads fanout output + analyzer overlay + manual overlay).
    LiveCompositor& compositor();

    // Phase 3: combined start/stop orchestrates all workers.
    // Start order: ingress → fanout → analyzer (if attached) → manual worker → compositor.
    // Stop order: compositor → manual worker → analyzer → fanout → ingress.
    void start();
    void stop();

    // Phase 3: persistent analysis overlay toggle.
    void set_persistent_analysis_overlay(bool enabled);
    bool persistent_analysis_overlay() const;

private:
    // ... Phase 1 + Phase 2 members ...
    std::unique_ptr<ManualOverlayDocument> manual_document_;
    std::unique_ptr<LiveManualOverlayWorker> manual_worker_;
    std::unique_ptr<LiveCompositor> compositor_;
};
```

**Annotate creates controller with manual overlay, no analyzer:**
```cpp
// Replaces current start_annotation_live_session (app.cpp:1272-1298):
auto config = LiveSessionConfig{...};
controller_ = std::make_unique<LiveSessionController>(config);
// ManualOverlayDocument is always created — it IS the annotation state.
// No attach_analyzer — annotate is manual-only by default.
controller_->start();
// Wire preview:
live_preview_texture_->begin_live_stream(*controller_, cuda_device_index);
```

**Predict creates controller with analyzer, optional manual overlay:**
```cpp
// Replaces current LivePredictSession creation (app.cpp:1750-1779):
auto config = LiveSessionConfig{...};
controller_ = std::make_unique<LiveSessionController>(config);
auto analyzer = std::make_unique<RfDetrFrameAnalyzer>(...);
controller_->attach_analyzer(std::move(analyzer));
controller_->start();
live_preview_texture_->begin_live_stream(*controller_, cuda_device_index);
```

### 9. Remove Obsolete CPU Preview Path

Once the compositor + manual overlay worker handle all rendering on GPU, the following code becomes dead:

**Code to remove:**
- `src/gui/annotation_core.cpp` — `build_annotation_preview` (line 675-721): entire function. CPU mask blending and `draw_rect_bgr` for preview is replaced by GPU manual overlay kernels.
- `src/gui/annotation_core.cpp` — `draw_rect_bgr` (line 112-151): CPU box outline drawing, replaced by `launch_draw_box_outline_rgba_pitched`.
- `src/gui/app.cpp` — `submit_annotation_preview` (line 1203-1270): entire function. Background task that snapshots → builds CPU preview → uploads via `submit_host_bgr`. Replaced by `ManualOverlayDocument::generation` → worker → compositor → `LivePreviewTexture`.
- `src/gui/live_preview_texture.cpp` — `submit_host_bgr` (line 207-255): CPU→GPU frame upload path. No longer needed since all preview content stays on GPU.
- `src/gui/live_preview_texture.h` — `PendingHostFrame` struct (line 57-64), `pending_host_frame_` member (line 100), `host_frame_generation_` (line 99).

**Code to keep:**
- `src/gui/annotation_core.h` — `AnnotationInstance`, `AnnotationFrame`, `AnnotationBox` structs stay for save/export.
- `src/gui/annotation_core.cpp` — `resolve_instance`, `save_annotation_scene`, category management, mask encoding — all save-path code stays.
- `src/gui/annotation_geometry.h` — `AnnotationGeometryToolMode` enum stays (used by ManualOverlayDocument).
- `src/gui/canvas_layers.h/cpp` — Rect drag infrastructure stays — the ImGui interaction code still uses it, only the GPU rendering of the result changes.
- `src/gui/app.cpp` — `draw_annotate_workspace` (line 3271-4004): ImGui interaction code stays. Box rendering via `ImDrawList::AddRect` (line 3761-3776) stays for UI-immediate feedback. The handle rendering via `draw_box_handles` (line 442-472) stays. Brush circle preview (line 3787-3798) stays. These ImGui overlays provide instant visual feedback; the GPU manual overlay provides the composited video overlay.

### Phase 3 Implementation Order

1. **ManualOverlayDocument** — standalone type. Subsumes `AnnotationGeometryDocument` (annotation_geometry.h:38-70) and the scattered annotation state from `App` (app.h:185-201). Thread-safe snapshot with generation counter.
2. **Manual overlay CUDA kernels** — `launch_draw_manual_mask_rgba_pitched`, `launch_draw_box_outline_rgba_pitched`, `launch_draw_selection_handles_rgba_pitched`. Add to `draw_cuda.cu` parallel to existing `draw_masks_boxes_labels_bgr_pitched_kernel` (line 268-319). Same 16×16 launch config.
3. **ManualOverlayImage buffer** — grow-only `cudaMallocPitch` RGBA surface with `ensure_capacity` and `clear`. Same pattern as `AnalysisOverlayImage` from Phase 2.
4. **LiveManualOverlayWorker** — dedicated thread polling `ManualOverlayDocument::generation()`, rendering snapshots to double-buffered `ManualOverlayImage`. Lock-free front/back swap.
5. **CompositorSlot ring** — `cudaMallocPitch` BGR888 per slot, stream + event per slot. Same allocation pattern as `initialize_rendered_preview_slots` (live_predict.cpp:868-921). Same `SlotState` machine.
6. **LiveCompositor** — dedicated thread: acquire output base from fanout → copy to slot → composite analysis overlay → composite manual overlay → publish. Persistent analysis overlay state machine.
7. **Dual-layer composite kernel** — optional fused `composite_dual_rgba_over_bgr_pitched` if profiling shows the two-pass approach is a bottleneck. Otherwise reuse Phase 2's single-overlay kernel called twice.
8. **Extend LiveSessionController** — add `ManualOverlayDocument`, `LiveManualOverlayWorker`, `LiveCompositor`. Orchestrate start/stop order. Expose `persistent_analysis_overlay` toggle.
9. **Adapt LivePreviewTexture** — `pump()` reads from compositor instead of fanout. Remove Phase 2 inline analysis composite. Remove `submit_host_bgr` path.
10. **Wire annotate** — `draw_annotate_workspace` routes mutations through `controller->manual_overlay_document()`. Remove `submit_annotation_preview`, `build_annotation_preview` usage, and `AnnotationGeometryDocument`. Keep ImGui overlay drawing for instant feedback.
11. **Wire predict** — compositor composites analysis overlay automatically when analyzer is attached. Persistent overlay checkbox wired via `set_persistent_analysis_overlay`.
12. **Remove dead code** — `build_annotation_preview`, `draw_rect_bgr`, `submit_host_bgr`, `PendingHostFrame`. Gut `AnnotationGeometryDocument` (absorbed into `ManualOverlayDocument`).

## Interface Changes
- Add internal types/classes: `LiveSessionController`, `LiveVideoIngress`, `LiveFrameFanout`, `LiveAnalyzerWorker`, `LiveManualOverlayWorker`, `LiveCompositor`, `LiveFrameId`, `UiCropState`, `RuntimeCropState`, `ManualOverlayDocument`, and per-stage GPU bundle/slot structs.
- Convert live predict to use the shared controller instead of owning capture/inference orchestration directly.
- Convert live annotate to use the shared controller instead of its own live capture session.
- Keep `LivePreviewTexture` as the GL upload/present layer, but make it consume only the controller's published composited output frame.

## Test Plan
- Unit test frame ID propagation and loud mismatch rejection across source, analyzer result, output, and save.
- Unit test grow-only buffer capacity behavior, including no shrink and retry-on-next-cycle when a resize/update is skipped.
- Unit test UI crop changes racing ahead of runtime crop without blocking capture, fanout, or analysis.
- Unit test 200 ms global GUI persistence debounce with coalesced writes and no queue buildup.
- Integration test live annotate preview uses the shared GPU controller and performs no preview-path D2H copy, even if a temporary host mirror remains for CPU-only annotate consumers.
- Integration test live predict and live annotate both render from the same controller output path.
- Integration test manual overlay and analysis overlay are separate GPU images and both composite correctly over the base image.
- Integration test persistent analysis overlay behavior across faster frame cadence and slower analyzer cadence.
- Integration test manual annotate save works with no analyzer attached; model-backed save fails loudly on frame/result ID mismatch.

## Assumptions
- Overlay images are separate GPU surfaces, not in-place edits of the base frame.
- The first implementation uses small internal CUDA overlay/composite kernels rather than pulling in a new rendering dependency.
- Dead code created by removing the old annotate live path and duplicate preview logic is removed in the same change set that replaces it.
